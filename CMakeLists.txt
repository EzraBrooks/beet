cmake_minimum_required(VERSION 3.25)
project(beet VERSION 0.1.0 LANGUAGES CXX)

option(BEET_BUILD_TESTS "Build beet tests" ${PROJECT_IS_TOP_LEVEL})
option(BEET_BUILD_EXAMPLES "Build beet examples" ${PROJECT_IS_TOP_LEVEL})

include(FetchContent)

FetchContent_Declare(
  tl-expected
  GIT_REPOSITORY https://github.com/TartanLlama/expected.git
  GIT_TAG v1.1.0
  FIND_PACKAGE_ARGS 1.1)
set(EXPECTED_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(tl-expected)

find_package(Threads REQUIRED)

add_library(beet INTERFACE)
add_library(beet::beet ALIAS beet)
target_include_directories(beet INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                          $<INSTALL_INTERFACE:include>)
target_compile_features(beet INTERFACE cxx_std_20)
target_link_libraries(beet INTERFACE tl::expected Threads::Threads)

if(BEET_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(BEET_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
