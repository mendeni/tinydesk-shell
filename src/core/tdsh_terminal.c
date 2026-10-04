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

static void redraw(const tdsh_terminal_io_t *io,
                   const char *prompt,
                   const char *buffer,
                   size_t length,
                   size_t cursor)
{
    (void)io_puts(io, "\r\033[2K");
    (void)io_puts(io, prompt ? prompt : "");
    (void)io_write(io, buffer, length);
    if (length > cursor)
        (void)io_printf(io, "\033[%uD", (unsigned)(length - cursor));
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

static void complete_command(const tdsh_terminal_io_t *io,
                             const char *prompt,
                             char *buffer,
                             size_t capacity,
                             size_t *length,
                             size_t *cursor,
                             size_t start,
                             size_t end)
{
    if (*cursor != end)
    {
        bell(io);
        return;
    }

    char prefix[TDSH_MAX_LINE + 1U];
    size_t prefix_len = end - start;
    if (prefix_len >= sizeof(prefix))
    {
        bell(io);
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
        bell(io);
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
        bell(io);
        return;
    }

    if (count > 1U && common == prefix_len)
    {
        (void)io_puts(io, "\r\n");
        for (size_t i = 0U; i < count; ++i)
        {
            (void)io_printf(io, "%-18s", matches[i]);
            if ((i + 1U) % 4U == 0U || i + 1U == count)
                (void)io_puts(io, "\r\n");
        }
    }
    redraw(io, prompt, buffer, *length, *cursor);
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
                          const tdsh_terminal_io_t *io,
                          const char *prompt,
                          char *buffer,
                          size_t capacity,
                          size_t *length,
                          size_t *cursor,
                          size_t start,
                          size_t end)
{
    if (*cursor != end)
    {
        bell(io);
        return;
    }

    char token[TDSH_MAX_PATH];
    size_t token_len = end - start;
    if (token_len >= sizeof(token))
    {
        bell(io);
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
            bell(io);
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
        bell(io);
        return;
    }

    DIR *directory = opendir(real);
    if (!directory)
    {
        bell(io);
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
        if (count == 0U)
        {
            snprintf(first_name, sizeof(first_name), "%s", entry->d_name);
            common = strlen(first_name);
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
        bell(io);
        return;
    }

    char replacement[TDSH_MAX_PATH];
    int n = snprintf(replacement, sizeof(replacement), "%s%.*s",
                     typed_prefix, (int)common, first_name);
    if (n < 0 || (size_t)n >= sizeof(replacement))
    {
        bell(io);
        return;
    }

    if (count == 1U && (first_is_dir || end == *length))
    {
        size_t replacement_len = strlen(replacement);
        if (replacement_len + 1U >= sizeof(replacement))
        {
            bell(io);
            return;
        }
        replacement[replacement_len] = first_is_dir ? '/' : ' ';
        replacement[replacement_len + 1U] = '\0';
    }

    if (replace_token(buffer, capacity, length, cursor, start, end, replacement) != 0)
    {
        bell(io);
        return;
    }

    if (count > 1U && common == base_len)
    {
        directory = opendir(real);
        if (directory)
        {
            size_t shown = 0U;
            (void)io_puts(io, "\r\n");
            while ((entry = readdir(directory)) != NULL && shown < TERM_PATH_DISPLAY_MAX)
            {
                if (!path_entry_matches(entry, base, base_len))
                    continue;
                bool is_dir = path_entry_is_dir(real, entry->d_name);
                (void)io_printf(io, "%-18s%s", entry->d_name, is_dir ? "/" : "");
                shown++;
                if (shown % 4U == 0U || shown == count || shown == TERM_PATH_DISPLAY_MAX)
                {
                    (void)io_puts(io, "\r\n");
                }
            }
            if (count > TERM_PATH_DISPLAY_MAX)
            {
                (void)io_printf(io, "... %u more\r\n", (unsigned)(count - TERM_PATH_DISPLAY_MAX));
            }
            closedir(directory);
        }
    }

    redraw(io, prompt, buffer, *length, *cursor);
}

static void complete(tdsh_session_t *session,
                     const tdsh_terminal_io_t *io,
                     const char *prompt,
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
        complete_command(io, prompt, buffer, capacity, length, cursor, start, end);
    else
        complete_path(session, io, prompt, buffer, capacity, length, cursor, start, end);
}

static int read_byte(const tdsh_terminal_io_t *io, uint8_t *out)
{
    if (!io || !io->read_byte || !out)
        return -EINVAL;
    return io->read_byte(io->context, out);
}

static int read_escape_sequence(const tdsh_terminal_io_t *io,
                                uint8_t *final_out,
                                unsigned *param_out,
                                bool *has_param_out)
{
    uint8_t second = 0U;
    if (read_byte(io, &second) != 0)
        return -EIO;
    if (second != '[' && second != 'O')
        return 1;

    if (second == 'O')
    {
        uint8_t final = 0U;
        if (read_byte(io, &final) != 0)
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
        if (read_byte(io, &ch) != 0)
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

    size_t length = 0U;
    size_t cursor = 0U;
    size_t history_back = 0U;
    char pre_history[TDSH_MAX_LINE + 1U] = {0};
    buffer[0] = '\0';
    (void)io_puts(io, prompt ? prompt : "");

    for (;;)
    {
        uint8_t ch = 0U;
        if (read_byte(io, &ch) != 0)
            return -EIO;

        if (ch == '\r' || ch == '\n')
        {
            buffer[length] = '\0';
            (void)io_puts(io, "\r\n");
            if (length)
                history_append(session, buffer);
            return (int)length;
        }
        if (ch == 0x03U)
        { /* Ctrl+C */
            buffer[0] = '\0';
            (void)io_puts(io, "^C\r\n");
            return 0;
        }
        if (ch == 0x01U)
        { /* Ctrl+A */
            cursor = 0U;
            redraw(io, prompt, buffer, length, cursor);
            continue;
        }
        if (ch == 0x05U)
        { /* Ctrl+E */
            cursor = length;
            redraw(io, prompt, buffer, length, cursor);
            continue;
        }
        if (ch == 0x15U)
        { /* Ctrl+U */
            memmove(buffer, buffer + cursor, length - cursor + 1U);
            length -= cursor;
            cursor = 0U;
            history_back = 0U;
            redraw(io, prompt, buffer, length, cursor);
            continue;
        }
        if (ch == 0x0BU)
        { /* Ctrl+K */
            buffer[cursor] = '\0';
            length = cursor;
            history_back = 0U;
            redraw(io, prompt, buffer, length, cursor);
            continue;
        }
        if (ch == 0x0CU)
        { /* Ctrl+L */
            (void)io_puts(io, "\033[2J\033[H");
            redraw(io, prompt, buffer, length, cursor);
            continue;
        }
        if (ch == 0x08U || ch == 0x7FU)
        {
            if (cursor == 0U)
            {
                bell(io);
                continue;
            }
            memmove(buffer + cursor - 1U, buffer + cursor, length - cursor + 1U);
            cursor--;
            length--;
            history_back = 0U;
            redraw(io, prompt, buffer, length, cursor);
            continue;
        }
        if (ch == '\t')
        {
            complete(session, io, prompt, buffer, capacity, &length, &cursor);
            continue;
        }
        if (ch == 0x1BU)
        {
            uint8_t final = 0U;
            unsigned param = 0U;
            bool has_param = false;
            int esc_rc = read_escape_sequence(io, &final, &param, &has_param);
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
                    redraw(io, prompt, buffer, length, cursor);
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
                        redraw(io, prompt, buffer, length, cursor);
                    }
                }
                else if (history_back == 1U)
                {
                    history_back = 0U;
                    snprintf(buffer, capacity, "%s", pre_history);
                    length = cursor = strlen(buffer);
                    redraw(io, prompt, buffer, length, cursor);
                }
                else
                    bell(io);
            }
            else if (final == 'C')
            { /* Right */
                if (cursor < length)
                {
                    cursor++;
                    (void)io_puts(io, "\033[C");
                }
                else
                    bell(io);
            }
            else if (final == 'D')
            { /* Left */
                if (cursor > 0U)
                {
                    cursor--;
                    (void)io_puts(io, "\033[D");
                }
                else
                    bell(io);
            }
            else if (final == 'H' || (final == '~' && has_param && (param == 1U || param == 7U)))
            {
                cursor = 0U;
                redraw(io, prompt, buffer, length, cursor);
            }
            else if (final == 'F' || (final == '~' && has_param && (param == 4U || param == 8U)))
            {
                cursor = length;
                redraw(io, prompt, buffer, length, cursor);
            }
            else if (final == '~' && has_param && param == 3U)
            { /* Delete */
                if (cursor < length)
                {
                    memmove(buffer + cursor, buffer + cursor + 1U, length - cursor);
                    length--;
                    history_back = 0U;
                    redraw(io, prompt, buffer, length, cursor);
                }
                else
                    bell(io);
            }
            continue;
        }

        if (ch < 0x20U)
            continue;
        if (length + 1U >= capacity)
        {
            bell(io);
            continue;
        }

        if (cursor == length)
        {
            buffer[length++] = (char)ch;
            buffer[length] = '\0';
            cursor = length;
            history_back = 0U;
            (void)io_write(io, &ch, 1U);
        }
        else
        {
            memmove(buffer + cursor + 1U, buffer + cursor, length - cursor + 1U);
            buffer[cursor] = (char)ch;
            cursor++;
            length++;
            history_back = 0U;
            redraw(io, prompt, buffer, length, cursor);
        }
    }
}
