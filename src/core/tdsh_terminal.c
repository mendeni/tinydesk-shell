#include "tdsh_terminal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define TERM_COMMAND_MATCH_MAX 64U
#define TERM_PATH_DISPLAY_MAX  32U

static int io_write(const tdsh_terminal_io_t *io, const void *data, size_t length)
{
    if (!io || !io->write_bytes || (!data && length))
        return -EINVAL;
    return io->write_bytes(io->context, data, length);
}

static int io_puts(const tdsh_terminal_io_t *io, const char *text)
{
    if (!text)
        text = "";
    return io_write(io, text, strlen(text));
}

static int io_printf(const tdsh_terminal_io_t *io, const char *fmt, ...)
{
    char temp[384];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(temp, sizeof(temp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return -EIO;
    size_t amount = (size_t)n < sizeof(temp) ? (size_t)n : sizeof(temp) - 1U;
    return io_write(io, temp, amount);
}

static void bell(const tdsh_terminal_io_t *io)
{
    static const char ch = '\a';
    (void)io_write(io, &ch, 1U);
}

static int history_real_path(tdsh_session_t *session, char *out, size_t out_size)
{
    return tdsh_path_to_real(session, "~/.tdsh_history", out, out_size, NULL, 0);
}

static int history_get_from_end(tdsh_session_t *session,
                                size_t back,
                                char *out,
                                size_t out_size)
{
    if (!session || !out || out_size < 2U || back == 0U)
        return -EINVAL;
    char path[TDSH_MAX_REAL_PATH];
    int rc = history_real_path(session, path, sizeof(path));
    if (rc != 0)
        return rc;

    FILE *fp = fopen(path, "r");
    if (!fp)
        return -errno;

    char line[TDSH_MAX_LINE + 2U];
    size_t count = 0U;
    while (fgets(line, sizeof(line), fp))
        count++;
    if (back > count)
    {
        fclose(fp);
        return -ENOENT;
    }

    rewind(fp);
    size_t target = count - back;
    size_t index = 0U;
    while (fgets(line, sizeof(line), fp))
    {
        if (index++ == target)
        {
            size_t n = strcspn(line, "\r\n");
            if (n >= out_size)
                n = out_size - 1U;
            memcpy(out, line, n);
            out[n] = '\0';
            fclose(fp);
            return 0;
        }
    }
    fclose(fp);
    return -ENOENT;
}

static void history_trim(tdsh_session_t *session)
{
    const tdsh_core_config_t *cfg = tdsh_core_config();
    if (!session || !cfg || cfg->history_length == 0U)
        return;

    char path[TDSH_MAX_REAL_PATH];
    if (history_real_path(session, path, sizeof(path)) != 0)
        return;
    FILE *fp = fopen(path, "r");
    if (!fp)
        return;

    char line[TDSH_MAX_LINE + 2U];
    size_t count = 0U;
    while (fgets(line, sizeof(line), fp))
        count++;
    const size_t keep = cfg->history_length;
    if (count <= keep + 8U)
    {
        fclose(fp);
        return;
    }

    char tmp[TDSH_MAX_REAL_PATH + 8U];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp))
    {
        fclose(fp);
        return;
    }

    rewind(fp);
    FILE *out = fopen(tmp, "w");
    if (!out)
    {
        fclose(fp);
        return;
    }

    size_t skip = count > keep ? count - keep : 0U;
    size_t index = 0U;
    while (fgets(line, sizeof(line), fp))
    {
        if (index++ >= skip)
            fputs(line, out);
    }
    fclose(fp);
    if (fclose(out) != 0 || rename(tmp, path) != 0)
        (void)remove(tmp);
}

static void history_append(tdsh_session_t *session, const char *line)
{
    if (!session || !line || !line[0])
        return;

    char previous[TDSH_MAX_LINE + 1U];
    if (history_get_from_end(session, 1U, previous, sizeof(previous)) == 0 &&
        strcmp(previous, line) == 0)
    {
        return;
    }

    char path[TDSH_MAX_REAL_PATH];
    if (history_real_path(session, path, sizeof(path)) != 0)
        return;
    FILE *fp = fopen(path, "a");
    if (!fp)
        return;
    fprintf(fp, "%s\n", line);
    fclose(fp);
    history_trim(session);
}

/* ------------------------------------------------------------------
 * Drawing a line that may wrap over several rows.
 *
 * The editor keeps what the last draw left on the screen: how many rows it
 * used and the row the cursor is on (0 = the prompt's row). A redraw goes
 * back up to the prompt's row, clears to the end of the screen and prints
 * prompt and buffer again, then places the cursor. Columns are counted in
 * UTF-8 code points, one cell each; escape sequences take none. The prompt
 * is assumed to start in the first column.
 *
 * When the text ends exactly at the right margin, terminals keep the cursor
 * on the last column until the next character arrives ("pending wrap"), so
 * the editor moves it to the start of the next row itself (CR LF) and counts
 * that row as used.
 */

#define TERM_DEFAULT_COLUMNS  80U
#define TERM_MAX_COLUMNS      1000U
#define TERM_QUERY_TIMEOUT_MS 200U
#define TERM_PUSHBACK_MAX     128U

typedef struct
{
    const tdsh_terminal_io_t *io;
    const char *prompt;
    size_t prompt_cols;
    size_t cols;       /* terminal width */
    size_t rows;       /* rows the text on the screen uses, at least 1 */
    size_t cursor_row; /* the cursor's row, counted from the prompt's row */
    bool end_wrapped;  /* the text ends at the margin: its last row is empty */
    /* Bytes read while waiting for the terminal's size reply that were not
     * part of it (type-ahead); the editor reads them first. */
    uint8_t pushback[TERM_PUSHBACK_MAX];
    size_t pushback_len;
    size_t pushback_pos;
} edit_t;

static bool utf8_continuation(uint8_t byte)
{
    return (byte & 0xC0U) == 0x80U;
}

/* Cells `text` takes: one per code point; escape sequences take none
 * (CSI ... final byte, OSC ... BEL or ST, ESC + one byte). */
static size_t text_columns(const char *text, size_t length)
{
    const uint8_t *s = (const uint8_t *)text;
    size_t i = 0U;
    size_t columns = 0U;
    while (i < length)
    {
        uint8_t c = s[i];
        if (c == 0x1BU)
        {
            i++;
            if (i < length && s[i] == '[')
            {
                i++;
                while (i < length && (s[i] < 0x40U || s[i] > 0x7EU))
                    i++;
                i++;
            }
            else if (i < length && s[i] == ']')
            {
                while (i < length && s[i] != 0x07U && !(s[i] == 0x1BU && i + 1U < length && s[i + 1U] == '\\'))
                    i++;
                i += (i < length && s[i] == 0x1BU) ? 2U : 1U;
            }
            else
            {
                i++;
            }
            continue;
        }
        if (c >= 0x20U && c != 0x7FU && !utf8_continuation(c))
            columns++;
        i++;
    }
    return columns;
}

static size_t prev_char(const char *buffer, size_t at)
{
    if (at == 0U)
        return 0U;
    at--;
    while (at > 0U && utf8_continuation((uint8_t)buffer[at]))
        at--;
    return at;
}

static size_t next_char(const char *buffer, size_t length, size_t at)
{
    if (at >= length)
        return length;
    at++;
    while (at < length && utf8_continuation((uint8_t)buffer[at]))
        at++;
    return at;
}

static void move_rows(const edit_t *e, size_t from, size_t to)
{
    if (to < from)
        (void)io_printf(e->io, "\033[%uA", (unsigned)(from - to));
    else if (to > from)
        (void)io_printf(e->io, "\033[%uB", (unsigned)(to - from));
}

/* The cursor goes to cell `pos` of the line (prompt included). */
static void place_cursor(edit_t *e, size_t pos)
{
    size_t row = pos / e->cols;
    size_t col = pos % e->cols;
    move_rows(e, e->cursor_row, row);
    (void)io_puts(e->io, "\r");
    if (col > 0U)
        (void)io_printf(e->io, "\033[%uC", (unsigned)col);
    e->cursor_row = row;
}

/* After printing text up to cell `total` from the start of the prompt's row,
 * with the terminal's cursor right after it. */
static void text_printed_to(edit_t *e, size_t total)
{
    e->end_wrapped = total > 0U && total % e->cols == 0U;
    if (e->end_wrapped)
        (void)io_puts(e->io, "\r\n");
    e->cursor_row = total / e->cols;
    e->rows = e->cursor_row + 1U;
}

static void refresh(edit_t *e, const char *buffer, size_t length, size_t cursor)
{
    move_rows(e, e->cursor_row, 0U);
    (void)io_puts(e->io, "\r\033[J");
    (void)io_puts(e->io, e->prompt);
    (void)io_write(e->io, buffer, length);
    text_printed_to(e, e->prompt_cols + text_columns(buffer, length));
    place_cursor(e, e->prompt_cols + text_columns(buffer, cursor));
}

/* Move the cursor within the text without redrawing it. */
static void move_cursor(edit_t *e, const char *buffer, size_t cursor)
{
    place_cursor(e, e->prompt_cols + text_columns(buffer, cursor));
}

/* Leave the line: the cursor goes to the start of the row below the text
 * (output that follows starts there), and the next draw starts afresh. */
static void leave_line(edit_t *e)
{
    move_rows(e, e->cursor_row, e->rows - 1U);
    (void)io_puts(e->io, e->end_wrapped ? "\r" : "\r\n");
    e->rows = 1U;
    e->cursor_row = 0U;
    e->end_wrapped = false;
}

static int edit_read(edit_t *e, uint8_t *out)
{
    if (e->pushback_pos < e->pushback_len)
    {
        *out = e->pushback[e->pushback_pos++];
        return 0;
    }
    e->pushback_pos = e->pushback_len = 0U;
    if (!e->io->read_byte)
        return -EINVAL;
    return e->io->read_byte(e->io->context, out);
}

static bool pushback_add(edit_t *e, const uint8_t *bytes, size_t count)
{
    if (e->pushback_len + count > sizeof(e->pushback))
        return false;
    memcpy(e->pushback + e->pushback_len, bytes, count);
    e->pushback_len += count;
    return true;
}

/* Wait for a cursor position report, ESC [ row ; col R. Other bytes that
 * arrive meanwhile (keys typed ahead) are kept for the editor. */
static bool read_position_report(edit_t *e, unsigned *col_out)
{
    const tdsh_terminal_io_t *io = e->io;
    uint8_t seen[16];
    size_t seen_len = 0U;
    unsigned col = 0U;
    int state = 0; /* 0 ESC, 1 '[', 2 row digits, 3 column digits */
    for (;;)
    {
        uint8_t b = 0U;
        if (io->read_byte_timeout(io->context, &b, TERM_QUERY_TIMEOUT_MS) != 0)
        {
            (void)pushback_add(e, seen, seen_len);
            return false;
        }
        bool fits = (state == 0 && b == 0x1BU) || (state == 1 && b == '[') ||
                    (state >= 2 && b >= '0' && b <= '9') || (state == 2 && b == ';') ||
                    (state == 3 && b == 'R');
        if (!fits || seen_len + 1U >= sizeof(seen))
        {
            /* Not a report: what was collected was typed. An ESC may start
             * the report (or another key) again. */
            if (!pushback_add(e, seen, seen_len))
                return false;
            seen_len = 0U;
            state = 0;
            col = 0U;
            if (b != 0x1BU)
            {
                if (!pushback_add(e, &b, 1U))
                    return false;
                continue;
            }
        }
        seen[seen_len++] = b;
        if (state == 0)
            state = 1;
        else if (state == 1)
            state = 2;
        else if (b == ';')
            state = 3;
        else if (b == 'R')
        {
            *col_out = col;
            return true;
        }
        else if (state == 3)
            col = col * 10U + (unsigned)(b - '0');
    }
}

/* The terminal's width: from the transport, else asked from the terminal
 * (linenoise's method: report the column, go to the far right, report it
 * again, go back), else 80. Nothing is asked when the transport cannot wait
 * for an answer, so a terminal that never answers costs at most one wait,
 * and no reply is left to be read as typing. A reply that comes after the
 * wait reads as an unknown key sequence and is ignored. */
static size_t terminal_columns(edit_t *e)
{
    const tdsh_terminal_io_t *io = e->io;
    if (io->columns)
    {
        int c = io->columns(io->context);
        if (c > 0)
            return (size_t)c < TERM_MAX_COLUMNS ? (size_t)c : TERM_MAX_COLUMNS;
    }
    /* The width learned last time, for when the terminal is not asked. */
    static size_t s_last_asked;
    const size_t fallback = s_last_asked ? s_last_asked : TERM_DEFAULT_COLUMNS;
    if (!io->read_byte_timeout)
        return TERM_DEFAULT_COLUMNS;

    /* Can this transport wait? A byte already waiting is type-ahead (a
     * paste, say): then nothing is asked, so the answer cannot end up in
     * the middle of it, and the bytes after the next Enter stay with the
     * transport for the next line. */
    uint8_t b = 0U;
    int rc = io->read_byte_timeout(io->context, &b, 0U);
    if (rc == 0)
    {
        (void)pushback_add(e, &b, 1U);
        return fallback;
    }
    if (rc != -ETIMEDOUT)
        return TERM_DEFAULT_COLUMNS;

    unsigned start = 0U, right = 0U;
    (void)io_puts(io, "\033[6n");
    if (!read_position_report(e, &start) || start == 0U)
        return fallback;
    (void)io_puts(io, "\033[999C\033[6n");
    bool answered = read_position_report(e, &right) && right > 0U;
    if (answered && right > start)
        (void)io_printf(io, "\033[%uD", right - start);
    else if (!answered)
    {
        (void)io_puts(io, "\r");
        if (start > 1U)
            (void)io_printf(io, "\033[%uC", start - 1U);
    }
    if (!answered)
        return fallback;
    s_last_asked = right < TERM_MAX_COLUMNS ? right : TERM_MAX_COLUMNS;
    return s_last_asked;
}

static int replace_token(char *buffer,
                         size_t capacity,
                         size_t *length,
                         size_t *cursor,
                         size_t start,
                         size_t end,
                         const char *replacement)
{
    size_t replacement_len = strlen(replacement);
    size_t old_len = end - start;
    if (start > end || end > *length || *length - old_len + replacement_len + 1U > capacity)
    {
        return -ENOSPC;
    }
    memmove(buffer + start + replacement_len,
            buffer + end,
            *length - end + 1U);
    memcpy(buffer + start, replacement, replacement_len);
    *length = *length - old_len + replacement_len;
    *cursor = start + replacement_len;
    return 0;
}

static size_t common_command_prefix(const char *const *names, size_t count)
{
    if (count == 0U)
        return 0U;
    size_t common = strlen(names[0]);
    for (size_t m = 1U; m < count; ++m)
    {
        size_t i = 0U;
        while (i < common && names[0][i] == names[m][i])
            i++;
        common = i;
    }
    return common;
}

static void complete_command(edit_t *e,
                             char *buffer,
                             size_t capacity,
                             size_t *length,
                             size_t *cursor,
                             size_t start,
                             size_t end)
{
    if (*cursor != end)
    {
        bell(e->io);
        return;
    }

    char prefix[TDSH_MAX_LINE + 1U];
    size_t prefix_len = end - start;
    if (prefix_len >= sizeof(prefix))
    {
        bell(e->io);
        return;
    }
    memcpy(prefix, buffer + start, prefix_len);
    prefix[prefix_len] = '\0';

    size_t command_count = 0U;
    const tdsh_command_t *commands = tdsh_commands_get(&command_count);
    const char *matches[TERM_COMMAND_MATCH_MAX];
    size_t count = 0U;
    for (size_t i = 0U; i < command_count && count < TERM_COMMAND_MATCH_MAX; ++i)
    {
        if (strncmp(commands[i].name, prefix, prefix_len) == 0)
            matches[count++] = commands[i].name;
    }
    if (count == 0U)
    {
        bell(e->io);
        return;
    }

    size_t common = common_command_prefix(matches, count);
    char replacement[TDSH_MAX_LINE + 1U];
    size_t copy = common < sizeof(replacement) - 1U ? common : sizeof(replacement) - 1U;
    memcpy(replacement, matches[0], copy);
    replacement[copy] = '\0';
    if (count == 1U && end == *length && copy + 1U < sizeof(replacement))
    {
        replacement[copy++] = ' ';
        replacement[copy] = '\0';
    }

    if (replace_token(buffer, capacity, length, cursor, start, end, replacement) != 0)
    {
        bell(e->io);
        return;
    }

    if (count > 1U && common == prefix_len)
    {
        leave_line(e);
        for (size_t i = 0U; i < count; ++i)
        {
            (void)io_printf(e->io, "%-18s", matches[i]);
            if ((i + 1U) % 4U == 0U || i + 1U == count)
                (void)io_puts(e->io, "\r\n");
        }
    }
    refresh(e, buffer, *length, *cursor);
}

static size_t common_prefix_pair(const char *a, const char *b, size_t current)
{
    size_t i = 0U;
    while (i < current && a[i] == b[i])
        i++;
    return i;
}

static bool path_entry_is_dir(const char *directory_real, const char *name)
{
    char candidate[TDSH_MAX_REAL_PATH + TDSH_MAX_PATH + 4U];
    int n = snprintf(candidate, sizeof(candidate), "%s/%s", directory_real, name);
    if (n < 0 || (size_t)n >= sizeof(candidate))
        return false;
    struct stat st;
    return stat(candidate, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool path_entry_matches(const struct dirent *entry, const char *base, size_t base_len)
{
    if (!entry || !base)
        return false;
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        return false;
    if (base[0] != '.' && entry->d_name[0] == '.')
        return false;
    return strncmp(entry->d_name, base, base_len) == 0;
}

static void complete_path(tdsh_session_t *session,
                          edit_t *e,
                          char *buffer,
                          size_t capacity,
                          size_t *length,
                          size_t *cursor,
                          size_t start,
                          size_t end)
{
    if (*cursor != end)
    {
        bell(e->io);
        return;
    }

    char token[TDSH_MAX_PATH];
    size_t token_len = end - start;
    if (token_len >= sizeof(token))
    {
        bell(e->io);
        return;
    }
    memcpy(token, buffer + start, token_len);
    token[token_len] = '\0';

    char directory_input[TDSH_MAX_PATH];
    char typed_prefix[TDSH_MAX_PATH];
    const char *base = token;
    const char *slash = strrchr(token, '/');
    if (slash)
    {
        size_t directory_len = (size_t)(slash - token);
        if (directory_len == 0U)
        {
            snprintf(directory_input, sizeof(directory_input), "/");
        }
        else
        {
            memcpy(directory_input, token, directory_len);
            directory_input[directory_len] = '\0';
        }
        size_t typed_len = directory_len + 1U;
        if (typed_len >= sizeof(typed_prefix))
        {
            bell(e->io);
            return;
        }
        memcpy(typed_prefix, token, typed_len);
        typed_prefix[typed_len] = '\0';
        base = slash + 1;
    }
    else
    {
        snprintf(directory_input, sizeof(directory_input), ".");
        typed_prefix[0] = '\0';
    }

    char real[TDSH_MAX_REAL_PATH];
    if (tdsh_path_to_real(session, directory_input, real, sizeof(real), NULL, 0) != 0)
    {
        bell(e->io);
        return;
    }

    DIR *directory = opendir(real);
    if (!directory)
    {
        bell(e->io);
        return;
    }

    const size_t base_len = strlen(base);
    char first_name[TDSH_MAX_PATH] = {0};
    size_t common = 0U;
    size_t count = 0U;
    bool first_is_dir = false;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
    {
        if (!path_entry_matches(entry, base, base_len))
            continue;
        const size_t name_len = strlen(entry->d_name);
        if (name_len >= sizeof(first_name))
            continue; /* too long for a tdsh path; cannot be completed */
        if (count == 0U)
        {
            memcpy(first_name, entry->d_name, name_len + 1);
            common = name_len;
            first_is_dir = path_entry_is_dir(real, entry->d_name);
        }
        else
        {
            common = common_prefix_pair(first_name, entry->d_name, common);
        }
        count++;
    }
    closedir(directory);

    if (count == 0U)
    {
        bell(e->io);
        return;
    }

    char replacement[TDSH_MAX_PATH];
    int n = snprintf(replacement, sizeof(replacement), "%s%.*s",
                     typed_prefix, (int)common, first_name);
    if (n < 0 || (size_t)n >= sizeof(replacement))
    {
        bell(e->io);
        return;
    }

    if (count == 1U && (first_is_dir || end == *length))
    {
        size_t replacement_len = strlen(replacement);
        if (replacement_len + 1U >= sizeof(replacement))
        {
            bell(e->io);
            return;
        }
        replacement[replacement_len] = first_is_dir ? '/' : ' ';
        replacement[replacement_len + 1U] = '\0';
    }

    if (replace_token(buffer, capacity, length, cursor, start, end, replacement) != 0)
    {
        bell(e->io);
        return;
    }

    if (count > 1U && common == base_len)
    {
        directory = opendir(real);
        if (directory)
        {
            size_t shown = 0U;
            leave_line(e);
            while ((entry = readdir(directory)) != NULL && shown < TERM_PATH_DISPLAY_MAX)
            {
                if (!path_entry_matches(entry, base, base_len))
                    continue;
                bool is_dir = path_entry_is_dir(real, entry->d_name);
                (void)io_printf(e->io, "%-18s%s", entry->d_name, is_dir ? "/" : "");
                shown++;
                if (shown % 4U == 0U || shown == count || shown == TERM_PATH_DISPLAY_MAX)
                {
                    (void)io_puts(e->io, "\r\n");
                }
            }
            if (count > TERM_PATH_DISPLAY_MAX)
            {
                (void)io_printf(e->io, "... %u more\r\n", (unsigned)(count - TERM_PATH_DISPLAY_MAX));
            }
            closedir(directory);
        }
    }

    refresh(e, buffer, *length, *cursor);
}

static void complete(tdsh_session_t *session,
                     edit_t *e,
                     char *buffer,
                     size_t capacity,
                     size_t *length,
                     size_t *cursor)
{
    size_t start = *cursor;
    while (start > 0U && !isspace((unsigned char)buffer[start - 1U]))
        start--;
    size_t end = *cursor;
    while (end < *length && !isspace((unsigned char)buffer[end]))
        end++;

    bool first = true;
    for (size_t i = 0U; i < start; ++i)
    {
        if (!isspace((unsigned char)buffer[i]))
        {
            first = false;
            break;
        }
    }
    if (first)
        complete_command(e, buffer, capacity, length, cursor, start, end);
    else
        complete_path(session, e, buffer, capacity, length, cursor, start, end);
}

static int read_escape_sequence(edit_t *e,
                                uint8_t *final_out,
                                unsigned *param_out,
                                bool *has_param_out)
{
    uint8_t second = 0U;
    if (edit_read(e, &second) != 0)
        return -EIO;
    if (second != '[' && second != 'O')
        return 1;

    if (second == 'O')
    {
        uint8_t final = 0U;
        if (edit_read(e, &final) != 0)
            return -EIO;
        *final_out = final;
        *param_out = 0U;
        *has_param_out = false;
        return 0;
    }

    unsigned param = 0U;
    bool has_param = false;
    for (;;)
    {
        uint8_t ch = 0U;
        if (edit_read(e, &ch) != 0)
            return -EIO;
        if (ch >= '0' && ch <= '9')
        {
            has_param = true;
            param = param * 10U + (unsigned)(ch - '0');
            continue;
        }
        if (ch == ';')
        {
            /* Ignore modifier parameters for now (e.g. Ctrl+Arrow). */
            continue;
        }
        *final_out = ch;
        *param_out = param;
        *has_param_out = has_param;
        return 0;
    }
}

/* Bytes of a UTF-8 sequence that starts with `lead` (1 for anything else). */
static size_t utf8_sequence_length(uint8_t lead)
{
    if (lead >= 0xF0U && lead <= 0xF4U)
        return 4U;
    if (lead >= 0xE0U)
        return lead <= 0xEFU ? 3U : 1U;
    if (lead >= 0xC2U)
        return 2U;
    return 1U;
}

int tdsh_terminal_readline(tdsh_session_t *session,
                           const tdsh_terminal_io_t *io,
                           const char *prompt,
                           char *buffer,
                           size_t capacity)
{
    if (!session || !io || !io->read_byte || !io->write_bytes || !buffer || capacity < 2U)
    {
        return -EINVAL;
    }

    edit_t *e = &(edit_t){0};
    e->io = io;
    e->prompt = prompt ? prompt : "";
    e->prompt_cols = text_columns(e->prompt, strlen(e->prompt));
    e->cols = terminal_columns(e);
    if (e->cols == 0U)
        e->cols = TERM_DEFAULT_COLUMNS;

    size_t length = 0U;
    size_t cursor = 0U;
    size_t history_back = 0U;
    char pre_history[TDSH_MAX_LINE + 1U] = {0};
    buffer[0] = '\0';
    (void)io_puts(io, e->prompt);
    text_printed_to(e, e->prompt_cols);

    for (;;)
    {
        uint8_t ch = 0U;
        if (edit_read(e, &ch) != 0)
            return -EIO;

        if (ch == '\r' || ch == '\n')
        {
            buffer[length] = '\0';
            leave_line(e);
            if (length)
                history_append(session, buffer);
            return (int)length;
        }
        if (ch == 0x03U)
        { /* Ctrl+C */
            move_cursor(e, buffer, length);
            (void)io_puts(io, "^C");
            e->end_wrapped = false; /* ^C is on the last row */
            leave_line(e);
            buffer[0] = '\0';
            return 0;
        }
        if (ch == 0x01U)
        { /* Ctrl+A */
            cursor = 0U;
            move_cursor(e, buffer, cursor);
            continue;
        }
        if (ch == 0x05U)
        { /* Ctrl+E */
            cursor = length;
            move_cursor(e, buffer, cursor);
            continue;
        }
        if (ch == 0x15U)
        { /* Ctrl+U */
            memmove(buffer, buffer + cursor, length - cursor + 1U);
            length -= cursor;
            cursor = 0U;
            history_back = 0U;
            refresh(e, buffer, length, cursor);
            continue;
        }
        if (ch == 0x0BU)
        { /* Ctrl+K */
            buffer[cursor] = '\0';
            length = cursor;
            history_back = 0U;
            refresh(e, buffer, length, cursor);
            continue;
        }
        if (ch == 0x0CU)
        { /* Ctrl+L */
            (void)io_puts(io, "\033[2J\033[H");
            e->rows = 1U;
            e->cursor_row = 0U;
            e->end_wrapped = false;
            refresh(e, buffer, length, cursor);
            continue;
        }
        if (ch == 0x08U || ch == 0x7FU)
        {
            if (cursor == 0U)
            {
                bell(io);
                continue;
            }
            size_t from = prev_char(buffer, cursor);
            memmove(buffer + from, buffer + cursor, length - cursor + 1U);
            length -= cursor - from;
            cursor = from;
            history_back = 0U;
            refresh(e, buffer, length, cursor);
            continue;
        }
        if (ch == '\t')
        {
            complete(session, e, buffer, capacity, &length, &cursor);
            continue;
        }
        if (ch == 0x1BU)
        {
            uint8_t final = 0U;
            unsigned param = 0U;
            bool has_param = false;
            int esc_rc = read_escape_sequence(e, &final, &param, &has_param);
            if (esc_rc != 0)
                continue;

            if (final == 'A')
            { /* Up */
                if (history_back == 0U)
                    snprintf(pre_history, sizeof(pre_history), "%s", buffer);
                char history[TDSH_MAX_LINE + 1U];
                if (history_get_from_end(session, history_back + 1U, history, sizeof(history)) == 0)
                {
                    history_back++;
                    snprintf(buffer, capacity, "%s", history);
                    length = cursor = strlen(buffer);
                    refresh(e, buffer, length, cursor);
                }
                else
                    bell(io);
            }
            else if (final == 'B')
            { /* Down */
                if (history_back > 1U)
                {
                    history_back--;
                    char history[TDSH_MAX_LINE + 1U];
                    if (history_get_from_end(session, history_back, history, sizeof(history)) == 0)
                    {
                        snprintf(buffer, capacity, "%s", history);
                        length = cursor = strlen(buffer);
                        refresh(e, buffer, length, cursor);
                    }
                }
                else if (history_back == 1U)
                {
                    history_back = 0U;
                    snprintf(buffer, capacity, "%s", pre_history);
                    length = cursor = strlen(buffer);
                    refresh(e, buffer, length, cursor);
                }
                else
                    bell(io);
            }
            else if (final == 'C')
            { /* Right */
                if (cursor < length)
                {
                    cursor = next_char(buffer, length, cursor);
                    move_cursor(e, buffer, cursor);
                }
                else
                    bell(io);
            }
            else if (final == 'D')
            { /* Left */
                if (cursor > 0U)
                {
                    cursor = prev_char(buffer, cursor);
                    move_cursor(e, buffer, cursor);
                }
                else
                    bell(io);
            }
            else if (final == 'H' || (final == '~' && has_param && (param == 1U || param == 7U)))
            {
                cursor = 0U;
                move_cursor(e, buffer, cursor);
            }
            else if (final == 'F' || (final == '~' && has_param && (param == 4U || param == 8U)))
            {
                cursor = length;
                move_cursor(e, buffer, cursor);
            }
            else if (final == '~' && has_param && param == 3U)
            { /* Delete */
                if (cursor < length)
                {
                    size_t to = next_char(buffer, length, cursor);
                    memmove(buffer + cursor, buffer + to, length - to + 1U);
                    length -= to - cursor;
                    history_back = 0U;
                    refresh(e, buffer, length, cursor);
                }
                else
                    bell(io);
            }
            continue;
        }

        if (ch < 0x20U)
            continue;

        /* A whole UTF-8 sequence goes in at once, so the cursor never
         * stops inside a character. A byte that cannot continue it is
         * handled next as a key of its own. */
        uint8_t seq[4] = {ch};
        size_t seq_len = 1U;
        size_t want = utf8_sequence_length(ch);
        while (seq_len < want)
        {
            uint8_t next = 0U;
            if (edit_read(e, &next) != 0)
                return -EIO;
            if (!utf8_continuation(next))
            {
                if (e->pushback_pos > 0U)
                    e->pushback[--e->pushback_pos] = next;
                else
                    (void)pushback_add(e, &next, 1U);
                break;
            }
            seq[seq_len++] = next;
        }

        if (length + seq_len >= capacity)
        {
            bell(io);
            continue;
        }

        if (cursor == length)
        {
            memcpy(buffer + length, seq, seq_len);
            length += seq_len;
            buffer[length] = '\0';
            cursor = length;
            history_back = 0U;
            (void)io_write(io, seq, seq_len);
            text_printed_to(e, e->prompt_cols + text_columns(buffer, length));
        }
        else
        {
            memmove(buffer + cursor + seq_len, buffer + cursor, length - cursor + 1U);
            memcpy(buffer + cursor, seq, seq_len);
            cursor += seq_len;
            length += seq_len;
            history_back = 0U;
            refresh(e, buffer, length, cursor);
        }
    }
}
