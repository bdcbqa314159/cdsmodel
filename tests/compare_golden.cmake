# ponytail: a 6-line cmake script beats pulling in a test framework to diff text.
execute_process(COMMAND ${EXE} OUTPUT_VARIABLE actual ERROR_VARIABLE actual RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "exe failed (rc=${rc}):\n${actual}")
endif()
file(READ ${GOLDEN} expected)
if(NOT actual STREQUAL expected)
  message(FATAL_ERROR "ORACLE MISMATCH\n--- expected ---\n${expected}\n--- actual ---\n${actual}")
endif()
