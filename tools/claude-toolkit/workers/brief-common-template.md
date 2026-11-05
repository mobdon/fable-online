# Worker rules template -- copy, then fill in the <...> lines for this job

You implement ONE item in your own git worktree (named in your task). You are a headless worker: nobody answers
questions, so make the call, write it down, keep going. Report BLOCKED only for a real blocker.

Read first: <the hand-off doc / ledger paragraphs / notes this item needs>.

Your FIRST three actions, in this order (the controller checks your transcript with audit.py):
1. One claude-mem search for your item's keywords.
2. `graphify update .` in your worktree, then at least one `graphify explain <symbol or file>` / `graphify path A B`
   (or a `graphify query` naming a function or file) for your item.
3. Serena get_symbols_overview on every source file you expect to change.
Then: every function you change is found with Serena find_symbol / find_referencing_symbols first.

Tools (all are expected to be used; your final reply must say how):
- graphify: before opening any big file; it finds the file or system, then Serena finds the exact symbol. Don't grep
  the repo for something graphify answers. Don't commit graphify-out/* (`git checkout -- graphify-out` before commit).
  graphify-out/ is tracked, so `graphify update .` leaves it dirty: run `git checkout -- graphify-out` before any
  rebase or pull (the Bash hook adds it to git pull/rebase/merge for you) and `graphify update .` after it.
- Serena (MCP, pointed at your worktree, tools interface): find_symbol, find_referencing_symbols, get_symbols_overview,
  and replace_symbol_body / insert_after_symbol for whole-function edits. For a few lines inside a long or nested
  function, locate it with find_symbol, then edit with Serena replace_content. Call mcp__serena__initial_instructions once
  first. Onboarding and its "ask the user" mode are off: you make the call. Its line numbers are 0-based. Plain Edit
  only for small in-line changes or files Serena can't parse (minified or generated code).
- claude-mem: search before re-investigating anything. Its smart_search/smart_outline are not for code: use Serena.
- Headroom: headroom_compress outputs over ~10k chars (test logs, long greps) instead of re-reading them.
- Context7: look up a library's current docs before guessing an API.
- SkillsMP: search_skills when the item is a kind of job no installed skill covers.
- superpowers: test-driven-development (failing test first), systematic-debugging, verification-before-completion.
- A PostToolUse hook syntax-checks every .py/.js you Edit/Write; a "syntax check failed" message means fix it now.
  The repo's pre-commit hook refuses archives and C2PA-stamped PNGs: never bypass it with --no-verify.
  It also refuses graphify-out/ changes (`git checkout -- graphify-out` first) and a file whose line endings flipped:
  scripted rewrites keep them with open(f, newline='') for both the read and the write; check `git diff --stat`.
- Test results: read failures from the raw log (`grep -n -E 'FAIL|ERROR|Error|Traceback' <log>`), never from a
  summary. If an output ever looks condensed, rerun it as `rtk bypass <command>`.
- rtk compacts git status/log and ls only; reads, greps, diffs and tests stay exact, inside ( ) subshells too, and
  `A=x; B=y; cmd $A` keeps its variables (the Bash hook rtk-guard.py fixes both).
  `git log` is compacted (long lines cut with "..."): when you need it exact, write `git --no-pager log ...`.
- Your inbox is $WORKER_INBOX (run-worker.sh names it in your task): re-read it before each commit and before your
  final report, and do what any new line in it says. The controller appends the owner's late changes there. Your final
  reply has one line "INBOX: ..." naming each inbox heading (the `## ...` lines) and done / not done, or "INBOX: empty";
  audit.py prints INBOX NOT ACKED for any heading the reply doesn't name.
- Scope: before your final report, list every item of your task (brief and inbox) as done / not done. A not-done item
  that no other worker blocks is not a stopping point: do it. DONE_WITH_CONCERNS is for real limits, not a first batch.
- Image checks (screenshots, art, layouts): every new image gets a new name (v2_, v3_ ...), never an overwrite. Your
  report says what is WRONG in each image, not only what is right. Any lost detail compared with the reference (holes,
  missing outline, something cut off or smaller than its siblings, two different things drawn the same) is a defect to
  fix, never a concern or "should be fine".
- Browser check scripts you write: a navigation step that didn't land fails the step; a click that must do something
  is retried until the server's reply shows it; another page's view is compared against what it showed before the
  action; run a new script 3 times side by side before trusting it.
- Stopping a process: by its pid (TaskStop, or `bash $KIT_DIR/workers/stop-worker.sh <name>` for a
  worker), never `pkill -f <pattern>`: the pattern also matches the shell that typed it.
- Sub-agents and you get a terse "caveman" voice automatically; code, comments, commits and docs stay in normal prose.

Hard rules: <project rules: what never to touch, secrets/data rules, ports, test suites to run, files shared with
other workers running in parallel>.

Before finishing, ALWAYS (even for a one-line diff or no diff at all; audit.py flags a skip and you get a --resume to do
it; with no diff, run verification-before-completion and say "no diff" on the reviewer and ponytail lines):
Skill superpowers:verification-before-completion, the reviewer sub-agent, then Skill ponytail:ponytail-review on
your diff. The reviewer is ONE Agent call (general-purpose) on `git diff <base>...HEAD`: correctness, project rules,
tests; fix what is real (one fix round). Then:
1. Commit in your worktree. Commit trailer: <the session's attribution lines>.
2. Report to <reports dir>/<item>.md, then a final reply under 15 lines: Status (DONE | DONE_WITH_CONCERNS | BLOCKED),
   commits, test summary, what a user now sees, concerns, the INBOX line, one line per tool (serena, graphify, claude-mem, headroom,
   context7, skillsmp, ponytail; "not needed, because ..." is fine), plus one line "OBSERVATIONS: ..." listing friction
   worth keeping (a tool clash, a rule that slowed you down, a step you had to work out yourself), or
   "OBSERVATIONS: none". Do not invoke task-observer or write to the task-observer workspace.
