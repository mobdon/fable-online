set(FO_MOD_SOURCES
  src/dllmain.cpp src/log.cpp src/probe.cpp src/devkit.cpp src/herolook.cpp src/config.cpp src/scoreboard.cpp src/crash.cpp
  src/sync.cpp src/crash_exit.cpp
  src/game/hero.cpp
  src/game/hook.cpp src/game/puppet.cpp src/game/shop_stock.cpp src/game/shop_stock_hook.cpp src/util/inflate.cpp src/game/script.cpp src/game/spawns.cpp src/game/thing.cpp src/game/things.cpp
  src/net/chat_client.cpp src/net/world_client.cpp
  src/world/entities.cpp src/world/objects.cpp src/world/snapshot.cpp src/world/reporter.cpp src/world/health.cpp src/world/held.cpp src/world/spawn_filter.cpp
  src/overlay/overlay.cpp src/overlay/patch.cpp src/overlay/renderer.cpp
  src/ui/chat_model.cpp src/ui/json.cpp src/ui/nametags.cpp src/ui/trade_model.cpp
  src/menu/menu.cpp src/menu/menu_model.cpp
  src/display/camera_patch.cpp src/display/widescreen.cpp src/display/frame_probe.cpp src/display/present.cpp src/display/hud_patch.cpp src/display/hud_scale.cpp src/display/mouse_look.cpp src/display/camera_probe.cpp src/display/camera_latch.cpp src/display/target_colour.cpp src/game/pause_menu.cpp
  src/assets/fable_font.cpp src/assets/fable_ui.cpp
  ../shared/art/big.cpp ../shared/art/lzo1x.cpp ../shared/art/dxt.cpp)

add_library(fable_online SHARED ${FO_MOD_SOURCES})
target_compile_definitions(fable_online PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0601)
target_link_libraries(fable_online PRIVATE ws2_32 gdi32 user32 advapi32)
set_target_properties(fable_online PROPERTIES PREFIX "" OUTPUT_NAME "fable_online")

# The start-up menu's fonts (SIL OFL) go in fonts/ next to the DLL.
add_custom_command(TARGET fable_online POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/fonts $<TARGET_FILE_DIR:fable_online>/fonts)

# The parts with no Windows or game dependencies are tested on their own; see tests/run.sh.
if(FO_BUILD_TESTS)
  add_executable(world_tests tests/world_test.cpp src/world/entities.cpp src/world/snapshot.cpp src/world/reporter.cpp src/world/health.cpp src/world/spawn_filter.cpp)
  target_include_directories(world_tests PRIVATE src)
  add_executable(objects_tests tests/objects_test.cpp src/world/objects.cpp)
  add_executable(shop_stock_tests tests/shop_stock_test.cpp src/game/shop_stock.cpp src/util/inflate.cpp)
  add_executable(held_tests tests/held_test.cpp src/world/held.cpp src/world/objects.cpp)
  target_include_directories(held_tests PRIVATE src)
  target_include_directories(objects_tests PRIVATE src)
  # The game connection against a running server (server/scripts/link-test.ts).
  add_executable(link_test tests/link_test.cpp src/log.cpp src/net/world_client.cpp src/world/snapshot.cpp src/world/reporter.cpp)
  target_include_directories(link_test PRIVATE src)
  target_compile_definitions(link_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0601)
  target_link_libraries(link_test PRIVATE ws2_32)
endif()
