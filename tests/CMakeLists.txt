FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.11
  FIND_PACKAGE_ARGS 2.4)
FetchContent_MakeAvailable(doctest)

set(BEET_WARNINGS -Wall -Wextra -Wpedantic -Werror)

add_executable(
  beet_tests
  main.cpp
  meta_test.cpp
  task_test.cpp
  node_test.cpp
  channel_test.cpp
  combinators_test.cpp
  parallel_test.cpp
  decorators_test.cpp
  observe_test.cpp
  offload_test.cpp)
target_link_libraries(beet_tests PRIVATE beet::beet doctest::doctest)
target_compile_options(beet_tests PRIVATE ${BEET_WARNINGS})
add_test(NAME beet_tests COMMAND beet_tests)

# Each file must fail to compile with the given static_assert message.
function(beet_compile_fail name pattern)
  add_executable(${name} EXCLUDE_FROM_ALL compile_fail/${name}.cpp)
  target_link_libraries(${name} PRIVATE beet::beet)
  add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ${name} --config $<CONFIG>)
  set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${pattern}")
endfunction()

beet_compile_fail(cf_sequence_type_mismatch "sequence\\(\\) child cannot accept the previous child's output type")
beet_compile_fail(cf_recover_unknown_error "recover<E>\\(\\) names an error type this node cannot produce")
beet_compile_fail(cf_await_uncovered_error "errors are not covered by this coroutine's error set")
beet_compile_fail(cf_parallel_input_mismatch "parallel children must share one input type")

# Tracing must leave no symbols in binaries that never attach an observer. The traced test binary
# is the positive control that proves the pattern matches when tracing is used.
set(BEET_TRACE_SYMBOLS "beet::detail::(observe|traced|halt_guard)<")
if(NOT CMAKE_NM)
  find_program(CMAKE_NM NAMES nm gcc-nm llvm-nm REQUIRED)
endif()
if(TARGET robot_patrol)
  add_test(NAME no_trace_symbols_untraced
           COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DBINARY=$<TARGET_FILE:robot_patrol>
                   -DPATTERN=${BEET_TRACE_SYMBOLS} -DEXPECT_MATCH=OFF -P ${CMAKE_CURRENT_SOURCE_DIR}/check_symbols.cmake)
endif()
add_test(NAME trace_symbols_traced
         COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DBINARY=$<TARGET_FILE:beet_tests>
                 -DPATTERN=${BEET_TRACE_SYMBOLS} -DEXPECT_MATCH=ON -P ${CMAKE_CURRENT_SOURCE_DIR}/check_symbols.cmake)
