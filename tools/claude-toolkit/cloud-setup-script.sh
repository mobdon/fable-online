#!/bin/bash
# Paste this into the cloud environment's Setup script. It runs the install before Claude starts, so plugins and MCP
# servers load in the first session. It never fails: a non-zero Setup script stops sessions from starting.
# fable-online: the kit lives in the repo at tools/claude-toolkit. KIT_DIR is found in the usual clone locations;
# if the repo is not cloned yet when this runs, the first session runs install-all.sh itself (see CLAUDE.md).
export PATH="$HOME/.local/bin:/opt/claude-code/bin:/opt/node22/bin:$PATH"
if [ -z "${KIT_DIR:-}" ]; then
  for d in "$HOME/fable-online" /home/claude/fable-online /home/user/fable-online /workspace/fable-online; do
    [ -f "$d/tools/claude-toolkit/install-all.sh" ] && { export KIT_DIR=$d/tools/claude-toolkit REPO_DIR=$d; break; }
  done
fi
[ -f "${KIT_DIR:-/nonexistent}/install-all.sh" ] && bash "$KIT_DIR/install-all.sh" > /tmp/toolkit-setup.log 2>&1
exit 0
