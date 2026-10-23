#!/usr/bin/env sh
# Claude Code command hook -> forwards session state to the companion bridge.
#
# Install: reference this script from ~/.claude/settings.json (see settings.snippet.json),
# passing a state label as the argument. The hook event JSON arrives on stdin; we tag it
# with the label and POST it to the bridge. Best effort — it must never block Claude.
#
#   label: working | needs_input | idle | done | end
#
LABEL="${1:-working}"
BRIDGE="${COMPANION_BRIDGE:-http://127.0.0.1:8787}"

cat | /usr/bin/env python3 -c '
import sys, json
try:
    d = json.load(sys.stdin)
except Exception:
    d = {}
d["_label"] = sys.argv[1]
sys.stdout.write(json.dumps(d))
' "$LABEL" 2>/dev/null \
  | curl -s -m 2 -X POST "$BRIDGE/hook" -H "Content-Type: application/json" --data-binary @- >/dev/null 2>&1 || true
