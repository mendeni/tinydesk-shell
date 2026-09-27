cmake_minimum_required(VERSION 3.16)
project(tdsh C)

option(TDSH_BUILD_POSIX "Build the POSIX host port" ON)
option(TDSH_BUILD_TESTS "Build portable regression tests" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(tdsh_core STATIC
    src/core/tdsh_core.c
    src/core/tdsh_path.c
    src/core/tdsh_parser.c
    src/core/tdsh_script.c
    src/core/tdsh_builtin.c
    src/core/tdsh_terminal.c
    src/core/tdsh_board.c
)
target_include_directories(tdsh_core PUBLIC include)
target_compile_options(tdsh_core PRIVATE -Wall -Wextra -Wpedantic)

if(TDSH_BUILD_POSIX)
    add_library(tdsh_posix STATIC
        ports/posix/tdsh_posix.c
        ports/posix/tdsh_posix_commands.c
        ports/posix/tdsh_nano_posix.c
    )
    target_include_directories(tdsh_posix PUBLIC include ports/posix)
    target_link_libraries(tdsh_posix PUBLIC tdsh_core pthread)
    target_compile_options(tdsh_posix PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(tdsh_host examples/posix/main.c)
    target_link_libraries(tdsh_host PRIVATE tdsh_posix)
endif()

if(TDSH_BUILD_TESTS)
    enable_testing()
    add_executable(tdsh_console_access_tests tests/test_console_access.c)
    target_include_directories(tdsh_console_access_tests PRIVATE include)
    add_test(NAME tdsh_console_access_tests COMMAND tdsh_console_access_tests)
    add_executable(tdsh_core_tests tests/test_core.c)
    target_include_directories(tdsh_core_tests PRIVATE ports/posix)
    target_link_libraries(tdsh_core_tests PRIVATE tdsh_posix)
    add_test(NAME tdsh_core_tests COMMAND tdsh_core_tests)

    add_executable(tdsh_board_tests tests/test_board.c)
    target_link_libraries(tdsh_board_tests PRIVATE tdsh_core)
    add_test(NAME tdsh_board_tests COMMAND tdsh_board_tests)

    add_executable(tdsh_memory_stress tests/test_memory_stress.c)
    target_include_directories(tdsh_memory_stress PRIVATE ports/posix)
    target_link_libraries(tdsh_memory_stress PRIVATE tdsh_posix)
    add_test(NAME tdsh_memory_stress COMMAND tdsh_memory_stress)

    add_executable(tdsh_posix_host_mapping_tests tests/test_posix_host_mapping.c)
    target_include_directories(tdsh_posix_host_mapping_tests PRIVATE ports/posix)
    target_link_libraries(tdsh_posix_host_mapping_tests PRIVATE tdsh_posix)
    add_test(NAME tdsh_posix_host_mapping_tests COMMAND tdsh_posix_host_mapping_tests)

    add_executable(tdsh_terminal_editor_tests tests/test_terminal_editor.c)
    target_include_directories(tdsh_terminal_editor_tests PRIVATE ports/posix)
    target_link_libraries(tdsh_terminal_editor_tests PRIVATE tdsh_posix)
    add_test(NAME tdsh_terminal_editor_tests COMMAND tdsh_terminal_editor_tests)

    add_executable(tdsh_full_uscript_tests tests/test_full_uscript.c)
    target_include_directories(tdsh_full_uscript_tests PRIVATE ports/posix)
    target_compile_definitions(tdsh_full_uscript_tests PRIVATE TDSH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(tdsh_full_uscript_tests PRIVATE tdsh_posix)
    add_test(NAME tdsh_full_uscript_tests COMMAND tdsh_full_uscript_tests)
endif()
