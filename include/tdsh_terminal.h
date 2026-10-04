#ifndef TDSH_TERMINAL_H
#define TDSH_TERMINAL_H

#include <stddef.h>
#include <stdint.h>

#include "tdsh.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    void *context;
    int (*read_byte)(void *context, uint8_t *byte_out);
    int (*write_bytes)(void *context, const void *data, size_t length);
    /* Optional (NULL: not available). The terminal's width in columns, or
     * <= 0 if it is not known; read once per line. */
    int (*columns)(void *context);
    /* Optional (NULL: not available). Like read_byte, but waits at most
     * timeout_ms: 0 with a byte, -ETIMEDOUT without one, any other negative
     * value if this transport cannot wait. Without columns, the editor uses
     * it to ask the terminal for its width (ESC[6n); without either, it
     * assumes 80 columns. */
    int (*read_byte_timeout)(void *context, uint8_t *byte_out, unsigned timeout_ms);
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
 * A line longer than the terminal is wide wraps over several rows; every
 * redraw clears and rewrites all of them. Columns are counted in UTF-8 code
 * points (one cell each); escape sequences in the prompt take no columns.
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
