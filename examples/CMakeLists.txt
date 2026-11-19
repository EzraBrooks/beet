cmake_minimum_required(VERSION 3.25)
project(beet_examples LANGUAGES CXX)

if(NOT TARGET beet::beet)
  find_package(beet 0.2 REQUIRED)
endif()
find_package(rerun_sdk REQUIRED)
find_package(roboplan_rrt REQUIRED)
find_package(roboplan_toppra REQUIRED)
find_package(roboplan_example_models REQUIRED)

include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-march=native BEET_HAS_MARCH_NATIVE)
check_cxx_compiler_flag(-mcpu=native BEET_HAS_MCPU_NATIVE)

add_executable(tick_rate tick_rate.cpp)
target_link_libraries(tick_rate PRIVATE beet::beet)
if(BEET_HAS_MARCH_NATIVE)
  target_compile_options(tick_rate PRIVATE -march=native)
elseif(BEET_HAS_MCPU_NATIVE)
  target_compile_options(tick_rate PRIVATE -mcpu=native)
endif()

add_executable(robot_patrol robot_patrol.cpp)
target_link_libraries(robot_patrol PRIVATE beet::beet)

add_executable(rerun_status rerun_status.cpp)
target_link_libraries(rerun_status PRIVATE beet::beet rerun_sdk)

add_executable(roboplan_ur5 roboplan_ur5.cpp)
target_link_libraries(
  roboplan_ur5 PRIVATE beet::beet rerun_sdk roboplan_rrt::roboplan_rrt roboplan_toppra::roboplan_toppra
                       roboplan_example_models::roboplan_example_models)