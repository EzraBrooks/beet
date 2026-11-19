# Standalone project. It is built against an installed beet, never as part of the main build, to
# prove that find_package(beet) works from a clean prefix.
cmake_minimum_required(VERSION 3.25)
project(beet_package_test LANGUAGES CXX)

find_package(beet 0.2 REQUIRED)

add_executable(beet_package_test main.cpp)
target_link_libraries(beet_package_test PRIVATE beet::beet)
