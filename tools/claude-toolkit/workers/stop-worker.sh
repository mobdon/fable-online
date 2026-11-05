#!/bin/bash
# Stop a headless worker by the pid run-worker.sh recorded, with its whole process tree (claude -p, MCP servers, tests).
# usage: bash stop-worker.sh <worktree dir | worker name>     (name = the worktree's folder name under ~/wt)
# Never `pkill -f <pattern>` from the Bash tool: the tool's own shell has the pattern in its argv and gets killed too.
W=$1; [ -d "$W" ] || W=$HOME/wt/$W
PIDF=$(cd "$W" 2>/dev/null && cd .. && pwd)/reports/$(basename "$W").pid
[ -s "$PIDF" ] || { echo "stop-worker: no pid file $PIDF (worker not running: use TaskStop)"; exit 1; }
pid=$(cat "$PIDF")
tree() { local c; for c in $(ps -o pid= --ppid "$1"); do tree "$c"; done; echo "$1"; }
pids=$(tree "$pid"); kill -TERM $pids 2>/dev/null; sleep 3; kill -KILL $pids 2>/dev/null
rm -f "$PIDF"; echo "stop-worker: stopped $(basename "$W") (pids $(echo $pids))"
