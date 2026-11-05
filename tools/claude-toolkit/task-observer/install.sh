#!/usr/bin/env bash
# task-observer (rebelytics/one-skill-to-rule-them-all, "One Skill to Rule Them All", CC BY 4.0, Eoghan Henn):
# watches how sessions do their tasks and logs friction, corrections and tool clashes as observation files, then
# proposes skill/rule improvements at a review that the owner approves. Idempotent; safe to run every session.
# install-all.sh calls it; standalone: bash task-observer/install.sh
KIT_DIR=${KIT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}; SHARED_DIR=${SHARED_DIR:-$([ -d /mnt/project-files ] && echo /mnt/project-files/toolkit-data || echo "$HOME/toolkit-data")}; export KIT_DIR SHARED_DIR
cd ~ || exit 1          # never run from inside the shared mount: a stale cwd there broke git/pip in install-all.sh
here=$KIT_DIR/task-observer
pin=56e8907             # v3.6.0 (2026-10-09). Bump on purpose, after reading the changelog.
ws=$SHARED_DIR/task-observer   # the ONE workspace: put SHARED_DIR on storage that survives containers
mkdir -p ~/.claude/skills ~/.claude/hooks "$ws"
tmp=$(mktemp -d)
if git clone -q --filter=blob:none https://github.com/rebelytics/one-skill-to-rule-them-all "$tmp/src" 2>/dev/null \
   && git -C "$tmp/src" checkout -q "$pin" 2>/dev/null; then
  mkdir -p "$tmp/task-observer" && cp -r "$tmp/src/SKILL.md" "$tmp/src/references" "$tmp/src/scripts" "$tmp/src/LICENSE.txt" "$tmp/task-observer/"
  # keep a pinned copy so a later GitHub outage doesn't stop the install
  [ -f "$here/task-observer-3.6.0-$pin.tgz" ] || tar czf "$here/task-observer-3.6.0-$pin.tgz" -C "$tmp" task-observer
elif [ -f "$here/task-observer-3.6.0-$pin.tgz" ]; then
  echo "task-observer: GitHub clone failed, using the pinned copy in $here"
  tar xzf "$here/task-observer-3.6.0-$pin.tgz" -C "$tmp"
else
  echo "task-observer: GitHub clone failed and no pinned copy yet; skipped"; rm -rf "$tmp"; exit 0
fi
# The skill folder must keep references/ and scripts/ beside SKILL.md (an incomplete install silently skips episodes).
rm -rf ~/.claude/skills/task-observer && cp -r "$tmp/task-observer" ~/.claude/skills/ && rm -rf "$tmp"
cp "$here/task-observer-start.py" ~/.claude/hooks/task-observer-start.py
python3 - <<'EOF'   # register the SessionStart hook next to caveman's and rtk's (idempotent; never touches other hooks)
import json, os
p = os.path.expanduser('~/.claude/settings.json')
s = json.load(open(p)) if os.path.exists(p) else {}
cmd = 'python3 ~/.claude/hooks/task-observer-start.py'
hooks = s.setdefault('hooks', {}).setdefault('SessionStart', [])
if not any(h.get('command') == cmd for e in hooks for h in e.get('hooks', [])):
    hooks.append({'hooks': [{'type': 'command', 'command': cmd, 'timeout': 10}]})
json.dump(s, open(p, 'w'), indent=2)
EOF
# The skill's "Workspace creation" command (references/observation-log.md), run every install: an empty workspace made the
# session-start scan print find errors and new-observation.sh refuse.
d="$ws/skill-observations"; mkdir -p "$d/observation-log/archive"
[ -e "$d/last-review-date.txt" ] || printf 'never\n' > "$d/last-review-date.txt"
[ -e "$d/cross-cutting-principles.md" ] || printf '# Cross-Cutting Principles\n\nPrinciples that apply to all skills. Read as a mandatory checklist during\nany skill creation or regeneration.\n\n---\n\n## Active Principles\n' > "$d/cross-cutting-principles.md"
test -f ~/.claude/skills/task-observer/references/environments.md && test -f ~/.claude/skills/task-observer/scripts/new-observation.sh \
  && echo "task-observer: installed (workspace $ws)"
