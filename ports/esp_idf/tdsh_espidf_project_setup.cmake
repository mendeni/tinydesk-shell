# Include this AFTER project(...) in an ESP-IDF application using the full
# TinyDesk Shell ESP-IDF port. It applies compatibility settings to the pinned
# third-party SMB/wolfSSL dependencies without editing managed_components.
get_filename_component(TDSH_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

idf_component_get_property(TDSH_LIBSMB2_LIB sahlberg__libsmb2 COMPONENT_LIB)
if(TDSH_LIBSMB2_LIB)
    target_compile_options(${TDSH_LIBSMB2_LIB} PRIVATE
        -Wno-error
        -Wno-error=array-parameter
        -include esp_random.h
    )
    target_include_directories(${TDSH_LIBSMB2_LIB} PRIVATE
        "$ENV{IDF_PATH}/components/esp_hw_support/include"
    )
endif()

idf_component_get_property(TDSH_WOLFSSL_LIB wolfssl__wolfssl COMPONENT_LIB)
if(NOT TDSH_WOLFSSL_LIB)
    message(FATAL_ERROR "Managed wolfSSL component wolfssl__wolfssl was not found")
endif()

target_include_directories(${TDSH_WOLFSSL_LIB} BEFORE PRIVATE
    "${TDSH_SDK_ROOT}/ports/esp_idf/components/wolfssh_local/include"
)
target_compile_definitions(${TDSH_WOLFSSL_LIB} PRIVATE WOLFSSL_USER_SETTINGS)
target_compile_options(${TDSH_WOLFSSL_LIB} PRIVATE -Wno-error)
