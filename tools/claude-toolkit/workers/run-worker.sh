#!/bin/bash
# Headless worker with the full tool set (Serena on ITS worktree, Context7, Headroom, claude-mem, SkillsMP;
# Caveman voice; rtk, syntax and task-observer hooks from ~/.claude/settings.json).
# usage: run-worker.sh <worktree dir> <brief file> [--resume]
#   brief file: the whole task (common rules + the item). With --resume the brief is sent as a follow-up to the
#   worker's last conversation in that worktree (claude -c), e.g. a controller update; its context is kept.
# Log: <worktree>/../reports/<name>.log ; final report path is whatever the brief says.
# Controller: start it with Bash run_in_background and timeout 7200000 (2 h; the 30 min default killed a worker
# mid-run with its edits uncommitted). If one is stopped anyway: --resume it with 'commit what you have, then continue'.
# Late owner changes: append them to <worktree>/../reports/inbox-<name>.md; the worker re-reads it before each commit
# and before its final report. Stop/--resume only for changes that invalidate work in progress.
# Stop a worker: TaskStop on its background task, or `bash stop-worker.sh <worktree>` (pid file); never pkill -f.
KIT_DIR=${KIT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}; SHARED_DIR=${SHARED_DIR:-$([ -d /mnt/project-files ] && echo /mnt/project-files/toolkit-data || echo "$HOME/toolkit-data")}; export KIT_DIR SHARED_DIR
set -u; WT=$(cd "$1" && pwd); BRIEF=$2; MODE=${3:-}; NAME=$(basename "$WT"); OUT=$(dirname "$WT")/reports; mkdir -p "$OUT"
cd "$WT"
# Refuse a brief copied from brief-common-template.md whose <...> lines were never filled in (check-tools expects this).
left=$(grep -oE '<(the hand-off doc|project rules:|the session.s attribution lines)[^>]*>' "$BRIEF" | head -3 | tr '\n' ' ')
[ -z "$left" ] || { echo "run-worker.sh: brief $BRIEF still has: $left(fill in the template's <...> lines first)" >&2; exit 2; }
INBOX="$OUT/inbox-$NAME.md"; [ -f "$INBOX" ] || printf '# Inbox for worker %s: the controller appends late owner changes here.\n' "$NAME" > "$INBOX"
echo $$ > "$OUT/$NAME.pid"; trap 'rm -f "$OUT/$NAME.pid"' EXIT
pgrep -x graphify >/dev/null || rm -f graphify-out/.rebuild.lock   # a stopped worker can leave a stale lock
[ -f .serena/project.yml ] || yes n | serena project create --name "$NAME" "$WT" >/dev/null 2>&1   # "n": extra-server prompts abort without a terminal
G=$(git rev-parse --git-common-dir); grep -qx '.serena/' "$G/info/exclude" 2>/dev/null || echo '.serena/' >> "$G/info/exclude"
MEM=$(ls -d /root/.claude/plugins/cache/thedotmack/claude-mem/*/ 2>/dev/null | sort -V | tail -1)
curl -s --noproxy '*' --max-time 2 http://127.0.0.1:8787/health >/dev/null || \
  (HEADROOM_BEACON=off nohup headroom proxy --host 127.0.0.1 --port 8787 --no-subscription-tracking >/tmp/headroom-proxy.log 2>&1 &)   # headroom_compress needs it
CFG="$OUT/mcp-$NAME.json"
# Serena's first start in a fresh container can pass Claude's 30 s MCP connect limit (CONNECT_TIMEOUT), and the
# worker then runs without it. Warm it with one handshake (retried once) and give MCP servers 2 minutes to connect.
serena_up() { python3 - "$WT" <<'PY'
import json, subprocess, sys
p = subprocess.Popen(['serena', 'start-mcp-server', '--context=claude-code', '--project', sys.argv[1], '--open-web-dashboard',
                      'false', '--agent-interface', 'tools'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
p.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {'protocolVersion': '2024-11-05',
              'capabilities': {}, 'clientInfo': {'name': 'run-worker', 'version': '1'}}}) + '\n'); p.stdin.flush()
import select
ok = bool(select.select([p.stdout], [], [], 90)[0]) and '"result"' in p.stdout.readline()
p.kill(); sys.exit(0 if ok else 1)
PY
}
serena_up || serena_up || echo "run-worker.sh: Serena did not answer twice; the worker starts without a warm Serena" >&2
export MCP_TIMEOUT=120000
python3 - "$WT" "$MEM" "$CFG" <<'PY'
import json,sys
wt,mem,cfg=sys.argv[1:4]
s={"serena":{"command":"serena","args":["start-mcp-server","--context=claude-code","--project",wt,"--open-web-dashboard","false","--agent-interface","tools"]},
   "context7":{"type":"http","url":"https://mcp.context7.com/mcp"},
   "headroom":{"command":"/usr/local/bin/headroom","args":["mcp","serve"]},
   "skillsmp":{"type":"http","url":"https://skillsmp.com/mcp"}}
import os
if os.environ.get("CONTEXT7_API_KEY"): s["context7"]["headers"]={"CONTEXT7_API_KEY":"${CONTEXT7_API_KEY}"}  # expanded by claude at connect time; the key is never written
if mem: s["claude-mem"]={"command":"node","args":[mem.rstrip('/')+"/scripts/mcp-server.cjs"],"env":{"CLAUDE_PLUGIN_ROOT":mem.rstrip('/')}}
json.dump({"mcpServers":s},open(cfg,"w"),indent=1)
PY
CAVE=$(python3 -c "import re,os;print(re.sub(r'\A---.*?---\s*','',open(os.path.expanduser('~/.claude/caveman/SKILL.md')).read(),flags=re.S))" 2>/dev/null)
NOTE="CAVEMAN MODE ACTIVE (worker only): your final reply goes to the controller Claude, never to a person. Keep code, comments, commits, docs, ledger and report files in normal prose.
Your inbox is $INBOX: re-read it before each commit and before your final report, and act on anything new in it (the controller appends late owner changes there)."
export TASK_OBSERVER_ROLE=worker WORKER_INBOX=$INBOX
C=(); [ "$MODE" = "--resume" ] && C=(-c)
claude "${C[@]}" -p "$(cat "$BRIEF")" --append-system-prompt "$CAVE

$NOTE" --strict-mcp-config --mcp-config "$CFG" \
  --allowedTools "Bash,Edit,Write,Read,Grep,Glob,Agent,Task,TodoWrite,Skill,mcp__serena__*,mcp__context7__*,mcp__headroom__*,mcp__claude-mem__*,mcp__skillsmp__*" \
  --max-turns 600 </dev/null >> "$OUT/$NAME.log" 2>&1
echo "WORKER $NAME EXIT $?" >> "$OUT/$NAME.log"
