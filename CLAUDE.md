# fable-online: working rules for Claude Code sessions

The Claude Code tool kit lives in this repo at `tools/claude-toolkit` (README there). Paths below are for a
claude.ai/code cloud session, where this repo is cloned at `/home/claude/fable-online`; on another machine use your
clone's path.

- `KIT_DIR` = `/home/claude/fable-online/tools/claude-toolkit`
- `REPO_DIR` = `/home/claude/fable-online` (Serena and the commit guard are pinned here by default)
- `SHARED_DIR` = `/mnt/project-files/toolkit-data` when the project folder is mounted, else `~/toolkit-data`
  (task-observer log and the claude-mem save live here, never in the repo)

```text
EVERY SESSION
1. First run `bash /home/claude/fable-online/tools/claude-toolkit/install-all.sh`. It ends with check-tools.sh and
   exits non-zero if any install step or check failed: read its FAIL lines. Plugins and MCP servers it installs load
   in sessions started after it. In the session that ran it, use a required Skill by reading its SKILL.md
   (`find ~/.claude/plugins/cache -path '*skills/<name>/SKILL.md'`) and following it; Serena and claude-mem work goes
   to a headless worker started after the install. If the cloud Setup script already ran it
   (/tmp/toolkit-setup.log exists and ends with the check), skip the reinstall and run
   `bash $KIT_DIR/check-tools.sh --quiet` instead.
2. Work in the local clone /home/claude/fable-online (Serena is pinned there). Run `graphify update .` there.
   graphify-out/ and .serena/ are git-ignored in this repo.
3. Invoke the task-observer skill and run its session-start scan:
   `bash ~/.claude/skills/task-observer/scripts/session-start-scan.sh /mnt/project-files/toolkit-data/task-observer`.
4. End of a session: `bash $KIT_DIR/claude-mem-save.sh`, then `headroom learn --project . --apply` (skip it and say
   so if Gemini is unreachable), then record what the next session needs in project memory.
- Call scripts as `bash x.sh` / `python3 x.py` (mounted folders can drop the execute bit).
- After any change to the kit: `bash $KIT_DIR/check-tools.sh --worker` (expect 0 FAIL).
  Project checks (node, server deps, cmake, mingw) are in tools/claude-toolkit/checks.d/fable-online.sh.

REPO RULES
- Work on the session's claude/* branch and open a PR. Merges are done with local git authored by Claude, never
  through the GitHub API merge button or endpoint. Production or hosted-server changes need the owner's explicit go.
- Everything that reaches GitHub (commits, PR titles and bodies, review comments, code comments, docs) is written in
  neutral technical terms. Never quote or paraphrase chat messages or prompts. No session links, thread links or
  Claude-Session trailers. PR bodies start with the project attribution block and end with the plain line
  "Generated with Claude Code" (no URL).
- Never commit or ship anything from the game: no Lionhead/Microsoft files (.big, .bin, .lev, .tng, Fable.exe, art,
  fonts, text). The mod reads them from the player's own install at runtime. .gitignore and the commit guard enforce
  part of this; the rest is on you.
- Test builds: every build handed to players is ONE combined online-N build (the online client plus every merged
  feature), made by the online-server-testing thread. Feature work lands as PRs on main and is handed to that thread;
  no separate per-feature test builds.
- Notes and reports: reverse-engineering notes in tools/re-notes/, design docs in docs/, game logs read from the
  player's fable_online.log go under /mnt/project-files/fable-online/logs/ (not the repo).
- Server checks before any push touching server/: `cd server && npm ci && npm run typecheck && npm test`.
  The 1,000-hero tick timing test is known to be flaky; rerun it once before treating it as a failure.

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
- Caveman: on for sub-agents and workers only. Replies to people stay in full sentences.
- rtk: compacts git status, git log and ls only; reads, greps, diffs, builds (cmake, npm run, tsx) and test runs stay
  exact. If an output looks cut, re-run it as `rtk bypass <command>`. Write `git --no-pager log ...` when you need the
  log exact.
- MarkItDown: convert uploaded PDF, Word, Excel or PowerPoint files with `markitdown <file>` before reading them.
- impeccable: web/UI layer only (launcher web views, staff pages). In-game UI follows Fable's own menu art instead.
  SkillsMP: find a skill for a new kind of job.
- claude-code-setup: run "recommend Claude Code automations for this project" when the setup is reviewed or a new
  stack arrives; adopt only what the other tools don't already cover.
- task-observer: log each correction, tool clash or slow rule with its new-observation.sh into
  /mnt/project-files/toolkit-data/task-observer (never the repo); workers never log, they end with an
  "OBSERVATIONS:" line and the controller logs it; the owner approves every rule change at a review.
- Syntax hook: "syntax check failed" after an Edit/Write means fix it now.
- Commit guard: the clone's pre-commit hook refuses archives and C2PA-stamped PNGs. Never bypass it with --no-verify.
- Browser QA: Playwright with the preinstalled Chromium; never run `playwright install` in a cloud session.

WORKERS (work that fans out)
- Start one with `bash $KIT_DIR/workers/run-worker.sh <worktree> <brief>` (Bash run_in_background, timeout 7200000).
  Each gets its own git worktree, Serena pinned to it, Context7, Headroom, claude-mem and SkillsMP. A late change goes
  in its inbox (~/wt/reports/inbox-<name>.md, under a `## <short heading>` line), not a stop and --resume.
  Stop one with TaskStop or `bash $KIT_DIR/workers/stop-worker.sh <name>`, never `pkill -f <pattern>`.
- Every brief starts from $KIT_DIR/workers/brief-common-template.md, with the REPO RULES above as its hard rules and
  the commit trailer `Co-Authored-By: Claude <noreply@anthropic.com>` (no session link).
- A worker's first three actions: one claude-mem search; `graphify update .` and one `graphify explain`/`path`;
  Serena get_symbols_overview on each source file it will change.
- Every worker ends with superpowers:verification-before-completion, one reviewer sub-agent, then
  ponytail:ponytail-review on its diff, even for a one-line change.
- Before merging a worker branch: `python3 $KIT_DIR/workers/audit.py <worktree>` (its MISSING line must say none)
  and a ponytail review of its diff.

RULES
- Verify before saying something is NOT in the source: find the exact file it would be in, check it, and search for
  any quoted words. Until then call the claim "unchecked".
- Say "tested offline" or "tested live" plainly. Nothing in the game is called fixed until it has been seen working
  in the running game (or in fable_online.log from a real run); every build hand-off lists verified and not-verified
  items separately.
- When a tool is skipped, clashes or misfires: fix the setup, log it with task-observer, and propose the rule change
  (the steps, the line it changes, and a check that it contradicts no other rule). Rules for keys, security and
  deploys need the owner's named approval.
- No keys or tokens in any file. They live only in environment variables (CONTEXT7_API_KEY, SKILLSMP_API_KEY).
```
