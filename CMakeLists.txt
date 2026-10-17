cmake_minimum_required(VERSION 3.25)
project(beet VERSION 0.1.0 LANGUAGES CXX)

include(CMakeDependentOption)

if(PROJECT_IS_TOP_LEVEL)
  # Defines BUILD_TESTING (ON by default) and calls enable_testing().
  include(CTest)
endif()

cmake_dependent_option(BEET_BUILD_TESTS "Build beet tests" ON "PROJECT_IS_TOP_LEVEL;BUILD_TESTING" OFF)
option(BEET_BUILD_EXAMPLES "Build beet examples" ${PROJECT_IS_TOP_LEVEL})
option(BEET_BUILD_RERUN_EXAMPLE "Build the Rerun status logging example (needs rerun_sdk)" OFF)
option(BEET_BUILD_ROBOPLAN_EXAMPLE "Build the roboplan UR5 example (needs roboplan and rerun_sdk)" OFF)
option(BEET_BUILD_BENCHMARKS "Build beet benchmarks" OFF)
option(BEET_INSTALL "Generate beet install rules" ${PROJECT_IS_TOP_LEVEL})
option(BEET_USE_SYSTEM_DEPS "Require installed dependencies instead of fetching them" OFF)

if(BEET_USE_SYSTEM_DEPS)
  find_package(tl-expected 1.1 REQUIRED)
else()
  include(FetchContent)
  FetchContent_Declare(
    tl-expected
    GIT_REPOSITORY https://github.com/TartanLlama/expected.git
    GIT_TAG v1.1.0
    FIND_PACKAGE_ARGS 1.1)
  set(EXPECTED_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(tl-expected)
endif()

find_package(Threads REQUIRED)

include(GNUInstallDirs)

add_library(beet INTERFACE)
add_library(beet::beet ALIAS beet)
target_include_directories(beet INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                          $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(beet INTERFACE cxx_std_20)
target_link_libraries(beet INTERFACE tl::expected Threads::Threads)

if(BEET_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(BEET_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(BEET_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(BEET_INSTALL)
  if(NOT BEET_USE_SYSTEM_DEPS AND NOT tl-expected_FOUND)
    message(
      WARNING
        "beet is being installed against a fetched copy of tl-expected. beetConfig.cmake calls "
        "find_dependency(tl-expected), which will fail for consumers because the fetched copy is "
        "never installed. Configure with -DBEET_USE_SYSTEM_DEPS=ON to require an installed one.")
  endif()

  include(CMakePackageConfigHelpers)

  # beet is header-only, so the package is architecture independent and belongs under share/.
  set(BEET_INSTALL_CMAKEDIR "${CMAKE_INSTALL_DATADIR}/cmake/beet")

  install(TARGETS beet EXPORT beetTargets)
  install(DIRECTORY include/beet DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(
    EXPORT beetTargets
    FILE beetTargets.cmake
    NAMESPACE beet::
    DESTINATION "${BEET_INSTALL_CMAKEDIR}")

  configure_package_config_file(cmake/beetConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/beetConfig.cmake"
                                INSTALL_DESTINATION "${BEET_INSTALL_CMAKEDIR}")
  write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/beetConfigVersion.cmake"
                                   COMPATIBILITY SameMinorVersion ARCH_INDEPENDENT)

  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/beetConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/beetConfigVersion.cmake"
          DESTINATION "${BEET_INSTALL_CMAKEDIR}")
endif()
