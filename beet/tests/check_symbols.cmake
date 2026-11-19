# Usage: cmake -DNM=<nm> -DBINARY=<file> -DPATTERN=<regex> -DEXPECT_MATCH=ON|OFF -P check_symbols.cmake
execute_process(COMMAND ${NM} -C ${BINARY} OUTPUT_VARIABLE symbols RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "${NM} failed on ${BINARY}")
endif()

if(symbols MATCHES "${PATTERN}")
  set(found ON)
else()
  set(found OFF)
endif()

if(EXPECT_MATCH AND NOT found)
  message(FATAL_ERROR "expected symbols matching '${PATTERN}' in ${BINARY}")
elseif(NOT EXPECT_MATCH AND found)
  message(FATAL_ERROR "unexpected symbol '${CMAKE_MATCH_0}' in ${BINARY}")
endif()
message(STATUS "${BINARY}: symbols matching '${PATTERN}' present = ${found}")
