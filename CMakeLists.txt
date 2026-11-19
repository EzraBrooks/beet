cmake_minimum_required(VERSION 3.25)
# Builds every package in the workspace into one tree, so there is a single compile_commands.json.
# Each package still builds standalone from its own directory.
project(beet_workspace LANGUAGES NONE)

include(CTest)

option(BEET_BUILD_EXAMPLES "Build the examples package" ON)

add_subdirectory(beet)
add_subdirectory(beet-rerun)
if(BEET_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
