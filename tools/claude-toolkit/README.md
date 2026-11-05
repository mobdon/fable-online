# Claude Code tool kit

A setup that installs a set of Claude Code plugins, MCP servers and hooks in one go, checks that they all work
side by side, and runs "headless workers" (background `claude -p` sessions, each in its own git worktree) with the
full tool set. Built for Claude Code cloud sessions (claude.ai/code), but it also works on any Linux box with
`claude`, Node 22, Python 3, git, pip and `uv`.

Nothing in here holds a key or a token. You supply your own through environment variables.

## What gets installed

| Tool | What it does | Source |
|---|---|---|
| superpowers | Skills: brainstorming, writing-plans, TDD, systematic-debugging, verification-before-completion, code review | Anthropic plugin directory |
| ponytail | Smallest-complete-change skill and a diff review | github.com/DietrichGebert/ponytail |
| claude-code-setup | Read-only recommender for hooks/skills/MCP servers for a repo | Anthropic plugin directory |
| claude-mem | Memory across sessions (search before re-investigating) | npm `claude-mem` |
| impeccable | UI/web design skill | github.com/pbakaus/impeccable |
| Headroom | MCP server that compresses big outputs; `headroom learn` learns from mistakes | pip `headroom-ai` |
| graphify | Code map: `graphify explain`, `graphify path`, `graphify query` | pip `graphifyy` |
| SkillsMP | MCP search for skills | skillsmp.com (needs an account key) |
| Serena | MCP symbol search and whole-function edits through language servers | github.com/oraios/serena |
| Caveman | Terse voice for sub-agents and workers only (main session stays normal) | github.com/JuliusBrussee/caveman |
| rtk | Compacts noisy Bash output (git status/log, ls); reads, greps, diffs and tests stay exact | github.com/rtk-ai/rtk |
| task-observer | Logs friction and tool clashes so rules can be improved at a review | github.com/rebelytics/one-skill-to-rule-them-all (pinned copy included, CC BY 4.0) |
| MarkItDown | `markitdown file.pdf` turns PDF/Word/Excel/PowerPoint into Markdown | pip `markitdown` |
| Context7 | MCP: current library docs | mcp.context7.com (key optional) |
| Hooks | Syntax check after every Edit/Write (.py, .js); rtk guard; git pre-commit guard (refuses zips, C2PA-stamped PNGs, graphify-out/, line-ending flips) | this kit |

## Settings (environment variables)

All optional. Put them in the cloud environment's variables (or your shell profile), never in a file.

| Variable | Meaning | Default |
|---|---|---|
| `KIT_DIR` | Where this folder lives | the folder the script is in |
| `SHARED_DIR` | Data that should outlive the container: task-observer log, claude-mem save | `~/toolkit-data` |
| `REPO_URL` | Repo to clone at install; Serena and the commit guard get pinned to the clone | none |
| `REPO_DIR` | Where that clone lives (or an existing checkout to pin to) | `~/<repo name>` |
| `CONTEXT7_API_KEY` | Context7 key (free tier works without one, with a monthly quota) | none |
| `SKILLSMP_API_KEY` | SkillsMP account key (without it SkillsMP answers 401) | none |
| `MCP_TIMEOUT` | Set to `120000`; Serena and Headroom take longer than the 30 s default to connect | install writes it into `~/.claude/settings.json` too |

Keys are passed to Claude Code as a literal `${VAR}` header, which Claude expands when it connects, so the value
never lands in `~/.claude.json`. `check-tools.sh` scans config, logs and the kit for the Context7 key and fails if it
finds it on disk.

In a cloud environment, a container is thrown away after the session. Point `SHARED_DIR` at something that survives
(a mounted folder) if you want task-observer's log and claude-mem's memory to carry over; otherwise they start fresh.

## Setup

1. Put this folder somewhere the session can reach: a mounted shared folder, or commit it into your repo (say
   `tools/`).
2. Cloud environment network: allow `github.com`, `raw.githubusercontent.com`, `registry.npmjs.org`, `pypi.org`,
   `files.pythonhosted.org`, `mcp.context7.com`, `skillsmp.com`. Add your AI provider host for `headroom learn`
   (it calls Gemini) if you want that.
3. Cloud environment Setup script: paste `cloud-setup-script.sh` (edit its `KIT_DIR` line to where step 1 put the
   kit). It runs the install before Claude starts, so plugins and MCP servers load in the first session, and it never
   fails, so a broken step can't stop sessions from starting.
4. Environment variables: the ones from the table above that you want.
5. In each new session, first run `bash $KIT_DIR/install-all.sh`. It ends with `check-tools.sh`: one
   PASS / WARN / FAIL line per tool and per clash, and a non-zero exit if anything failed. Read the FAIL lines.
   Plugins and MCP servers it adds only load in sessions started after it; in the same session, use a skill by
   reading its `SKILL.md` (`find ~/.claude/plugins/cache -path '*skills/<name>/SKILL.md'`).
6. After changing anything in the kit: `bash $KIT_DIR/check-tools.sh --worker`. It also starts one headless worker
   that uses every tool in one session (5-8 minutes, some tokens). Expect 0 FAIL. It needs a git repo with at least
   one commit (`REPO_DIR`, or run it from inside one).

Run the scripts with `bash x.sh` / `python3 x.py`: some mounted folders drop the executable bit.

Two things on purpose: the install swaps rtk's own Bash hook for `rtk-guard.py`, so don't run `rtk init` again by
hand; and no AI gateway or proxy is ever put in the API path (`ANTHROPIC_BASE_URL`), and `check-tools.sh` fails if one is.

## Files

- `install-all.sh`: installs everything, then runs the check.
- `cloud-setup-script.sh`: the stub for the cloud Setup script.
- `check-tools.sh` / `check_tools.py`: the check (`--worker` adds the all-tools worker, `--quiet` hides PASS lines).
  Drop extra checks in `checks.d/*.sh` printing `PASS|WARN|FAIL <tool>: detail`.
- `hooks/`: `syntax-check.py` (PostToolUse) and `rtk-guard.py` (PreToolUse on Bash).
- `git-hooks/pre-commit`: the commit guard, copied into the pinned clone.
- `task-observer/`: installer, SessionStart hook, pinned copy of the skill.
- `workers/run-worker.sh`: start a headless worker in a worktree; `stop-worker.sh` stops one by pid; `audit.py`
  reads a worker's transcript and lists which tools it really called; `brief-common-template.md` is the worker brief
  to copy and fill in; `toolcheck-brief.md` is the brief `check-tools.sh --worker` uses.
- `claude-mem-save.sh`: save claude-mem's store to `SHARED_DIR` at the end of a session (install restores it).
- `RULES-TEMPLATE.md`: rules to paste into your project instructions or `CLAUDE.md`, then edit.

## Headless workers in short

```bash
git worktree add ~/wt/my-task -b my-task
cp $KIT_DIR/workers/brief-common-template.md ~/wt/my-task.md   # fill in every <...> line, add the task
bash $KIT_DIR/workers/run-worker.sh ~/wt/my-task ~/wt/my-task.md          # from Claude: Bash run_in_background, timeout 7200000
python3 $KIT_DIR/workers/audit.py ~/wt/my-task                            # before merging: MISSING must say none
```

Log: `~/wt/reports/my-task.log`. A late change goes into `~/wt/reports/inbox-my-task.md` under a `## heading`; the
worker re-reads it before each commit. `--resume` as a third argument sends a follow-up to the same worker.

## fable-online changes

- Lives in the repo at `tools/claude-toolkit`; the rules filled in for this project are in the repo's `CLAUDE.md`.
- `REPO_DIR` defaults to the repo that carries the kit, so Serena, graphify and the commit guard are pinned to the
  fable-online clone without setting anything.
- `SHARED_DIR` defaults to `/mnt/project-files/toolkit-data` when the project folder is mounted, so the task-observer
  log and the claude-mem save carry over between sessions.
- `cloud-setup-script.sh` finds the kit in the usual clone locations on its own.
- `checks.d/fable-online.sh` checks the repo's own toolchain: node >= 22.13, server deps, cmake, the i686 mingw
  cross compiler for the client mod and launcher.
- rtk also leaves `cmake`, `tsx` and `node --test` output exact.
- `graphify-out/` and `.serena/` are git-ignored in this repo.
