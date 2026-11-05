set(FO_MOD_SOURCES
  src/dllmain.cpp src/log.cpp src/probe.cpp src/devkit.cpp src/herolook.cpp src/config.cpp src/scoreboard.cpp
  src/sync.cpp
  src/game/hero.cpp
  src/game/hook.cpp src/game/puppet.cpp src/game/script.cpp src/game/spawns.cpp src/game/thing.cpp src/game/things.cpp
  src/net/chat_client.cpp src/net/world_client.cpp
  src/world/entities.cpp src/world/objects.cpp src/world/snapshot.cpp
  src/overlay/overlay.cpp src/overlay/patch.cpp src/overlay/renderer.cpp
  src/ui/chat_model.cpp src/ui/json.cpp src/ui/nametags.cpp
  src/menu/menu.cpp src/menu/menu_model.cpp
  src/display/camera_patch.cpp src/display/widescreen.cpp src/display/frame_probe.cpp src/display/present.cpp src/display/hud_patch.cpp src/display/hud_scale.cpp
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
  add_executable(world_tests tests/world_test.cpp src/world/entities.cpp src/world/snapshot.cpp)
  target_include_directories(world_tests PRIVATE src)
  add_executable(objects_tests tests/objects_test.cpp src/world/objects.cpp)
  target_include_directories(objects_tests PRIVATE src)
endif()
