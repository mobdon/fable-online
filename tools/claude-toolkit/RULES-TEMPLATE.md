# Rules template

Paste this into your project instructions or your repo's `CLAUDE.md`, then edit the <...> parts. `$KIT_DIR` is where
the kit lives; write the real path in.

```text
EVERY SESSION
1. First run `bash $KIT_DIR/install-all.sh`. It ends with check-tools.sh and exits non-zero if any install step or
   check failed: read its FAIL lines. Plugins and MCP servers it installs load in sessions started after it. In the
   session that ran it, use a required Skill by reading its SKILL.md
   (`find ~/.claude/plugins/cache -path '*skills/<name>/SKILL.md'`) and following it; Serena and claude-mem work goes
   to a headless worker started after the install.
2. Work in the local clone <REPO_DIR> (Serena is pinned there). Run `graphify update .` there.
3. Invoke the task-observer skill and run its session-start scan:
   `bash ~/.claude/skills/task-observer/scripts/session-start-scan.sh $SHARED_DIR/task-observer`.
4. End of a session: `bash $KIT_DIR/claude-mem-save.sh`, then `headroom learn --project . --apply` (skip it and say
   so if Gemini is unreachable), then update the hand-off doc <path>.
- Call scripts as `bash x.sh` / `python3 x.py` (mounted folders can drop the execute bit).
- After any change to the kit: `bash $KIT_DIR/check-tools.sh --worker` (expect 0 FAIL).

REPO RULES
- <what may be pushed, what needs my explicit go (merges, deploys, production changes)>.
- <where patches, notes and reports go>.

TOOLS
- graphify: the code map. `graphify explain <symbol or file>` / `graphify path <A> <B>` for known names; `graphify
  query` only with a pointed question naming a function or file. Ask it before grepping or opening big files; update
  it after every merge or commit.
- Serena (MCP, tools interface): find_symbol, find_referencing_symbols, get_symbols_overview, replace_symbol_body.
  graphify finds the file or system, Serena then finds the exact symbol. Plain Edit only for small in-line changes and
  files Serena can't parse (minified or generated code). If only serena_repl shows, the install is broken: fix it first.
- claude-mem: search it before re-investigating anything. Its smart_search/smart_outline are not for code: use Serena.
- Headroom: compress big outputs (headroom_compress / headroom_retrieve). MCP only, never in the API path.
- Context7 (MCP): current docs for a library before guessing its API. Its key lives only in CONTEXT7_API_KEY.
- superpowers: brainstorming, writing-plans, using-git-worktrees, subagent-driven-development, test-driven-development,
  verification-before-completion, systematic-debugging; requesting-code-review for the final review.
- ponytail: the smallest complete change; a ponytail review before each merge and on the final diff.
- Caveman: on for sub-agents and workers only. Replies to me stay in full sentences.
- rtk: compacts git status, git log and ls only; reads, greps, diffs and test runs stay exact. If an output looks cut,
  re-run it as `rtk bypass <command>`. Write `git --no-pager log ...` when you need the log exact.
- MarkItDown: convert files I upload (PDF, Word, Excel, PowerPoint) with `markitdown <file>` before reading them.
- impeccable: web/UI layer only. SkillsMP: find a skill for a new kind of job.
- claude-code-setup: run "recommend Claude Code automations for this project" when the setup is reviewed or a new
  stack arrives; adopt only what the other tools don't already cover.
- task-observer: log each correction, tool clash or slow rule with its new-observation.sh into
  $SHARED_DIR/task-observer (never the repo); workers never log, they end with an "OBSERVATIONS:" line and the
  controller logs it; I approve every rule change at a review.
- Syntax hook: "syntax check failed" after an Edit/Write means fix it now.
- Commit guard: the clone's pre-commit hook refuses archives and C2PA-stamped PNGs. Never bypass it with --no-verify.
- Browser QA: Playwright with the preinstalled Chromium; never run `playwright install` in a cloud session.

WORKERS (work that fans out)
- Start one with `bash $KIT_DIR/workers/run-worker.sh <worktree> <brief>` (Bash run_in_background, timeout 7200000).
  Each gets its own git worktree, Serena pinned to it, Context7, Headroom, claude-mem and SkillsMP. A late change goes
  in its inbox (<reports>/inbox-<name>.md, under a `## <short heading>` line), not a stop and --resume.
  Stop one with TaskStop or `bash $KIT_DIR/workers/stop-worker.sh <name>`, never `pkill -f <pattern>`.
- Every brief starts from $KIT_DIR/workers/brief-common-template.md.
- A worker's first three actions: one claude-mem search; `graphify update .` and one `graphify explain`/`path`;
  Serena get_symbols_overview on each source file it will change.
- Every worker ends with superpowers:verification-before-completion, one reviewer sub-agent, then
  ponytail:ponytail-review on its diff, even for a one-line change.
- Before merging a worker branch: `python3 $KIT_DIR/workers/audit.py <worktree>` (its MISSING line must say none)
  and a ponytail review of its diff.

RULES
- Verify before saying something is NOT in the source: find the exact file it would be in, check it, and search for
  any words I quote. Until then call the claim "unchecked".
- Say "tested offline" or "tested live" plainly; never call something fixed without the check that shows it.
- When a tool is skipped, clashes or misfires: fix the setup, log it with task-observer, and propose the rule change
  (the steps, the line it changes, and a check that it contradicts no other rule). Rules for keys, security and
  deploys need my named approval.
```
