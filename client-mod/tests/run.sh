#!/bin/sh
# Builds and runs the portable client tests with the host compiler (Linux, WSL or macOS).
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/fable_online_ui_test
c++ -std=c++17 -Wall -Wextra -O1 -fsanitize=address,undefined -o "$out" \
  "$here/ui_test.cpp" "$here/../src/ui/json.cpp" "$here/../src/ui/chat_model.cpp" "$here/../src/ui/nametags.cpp"
"$out"
