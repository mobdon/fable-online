# Tool check worker (check-tools.sh --worker): use every tool once, in one session, and report

You are a headless worker in a throwaway worktree of the project repo. Change NO repository file and make no commit.
Nobody answers questions. Do the steps in order; each one proves a tool works next to the others. If a step fails,
say so on its line and continue.

1. claude-mem: call mcp__claude-mem__search with query "setup" (limit 3).
2. graphify: run `graphify update .` then `graphify explain {TARGET}`.
3. Serena: call mcp__serena__initial_instructions, then mcp__serena__get_symbols_overview on {TARGET} and
   mcp__serena__find_symbol for the first symbol that overview lists.
4. Context7: mcp__context7__resolve-library-id for "playwright", then mcp__context7__query-docs on that id asking how
   to launch headless Chromium.
5. Headroom: run `git --no-pager log --format='{"h":"%h","s":"%s"}' -150` (--no-pager keeps it exact past rtk) and pass its output to mcp__headroom__headroom_compress;
   then mcp__headroom__headroom_retrieve with the hash it returns and confirm the first line matches.
6. SkillsMP: mcp__skillsmp__search_skills with query "code review", limit 3.
7. rtk and exact output, as separate Bash calls exactly as written:
   a. `d={KIT}; ls -la $d` (must list check_tools.py, i.e. the variable survived rtk's rewrite)
   b. `grep -c "import" {TARGET}` (must print a bare number)
   c. `git status`
   d. `A={KIT}; B=x; ls $A` (must list check_tools.py: two assignments in a chain survive too)
   e. `cat "$WORKER_INBOX"` (your inbox; must print its "# Inbox for worker" header)
8. MarkItDown: `printf 'tile,room\n1,Hello\n' > /tmp/claude-0/toolcheck.csv && markitdown /tmp/claude-0/toolcheck.csv`.
9. impeccable: `printf '<button style="color:#999;background:#aaa">Go</button>' > /tmp/claude-0/toolcheck.html` then
   `sh ~/.claude/skills/impeccable/scripts/impeccable detect /tmp/claude-0/toolcheck.html`.
10. Playwright: `NODE_PATH=$(npm root -g) node -e "const {chromium}=require('playwright');(async()=>{const b=await chromium.launch();const p=await b.newPage();await p.setContent('<p>ok</p>');console.log(await p.textContent('p'));await b.close()})()"`.
11. Skill superpowers:verification-before-completion (load it and apply it to this report: every claim backed by
    the output above).
12. Reviewer sub-agent: one Agent call (general-purpose) asking it to read {TARGET} and say in one line
    what it does. Report what it said.
13. Skill ponytail:ponytail-review on the (empty) diff of this worktree: `git diff` should be empty apart from
    graphify-out/; then run `git checkout -- graphify-out` so the worktree is clean.

Do NOT invoke the task-observer skill or write to the task-observer workspace.

Final reply, at most 16 lines: one line per tool (claude-mem, graphify, serena, context7, headroom, skillsmp, rtk,
markitdown, impeccable, playwright, superpowers, sub-agent, ponytail) saying what it returned, then a last line
"OBSERVATIONS: ..." listing any clash or friction you hit between tools, or "OBSERVATIONS: none".
