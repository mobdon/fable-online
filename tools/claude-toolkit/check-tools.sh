#!/usr/bin/env bash
# Is every tool installed and working, and do any of them clash? One line per check.
# usage: bash check-tools.sh [--worker] [--quiet]
#   --worker adds one combined headless worker that uses every tool in one session (5-8 min; after kit changes).
# install-all.sh runs it (without --worker) at the end and exits with its result.
KIT_DIR=${KIT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}; SHARED_DIR=${SHARED_DIR:-$([ -d /mnt/project-files ] && echo /mnt/project-files/toolkit-data || echo "$HOME/toolkit-data")}; export KIT_DIR SHARED_DIR
cd ~ || exit 1
export no_proxy="${no_proxy:+$no_proxy,}localhost,127.0.0.1" NO_PROXY="${NO_PROXY:+$NO_PROXY,}localhost,127.0.0.1"
exec python3 "$KIT_DIR/check_tools.py" "$@"
