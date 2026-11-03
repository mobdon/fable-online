#!/bin/sh
# Builds and runs the portable client tests with the host compiler (Linux, WSL or macOS).
# Set FABLE_DIR to a Fable install to also check the fonts.big / frontend.big readers.
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../src"
art="$here/../../shared/art"
out=${TMPDIR:-/tmp}
flags="-std=c++17 -Wall -Wextra -O1 -fsanitize=address,undefined"
c++ $flags -o "$out/fable_online_ui_test" \
  "$here/ui_test.cpp" "$src/ui/json.cpp" "$src/ui/chat_model.cpp" "$src/ui/nametags.cpp"
"$out/fable_online_ui_test"
c++ $flags -o "$out/fable_online_assets_test" "$here/assets_test.cpp" \
  "$src/assets/fable_font.cpp" "$src/assets/fable_ui.cpp" "$art/big.cpp" "$art/lzo1x.cpp" "$art/dxt.cpp"
"$out/fable_online_assets_test"
c++ $flags -o "$out/fable_online_menu_test" \
  "$here/menu_test.cpp" "$src/menu/menu_model.cpp" "$art/big.cpp" "$art/lzo1x.cpp"
"$out/fable_online_menu_test"
