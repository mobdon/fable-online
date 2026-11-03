add_library(fable_online SHARED src/dllmain.cpp src/log.cpp src/probe.cpp)
target_compile_definitions(fable_online PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set_target_properties(fable_online PROPERTIES PREFIX "" OUTPUT_NAME "fable_online")
