import { Server } from '@modelcontextprotocol/sdk/server/index.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import { ListToolsRequestSchema, CallToolRequestSchema } from '@modelcontextprotocol/sdk/types.js';
import { spawn } from 'node:child_process';
import { writeFileSync } from 'node:fs';

// ── State persisted across tool calls ─────────────────────────────────────────
const state = {
  fpEvents: process.env.WSS_PERF_FP_EVENTS || '',
  capabilityChecked: false,
};

// ── Shell helper ───────────────────────────────────────────────────────────────
// Async so long-running profiling jobs don't block the MCP server event loop.
function sh(cmd, extraEnv = {}) {
  return new Promise((resolve) => {
    const proc = spawn('/bin/sh', ['-c', cmd], {
      env: { ...process.env, ...extraEnv },
    });
    let stdout = '';
    let stderr = '';
    proc.stdout.on('data', (d) => { stdout += d.toString(); });
    proc.stderr.on('data', (d) => { stderr += d.toString(); });
    proc.on('close', (code) => resolve({ stdout, stderr, exitCode: code ?? -1 }));
    proc.on('error', (err) => resolve({ stdout, stderr: err.message, exitCode: -1 }));
  });
}

function parseKeyValueOutput(output) {
  const values = {};
  for (const line of output.split('\n')) {
    const idx = line.indexOf('=');
    if (idx <= 0) continue;
    const key = line.slice(0, idx).trim();
    const value = line.slice(idx + 1).trim();
    values[key] = value;
  }
  return values;
}

// ── MPI rank-0 wrapper ─────────────────────────────────────────────────────────
// Writes a wrapper script so quoting is not an issue, then invokes it.
// If mpirunPrefix is empty, just runs rank0Prefix + binaryCommand directly.
function buildMpiRank0Command(mpirunPrefix, rank0Prefix, binaryCommand) {
  const wrapPath = '/tmp/wss_rank0_wrap.sh';
  const wrapContent = [
    '#!/bin/sh',
    `if [ "$OMPI_COMM_WORLD_RANK" = "0" ]; then`,
    `  exec ${rank0Prefix} ${binaryCommand}`,
    `else`,
    `  exec ${binaryCommand} 2>/dev/null`,
    `fi`,
  ].join('\n') + '\n';
  writeFileSync(wrapPath, wrapContent, { mode: 0o755 });

  if (mpirunPrefix) {
    return `${mpirunPrefix} ${wrapPath}`;
  } else {
    return `${rank0Prefix} ${binaryCommand}`;
  }
}

// ── WSS output parser ──────────────────────────────────────────────────────────
// Parses [WSS] lines from combined stdout+stderr.
function parseWssOutput(combined) {
  const measurements = [];
  const errors = [];
  const initMessages = [];

  // Regex for measurement lines:
  // [WSS] kernel_name   512.0 MB hot   1024.0 MB accessed   0.480 GFLOP   0.98 FLOP/B-hot   0.47 FLOP/B-acc
  const measureRe = /\[WSS\]\s+(\S+)\s+([\d.]+)\s+MB\s+hot\s+([\d.]+)\s+MB\s+accessed\s+([\d.]+)\s+GFLOP\s+([\d.]+)\s+FLOP\/B-hot\s+([\d.]+)\s+FLOP\/B-acc/;

  for (const line of combined.split('\n')) {
    if (!line.includes('[WSS]')) continue;

    const m = measureRe.exec(line);
    if (m) {
      measurements.push({
        kernel: m[1],
        hot_mb: parseFloat(m[2]),
        accessed_mb: parseFloat(m[3]),
        gflop: parseFloat(m[4]),
        flop_per_byte_hot: parseFloat(m[5]),
        flop_per_byte_acc: parseFloat(m[6]),
      });
    } else if (line.includes('[WSS] ERROR')) {
      errors.push(line.trim());
    } else {
      initMessages.push(line.trim());
    }
  }

  return { measurements, errors, initMessages };
}

// ── Tool definitions ───────────────────────────────────────────────────────────
const TOOLS = [
  {
    name: 'wss_capability_check',
    description:
      'Phase 0: Detect available metrics on this machine. Always run first before any other tool. ' +
      'Checks PAPI component status, PAPI FP/mem events, perf_event_paranoid, perf stat, raw PMU FP event codes ' +
      '(via wss_probe_fp_events), and /proc/self/clear_refs. Stores fp_events internally for use by wss_run_profiled.',
    inputSchema: {
      type: 'object',
      properties: {
        extra_codes: {
          type: 'array',
          items: { type: 'string' },
          description: 'Extra PMU event codes to probe beyond defaults (e.g. ["0x74","0x75"])',
        },
      },
    },
  },
  {
    name: 'wss_measure_baseline',
    description:
      'Phase 1: Measure peak RSS (total memory allocation) using /usr/bin/time -v. Returns peak_rss_mb for rank 0. ' +
      'Run before instrumentation — this is the upper bound that hot working sets will be compared against.',
    inputSchema: {
      type: 'object',
      properties: {
        mpirun_prefix: {
          type: 'string',
          description: 'Full mpirun command prefix (e.g. "mpirun -np 4"). Omit for non-MPI codes.',
        },
        binary_command: {
          type: 'string',
          description: 'The binary and its arguments (e.g. "./solver input.cfg")',
        },
      },
      required: ['binary_command'],
    },
  },
  {
    name: 'wss_perf_profile',
    description:
      'Phase 2: Identify hotspot functions via perf record/report. Run after baseline to know which kernels to instrument. ' +
      'Returns top functions by sample percentage and raw perf report.',
    inputSchema: {
      type: 'object',
      properties: {
        mpirun_prefix: {
          type: 'string',
          description: 'Full mpirun command prefix (e.g. "mpirun -np 4"). Omit for non-MPI codes.',
        },
        binary_command: {
          type: 'string',
          description: 'The binary and its arguments (e.g. "./solver input.cfg")',
        },
      },
      required: ['binary_command'],
    },
  },
  {
    name: 'wss_run_profiled',
    description:
      'Phase 3: Run a binary instrumented with WSS_BEGIN/WSS_END macros (built with -DPROFILE_WSS). ' +
      'Automatically injects WSS_PERF_FP_EVENTS from the capability check. Returns structured per-kernel measurements. ' +
      'wss_capability_check must be called first.',
    inputSchema: {
      type: 'object',
      properties: {
        mpirun_prefix: {
          type: 'string',
          description: 'Full mpirun command prefix (e.g. "mpirun -np 4"). Omit for non-MPI codes.',
        },
        binary_command: {
          type: 'string',
          description: 'The binary and its arguments (e.g. "./solver input.cfg")',
        },
      },
      required: ['binary_command'],
    },
  },
];

// ── Tool implementations ───────────────────────────────────────────────────────

async function wssCapabilityCheck(args) {
  const extraCodes = args.extra_codes || [];
  const available = [];
  const unavailable = [];
  state.fpEvents = '';

  // 1. Architecture
  const archResult = await sh('uname -m');
  const arch = archResult.stdout.trim();

  // 2. perf_event_paranoid
  const paranoidResult = await sh('cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null');
  const perfEventParanoid = paranoidResult.exitCode === 0
    ? parseInt(paranoidResult.stdout.trim(), 10)
    : null;

  // 3. perf stat test
  const perfStatResult = await sh('perf stat -e cycles -- echo ok 2>&1');
  const perfStatOk = perfStatResult.exitCode === 0 && !perfStatResult.stdout.includes('Permission denied');
  if (perfStatOk) {
    available.push('perf sampling (hotspot discovery via perf record/report)');
  } else {
    unavailable.push('perf stat (permission denied — perf_event_paranoid may need to be set to ≤1 by sysadmin)');
  }

  // 4. authoritative WSS runtime probe
  const runtimeArg = extraCodes.length > 0 ? ` ${extraCodes.join(',')}` : '';
  const runtimeResult = await sh(`wss_runtime_probe${runtimeArg} 2>&1`);
  const runtimeOutput = runtimeResult.stdout + (runtimeResult.stderr || '');
  const runtimeValues = parseKeyValueOutput(runtimeResult.stdout);

  const hotMb = parseFloat(runtimeValues.HOT_MB || '0');
  const accessedMb = parseFloat(runtimeValues.ACCESSED_MB || '0');
  const gflop = parseFloat(runtimeValues.GFLOP || '0');
  const hotBytesOk = runtimeValues.HOT_BYTES_OK === '1';
  const fpOk = runtimeValues.FP_OK === '1';
  const memBytesOk = runtimeValues.MEM_BYTES_OK === '1';
  const fpSource = runtimeValues.FP_SOURCE || 'none';
  const fpEventsEnv = runtimeValues.WSS_PERF_FP_EVENTS_ENV || '';
  const fpEventsProvenance = runtimeValues.FP_EVENTS_PROVENANCE || 'none';
  const capabilityTruthSource = runtimeValues.CAPABILITY_TRUTH_SOURCE || 'wss_runtime_probe';
  const papiFpEventCount = parseInt(runtimeValues.PAPI_FP_EVENT_COUNT || '0', 10);
  const papiMemEventCount = parseInt(runtimeValues.PAPI_MEM_EVENT_COUNT || '0', 10);
  const perfFpFdCount = parseInt(runtimeValues.PERF_FP_FD_COUNT || '0', 10);
  const fpEvents = fpEventsEnv ? fpEventsEnv.split(',').filter(Boolean) : [];

  if (hotBytesOk) {
    available.push('hot-byte measurement (verified through WSS runtime probe)');
  } else {
    unavailable.push('hot-byte measurement — WSS runtime probe reported 0 hot MB (run container with --privileged)');
  }

  if (fpOk && fpSource === 'papi') {
    available.push('FLOP count (verified through WSS runtime probe using PAPI)');
  } else if (fpOk && fpSource === 'perf_fallback' && fpEventsEnv) {
    state.fpEvents = fpEventsEnv;
    available.push(`perf_event_open FP fallback: WSS_PERF_FP_EVENTS=${state.fpEvents}`);
  } else {
    unavailable.push('FLOP count — WSS runtime probe reported 0 GFLOP');
  }

  if (memBytesOk) {
    available.push('Bytes accessed + reuse factor (verified through WSS runtime probe)');
  } else {
    unavailable.push('Bytes accessed / reuse factor — WSS runtime probe reported 0 accessed MB');
  }

  state.capabilityChecked = true;

  return {
    arch,
    capability_truth_source: capabilityTruthSource,
    papi_component: fpSource === 'papi' || papiMemEventCount > 0 ? 'active' : 'inactive',
    papi_component_error: '',
    papi_fp_events: [],
    papi_mem_events: [],
    papi_fp_event_count: papiFpEventCount,
    papi_mem_event_count: papiMemEventCount,
    perf_event_paranoid: perfEventParanoid,
    perf_stat_ok: perfStatOk,
    fp_probe_output: runtimeOutput,
    fp_events: fpEvents,
    fp_events_provenance: fpEventsProvenance,
    clear_refs_ok: hotBytesOk,
    fp_source: fpSource,
    perf_fp_fd_count: perfFpFdCount,
    hot_mb_probe: hotMb,
    accessed_mb_probe: accessedMb,
    gflop_probe: gflop,
    runtime_probe_output: runtimeOutput,
    summary: { available, unavailable },
  };
}

async function wssMeasureBaseline(args) {
  const { mpirun_prefix: mpirunPrefix = '', binary_command: binaryCommand } = args;
  const cmd = buildMpiRank0Command(mpirunPrefix, '/usr/bin/time -v', binaryCommand);
  const result = await sh(cmd);
  const combined = result.stdout + result.stderr;

  let peakRssMb = null;
  const rssMatch = combined.match(/Maximum resident set size \(kbytes\):\s*(\d+)/);
  if (rssMatch) {
    peakRssMb = Math.round(parseInt(rssMatch[1], 10) / 1024 * 100) / 100;
  }

  const note = peakRssMb === null
    ? 'Could not parse peak RSS — check raw_output for "/usr/bin/time -v" output'
    : `Peak RSS for rank 0: ${peakRssMb} MB. This is the upper bound for hot working set comparisons.`;

  return {
    peak_rss_mb: peakRssMb,
    exit_code: result.exitCode,
    note,
    raw_output: combined,
  };
}

async function wssPerfProfile(args) {
  const { mpirun_prefix: mpirunPrefix = '', binary_command: binaryCommand } = args;
  const recordCmd = buildMpiRank0Command(
    mpirunPrefix,
    'perf record -g -F 99 -o /tmp/wss_perf.data --',
    binaryCommand,
  );
  const recordResult = await sh(recordCmd);

  const reportResult = await sh(
    'perf report -n --stdio --no-children -i /tmp/wss_perf.data 2>/dev/null | head -80',
  );

  return {
    raw_report: reportResult.stdout,
    perf_record_output: recordResult.stdout + recordResult.stderr,
    exit_code: recordResult.exitCode,
  };
}

async function wssRunProfiled(args) {
  if (!state.capabilityChecked) {
    return {
      error: 'wss_capability_check must be called before wss_run_profiled. Run it first to detect available metrics and configure FP event codes.',
    };
  }

  const { mpirun_prefix: mpirunPrefix = '', binary_command: binaryCommand } = args;

  let cmd;
  let extraEnv = {};
  if (state.fpEvents) {
    extraEnv = { WSS_PERF_FP_EVENTS: state.fpEvents };
  }

  if (mpirunPrefix) {
    // Inject WSS_PERF_FP_EVENTS into the environment for the mpirun invocation
    const envPrefix = state.fpEvents ? `WSS_PERF_FP_EVENTS=${state.fpEvents}` : '';
    cmd = `${envPrefix ? envPrefix + ' ' : ''}${mpirunPrefix} ${binaryCommand}`;
    extraEnv = {}; // already baked into cmd string
  } else {
    cmd = binaryCommand;
    // extraEnv carries WSS_PERF_FP_EVENTS
  }

  const result = await sh(cmd, extraEnv);
  const combined = result.stdout + result.stderr;
  const { measurements, errors, initMessages } = parseWssOutput(combined);

  return {
    measurements,
    errors,
    init_messages: initMessages,
    fp_events_used: state.fpEvents || null,
    exit_code: result.exitCode,
    stdout: result.stdout,
    stderr: result.stderr,
  };
}

// ── MCP server setup ───────────────────────────────────────────────────────────
const server = new Server(
  { name: 'wss-profiler', version: '1.0.0' },
  { capabilities: { tools: {} } },
);

server.setRequestHandler(ListToolsRequestSchema, async () => ({ tools: TOOLS }));

server.setRequestHandler(CallToolRequestSchema, async (request) => {
  const { name, arguments: args = {} } = request.params;

  let result;
  switch (name) {
    case 'wss_capability_check':
      result = await wssCapabilityCheck(args);
      break;
    case 'wss_measure_baseline':
      result = await wssMeasureBaseline(args);
      break;
    case 'wss_perf_profile':
      result = await wssPerfProfile(args);
      break;
    case 'wss_run_profiled':
      result = await wssRunProfiled(args);
      break;
    default:
      result = { error: `Unknown tool: ${name}` };
  }

  return {
    content: [{ type: 'text', text: JSON.stringify(result, null, 2) }],
  };
});

const transport = new StdioServerTransport();
await server.connect(transport);
