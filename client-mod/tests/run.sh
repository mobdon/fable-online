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
  "$here/ui_test.cpp" "$src/ui/json.cpp" "$src/ui/chat_model.cpp" "$src/ui/nametags.cpp" "$src/ui/party_model.cpp"
"$out/fable_online_ui_test"
$cxx $flags -o "$out/fable_online_trade_test" \
  "$here/trade_test.cpp" "$src/ui/json.cpp" "$src/ui/chat_model.cpp" "$src/ui/trade_model.cpp"
"$out/fable_online_trade_test"
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
  "$here/world_test.cpp" "$src/world/entities.cpp" "$src/world/snapshot.cpp" "$src/world/reporter.cpp" "$src/world/health.cpp" "$src/world/death.cpp" "$src/world/spawn_filter.cpp" "$src/world/gear_report.cpp"
"$out/fable_online_world_tests"
$cxx $flags -I"$src" -o "$out/fable_online_sky_test" "$here/sky_test.cpp" "$src/world/sky.cpp" "$src/ui/json.cpp"
"$out/fable_online_sky_test"
$cxx -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -I"$src" -o "$out/fable_online_objects_tests" \
  "$here/objects_test.cpp" "$src/world/objects.cpp"
"$out/fable_online_objects_tests"
$cxx $flags -o "$out/fable_online_hud_test" "$here/hud_test.cpp" "$src/display/hud_patch.cpp"
"$out/fable_online_hud_test"
$cxx -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -I"$src" -o "$out/fable_online_held_tests" \
  "$here/held_test.cpp" "$src/world/held.cpp" "$src/world/objects.cpp"
"$out/fable_online_held_tests"
# Shop stock filter (game.bin rewrite) and the zlib reader; the fixture and cross-check use the
# server's own game.bin reader, so this part needs Node and server/node_modules.
if command -v npx >/dev/null 2>&1 && [ -d "$here/../../server/node_modules" ]; then
  fx="$out/fable_online_shop_fixture"
  rm -rf "$fx"
  (cd "$here/../../server" && npx tsx --disable-warning=ExperimentalWarning ../client-mod/tests/shop_stock_check.ts make "$fx")
  $cxx $flags -o "$out/fable_online_shop_stock_test" "$here/shop_stock_test.cpp" "$src/game/shop_stock.cpp" "$src/util/inflate.cpp"
  "$out/fable_online_shop_stock_test" "$fx"
  (cd "$here/../../server" && npx tsx --disable-warning=ExperimentalWarning ../client-mod/tests/shop_stock_check.ts check "$fx")
else
  echo "shop_stock: skipped (needs Node and npm ci in server/)"
fi
$cxx $flags -o "$out/fable_online_menu_row_test" "$here/menu_row_test.cpp" "$src/game/defs_edit.cpp" "$src/game/menu_row.cpp" "$src/util/inflate.cpp"
"$out/fable_online_menu_row_test"
$cxx $flags -o "$out/fable_online_modsettings_test" "$here/modsettings_test.cpp" "$src/modsettings/settings_model.cpp"
"$out/fable_online_modsettings_test"
