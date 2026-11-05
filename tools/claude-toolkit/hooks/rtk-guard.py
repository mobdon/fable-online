#!/usr/bin/env python3
"""PreToolUse hook for Bash, in place of rtk's own `rtk hook claude`: runs rtk's rewrite, then fixes three things.
1. A bare assignment whose value ends in a task-runner name (S=/x/task-observer, S="/x/just") became `rtk S=...`,
   which runs "S=..." as a program: the variable is lost and every later $S path is empty.
   `rtk ` before NAME= is removed.
2. Inside ( ) subshells rtk ignores exclude_commands, so `(cd x; git diff)` became `rtk git diff` (condensed).
   `rtk ` before any excluded command is removed, so reads, greps, diffs and tests stay exact everywhere.
3. `graphify update .` rewrites the tracked graphify-out/, so a later pull/rebase/merge stopped on "unstaged
   changes". Each `git pull|rebase|merge` (not --continue/--abort) first restores graphify-out/;
   run `graphify update .` after it.
Only command positions outside heredoc bodies are touched (shortcut: `echo "a; git pull"` still
matches; parse with bashlex if that ever bites). Everything else rtk rewrites (git status, git log, ls ...) passes through."""
import json, os, re, subprocess, sys
try:
    import tomllib
    EXCL = tomllib.load(open(os.path.expanduser('~/.config/rtk/config.toml'), 'rb'))['hooks']['exclude_commands']
except Exception:  # noqa: BLE001 - no config: keep the other fixes
    EXCL = []
raw = sys.stdin.read()
try:
    orig = json.loads(raw)['tool_input']['command']
except (ValueError, KeyError, TypeError):
    sys.exit(0)
try:
    out = subprocess.run(['rtk', 'hook', 'claude'], input=raw, capture_output=True, text=True, timeout=20).stdout.strip()
    o = json.loads(out) if out else None
    cmd = o['hookSpecificOutput']['updatedInput']['command'] if o else orig
except (OSError, subprocess.TimeoutExpired, ValueError, KeyError, TypeError):
    o, cmd = None, orig                # rtk missing, hung or odd output: no compaction, the other fixes still apply
alts = '|'.join(re.escape(e) for e in sorted(EXCL, key=len, reverse=True))
AT = r'(^|[;&|({\n]\s*)'              # command position only, so text in strings and messages is left alone
STRIP = re.compile(AT + r'rtk (?=[A-Za-z_][A-Za-z0-9_]*=' + (r'|(?:' + alts + r')(?:[\s;|&)]|$))' if alts else ')'))
PULL = re.compile(AT + r'(rtk )?git ((?:-C \S+ )?)(pull|rebase|merge)\b(?! --(?:continue|abort|skip|quit))')


def fix(code):
    code = STRIP.sub(r'\1', code)
    return PULL.sub(r'\1{ git \3checkout -q -- graphify-out 2>/dev/null; true; } && \2git \3\4', code)


# Heredoc bodies are data (file contents, commit messages): only the lines outside them are fixed.
parts, end = [], None
for line in cmd.split('\n'):
    if end is not None:
        parts.append(line)
        if line.strip() == end:
            end = None
        continue
    parts.append(fix(line))
    m = re.search(r'<<-?\s*([\'"]?)(\w+)\1', line)
    end = m.group(2) if m else None
cmd = '\n'.join(parts)
if cmd == orig:
    sys.exit(0)                        # no output: run the command as typed
if o is None:
    o = {'hookSpecificOutput': {'hookEventName': 'PreToolUse', 'permissionDecisionReason': 'kit Bash guard',
                                'updatedInput': dict(json.loads(raw)['tool_input'])}}
o['hookSpecificOutput']['updatedInput']['command'] = cmd
print(json.dumps(o))
