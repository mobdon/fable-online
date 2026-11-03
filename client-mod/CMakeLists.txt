add_library(fable_online SHARED
  src/dllmain.cpp src/log.cpp src/probe.cpp src/config.cpp
  src/game/hero.cpp
  src/net/chat_client.cpp
  src/overlay/overlay.cpp src/overlay/patch.cpp src/overlay/renderer.cpp
  src/ui/chat_model.cpp src/ui/json.cpp src/ui/nametags.cpp
  src/assets/fable_font.cpp src/assets/fable_ui.cpp
  ../shared/art/big.cpp ../shared/art/lzo1x.cpp ../shared/art/dxt.cpp)
target_compile_definitions(fable_online PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0601)
target_link_libraries(fable_online PRIVATE ws2_32 gdi32 user32 advapi32)
set_target_properties(fable_online PROPERTIES PREFIX "" OUTPUT_NAME "fable_online")
