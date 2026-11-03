set(FO_MOD_SOURCES
  src/dllmain.cpp
  src/log.cpp
  src/probe.cpp
  src/devkit.cpp
  src/sync.cpp
  src/game/hook.cpp
  src/game/puppet.cpp
  src/game/script.cpp
  src/game/spawns.cpp
  src/game/thing.cpp
  src/net/world_client.cpp
  src/world/entities.cpp
  src/world/snapshot.cpp)

add_library(fable_online SHARED ${FO_MOD_SOURCES})
target_compile_definitions(fable_online PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0601)
target_link_libraries(fable_online PRIVATE ws2_32)
set_target_properties(fable_online PROPERTIES PREFIX "" OUTPUT_NAME "fable_online")

# The parts with no Windows or game dependencies are tested on their own; see tests/run.sh.
if(FO_BUILD_TESTS)
  add_executable(world_tests tests/world_test.cpp src/world/entities.cpp src/world/snapshot.cpp)
  target_include_directories(world_tests PRIVATE src)
endif()
