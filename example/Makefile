CC       = mpicc
CFLAGS   = -O2 -fopenmp -Wall
LDFLAGS  =

# Build with profiling enabled.
# Inside the container the header lives at /skills/wss-profiler/wss_profiler.h.
# Outside the container (repo checkout) it lives one level up.
SKILL_DIR ?= /skills/wss-profiler
ifneq ($(wildcard $(SKILL_DIR)/wss_profiler.h),)
  WSS_INCLUDE = $(SKILL_DIR)
else
  WSS_INCLUDE = ..
endif

profile: CFLAGS  += -DPROFILE_WSS -I$(WSS_INCLUDE)
profile: LDFLAGS += -lpapi
profile: bench

# Build without profiling (macros compile away)
bench: bench.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Run (2 ranks); capture WSS output from stderr
run: bench
	mpirun -np 2 ./bench 2>&1

clean:
	rm -f bench

.PHONY: profile run clean
