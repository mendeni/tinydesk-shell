#ifndef TDSH_TERMINAL_H
#define TDSH_TERMINAL_H

#include <stddef.h>
#include <stdint.h>

#include "tdsh.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        void *context;
        int (*read_byte)(void *context, uint8_t *byte_out);
        int (*write_bytes)(void *context, const void *data, size_t length);
    } tdsh_terminal_io_t;

/* Portable VT100/ANSI line editor used by terminal transports.
 *
 * Supported editing:
 *   TAB            command/path completion
 *   Up/Down        persistent per-user history
 *   Left/Right     cursor movement
 *   Home/End       cursor to beginning/end (CSI H/F and 1~/4~/7~/8~)
 *   Delete         delete under cursor (CSI 3~)
 *   Backspace      delete before cursor
 *   Ctrl+A/E       beginning/end
 *   Ctrl+U/K       erase before/after cursor
 *   Ctrl+L         redraw/clear screen
 *   Ctrl+C         cancel current line
 *
 * History is stored at ~/.tdsh_history through the normal TinyDesk Shell virtual
 * filesystem resolver, so it remains inside the configured sandbox.
 */
    int tdsh_terminal_readline(tdsh_session_t *session,
                               const tdsh_terminal_io_t *io,
                               const char *prompt,
                               char *buffer,
                               size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* TDSH_TERMINAL_H */
