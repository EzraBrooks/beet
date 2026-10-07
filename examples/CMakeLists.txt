add_executable(robot_patrol robot_patrol.cpp)
target_link_libraries(robot_patrol PRIVATE beet::beet)

if(BEET_BUILD_RERUN_EXAMPLE)
  find_package(rerun_sdk REQUIRED)
  add_executable(rerun_status rerun_status.cpp)
  target_link_libraries(rerun_status PRIVATE beet::beet rerun_sdk)
endif()

if(BEET_BUILD_ROBOPLAN_EXAMPLE)
  find_package(rerun_sdk REQUIRED)
  find_package(roboplan_rrt REQUIRED)
  find_package(roboplan_toppra REQUIRED)
  find_package(roboplan_example_models REQUIRED)
  add_executable(roboplan_ur5 roboplan_ur5.cpp)
  target_link_libraries(
    roboplan_ur5 PRIVATE beet::beet rerun_sdk roboplan_rrt::roboplan_rrt roboplan_toppra::roboplan_toppra
                         roboplan_example_models::roboplan_example_models)
endif()
