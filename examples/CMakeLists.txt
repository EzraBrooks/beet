add_executable(robot_patrol robot_patrol.cpp)
target_link_libraries(robot_patrol PRIVATE beet::beet)

if(BEET_BUILD_RERUN_EXAMPLE)
  find_package(rerun_sdk REQUIRED)
  add_executable(rerun_status rerun_status.cpp)
  target_link_libraries(rerun_status PRIVATE beet::beet rerun_sdk)
endif()
