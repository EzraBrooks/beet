cmake_minimum_required(VERSION 3.25)
project(beet_rerun VERSION 0.2.0 LANGUAGES CXX)

option(BEET_RERUN_INSTALL "Generate beet_rerun install rules" ${PROJECT_IS_TOP_LEVEL})

if(NOT TARGET beet::beet)
  find_package(beet 0.2 REQUIRED)
endif()
find_package(rerun_sdk REQUIRED)

include(GNUInstallDirs)

add_library(beet_rerun INTERFACE)
add_library(beet_rerun::beet_rerun ALIAS beet_rerun)
target_include_directories(beet_rerun INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(beet_rerun INTERFACE beet::beet rerun_sdk)

if(BEET_RERUN_INSTALL)
  include(CMakePackageConfigHelpers)

  set(BEET_RERUN_INSTALL_CMAKEDIR "${CMAKE_INSTALL_DATADIR}/cmake/beet_rerun")

  install(TARGETS beet_rerun EXPORT beet_rerunTargets)
  install(DIRECTORY include/beet_rerun DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(
    EXPORT beet_rerunTargets
    FILE beet_rerunTargets.cmake
    NAMESPACE beet_rerun::
    DESTINATION "${BEET_RERUN_INSTALL_CMAKEDIR}")

  configure_package_config_file(cmake/beet_rerunConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/beet_rerunConfig.cmake"
                                INSTALL_DESTINATION "${BEET_RERUN_INSTALL_CMAKEDIR}")
  write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/beet_rerunConfigVersion.cmake"
                                   COMPATIBILITY SameMinorVersion ARCH_INDEPENDENT)

  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/beet_rerunConfig.cmake"
                "${CMAKE_CURRENT_BINARY_DIR}/beet_rerunConfigVersion.cmake"
          DESTINATION "${BEET_RERUN_INSTALL_CMAKEDIR}")
endif()
