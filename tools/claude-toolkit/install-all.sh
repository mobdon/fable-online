#!/usr/bin/env bash
# Install the full tool list in a fresh cloud workspace. Run first in every new session.
# Settings (environment variables, all optional): KIT_DIR (this folder), SHARED_DIR (persistent data: task-observer log,
# claude-mem save; default ~/toolkit-data), REPO_URL + REPO_DIR (clone your repo and pin Serena/graphify there),
# CONTEXT7_API_KEY, SKILLSMP_API_KEY. Keys stay in the environment only, never in files.
KIT_DIR=${KIT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}; SHARED_DIR=${SHARED_DIR:-$([ -d /mnt/project-files ] && echo /mnt/project-files/toolkit-data || echo "$HOME/toolkit-data")}; export KIT_DIR SHARED_DIR
# Plugins and MCP servers load in the NEXT session; scripts (graphify, headroom learn, impeccable) work at once.
# Ends with check-tools.sh (one PASS/WARN/FAIL line per tool and per clash) and exits with its result, so a step that
# failed silently can't pass. Combined all-tools worker test: bash "$KIT_DIR/check-tools.sh" --worker
cd ~ || exit 1   # run from inside a mounted folder, the mount dropped the cwd mid-run and every later step failed silently
fails=0; trap 'r=$?; fails=$((fails+1)); echo "install-all: FAILED (exit $r) at line $LINENO: $BASH_COMMAND" >&2' ERR
export no_proxy="${no_proxy:+$no_proxy,}localhost,127.0.0.1" NO_PROXY="${NO_PROXY:+$NO_PROXY,}localhost,127.0.0.1" npm_config_noproxy=localhost,127.0.0.1   # append: keep the environment's direct hosts
set -x
# The cloud Setup script runs with a bare PATH: make claude, node and ~/.local/bin tools (uv, serena, rtk) findable
export PATH="$HOME/.local/bin:/opt/claude-code/bin:/opt/node22/bin:$PATH"
command -v claude >/dev/null || npm install -g -q @anthropic-ai/claude-code
claude plugin install superpowers@anthropic-plugin-directory
claude plugin marketplace add DietrichGebert/ponytail
claude plugin install ponytail@ponytail
# claude-code-setup (Anthropic, official directory): one read-only skill, claude-automation-recommender, that scans a
# repo and recommends hooks, skills, MCP servers and subagents. No hooks, MCP servers or settings of its own.
claude plugin install claude-code-setup@anthropic-plugin-directory
# claude-mem keeps its store in ~/.claude-mem, which dies with the container: restore the project's saved copy first
# (a session saves it at the end with claude-mem-save.sh).
[ -f "$SHARED_DIR/claude-mem/claude-mem.tgz" ] && [ ! -f ~/.claude-mem/claude-mem.db ] && tar xzf "$SHARED_DIR/claude-mem/claude-mem.tgz" -C ~
npx -y claude-mem install </dev/null
# claude-mem's file-read gate denies a full-file Read of any file it has notes on and points to its own smart_outline:
# that clashes with exact reads and with Serena for symbols. Off; its search/memory stay on.
python3 -c "
import json, os
p = os.path.expanduser('~/.claude-mem/settings.json'); os.makedirs(os.path.dirname(p), exist_ok=True)
s = json.load(open(p)) if os.path.exists(p) else {}
s['CLAUDE_MEM_FILE_READ_GATE_ENABLED'] = 'false'; json.dump(s, open(p, 'w'), indent=2)
"
# impeccable: `npx impeccable install` downloads from impeccable.style, which this workspace's network blocks.
# Install its skill from GitHub instead; its engine comes from the npm registry on first use.
tmp=$(mktemp -d) && git clone -q --depth 1 https://github.com/pbakaus/impeccable "$tmp/impeccable" \
  && mkdir -p ~/.claude/skills && rm -rf ~/.claude/skills/impeccable \
  && cp -r "$tmp/impeccable/.claude/skills/impeccable" ~/.claude/skills/ && rm -rf "$tmp"
sh ~/.claude/skills/impeccable/scripts/impeccable telemetry off >/dev/null 2>&1 || true   # no usage beacon
# headroom-ai needs a newer PyYAML than Debian's, which pip cannot uninstall
pip install --break-system-packages -q --ignore-installed PyYAML
pip install --break-system-packages -q "headroom-ai[mcp,proxy]" &&   # [mcp] pins mcp<2; mcp 2.x breaks headroom mcp serve
 headroom mcp install --agent claude
# headroom_compress only shrinks output when Headroom's local proxy runs (else it stores the text uncompressed, "router:noop").
# 127.0.0.1 only, upload beacon off. JSON/structured output compresses; plain-text compression needs huggingface.co (blocked).
curl -s --noproxy '*' --max-time 2 http://127.0.0.1:8787/health >/dev/null || \
  (HEADROOM_BEACON=off nohup headroom proxy --host 127.0.0.1 --port 8787 --no-subscription-tracking >/tmp/headroom-proxy.log 2>&1 &)
pip install --break-system-packages -q graphifyy && graphify install --platform claude
# user scope: without --scope it landed in local scope for the cwd only, invisible to threads in the local clone and workers
python3 -c "
import json, os
p = os.path.expanduser('~/.claude.json'); d = json.load(open(p)) if os.path.exists(p) else {}
for v in d.get('projects', {}).values(): v.get('mcpServers', {}).pop('skillsmp', None)   # old local-scope copies
json.dump(d, open(p, 'w'), indent=2)
"
claude mcp remove -s user skillsmp >/dev/null 2>&1 || true
# skillsmp.com/mcp needs a SkillsMP account (401 AUTH_REQUIRED without one). The key lives only in
# the environment variable SKILLSMP_API_KEY; the literal ${SKILLSMP_API_KEY} is expanded by Claude Code at connect time.
if [ -n "${SKILLSMP_API_KEY:-}" ]; then
  claude mcp add --scope user --transport http skillsmp https://skillsmp.com/mcp --header 'Authorization: Bearer ${SKILLSMP_API_KEY}'
else
  echo "skillsmp: no SKILLSMP_API_KEY set; registered without auth (it will answer 401)"
  claude mcp add --scope user --transport http skillsmp https://skillsmp.com/mcp
fi
git config --global --add safe.directory '*'      # the shared folder's repo is owned by another user id
# Left over from older setups: the markitdown MCP server (it needed mcp 2.x in its own env, and timed out at
# session start; the CLI below is what the rules use), the full caveman plugin (made the main session terse) and its marketplace.
claude mcp remove -s user markitdown >/dev/null 2>&1 || true
claude plugin uninstall caveman@caveman >/dev/null 2>&1 || true; claude plugin marketplace remove caveman >/dev/null 2>&1 || true

# --- Serena, Caveman, rtk, MarkItDown ---
# Serena: symbol-level code navigation/editing over language servers (find_symbol, find_referencing_symbols,
# replace_symbol_body). MCP server, loads next session. Pinned to the local clone of the shared repo when there is one, else the session cwd.
uv tool install -q git+https://github.com/oraios/serena
# fable-online is TypeScript (server, tools) and C++ (client-mod, launcher): make sure Serena runs both language servers
serena_langs() { python3 - "$1/.serena/project.yml" ${SERENA_LANGUAGES:-typescript cpp} <<'PY'
import re, sys
p, want = sys.argv[1], sys.argv[2:]
try: s = open(p).read()
except OSError: sys.exit(0)
m = re.search(r'^language_servers:\n((?:- .*\n)*)', s, re.M)
if not m: sys.exit(0)
have = [l[2:].strip() for l in m.group(1).splitlines()]
new = have + [w for w in want if w not in have]
if new != have: open(p, 'w').write(s[:m.start()] + 'language_servers:\n' + ''.join('- %s\n' % l for l in new) + s[m.end():])
PY
}
# Work happens in a local clone (REPO_DIR, default ~/<repo name>); Serena and graphify are pinned there.
# Set REPO_URL to have it cloned here; with neither set, Serena follows the folder the session starts in.
serena_proj=${REPO_DIR:-$(git -C "$KIT_DIR" rev-parse --show-toplevel 2>/dev/null)}   # fable-online: default to the repo that carries this kit
if [ -n "${REPO_URL:-}" ]; then
  serena_proj=${serena_proj:-$HOME/$(basename "${REPO_URL%.git}")}
  [ -d "$serena_proj/.git" ] || git clone -q "$REPO_URL" "$serena_proj"
fi
if [ -n "$serena_proj" ] && [ -d "$serena_proj/.git" ]; then
  [ -f "$serena_proj/.serena/project.yml" ] || yes n | serena project create "$serena_proj"   # main language inferred; "n" answers the extra-server prompts, which abort without a terminal
  serena_langs "$serena_proj"
  grep -qx '.serena/' "$serena_proj/.git/info/exclude" || echo '.serena/' >> "$serena_proj/.git/info/exclude"   # never committed
  serena_where="--project $serena_proj"
else
  serena_where="--project-from-cwd"
fi
# Serena 2.x defaults to a REPL interface (only initial_instructions + serena_repl): --agent-interface tools gives the
# named tools (get_symbols_overview, find_symbol, find_referencing_symbols, replace_symbol_body ...) the rules use.
# Its base modes include "interactive" (tells the agent to ask the user): replaced by editing + no-onboarding, since
# threads and workers make the call themselves.
serena start-mcp-server --help >/dev/null 2>&1   # creates ~/.serena/serena_config.yml on first run
python3 -c "
import os, re
p = os.path.expanduser('~/.serena/serena_config.yml')
if os.path.exists(p):
    s = open(p).read()
    open(p, 'w').write(re.sub(r'^base_modes:\n(?:- .*\n)*', 'base_modes:\n- editing\n- no-onboarding\n', s, flags=re.M))
"
claude mcp remove -s user serena >/dev/null 2>&1 || true
claude mcp add --scope user serena -- serena start-mcp-server --context=claude-code $serena_where --open-web-dashboard false --agent-interface tools
# Caveman (JuliusBrussee/caveman): terse voice for SUB-AGENTS only, via our own SubagentStart hook.
# The main session stays in normal prose (the owner reads it), so the full caveman plugin (which turns the main session terse) is not installed.
tmp=$(mktemp -d) && git clone -q --depth 1 https://github.com/JuliusBrussee/caveman "$tmp/caveman" \
  && mkdir -p ~/.claude/caveman ~/.claude/hooks && cp "$tmp/caveman/skills/caveman/SKILL.md" ~/.claude/caveman/ && rm -rf "$tmp"
cat > ~/.claude/hooks/caveman-subagent.py <<'EOF'
import json, os, re, sys
sys.stdin.read()
p = os.path.expanduser('~/.claude/caveman/SKILL.md')
if not os.path.exists(p): sys.exit(0)
body = re.sub(r'\A---.*?---\s*', '', open(p, encoding='utf-8').read(), flags=re.S)
note = ('\n\nScope: you are a sub-agent. Your report goes to the main Claude, never straight to the user. '
        'Keep code, comments, commits, docs, ledger and memory files, and anything a person will read in normal prose.')
print(json.dumps({'hookSpecificOutput': {'hookEventName': 'SubagentStart', 'additionalContext': 'CAVEMAN MODE ACTIVE (sub-agent only)\n\n' + body + note}}))
EOF
# rtk (rtk-ai/rtk, Apache-2.0 edition): PreToolUse hook that compacts noisy Bash output (git status/log, ls, find).
# GitHub's /releases/latest page is blocked here, so pin the newest tag. Code reads, greps and diffs stay exact (config below);
# test and build commands (test runners, build scripts) are never rewritten.
rtk_v=$(git ls-remote --tags --refs https://github.com/rtk-ai/rtk 'v*' | awk -F/ '{print $3}' | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | sort -V | tail -1)
curl -fsSL https://raw.githubusercontent.com/rtk-ai/rtk/refs/heads/master/install.sh | RTK_VERSION=$rtk_v RTK_EDITION=oss sh \
  && rtk init -g --auto-patch
mkdir -p ~/.config/rtk && cat > ~/.config/rtk/config.toml <<'EOF'
# Code reads, searches and diffs pass through untouched; only status/log/ls noise is compacted.
# find stays exact too: rtk's find silently skipped .worktrees/, out/ and .serena/, so a search could miss files.
[hooks]
# diff, wc, pytest, playwright, jq, curl and git with -C / -c / --no-pager / log -p / stash show were condensed too
# (found by check-tools.sh): all exact now. npm run / npx too: rtk turned `npm run lint` into `rtk lint` (plain ESLint, not the
# repo's lint script) and condensed `npm run types` / `npm run test:changed`, the repo's required gates.
# rtk ignores this list inside ( ) subshells; rtk-guard.py (below) puts it back.
exclude_commands = ["find", "grep", "rg", "cat", "head", "tail", "sed", "git diff", "git show", "git blame", "git -C", "git -c", "git --no-pager", "git log -p", "git log --patch", "git stash show", "git format-patch", "diff", "wc", "pytest", "npx playwright", "playwright", "jq", "curl", "make", "cargo test", "go test", "npx vitest", "vitest", "docker logs", "npm run", "npm test", "npx", "tsc", "eslint", "cmake", "tsx", "node --test"]

[telemetry]
enabled = false
EOF
# Syntax hook: after Edit/Write, parse .py (no .pyc) and node --check .js; silent unless the file is broken.
cp "$KIT_DIR/hooks/syntax-check.py" ~/.claude/hooks/syntax-check.py
# rtk guard: runs rtk's rewrite, then keeps NAME=... assignments
# and excluded commands inside ( ) exact, and restores graphify-out/ before git pull/rebase/merge.
cp "$KIT_DIR/hooks/rtk-guard.py" ~/.claude/hooks/rtk-guard.py
python3 - <<'EOF'   # register the caveman sub-agent hook and the syntax hook, and put the rtk guard in rtk's place (idempotent)
import json, os
p = os.path.expanduser('~/.claude/settings.json')
s = json.load(open(p)) if os.path.exists(p) else {}
guard = 'python3 ~/.claude/hooks/rtk-guard.py'
pre = s.setdefault('hooks', {}).setdefault('PreToolUse', [])
seen = False
for e in pre:
    keep = []
    for h in e.get('hooks', []):
        if h.get('command') in ('rtk hook claude', guard):
            if seen:
                continue                       # drop duplicates
            h['command'], seen = guard, True
        keep.append(h)
    e['hooks'] = keep
pre[:] = [e for e in pre if e.get('hooks')]
if not seen:
    pre.append({'matcher': 'Bash', 'hooks': [{'type': 'command', 'command': guard}]})
for ev, cmd, matcher in (('SubagentStart', 'python3 ~/.claude/hooks/caveman-subagent.py', None),
                         ('PostToolUse', 'python3 ~/.claude/hooks/syntax-check.py', 'Edit|Write|MultiEdit')):
    hooks = s.setdefault('hooks', {}).setdefault(ev, [])
    if not any(h.get('command') == cmd for e in hooks for h in e.get('hooks', [])):
        e = {'hooks': [{'type': 'command', 'command': cmd, 'timeout': 60 if matcher else 10}]}
        if matcher:
            e['matcher'] = matcher
        hooks.append(e)
# MCP_TIMEOUT: Serena/Headroom need over the 30 s default to connect; the cloud env var was missing once and Serena timed
# out . RTK_SUPPRESS_HOOK_WARNING: rtk-guard replaces rtk's own hook, and rtk otherwise prints
# "No hook installed, run rtk init" into command output . 
for k, v in (('MCP_TIMEOUT', '120000'), ('RTK_SUPPRESS_HOOK_WARNING', '1')):
    s.setdefault('env', {}).setdefault(k, v)
json.dump(s, open(p, 'w'), indent=2)
EOF
# task-observer (rebelytics, pinned 56e8907): logs task friction and tool clashes to
# $SHARED_DIR/task-observer for review. Workers get TASK_OBSERVER_ROLE=worker from run-worker.sh.
bash "$KIT_DIR/task-observer/install.sh"
# MarkItDown: turn uploaded PDF/Word/Excel/PowerPoint files into Markdown: `markitdown file.xlsx > file.md`
pip install --break-system-packages -q "markitdown[pdf,docx,xlsx,pptx]"
# Context7 (Upstash): current docs for libraries/tools (Playwright, npm packages). Needs mcp.context7.com allowed in the
# cloud environment's network settings; skipped quietly until then.
if [ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 https://mcp.context7.com/mcp)" != "000" ]; then
  # The free anonymous tier runs out of monthly quota: put a key in the environment variable CONTEXT7_API_KEY
  # (cloud environment settings; never in files). set +x keeps the key out of the log.
  set +x
  claude mcp remove -s user context7 >/dev/null 2>&1 || true
  if [ -n "${CONTEXT7_API_KEY:-}" ]; then
    # Literal ${CONTEXT7_API_KEY}: Claude Code expands it from the environment when it connects, so the key never lands in ~/.claude.json.
    claude mcp add --scope user --transport http context7 https://mcp.context7.com/mcp --header 'CONTEXT7_API_KEY: ${CONTEXT7_API_KEY}'
  else
    echo "context7: no CONTEXT7_API_KEY set; using the anonymous tier (monthly quota)"
    claude mcp add --scope user --transport http context7 https://mcp.context7.com/mcp
  fi
  set -x
else
  echo "context7: mcp.context7.com not reachable (allow it in the environment's network settings); skipped"
fi

# --- Commit guard, then the check ---
# OmniRoute (AI gateway) is opt-in and NOT installed here: never in the API path without the owner's yes.
# Commit guard in the local clone (worktrees share it): refuses zips, C2PA-stamped PNGs, graphify-out/ and line-ending flips.
if [ -d "${serena_proj:-/nonexistent}/.git" ]; then
  cp "$KIT_DIR/git-hooks/pre-commit" "$serena_proj/.git/hooks/pre-commit" && chmod +x "$serena_proj/.git/hooks/pre-commit"
fi
set +x
trap - ERR
[ "$fails" -gt 0 ] && echo "install-all: $fails install command(s) failed (see FAILED lines above); the check below shows which tools it hit"
bash "$KIT_DIR/check-tools.sh"
rc=$?
# The session that ran this install never sees the plugins and MCP servers it added.
echo "install-all: this session can't call newly installed plugin Skills or MCP servers. For a Skill, read its SKILL.md"
echo "  (find ~/.claude/plugins/cache -path '*skills/<name>/SKILL.md') and follow it; hand Serena/claude-mem work to a worker."
[ "$fails" -gt 0 ] && exit 1
exit $rc
