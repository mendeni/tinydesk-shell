# wolfSSH v1.5.0 local ESP-IDF component used by the full C6 port.
get_filename_component(TDSH_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(WOLFSSH_ROOT "${TDSH_SDK_ROOT}/third_party/wolfssh")

if(NOT EXISTS "${WOLFSSH_ROOT}/wolfssh/ssh.h")
    message(FATAL_ERROR "wolfSSH source is missing: ${WOLFSSH_ROOT}/wolfssh/ssh.h")
endif()

set(WOLFSSH_SRCS
    "${WOLFSSH_ROOT}/src/agent.c"
    "${WOLFSSH_ROOT}/src/certman.c"
    "${WOLFSSH_ROOT}/src/internal.c"
    "${WOLFSSH_ROOT}/src/io.c"
    "${WOLFSSH_ROOT}/src/keygen.c"
    "${WOLFSSH_ROOT}/src/log.c"
    "${WOLFSSH_ROOT}/src/misc.c"
    "${WOLFSSH_ROOT}/src/port.c"
    "${WOLFSSH_ROOT}/src/ssh.c"
    "${WOLFSSH_ROOT}/src/wolfscp.c"
    "${WOLFSSH_ROOT}/src/wolfsftp.c"
    "${WOLFSSH_ROOT}/src/wolfterm.c"
)

idf_component_register(
    SRCS ${WOLFSSH_SRCS} "${CMAKE_CURRENT_LIST_DIR}/sftp_jail.c"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/include"
        "${WOLFSSH_ROOT}"
    REQUIRES wolfssl__wolfssl lwip vfs freertos pthread
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    WOLFSSL_USER_SETTINGS
    WOLFSSH_SFTP
    USE_OSE_API
)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error
    -Wno-error=maybe-uninitialized
    -Wno-unused-function
    -Wno-unused-variable
)
