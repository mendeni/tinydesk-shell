# run_with_input.cmake - run PROGRAM with INPUT as its standard input and
# print what it wrote (ctest checks the output). cmake -DPROGRAM=... -DINPUT=... -P
execute_process(COMMAND "${PROGRAM}"
                INPUT_FILE "${INPUT}"
                OUTPUT_VARIABLE out
                ERROR_VARIABLE err
                RESULT_VARIABLE rc
                TIMEOUT 30)
message("${out}${err}")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${PROGRAM} exited with ${rc}")
endif()
