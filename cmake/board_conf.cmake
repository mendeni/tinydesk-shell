# The board configuration built into a standalone firmware: board.conf of
# the project (your own wiring; ignored by git) if it exists, else its
# board.example.conf. /fs/etc/board.conf on the device overrides it.
# Set TDSH_BOARD_DIR (the project folder) before including this; it sets
# TDSH_BOARD_BUILTIN to the copy to embed (EMBED_TXTFILES).
set(TDSH_BOARD_CONF "${TDSH_BOARD_DIR}/board.conf")
if(NOT EXISTS "${TDSH_BOARD_CONF}")
    set(TDSH_BOARD_CONF "${TDSH_BOARD_DIR}/board.example.conf")
endif()
set(TDSH_BOARD_BUILTIN "${CMAKE_CURRENT_BINARY_DIR}/board_builtin.conf")
configure_file("${TDSH_BOARD_CONF}" "${TDSH_BOARD_BUILTIN}" COPYONLY)
message(STATUS "TinyDesk Shell board configuration: ${TDSH_BOARD_CONF}")
