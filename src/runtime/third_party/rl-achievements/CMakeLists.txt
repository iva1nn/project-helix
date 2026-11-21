cmake_minimum_required(VERSION 3.16)
project(rl_achievements C)

file(GLOB RC_SRC third_party/rcheevos/src/runtime/*.c third_party/rcheevos/src/util/*.c)

add_library(rl_achievements STATIC src/rl_achievements.c third_party/cjson/cJSON.c ${RC_SRC})
target_include_directories(rl_achievements
  PUBLIC include
  PRIVATE third_party/rcheevos/include third_party/rcheevos/src third_party/cjson)
target_compile_definitions(rl_achievements PRIVATE RC_DISABLE_LUA)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(rl_achievements PUBLIC Threads::Threads m)
endif()

option(RL_ACH_TESTS "Build the tests" OFF)
if(RL_ACH_TESTS)
  add_executable(test_rules tests/test_rules.c)
  target_link_libraries(test_rules rl_achievements)
  add_executable(test_channel tests/test_channel.c)
  target_link_libraries(test_channel rl_achievements)
endif()
