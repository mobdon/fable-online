#!/usr/bin/env python3
"""PostToolUse hook (Edit|Write|MultiEdit): syntax-check the file just written. Silent on success; on a syntax error
exit 2 so Claude sees the message at once. Syntax only (py_compile / node --check), never tests, so it stays fast."""
import json, os, subprocess, sys
try:
    d = json.load(sys.stdin)
except ValueError:
    sys.exit(0)
f = (d.get('tool_input') or {}).get('file_path') or ''
if not os.path.isfile(f):
    sys.exit(0)
if f.endswith('.py'):
    cmd = [sys.executable, '-c', 'import ast,sys; ast.parse(open(sys.argv[1], "rb").read(), sys.argv[1])', f]  # no .pyc written
elif f.endswith(('.js', '.cjs', '.mjs')):
    cmd = ['node', '--check', f]
else:
    sys.exit(0)
p = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
if p.returncode:
    print(f'syntax check failed for {f}:\n' + (p.stderr or p.stdout)[-1500:], file=sys.stderr)
    sys.exit(2)
