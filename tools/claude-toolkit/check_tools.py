#!/usr/bin/env python3
"""Tool check: is every tool installed AND working, and do the tools clash?
usage: bash check-tools.sh [--worker] [--quiet]
  default   : every tool answers a real call (MCP servers over stdio/HTTP, hooks fed sample input, CLIs on tiny files),
              plus the conflict checks (hooks, MCP names/scopes, exact output, Caveman scope, Headroom path,
              Context7 key on disk, Serena pin, worker kit wiring). About 1-2 minutes.
  --worker  : also one combined headless worker (run-worker.sh) that calls every tool in one session, then audit.py
              and transcript checks. About 5-8 minutes and some tokens; run after any kit change.
Prints one line per check: PASS / WARN / FAIL <tool>: detail. Exit 1 if any FAIL.
Drop-in checks: tools/checks.d/*.sh print lines in the same format (for tools other threads add)."""
import glob, json, os, re, select, shutil, ssl, subprocess, sys, tempfile, time, urllib.request

HOME = os.path.expanduser('~')
KIT = os.environ.get('KIT_DIR') or os.path.dirname(os.path.abspath(__file__))
SHARED = os.environ.get('SHARED_DIR') or ('/mnt/project-files/toolkit-data' if os.path.isdir('/mnt/project-files') else HOME + '/toolkit-data')     # persistent data (task-observer log, claude-mem save)
_url = os.environ.get('REPO_URL', '')
REPO = os.environ.get('REPO_DIR') or (HOME + '/' + os.path.basename(_url.rstrip('/'))[:-4 if _url.endswith('.git') else None] if _url else '')
if not REPO:   # fable-online: install-all.sh defaults REPO_DIR to the repo that carries this kit
    _top = subprocess.run(['git', '-C', os.path.dirname(os.path.abspath(__file__)), 'rev-parse', '--show-toplevel'], capture_output=True, text=True)
    REPO = _top.stdout.strip() if _top.returncode == 0 else ''
PINNED = bool(REPO)                      # install-all.sh pins Serena to REPO_DIR; without one it follows the session cwd
REPO = REPO or os.getcwd()               # unpinned: check the repo you run this from
WT_ROOT = HOME + '/wt'
SET = HOME + '/.claude/settings.json'
CLAUDE_JSON = HOME + '/.claude.json'
PLUG = HOME + '/.claude/plugins/cache'
QUIET = '--quiet' in sys.argv
results = []


def out(level, tool, msg):
    results.append((level, tool, msg))
    if not (QUIET and level == 'PASS'):
        print(f'{level:4} {tool}: {msg}', flush=True)


def sh(cmd, timeout=120, env=None, cwd=None, inp=None):
    try:
        p = subprocess.run(cmd, shell=isinstance(cmd, str), capture_output=True, text=True, timeout=timeout,
                           env={**os.environ, **(env or {})}, cwd=cwd or HOME, input=inp if inp is not None else '')
        return p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        return 124, 'timeout'
    except OSError as e:
        return 127, str(e)


def jload(p, default=None):
    try:
        return json.load(open(p))
    except (OSError, ValueError):
        return default


def check(tool):
    """decorator: run a check, turn an exception into a FAIL line"""
    def wrap(f):
        try:
            f()
        except Exception as e:  # noqa: BLE001 - a crash in one check must not hide the others
            out('FAIL', tool, f'check crashed: {type(e).__name__}: {str(e)[:200]}')
        return f
    return wrap


# ---------- MCP clients (stdio + streamable HTTP), so every server answers a real call ----------
def mcp_stdio(cmd, env=None, cwd=None):
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                         env={**os.environ, **(env or {})}, cwd=cwd or HOME, text=True, bufsize=1)
    n = [0]

    def rpc(method, params=None, notify=False, timeout=120):
        msg = {'jsonrpc': '2.0', 'method': method, 'params': params or {}}
        if not notify:
            n[0] += 1
            msg['id'] = n[0]
        p.stdin.write(json.dumps(msg) + '\n')
        p.stdin.flush()
        if notify:
            return None
        end = time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([p.stdout], [], [], max(0, end - time.time()))
            if not r:
                break
            line = p.stdout.readline()
            if not line:
                break
            try:
                m = json.loads(line)
            except ValueError:
                continue
            if m.get('id') == n[0]:
                return m
        raise TimeoutError(method)
    rpc('initialize', {'protocolVersion': '2025-03-26', 'capabilities': {}, 'clientInfo': {'name': 'check-tools', 'version': '1'}})
    rpc('notifications/initialized', notify=True)
    return p, rpc


def _ssl():
    for f in (os.environ.get('SSL_CERT_FILE'), os.environ.get('REQUESTS_CA_BUNDLE'), '/root/.ccr/ca-bundle.crt'):
        if f and os.path.exists(f):
            return ssl.create_default_context(cafile=f)
    return ssl.create_default_context()


def mcp_http(url, headers):
    state = {'sid': None, 'n': 0}

    def call(method, params, rid):
        # A browser-like UA: skillsmp's Cloudflare front answers Python-urllib with a 403 challenge page.
        h = {'User-Agent': 'check-tools/1.0', 'Content-Type': 'application/json',
             'Accept': 'application/json, text/event-stream', 'MCP-Protocol-Version': '2025-06-18', **headers}
        if state['sid']:
            h['Mcp-Session-Id'] = state['sid']
        body = {'jsonrpc': '2.0', 'method': method, 'params': params}
        if rid is not None:
            body['id'] = rid
        r = urllib.request.urlopen(urllib.request.Request(url, json.dumps(body).encode(), h), timeout=40, context=_ssl())
        state['sid'] = r.headers.get('Mcp-Session-Id') or state['sid']
        t = r.read().decode('utf-8', 'replace')
        if rid is None:
            return None
        for line in t.splitlines():
            if line.startswith('data:'):
                t = line[5:]
        return json.loads(t) if t.strip() else None

    def rpc(method, params=None):
        state['n'] += 1
        return call(method, params or {}, state['n'])
    rpc('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {}, 'clientInfo': {'name': 'check-tools', 'version': '1'}})
    try:
        call('notifications/initialized', {}, None)
    except Exception:  # noqa: BLE001 - some servers answer 202/400 to the notification; harmless
        pass
    return rpc


def text_of(r):
    return ''.join(c.get('text', '') for c in ((r or {}).get('result') or {}).get('content', []))


# ---------- shared state ----------
settings = jload(SET, {}) or {}
cj = jload(CLAUDE_JSON, {}) or {}
user_mcp = cj.get('mcpServers', {})
plugins_enabled = settings.get('enabledPlugins', {})


def plugin_dir(market, name):
    ds = sorted(glob.glob(f'{PLUG}/{market}/{name}/*/'), key=lambda d: [int(x) if x.isdigit() else x for x in re.split(r'[.-]', os.path.basename(d.rstrip('/')))])
    return ds[-1].rstrip('/') if ds else None


def skill_exists(root, name):
    return any(os.path.basename(os.path.dirname(f)) == name for f in glob.glob(root + '/**/SKILL.md', recursive=True))


def all_hooks():
    """(source, event, matcher, command) for every hook Claude Code will merge: user settings, the harness launcher
    settings, the repo's project settings and each enabled plugin's hooks.json."""
    srcs = [('user settings', settings), ('harness launcher', jload(HOME + '/.claude/launcher-settings.json', {}) or {})]
    for p in (REPO + '/.claude/settings.json', REPO + '/.claude/settings.local.json'):
        if os.path.exists(p):
            srcs.append(('repo ' + os.path.basename(p), jload(p, {}) or {}))
    for key in plugins_enabled:
        name, _, market = key.partition('@')
        d = plugin_dir(market, name)
        if d and os.path.exists(d + '/hooks/hooks.json'):
            srcs.append(('plugin ' + name, jload(d + '/hooks/hooks.json', {}) or {}))
    rows = []
    for src, s in srcs:
        for ev, entries in (s.get('hooks') or {}).items():
            for e in entries or []:
                for h in e.get('hooks', []):
                    rows.append((src, ev, e.get('matcher', ''), ' '.join([h.get('command', '')] + h.get('args', []))))
    return rows


HOOKS = all_hooks()


def pick_target():
    """a real source file of the repo for the graphify/Serena probes: the largest tracked file of the first language found"""
    exts = ['py', 'ts', 'tsx', 'js', 'go', 'rs', 'java', 'cs', 'cpp', 'c', 'lua', 'rb', 'php']
    # probe a file in Serena's own project language first (a TypeScript repo with a few .py fixtures gave a false FAIL)
    try:
        if 'typescript' in open(REPO + '/.serena/project.yml').read():
            exts = ['ts', 'js'] + [e for e in exts if e not in ('ts', 'js')]
    except OSError:
        pass
    for ext in exts:
        rc, o = sh(['git', 'ls-files', '-z', '*.' + ext], cwd=REPO)
        fs = [f for f in o.split('\0') if f and '.min.' not in f and os.path.isfile(os.path.join(REPO, f))]
        if not rc and fs:
            return max(fs, key=lambda f: os.path.getsize(os.path.join(REPO, f)))
    return ''


TARGET = pick_target() if os.path.isdir(REPO + '/.git') else ''


def rtk_rewrite(cmd):
    """What the installed Bash hook (rtk-guard.py around rtk's own rewrite) turns cmd into; None = runs as typed."""
    p = subprocess.run(['python3', HOME + '/.claude/hooks/rtk-guard.py'], input=json.dumps({'hook_event_name': 'PreToolUse', 'tool_name': 'Bash', 'tool_input': {'command': cmd}}),
                       capture_output=True, text=True, timeout=20)
    t = p.stdout.strip()
    return (json.loads(t).get('hookSpecificOutput', {}).get('updatedInput', {}).get('command') if t else None)


print('== tools: installed and answering a real call ==', flush=True)


@check('superpowers')
def _():
    d = plugin_dir('anthropic-plugin-directory', 'superpowers')
    need = ['verification-before-completion', 'test-driven-development', 'subagent-driven-development', 'systematic-debugging',
            'using-git-worktrees', 'writing-plans', 'brainstorming', 'requesting-code-review']
    miss = [n for n in need if not (d and skill_exists(d, n))]
    if not plugins_enabled.get('superpowers@anthropic-plugin-directory') or not d or miss:
        return out('FAIL', 'superpowers', f'plugin enabled={bool(plugins_enabled.get("superpowers@anthropic-plugin-directory"))}, missing skills {miss}')
    tpl = open(KIT + '/workers/brief-common-template.md').read()
    m = re.search(r'(/root/\.claude/plugins/cache/anthropic-plugin-directory/superpowers/[^/\s]+/skills)', tpl)
    if m and not os.path.isdir(m.group(1)):
        return out('FAIL', 'superpowers', f'brief template points at {m.group(1)}, installed is {d}/skills')
    out('PASS', 'superpowers', f'{os.path.basename(d)}, {len(need)} skills present, brief template path matches')


@check('ponytail')
def _():
    d = plugin_dir('ponytail', 'ponytail')
    ok = plugins_enabled.get('ponytail@ponytail') and d and skill_exists(d, 'ponytail-review')
    out('PASS' if ok else 'FAIL', 'ponytail', f'{os.path.basename(d or "?")}, ponytail-review skill ' + ('present' if ok else 'MISSING'))


@check('claude-code-setup')
def _():
    d = plugin_dir('anthropic-plugin-directory', 'claude-code-setup')
    ok = plugins_enabled.get('claude-code-setup@anthropic-plugin-directory') and d and skill_exists(d, 'claude-automation-recommender')
    out('PASS' if ok else 'FAIL', 'claude-code-setup', 'claude-automation-recommender skill ' + ('present' if ok else 'MISSING'))


@check('claude-mem')
def _():
    d = plugin_dir('thedotmack', 'claude-mem')
    if not (plugins_enabled.get('claude-mem@thedotmack') and d and os.path.exists(d + '/scripts/mcp-server.cjs')):
        return out('FAIL', 'claude-mem', 'plugin or its MCP server missing')
    if not os.path.exists(HOME + '/.claude-mem/claude-mem.db'):
        return out('WARN', 'claude-mem', 'no store yet (~/.claude-mem/claude-mem.db): first thread? claude-mem-save.sh at its end creates the saved copy')
    p, rpc = mcp_stdio(['node', d + '/scripts/mcp-server.cjs'], env={'CLAUDE_PLUGIN_ROOT': d}, cwd=REPO)
    try:
        t = text_of(rpc('tools/call', {'name': 'search', 'arguments': {'query': 'setup', 'limit': 3}}, timeout=90))
    finally:
        p.kill()
    m = re.search(r'Found (\d+) result', t)
    if not m and 'no results' not in t.lower():
        return out('FAIL', 'claude-mem', 'search answered without results: ' + t[:120].replace('\n', ' '))
    out('PASS', 'claude-mem', f'MCP search answered ({m.group(1) if m else 0} results for "setup")')


@check('impeccable')
def _():
    sk = HOME + '/.claude/skills/impeccable'
    if not os.path.exists(sk + '/SKILL.md'):
        return out('FAIL', 'impeccable', 'skill missing in ~/.claude/skills/impeccable')
    tmp = tempfile.mkdtemp()
    open(tmp + '/t.html', 'w').write('<html><body><div style="color:#777;background:#888">low contrast</div></body></html>')
    rc, o = sh(['sh', sk + '/scripts/impeccable', 'detect', tmp + '/t.html'], timeout=180, cwd=tmp)
    shutil.rmtree(tmp, ignore_errors=True)
    if rc not in (0, 1, 2) or 'not found' in o.lower() or 'error' in o.lower()[:200]:
        return out('FAIL', 'impeccable', f'engine did not run (rc {rc}): {o[:150]}')
    out('PASS', 'impeccable', 'skill present, engine ran `impeccable detect` on a sample page')


@check('headroom')
def _():
    if not shutil.which('headroom'):
        return out('FAIL', 'headroom', 'not installed')
    if 'headroom' not in user_mcp:
        return out('FAIL', 'headroom', 'MCP server not registered at user scope')
    rc, _ = sh("curl -s --noproxy '*' --max-time 3 -o /dev/null -w '%{http_code}' http://127.0.0.1:8787/health | grep -q 200")
    if rc:
        return out('FAIL', 'headroom', 'proxy on 127.0.0.1:8787 is down: headroom_compress stores text uncompressed; start it (install-all.sh / run-worker.sh do)')
    data = json.dumps([{'id': i, 'name': f'room{i}', 'tiles': [0] * 20, 'flags': {'dark': False}} for i in range(200)])
    p, rpc = mcp_stdio([shutil.which('headroom'), 'mcp', 'serve'])
    try:
        t = text_of(rpc('tools/call', {'name': 'headroom_compress', 'arguments': {'content': data}}, timeout=90))
    finally:
        p.kill()
    if 'hash' not in t or len(t) >= len(data) * 0.9:
        return out('FAIL', 'headroom', f'headroom_compress did not shrink JSON ({len(data)} -> {len(t)} chars)')
    out('PASS', 'headroom', f'MCP headroom_compress shrank sample JSON {len(data)} -> {len(t)} chars, proxy up')


@check('headroom learn')
def _():
    """`headroom learn` calls Gemini; a thread linked to the owner's PC gets a tighter network rule that blocks it.
    Google answers even a keyless call with a JSON error; a proxy block gives no body at all."""
    rc, o = sh('curl -s --max-time 10 https://generativelanguage.googleapis.com/v1beta/models')
    if '"error"' in o or '"models"' in o:
        return out('PASS', 'headroom learn', 'Gemini reachable: end-of-thread `headroom learn --project . --apply` can run here')
    out('WARN', 'headroom learn', 'Gemini blocked here (a thread linked to the PC?): skip `headroom learn` at thread end, say so, '
        'and run it from a thread that is not linked')


@check('graphify')
def _():
    if not shutil.which('graphify'):
        return out('FAIL', 'graphify', 'not installed')
    if not os.path.exists(HOME + '/.claude/skills/graphify/SKILL.md'):
        return out('FAIL', 'graphify', 'skill missing (graphify install --platform claude)')
    if not os.path.exists(REPO + '/graphify-out/graph.json'):
        return out('WARN', 'graphify', f'no graph in {REPO}/graphify-out yet: run `graphify update .` there')
    rc, o = sh(['graphify', 'explain', TARGET], timeout=120, cwd=REPO)
    if rc or len(o.strip()) < 40:
        return out('FAIL', 'graphify', f'`graphify explain {TARGET}` failed (rc {rc}): {o[:150]}')
    out('PASS', 'graphify', f'`graphify explain {TARGET}` answered ({len(o.splitlines())} lines)')


@check('skillsmp')
def _():
    if 'skillsmp' not in user_mcp:
        local = [k for k, v in cj.get('projects', {}).items() if 'skillsmp' in v.get('mcpServers', {})]
        return out('FAIL', 'skillsmp', 'not registered at user scope' + (f' (only local scope for {local}: invisible from other folders)' if local else ''))
    if not os.environ.get('SKILLSMP_API_KEY'):
        return out('FAIL', 'skillsmp', 'no SKILLSMP_API_KEY in the environment: skillsmp.com/mcp needs an account key (add it to the cloud environment variables)')
    if (user_mcp['skillsmp'].get('headers') or {}).get('Authorization') != 'Bearer ${SKILLSMP_API_KEY}':
        return out('FAIL', 'skillsmp', 'header is not the literal Bearer ${SKILLSMP_API_KEY} (re-run install-all.sh; key may be written in ~/.claude.json)')
    rpc = mcp_http(user_mcp['skillsmp']['url'], {'Authorization': 'Bearer ' + os.environ['SKILLSMP_API_KEY']})
    t = text_of(rpc('tools/call', {'name': 'search_skills', 'arguments': {'query': 'code review', 'limit': 3}}))
    if not t:
        return out('FAIL', 'skillsmp', 'search_skills returned nothing')
    out('PASS', 'skillsmp', 'user scope; MCP search_skills answered')


@check('serena')
def _():
    if not shutil.which('serena'):
        return out('FAIL', 'serena', 'not installed')
    cfg = user_mcp.get('serena')
    if not cfg:
        return out('FAIL', 'serena', 'MCP server not registered at user scope')
    args = cfg.get('args', [])
    if PINNED and ('--project' not in args or args[args.index('--project') + 1] != REPO):
        return out('FAIL', 'serena', f'pin wrong: {args} (must be --project {REPO})')
    if not PINNED and '--project-from-cwd' not in args:
        return out('FAIL', 'serena', f'no REPO_DIR/REPO_URL set, so expected --project-from-cwd: {args}')
    if args[args.index('--agent-interface') + 1:][:1] != ['tools'] if '--agent-interface' in args else True:
        return out('FAIL', 'serena', 'not started with --agent-interface tools: Serena 2.x then exposes only serena_repl, not get_symbols_overview/find_symbol')
    p, rpc = mcp_stdio([cfg['command']] + args)
    try:
        names = [t['name'] for t in rpc('tools/list', timeout=120)['result']['tools']]
        miss = [t for t in ('get_symbols_overview', 'find_symbol', 'find_referencing_symbols', 'replace_symbol_body') if t not in names]
        if miss:
            return out('FAIL', 'serena', f'symbol tools missing from MCP tool list: {miss}')
        txt = text_of(rpc('tools/call', {'name': 'initial_instructions', 'arguments': {}}, timeout=120))
        modes = re.findall(r'<mode name="([^"]+)"', txt)
        # An empty clone (shared repo with no commits) has no file to probe: overview of '' errors, which is not Serena's fault.
        ov = text_of(rpc('tools/call', {'name': 'get_symbols_overview', 'arguments': {'relative_path': TARGET}}, timeout=240)) if TARGET else ''
    finally:
        p.kill()
    if 'interactive' in modes:
        return out('FAIL', 'serena', 'mode "interactive" is on: Serena tells agents to ask the user, against "don\'t stop to ask" (set base_modes: [editing, no-onboarding] in ~/.serena/serena_config.yml)')
    if not TARGET:
        return out('WARN', 'serena', f'pinned to {REPO}, {len(names)} tools (tools interface), but the clone has no committed source file: symbol probe skipped (commit the shared repo)')
    if not re.search(r'Variable|Function|Class|Method|Constant|Struct|Interface', ov):
        return out('FAIL', 'serena', 'get_symbols_overview gave no symbols: ' + ov[:150])
    excl = open(REPO + '/.git/info/exclude').read() if os.path.exists(REPO + '/.git/info/exclude') else ''
    if '.serena/' not in excl.split():
        return out('FAIL', 'serena', '.serena/ is not git-excluded in the local clone')
    out('PASS', 'serena', f'pinned to {REPO}; {len(names)} tools (tools interface); modes {modes}; get_symbols_overview answered; .serena git-excluded')


@check('caveman')
def _():
    p = HOME + '/.claude/hooks/caveman-subagent.py'
    if not os.path.exists(HOME + '/.claude/caveman/SKILL.md') or not os.path.exists(p):
        return out('FAIL', 'caveman', 'SKILL.md or the SubagentStart hook script missing')
    rc, o = sh(['python3', p], inp='{}')
    try:
        ctx = json.loads(o)['hookSpecificOutput']
        ok = ctx['hookEventName'] == 'SubagentStart' and 'CAVEMAN MODE ACTIVE (sub-agent only)' in ctx['additionalContext']
    except (ValueError, KeyError):
        ok = False
    out('PASS' if ok else 'FAIL', 'caveman', 'SubagentStart hook emits the sub-agent-only caveman context' if ok else f'hook output wrong: {o[:150]}')


@check('rtk')
def _():
    if not shutil.which('rtk'):
        return out('FAIL', 'rtk', 'not installed')
    rc, v = sh(['rtk', '--version'])
    r = rtk_rewrite('git status')
    if r != 'rtk git status':
        return out('FAIL', 'rtk', f'hook does not compact `git status` (got {r})')
    out('PASS', 'rtk', f'{v.strip()}; hook compacts git status (exact-output checks below)')


@check('markitdown')
def _():
    if not shutil.which('markitdown'):
        return out('FAIL', 'markitdown', 'not installed')
    tmp = tempfile.mkdtemp()
    try:
        import openpyxl
        wb = openpyxl.Workbook()
        wb.active['A1'] = 'MarkItDownCheck'
        wb.save(tmp + '/t.xlsx')
        rc, o = sh(['markitdown', tmp + '/t.xlsx'], timeout=120)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    out('PASS' if 'MarkItDownCheck' in o else 'FAIL', 'markitdown', 'converted a sample .xlsx to Markdown' if 'MarkItDownCheck' in o else f'conversion failed: {o[:150]}')


@check('context7')
def _():
    cfg = user_mcp.get('context7')
    if not cfg:
        return out('FAIL', 'context7', 'MCP server not registered at user scope')
    hv = (cfg.get('headers') or {}).get('CONTEXT7_API_KEY')
    if os.environ.get('CONTEXT7_API_KEY') and hv != '${CONTEXT7_API_KEY}':
        return out('FAIL', 'context7', 'header is not the literal ${CONTEXT7_API_KEY} (key may be written in ~/.claude.json)')
    h = {'CONTEXT7_API_KEY': os.environ['CONTEXT7_API_KEY']} if os.environ.get('CONTEXT7_API_KEY') else {}
    t = text_of(mcp_http(cfg['url'], h)('tools/call', {'name': 'resolve-library-id', 'arguments': {'libraryName': 'playwright', 'query': 'playwright'}}))
    if '/microsoft/playwright' not in t:
        return out('FAIL', 'context7', 'resolve-library-id gave no Playwright id: ' + t[:120])
    out('PASS', 'context7', ('key from the environment (literal header)' if h else 'anonymous tier (no CONTEXT7_API_KEY)') + '; resolve-library-id answered')


@check('task-observer')
def _():
    sk = HOME + '/.claude/skills/task-observer'
    hook = HOME + '/.claude/hooks/task-observer-start.py'
    if not (os.path.exists(sk + '/SKILL.md') and os.path.exists(sk + '/scripts/new-observation.sh') and os.path.exists(hook)):
        return out('FAIL', 'task-observer', 'skill or SessionStart hook missing (bash $KIT_DIR/task-observer/install.sh)')
    rc, o = sh(['python3', hook], inp='{}')
    rc2, o2 = sh(['python3', hook], inp='{}', env={'TASK_OBSERVER_ROLE': 'worker'})
    try:
        main = json.loads(o)['hookSpecificOutput']['additionalContext']
        work = json.loads(o2)['hookSpecificOutput']['additionalContext']
    except (ValueError, KeyError):
        return out('FAIL', 'task-observer', 'hook output is not valid hook JSON')
    if 'invoke the task-observer skill' not in main or 'Do NOT invoke' not in work:
        return out('FAIL', 'task-observer', 'hook does not separate controller and worker roles')
    ws = os.environ.get('TASK_OBSERVER_WORKSPACE') or SHARED + '/task-observer'
    if not os.access(ws, os.W_OK):
        return out('FAIL', 'task-observer', f'workspace {ws} not writable')
    if not os.path.isdir(ws + '/skill-observations/observation-log'):
        return out('FAIL', 'task-observer', f'{ws}/skill-observations/observation-log missing (bash $KIT_DIR/task-observer/install.sh)')
    env = json.load(open(HOME + '/.claude/settings.json')).get('env', {})
    if env.get('MCP_TIMEOUT') != '120000' or env.get('RTK_SUPPRESS_HOOK_WARNING') != '1':
        return out('FAIL', 'task-observer', 'settings.json env lacks MCP_TIMEOUT=120000 / RTK_SUPPRESS_HOOK_WARNING=1 (re-run install-all.sh)')
    out('PASS', 'task-observer', 'skill + SessionStart hook; controller vs worker messages correct; workspace and log present; settings env set')


@check('playwright')
def _():
    rc, o = sh("NODE_PATH=$(npm root -g) node -e \"const {chromium}=require('playwright');(async()=>{const b=await chromium.launch();"
               "const p=await b.newPage();await p.setContent('<h1>qa-ok</h1>');console.log(await p.textContent('h1'));await b.close()})()\"", timeout=120)
    out('PASS' if 'qa-ok' in o else 'FAIL', 'playwright', 'headless Chromium rendered a page (browser QA ready)' if 'qa-ok' in o else f'browser QA broken: {o[-200:]}')


@check('pre-commit guard')
def _():
    hk = REPO + '/.git/hooks/pre-commit'
    if not os.path.exists(hk) or 'commit-guard' not in open(hk).read():
        return out('FAIL', 'pre-commit guard', f'{hk} not installed (install-all.sh copies tools/git-hooks/pre-commit)')
    tmp = tempfile.mkdtemp()
    try:
        sh('git init -q . && git config user.email t@t && git config user.name t', cwd=tmp)
        shutil.copy(hk, tmp + '/.git/hooks/pre-commit')
        os.chmod(tmp + '/.git/hooks/pre-commit', 0o755)
        open(tmp + '/ok.py', 'w').write('x = 1\n')
        rc_ok, _ = sh('git add ok.py && git commit -qm ok', cwd=tmp)
        open(tmp + '/a.zip', 'wb').write(b'\0' * 64)
        rc_rom, _ = sh('git add a.zip && git commit -qm rom', cwd=tmp)
        sh('git rm -q --cached a.zip', cwd=tmp)
        open(tmp + '/p.png', 'wb').write(b'\x89PNG\r\n\x1a\n' + b'\0' * 16 + b'caBX' + b'c2pa' + b'\0' * 16)
        rc_png, _ = sh('git add p.png && git commit -qm png', cwd=tmp)
        sh('git rm -q --cached p.png', cwd=tmp)
        open(tmp + '/w.js', 'wb').write(b'a\r\nb\r\n')
        os.makedirs(tmp + '/graphify-out')
        open(tmp + '/graphify-out/g.json', 'w').write('1')
        sh('git add w.js graphify-out && GRAPHIFY_COMMIT=1 git commit -qm base', cwd=tmp)
        open(tmp + '/w.js', 'wb').write(b'a\nb\nc\n')
        rc_eol, _ = sh('git add w.js && git commit -qm eol', cwd=tmp)
        sh('git checkout -q HEAD -- w.js', cwd=tmp)
        open(tmp + '/graphify-out/g.json', 'w').write('2')
        rc_gr, _ = sh('git add graphify-out && git commit -qm g', cwd=tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    ok = rc_ok == 0 and rc_rom != 0 and rc_png != 0 and rc_eol != 0 and rc_gr != 0
    out('PASS' if ok else 'FAIL', 'pre-commit guard', 'blocks a staged .zip, a C2PA-stamped PNG, a CRLF->LF flip and graphify-out/, lets normal files through' if ok
        else f'guard results wrong (normal rc {rc_ok}, rom rc {rc_rom}, c2pa png rc {rc_png}, eol rc {rc_eol}, graphify rc {rc_gr})')


@check('syntax hook')
def _():
    hk = HOME + '/.claude/hooks/syntax-check.py'
    if not os.path.exists(hk):
        return out('FAIL', 'syntax hook', 'PostToolUse syntax hook missing')
    tmp = tempfile.mkdtemp()
    try:
        open(tmp + '/bad.py', 'w').write('def f(:\n')
        open(tmp + '/good.js', 'w').write('const a = 1;\n')
        open(tmp + '/bad.js', 'w').write('const = ;\n')
        r = {n: sh(['python3', hk], inp=json.dumps({'hook_event_name': 'PostToolUse', 'tool_name': 'Edit', 'tool_input': {'file_path': f'{tmp}/{n}'}}))[0]
             for n in ('bad.py', 'good.js', 'bad.js')}
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    ok = r['bad.py'] == 2 and r['bad.js'] == 2 and r['good.js'] == 0
    out('PASS' if ok else 'FAIL', 'syntax hook', 'flags broken .py/.js after Edit/Write, silent on good files' if ok else f'wrong exit codes {r}')


print('== conflicts ==', flush=True)


@check('hooks')
def _():
    if not isinstance(settings, dict) or not settings:
        return out('FAIL', 'hooks', '~/.claude/settings.json missing or invalid JSON')
    ours = {('PreToolUse', 'python3 ~/.claude/hooks/rtk-guard.py'), ('SubagentStart', 'python3 ~/.claude/hooks/caveman-subagent.py'),
            ('SessionStart', 'python3 ~/.claude/hooks/task-observer-start.py'), ('PostToolUse', 'python3 ~/.claude/hooks/syntax-check.py')}
    user = [(ev, c) for s, ev, m, c in HOOKS if s == 'user settings']
    probs = [f'{ev} "{c}" registered {user.count((ev, c))}x' for ev, c in ours if user.count((ev, c)) != 1]
    if ('PreToolUse', 'rtk hook claude') in user:
        probs.append('raw `rtk hook claude` still registered (rtk init re-adds it): install-all.sh swaps it for rtk-guard.py, which runs it and fixes NAME=... and ( ) rewrites')
    probs += [f'duplicate {ev} "{c[:50]}"' for ev, c in set(user) if user.count((ev, c)) > 1 and (ev, c) not in ours]
    # Only rtk (through rtk-guard.py) may rewrite Bash input: any other PreToolUse hook that can see Bash is a clash candidate.
    # claude-mem's file-context hook only reads tool_input.file_path (a no-op for Bash); its Read gate must be off.
    pre = [(s, c) for s, ev, m, c in HOOKS if ev == 'PreToolUse' and (m in ('', '*') or re.search(r'\bBash\b', m or ''))
           and c not in ('python3 ~/.claude/hooks/rtk-guard.py', 'rtk hook claude', 'claude-mem hook claude-code file-context')]
    gate = (jload(HOME + '/.claude-mem/settings.json', {}) or {}).get('CLAUDE_MEM_FILE_READ_GATE_ENABLED', 'true')
    if str(gate).lower() != 'false':
        probs.append('claude-mem file-read gate is on: it denies full-file Reads of files it has notes on (set CLAUDE_MEM_FILE_READ_GATE_ENABLED=false in ~/.claude-mem/settings.json)')
    probs += [f'extra PreToolUse hook on Bash from {s}: {c[:60]}' for s, c in pre]
    for s, ev, m, c in HOOKS:      # every hook's own script must exist
        f = re.search(r'(~/\.claude/[^\s"]+\.(?:py|sh|cjs|js))', c)
        if f and not os.path.exists(os.path.expanduser(f.group(1))):
            probs.append(f'{s} {ev} hook script missing: {f.group(1)}')
    if probs:
        return out('FAIL', 'hooks', '; '.join(probs))
    per = {}
    for s, ev, m, c in HOOKS:
        per.setdefault(ev, set()).add(s.replace('plugin ', ''))
    out('PASS', 'hooks', 'each kit hook once, only rtk rewrites Bash, claude-mem read gate off; ' + ', '.join(f'{ev}: {"+".join(sorted(v))}' for ev, v in sorted(per.items())))


@check('mcp')
def _():
    names = {}
    for n in user_mcp:
        names.setdefault(n, []).append('user')
    for proj, v in cj.get('projects', {}).items():
        for n in v.get('mcpServers', {}):
            names.setdefault(n, []).append('local:' + proj)
    for p in (REPO + '/.mcp.json',):
        for n in (jload(p, {}) or {}).get('mcpServers', {}):
            names.setdefault(n, []).append('repo .mcp.json')
    for key in plugins_enabled:
        name, _, market = key.partition('@')
        d = plugin_dir(market, name)
        for f in (glob.glob(d + '/.mcp.json') if d else []):
            for n in (jload(f, {}) or {}).get('mcpServers', {}):
                names.setdefault('plugin:' + name + ':' + n, []).append('plugin')
    probs = [f'{n} defined {len(w)}x ({", ".join(w)})' for n, w in names.items() if len(w) > 1]
    probs += [f'{n} only at {w[0]} (other folders and workers never see it; use --scope user)' for n, w in names.items() if w and w[0].startswith('local:')]
    need = ['serena', 'context7', 'headroom', 'skillsmp']
    probs += [f'{n} missing at user scope' for n in need if n not in user_mcp]
    if probs:
        return out('FAIL', 'mcp', '; '.join(probs))
    out('PASS', 'mcp', 'no duplicate or local-only servers: ' + ', '.join(sorted(names)))
    out('WARN', 'mcp', 'claude-mem also offers smart_search/smart_outline/smart_unfold (code outline): code symbols go through Serena, claude-mem is for memory only')


@check('exact output')
def _():
    exact = ['find . -name "*.py"', 'grep -rn foo .', 'rg foo', 'cat a.py', 'head -5 a', 'tail -5 a', 'sed -n 1,5p a',
             'git diff HEAD', 'git -C /tmp/wt/x diff', 'git --no-pager diff', 'git -c core.pager=cat diff', 'git show HEAD:x',
             'git log -p -3', 'git stash show -p', 'git blame a.py', 'diff a b', 'wc -l a b', 'python3 -m unittest test_core',
             'timeout 2400 python3 -m unittest test_core', 'node test_app.js', 'node browser_qa.js', 'npx playwright test',
             'pytest -q', 'python3 tools/build.py out.zip', 'bash ./run-tests.sh',
             'cd /x && git diff', 'd=/x; cat $d/a', 'jq . a.json', 'curl -s http://127.0.0.1:18000/',
             'A=1; B=2; echo $A$B', 'S=/root/.claude/skills/task-observer; W=/x; grep -m1 v $S/SKILL.md', 'S="/x/just"; echo $S',
             '(cd /x; git diff)', '(cd /x && grep -n a b)', 'git rebase --continue', 'git merge --abort',
             'make -C /w test', 'cargo test', 'go test ./...', 'npx vitest run', 'docker logs x',
             'npm run lint', 'npm run types', 'npm run test:changed', 'npx tsc --noEmit', 'npx eslint src',
             'echo "rtk S=1, then git pull"', "cat > /tmp/x.md <<'EOF'\ngit pull\nrtk S=1\nEOF"]   # text in strings/heredocs stays as written
    bad = [f'`{c}` -> `{r}`' for c in exact for r in [rtk_rewrite(c)] if r]
    keep = rtk_rewrite('d=/opt/toolkit; ls -la $d')
    if not keep or not keep.startswith('d=/opt/toolkit; '):
        bad.append(f'variable assignment lost: `d=...; ls $d` -> `{keep}`')
    keep2 = rtk_rewrite('A=/opt/toolkit; B=x; ls $A')
    if not keep2 or not keep2.startswith('A=/opt/toolkit; B=x; '):
        bad.append(f'variable assignment lost: `A=...; B=x; ls $A` -> `{keep2}`')
    gp = rtk_rewrite('git pull -q --rebase origin x') or ''
    if 'checkout -q -- graphify-out' not in gp or not gp.rstrip().endswith('git pull -q --rebase origin x'):
        bad.append(f'graphify-out is not restored before `git pull`: -> `{gp}`')
    if bad:
        return out('FAIL', 'exact output', 'rtk rewrites commands that must stay exact: ' + '; '.join(bad))
    out('PASS', 'exact output', f'{len(exact)} read/grep/diff/test/build commands (subshells and NAME=... chains too) pass through untouched; '
        '`ls $d` keeps its assignments; git pull restores graphify-out/ first')


@check('api path')
def _():
    """Headroom and OmniRoute never in the API path (exact reads/diffs/tests would be compressed): direct to Anthropic."""
    probs = []
    files = [SET, HOME + '/.claude/settings.local.json', REPO + '/.claude/settings.json', HOME + '/.bashrc', HOME + '/.profile',
             '/etc/environment', HOME + '/.claude/launcher-settings.json']
    vals = [('env', os.environ.get(k, '')) for k in ('ANTHROPIC_BASE_URL', 'ANTHROPIC_API_URL')]
    for f in files:
        if os.path.exists(f):
            vals += [(f, m) for m in re.findall(r'ANTHROPIC_(?:BASE|API)_URL[^\n]*', open(f, errors='ignore').read())]
    for where, v in vals:
        if re.search(r'8787|headroom', v):
            probs.append(f'{where}: ANTHROPIC_BASE_URL through Headroom')
        elif re.search(r'20128|omniroute', v):
            probs.append(f'{where}: ANTHROPIC_BASE_URL through OmniRoute (opt-in only, needs the owner\'s yes)')
    out('FAIL' if probs else 'PASS', 'api path', '; '.join(probs) if probs else 'direct to Anthropic (no ANTHROPIC_BASE_URL); Headroom MCP-only')


@check('caveman scope')
def _():
    probs = []
    for f in (HOME + '/.claude/CLAUDE.md', REPO + '/CLAUDE.md', REPO + '/CLAUDE.local.md'):
        if os.path.exists(f) and re.search(r'caveman', open(f, errors='ignore').read(), re.I):
            probs.append(f'{f} mentions caveman (would reach the main session)')
    if re.search('caveman', str(settings.get('outputStyle', '')), re.I):
        probs.append('outputStyle is caveman')
    if any('caveman' in k.lower() for k in plugins_enabled):
        probs.append('the caveman plugin is installed (turns the main session terse)')
    probs += [f'{s} {ev} hook injects caveman' for s, ev, m, c in HOOKS if 'caveman' in c.lower() and ev != 'SubagentStart']
    rw = open(KIT + '/workers/run-worker.sh').read()
    if 'append-system-prompt "$CAVE' not in rw:
        probs.append('run-worker.sh no longer gives workers the caveman voice')
    out('FAIL' if probs else 'PASS', 'caveman scope', '; '.join(probs) if probs else 'caveman only via SubagentStart + headless workers; main sessions stay in prose')


@check('context7 key')
def _():
    key = os.environ.get('CONTEXT7_API_KEY', '')
    if len(key) < 8:
        return out('WARN', 'context7 key', 'no CONTEXT7_API_KEY in this environment; nothing to scan')
    roots = [CLAUDE_JSON, HOME + '/.claude', HOME + '/.config', HOME + '/.serena', HOME + '/.claude-mem/settings.json', WT_ROOT + '/reports',
             '/tmp/claude-0/logs', KIT, SHARED, '/tmp/claude/memory', REPO + '/.serena',
             '/tmp/headroom-proxy.log']
    hits, n, kb = [], 0, key.encode()
    for r in roots:
        files = [r] if os.path.isfile(r) else [os.path.join(d, f) for d, _, fs in os.walk(r) for f in fs] if os.path.isdir(r) else []
        for f in files:
            try:
                if os.path.getsize(f) > 30 << 20:
                    continue
                n += 1
                if kb in open(f, 'rb').read():
                    hits.append(f)
            except OSError:
                pass
    out('FAIL' if hits else 'PASS', 'context7 key', f'key value found on disk in: {hits[:6]}' if hits else f'key value in none of {n} config/log/kit/memory/transcript files')


@check('worker kit')
def _():
    rw = open(KIT + '/workers/run-worker.sh').read()
    tpl = open(KIT + '/workers/brief-common-template.md').read()
    probs = []
    if '</dev/null' not in rw:
        probs.append('run-worker.sh: claude -p without </dev/null (stalls on stdin)')
    if re.search(r'--allowedTools "[^"]*Bash\(', rw):
        probs.append('run-worker.sh: narrow Bash(...) allowlist; rtk turns `ls` into `rtk ls`, so also list Bash(rtk ...:*) or allow plain Bash')
    for s in ('serena', 'context7', 'headroom', 'claude-mem', 'skillsmp'):
        if f'"{s}"' not in rw:
            probs.append(f'run-worker.sh: worker MCP config lacks {s}')
        if f'mcp__{s}__*' not in rw:
            probs.append(f'run-worker.sh: --allowedTools lacks mcp__{s}__*')
    if 'TASK_OBSERVER_ROLE=worker' not in rw:
        probs.append('run-worker.sh: TASK_OBSERVER_ROLE=worker missing')
    if 'Skill superpowers:verification-before-completion, the reviewer sub-agent, then Skill ponytail:ponytail-review' not in tpl:
        probs.append('brief template lost the ALWAYS verification + reviewer + ponytail line')
    if 'OBSERVATIONS:' not in tpl:
        probs.append('brief template lacks the OBSERVATIONS: line')
    if 'MCP_TIMEOUT' not in rw or 'serena_up' not in rw or 'still has:' not in rw:
        probs.append('run-worker.sh: Serena warm-up, MCP_TIMEOUT or the unfilled-brief refusal missing')
    if 'INBOX NOT ACKED' not in open(KIT + '/workers/audit.py').read():
        probs.append('audit.py no longer flags unanswered inbox items')
    if 'INBOX=' not in rw or 'WORKER_INBOX' not in rw or '.pid' not in rw:
        probs.append('run-worker.sh: worker inbox ($WORKER_INBOX) or pid file missing')
    for k, why in (('WORKER_INBOX', 'inbox re-read before each commit'), ('stop-worker.sh', 'stop rule (never pkill -f)'), ('graphify-out', 'graphify pull rule'),
                   ('replace_content', 'Serena nested-edit rule'),
                   ("newline=''", 'line-ending rule for scripted edits'), ('rtk bypass', 'raw test-log rule'),
                   ('Scope:', 'finish-the-briefed-scope rule'), ('no diff', 'no-diff verification rule'), ('Image checks', 'image self-check rule'),
                   ('"INBOX: ..."', 'INBOX reply line')):
        if k not in tpl:
            probs.append(f'brief template lacks the {why}')
    for f in ('stop-worker.sh',):
        if not os.path.exists(KIT + '/workers/' + f):
            probs.append(f'workers/{f} missing')
    head = open(KIT + '/install-all.sh').read().split('\n')[:14]
    if not any(l.strip().startswith('cd ~ || exit 1') for l in head):
        probs.append('install-all.sh does not start with `cd ~ || exit 1` (a mounted folder can drop the cwd mid-run)')
    out('FAIL' if probs else 'PASS', 'worker kit', '; '.join(sorted(set(probs))) if probs else
        'run-worker.sh (all 5 MCP servers, plain Bash, empty stdin, worker role, inbox, pid file) + stop helper + brief template rules + install-all.sh cwd guard')


for f in sorted(glob.glob(KIT + '/checks.d/*.sh')):
    rc, o = sh(['bash', f], timeout=300)
    for line in o.strip().splitlines():
        m = re.match(r'(PASS|WARN|FAIL)\s+([^:]+):\s*(.*)', line)
        if m:
            out(m.group(1), m.group(2), m.group(3))
    if rc:
        out('FAIL', os.path.basename(f), f'drop-in check exited {rc}')


# ---------- combined headless worker: every tool in one session ----------
if '--worker' in sys.argv:
    print('== combined headless worker (all tools in one session) ==', flush=True)

    @check('combined worker')
    def _():
        wt = WT_ROOT + '/toolcheck'
        os.makedirs(WT_ROOT, exist_ok=True)
        if os.path.exists(wt):
            sh(['git', 'worktree', 'remove', '--force', wt], cwd=REPO)
            shutil.rmtree(wt, ignore_errors=True)
        rc, o = sh(['git', 'worktree', 'add', '-q', '--detach', wt, 'HEAD'], cwd=REPO, timeout=300)
        if rc:
            return out('FAIL', 'combined worker', 'git worktree add failed: ' + o[:150])
        for t in glob.glob(os.path.expanduser('~/.claude/projects/' + re.sub(r'[^A-Za-z0-9]', '-', wt)) + '/*.jsonl'):
            os.remove(t)
        brief = '/tmp/claude-0/logs/toolcheck-brief.md'
        os.makedirs('/tmp/claude-0/logs', exist_ok=True)
        open(brief, 'w').write(open(KIT + '/workers/toolcheck-brief.md').read().replace('{TARGET}', TARGET).replace('{KIT}', KIT))
        t0 = time.time()
        rc, o = sh(['bash', KIT + '/workers/run-worker.sh', wt, brief], timeout=1500, cwd=wt)
        log = open(WT_ROOT + '/reports/toolcheck.log', errors='ignore').read()
        tr = sorted(glob.glob(os.path.expanduser('~/.claude/projects/' + re.sub(r'[^A-Za-z0-9]', '-', wt)) + '/*.jsonl'), key=os.path.getmtime)
        if not tr:
            return out('FAIL', 'combined worker', 'no transcript; log tail: ' + log[-300:])
        raw = open(tr[-1], errors='ignore').read()
        sub = ''.join(open(f, errors='ignore').read() for f in glob.glob(os.path.dirname(tr[-1]) + '/*/subagents/*.jsonl'))
        rc, aud = sh(['python3', KIT + '/workers/audit.py', wt], timeout=60)
        print('     audit: ' + aud.strip().replace('\n', '\n     audit: '), flush=True)
        calls, results_txt, final = [], {}, ''
        for l in raw.splitlines():
            try:
                m = json.loads(l).get('message') or {}
            except ValueError:
                continue
            for b in m.get('content') if isinstance(m.get('content'), list) else ():
                if b.get('type') == 'tool_use':
                    calls.append((b['id'], b['name'], json.dumps(b['input'])))
                elif b.get('type') == 'tool_result':
                    c = b.get('content')
                    results_txt[b.get('tool_use_id')] = c if isinstance(c, str) else json.dumps(c)
                elif b.get('type') == 'text' and m.get('role') == 'assistant':
                    final = b['text']
        names = ' '.join(n for _, n, _ in calls)
        used = {
            'claude-mem': 'mcp__claude-mem__' in names, 'serena': 'mcp__serena__' in names, 'context7': 'mcp__context7__' in names,
            'headroom': 'mcp__headroom__' in names, 'skillsmp': 'mcp__skillsmp__' in names,
            'graphify update+ask': any('graphify update' in i for _, _, i in calls) and any(re.search(r'graphify (explain|path|query)', i) for _, _, i in calls),
            'superpowers verification': any(n == 'Skill' and 'verification-before-completion' in i for _, n, i in calls),
            'ponytail review': any(n == 'Skill' and 'ponytail' in i for _, n, i in calls),
            'impeccable': any((n == 'Skill' and 'impeccable' in i) or 'impeccable detect' in i for _, n, i in calls),
            'markitdown': any('markitdown' in i for _, n, i in calls if n == 'Bash'),
            'reviewer sub-agent': any(n in ('Agent', 'Task') for _, n, _ in calls),
            'playwright': any('chromium' in i for _, n, i in calls if n == 'Bash'),
        }
        miss = [k for k, v in used.items() if not v]
        checks = {
            'caveman reached the sub-agent': 'CAVEMAN MODE ACTIVE (sub-agent only)' in sub,
            'task-observer told the worker not to log': 'you are a worker. Do NOT invoke the task-observer' in raw,
            'worker did not run task-observer': 'new-observation.sh' not in ' '.join(i for _, _, i in calls) and 'skill": "task-observer' not in raw,
            'OBSERVATIONS line': 'OBSERVATIONS:' in final,
            '`d=...; ls $d` listed the kit folder': any('ls' in i and '$d' in i and 'check_tools.py' in results_txt.get(cid, '') for cid, n, i in calls if n == 'Bash'),
            '`A=..; B=x; ls $A` listed the kit folder': any('ls $A' in i and 'check_tools.py' in results_txt.get(cid, '') for cid, n, i in calls if n == 'Bash'),
            'worker read its inbox': any('WORKER_INBOX' in i and '# Inbox for worker' in results_txt.get(cid, '') for cid, n, i in calls if n == 'Bash'),
            'grep output exact': any('grep -c' in i and re.search(r'^\s*\d+\s*$', results_txt.get(cid, '').strip() or 'x') for cid, n, i in calls if n == 'Bash'),
            'no tool errors from hooks': 'hook error' not in raw.lower() and 'requires approval' not in raw,
        }
        rc_g, st = sh(['git', 'status', '--porcelain'], cwd=wt)
        checks['worker left no repo changes'] = all(l[3:].startswith(('graphify-out', '.serena')) for l in st.splitlines())
        bad = [k for k, v in checks.items() if not v]
        if 'MISSING (defect): none' not in aud:
            bad.append('audit.py MISSING line not empty')
        dur = int(time.time() - t0)
        if miss or bad:
            return out('FAIL', 'combined worker', f'{dur}s; tools not called: {miss}; failed: {bad}')
        out('PASS', 'combined worker', f'{dur}s; one headless worker called all {len(used)} tool groups together; audit clean; '
            'caveman in sub-agent, task-observer worker role, inbox read, rtk kept $d, $A and grep exact')
        sh(['git', 'worktree', 'remove', '--force', wt], cwd=REPO)

fails = [r for r in results if r[0] == 'FAIL']
warns = [r for r in results if r[0] == 'WARN']
print(f'== check-tools: {len(results) - len(fails) - len(warns)} PASS, {len(warns)} WARN, {len(fails)} FAIL ==')
sys.exit(1 if fails else 0)
