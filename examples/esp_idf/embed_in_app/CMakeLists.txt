cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "tdsh-embed-example")
get_filename_component(TDSH_shell_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
list(APPEND EXTRA_COMPONENT_DIRS
    "${TDSH_shell_ROOT}/ports/esp_idf/components"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tdsh_embed_example)
include("${TDSH_shell_ROOT}/ports/esp_idf/tdsh_espidf_project_setup.cmake")
