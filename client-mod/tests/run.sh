#!/bin/sh
# Builds and runs the portable client tests with the host compiler (Linux, WSL or macOS).
# Set FABLE_DIR to a Fable install to also check the fonts.big / frontend.big readers.
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../src"
art="$here/../../shared/art"
out=${TMPDIR:-/tmp}
cxx=${CXX:-c++}
flags="-std=c++17 -Wall -Wextra -O1 -fsanitize=address,undefined"
$cxx $flags -o "$out/fable_online_ui_test" \
  "$here/ui_test.cpp" "$src/ui/json.cpp" "$src/ui/chat_model.cpp" "$src/ui/nametags.cpp"
"$out/fable_online_ui_test"
$cxx $flags -o "$out/fable_online_assets_test" "$here/assets_test.cpp" \
  "$src/assets/fable_font.cpp" "$src/assets/fable_ui.cpp" "$art/big.cpp" "$art/lzo1x.cpp" "$art/dxt.cpp"
"$out/fable_online_assets_test"
$cxx $flags -o "$out/fable_online_menu_test" \
  "$here/menu_test.cpp" "$src/menu/menu_model.cpp" "$art/big.cpp" "$art/lzo1x.cpp"
"$out/fable_online_menu_test"
$cxx $flags -o "$out/fable_online_display_test" "$here/display_test.cpp" "$src/display/camera_patch.cpp"
"$out/fable_online_display_test"
# Frame decoding and the entity table.
$cxx -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -I"$src" -o "$out/fable_online_world_tests" \
  "$here/world_test.cpp" "$src/world/entities.cpp" "$src/world/snapshot.cpp"
"$out/fable_online_world_tests"
$cxx -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -I"$src" -o "$out/fable_online_objects_tests" \
  "$here/objects_test.cpp" "$src/world/objects.cpp"
"$out/fable_online_objects_tests"
$cxx $flags -o "$out/fable_online_hud_test" "$here/hud_test.cpp" "$src/display/hud_patch.cpp"
"$out/fable_online_hud_test"
$cxx $flags -o "$out/fable_online_tint_test" "$here/tint_test.cpp" "$art/tint.cpp"
"$out/fable_online_tint_test"
