#!/usr/bin/env python3
"""SessionStart hook for task-observer (rebelytics/one-skill-to-rule-them-all).
install.sh copies this to ~/.claude/hooks/ and registers it. It only adds context (never blocks), so it sits next to
superpowers', claude-mem's and ponytail's SessionStart hooks without clashing.
- Main sessions (threads, the coordinator): invoke the skill, run its Session Start scan, offer the review when due.
- Headless workers (run-worker.sh exports TASK_OBSERVER_ROLE=worker): never load the skill, never write the log;
  end the report with an OBSERVATIONS: line and the controller logs it (the skill's own subagent rule)."""
import datetime, glob, json, os, re, sys

sys.stdin.read()
WS = os.environ.get('TASK_OBSERVER_WORKSPACE') or os.path.join(os.environ.get('SHARED_DIR') or ('/mnt/project-files/toolkit-data' if os.path.isdir('/mnt/project-files') else os.path.expanduser('~/toolkit-data')), 'task-observer')
SKILL = os.path.expanduser('~/.claude/skills/task-observer')

if os.environ.get('TASK_OBSERVER_ROLE') == 'worker':
    msg = ('task-observer: you are a worker. Do NOT invoke the task-observer skill and do NOT write to ' + WS + '. '
           'If you hit friction worth keeping (a tool clash, a rule that slowed you down, a correction, a step you had to '
           'work out yourself), end your report with one line "OBSERVATIONS: <short items>" (or "OBSERVATIONS: none"). '
           'The controller logs them.')
else:
    log = os.path.join(WS, 'skill-observations', 'observation-log')
    n_open = 0
    for f in glob.glob(os.path.join(log, '*.md')):
        try:
            if re.search(r'^status: open$', open(f, encoding='utf-8').read(), re.M): n_open += 1
        except OSError:
            pass
    try:
        last = open(os.path.join(WS, 'skill-observations', 'last-review-date.txt')).read().strip() or 'never'
    except OSError:
        last = 'never'
    msg = ('task-observer: invoke the task-observer skill before planning, then run its Session Start scan: '
           'bash ' + SKILL + '/scripts/session-start-scan.sh ' + WS + ' . The workspace is ' + WS +
           ' (never the repo or the cwd). Create observation files only with: bash ' + SKILL +
           '/scripts/new-observation.sh <slug> ' + WS + ' . Workers you start do not log; you log what their '
           'OBSERVATIONS: lines report. ' + str(n_open) + ' open observations; last review: ' + last + '.')
    cutoff = (datetime.date.today() - datetime.timedelta(days=7)).isoformat()
    if n_open and (last == 'never' or last <= cutoff):
        msg += ' A review is due: offer it to the owner in the end-of-patch report. Kit and worker-rule fixes go in under the owner\'s standing order; keys, security and deploys still need the owner\'s named approval.'
    if not os.path.isdir(SKILL):
        msg = 'task-observer: skill not installed in this container; run bash $KIT_DIR/task-observer/install.sh'

print(json.dumps({'hookSpecificOutput': {'hookEventName': 'SessionStart', 'additionalContext': msg}}))
