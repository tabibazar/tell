#!/bin/sh
# PostToolUse hook: summarise a tool result onto the screen feed.
#
# Only the first two non-empty lines, clipped to the panel width. The board
# holds 48 lines, so echoing full output would bury the thread in seconds;
# the terminal still has everything.
LOG="${CLAUDE_SCREEN_LOG:-/tmp/claude-screen.log}"

payload=$(cat)

# Skip results of commands that touch the feed, or the log echoes itself.
cmd=$(printf '%s' "$payload" | jq -r '.tool_input.command // empty' 2>/dev/null)
case "$cmd" in *claude-screen.log*) exit 0 ;; esac

printf '%s' "$payload" | jq -r 'def flat:
         if type == "object" then (.stdout // .output // .content // "")
         elif type == "array" then map(tostring) | join("\n")
         elif type == "null" then ""
         else tostring end;
       (.tool_response | flat | flat)' 2>/dev/null \
  | grep -v '^[[:space:]]*$' \
  | head -2 \
  | cut -c1-60 \
  | sed 's/^/> /' >> "$LOG"
