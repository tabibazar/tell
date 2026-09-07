#!/bin/sh
# PreToolUse hook: put the meaningful part of a Bash command on the feed.
LOG="${CLAUDE_SCREEN_LOG:-/tmp/claude-screen.log}"

payload=$(cat)
cmd=$(printf '%s' "$payload" | jq -r '.tool_input.command // empty' 2>/dev/null)
[ -n "$cmd" ] || exit 0

# Reading or writing the feed would echo the log back into the log.
case "$cmd" in *claude-screen.log*) exit 0 ;; esac

# Most commands begin by cd-ing into the project, either as a leading
# "cd ... &&" or as a line of its own. That prefix is never the interesting
# part, so drop it and show what actually ran.
printf '%s\n' "$cmd" \
  | sed -e 's/^[[:space:]]*cd [^&|;]*&&[[:space:]]*//' \
  | grep -vE '^[[:space:]]*cd [^&|;]*$' \
  | grep -v '^[[:space:]]*$' \
  | head -1 \
  | cut -c1-60 \
  | sed 's/^/$ /' >> "$LOG"
