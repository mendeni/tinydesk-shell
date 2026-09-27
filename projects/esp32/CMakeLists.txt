# TinyDesk Shell firmware for the classic ESP32 (4 MB flash or more, PSRAM
# optional), ESP-IDF 5.3.1. The shell runs on UART0, the dev board's
# USB-UART chip, at 115200 baud.
#
#   cd projects/esp32
#   idf.py build
#   idf.py -p PORT flash monitor
cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET "esp32" CACHE STRING "TinyDesk Shell firmware target")
get_filename_component(TDSH_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
file(STRINGS "${TDSH_SDK_ROOT}/VERSION" PROJECT_VER LIMIT_COUNT 1)
list(APPEND EXTRA_COMPONENT_DIRS "${TDSH_SDK_ROOT}/ports/esp_idf/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tdsh_esp32)
include("${TDSH_SDK_ROOT}/ports/esp_idf/tdsh_espidf_project_setup.cmake")
