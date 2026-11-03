enable_language(RC)
add_executable(fable_online_launcher WIN32
  src/main.cpp src/ui.cpp ../shared/art/big.cpp ../shared/art/lzo1x.cpp ../shared/art/dxt.cpp src/api.cpp src/updater.cpp src/game.cpp
  src/http.cpp src/json.cpp src/crypto.cpp src/config.cpp src/util.cpp
  res/launcher.rc)
target_compile_definitions(fable_online_launcher PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0601)
# Bake a gateway into release builds: -DFO_DEFAULT_GATEWAY=https://gateway.example.net
if(FO_DEFAULT_GATEWAY)
  target_compile_definitions(fable_online_launcher PRIVATE FO_DEFAULT_GATEWAY="${FO_DEFAULT_GATEWAY}")
endif()
target_link_libraries(fable_online_launcher PRIVATE winhttp bcrypt crypt32 comctl32 shell32 ole32 advapi32 msimg32 uxtheme dwmapi gdi32)
if(MINGW)
  target_link_options(fable_online_launcher PRIVATE -municode)
endif()
if(MSVC)
  # The .rc embeds our manifest; don't add a second one.
  target_link_options(fable_online_launcher PRIVATE /MANIFEST:NO)
endif()

option(FO_BUILD_TESTS "Build launcher_tests.exe (needs tools/dev-gateway running)" OFF)
if(FO_BUILD_TESTS)
  add_executable(launcher_tests tests/launcher_tests.cpp src/api.cpp src/updater.cpp src/game.cpp
    src/http.cpp src/json.cpp src/crypto.cpp src/config.cpp src/util.cpp)
  target_compile_definitions(launcher_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0601)
  add_executable(art_tests tests/art_tests.cpp ../shared/art/big.cpp ../shared/art/lzo1x.cpp ../shared/art/dxt.cpp)
  target_compile_definitions(art_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_link_libraries(launcher_tests PRIVATE winhttp bcrypt crypt32 shell32 advapi32)
endif()
