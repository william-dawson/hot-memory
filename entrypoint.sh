#!/bin/bash
# Copy the user's code skill into ~/.claude/commands/ so it's available
# as a /my-code slash command inside Claude Code.
if [ -f /skills/my-code/SKILL.md ]; then
    cp /skills/my-code/SKILL.md /root/.claude/commands/my-code.md
fi
exec "$@"
