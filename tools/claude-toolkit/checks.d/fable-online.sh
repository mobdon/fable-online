#!/usr/bin/env bash
# fable-online drop-in check for check_tools.py: the repo's own toolchain. One PASS|WARN|FAIL line each.
KIT=${KIT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
REPO=${REPO_DIR:-$(git -C "$KIT" rev-parse --show-toplevel 2>/dev/null)}
[ -d "$REPO/server" ] || { echo "FAIL fable-online: no server/ under '$REPO' (set REPO_DIR to the fable-online clone)"; exit 0; }
v=$(node -p 'process.versions.node' 2>/dev/null)
if [ -z "$v" ]; then echo "FAIL fable-online node: node not found"
elif node -e 'const [a,b]=process.versions.node.split(".").map(Number);process.exit(a>22||(a===22&&b>=13)?0:1)'; then echo "PASS fable-online node: $v (server needs >=22.13)"
else echo "FAIL fable-online node: $v is older than the server's >=22.13"; fi
if [ -d "$REPO/server/node_modules" ]; then echo "PASS fable-online server deps: server/node_modules present"
else echo "WARN fable-online server deps: run (cd $REPO/server && npm ci) before npm test / npm run typecheck"; fi
command -v cmake >/dev/null && echo "PASS fable-online cmake: $(cmake --version | head -1)" || echo "WARN fable-online cmake: not installed (needed for client-mod and launcher)"
# client-mod and launcher cross-compile for 32-bit Windows (cmake/mingw-i686.cmake)
if command -v i686-w64-mingw32-g++ >/dev/null; then echo "PASS fable-online mingw: i686-w64-mingw32-g++ found"
else echo "WARN fable-online mingw: i686-w64-mingw32-g++ missing; the DLL/launcher build needs it (apt-get install -y g++-mingw-w64-i686)"; fi
exit 0
