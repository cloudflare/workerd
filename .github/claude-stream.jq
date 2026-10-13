# Renders Claude Code's stream-json output as readable log lines. Tool calls appear as soon as
# Claude makes them, so a command that hangs shows up as a call with no result.
def clip: if length > 2000 then .[:2000] + "..." else . end;

fromjson?
| if .type == "assistant" then
    .message.content[]
    | if .type == "text" then "Claude: \(.text)"
      elif .type == "thinking" then "Thinking: \(.thinking)"
      elif .type == "tool_use" then "Tool call: \(.name) \(.input | tojson | clip)"
      else empty end
  elif .type == "user" then
    .message.content[]?
    | select(type == "object" and .type == "tool_result")
    | "Tool result: \(.content | if type == "string" then . else tojson end | clip)"
  elif .type == "result" then
    "Result: \(.subtype) after \(.num_turns) turns, \(.duration_ms / 60000 | floor) minutes"
  else empty end
