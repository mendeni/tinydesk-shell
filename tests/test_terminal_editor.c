#define _GNU_SOURCE
#include "tdsh.h"
#include "tdsh_posix.h"
#include "tdsh_terminal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const unsigned char *input;
    size_t input_len;
    size_t input_pos;
    char output[32768];
    size_t output_len;
} mem_terminal_t;

static int mem_read(void *context, uint8_t *byte_out)
{
    mem_terminal_t *t = context;
    if (!t || !byte_out || t->input_pos >= t->input_len) return -1;
    *byte_out = t->input[t->input_pos++];
    return 0;
}

static int mem_write(void *context, const void *data, size_t length)
{
    mem_terminal_t *t = context;
    if (!t || (!data && length)) return -1;
    if (t->output_len + length >= sizeof(t->output)) return -1;
    memcpy(t->output + t->output_len, data, length);
    t->output_len += length;
    t->output[t->output_len] = '\0';
    return 0;
}

static int run_line(tdsh_session_t *session,
                    const unsigned char *input,
                    size_t input_len,
                    char *line,
                    size_t line_size)
{
    mem_terminal_t terminal = {
        .input = input,
        .input_len = input_len,
    };
    tdsh_terminal_io_t io = {
        .context = &terminal,
        .read_byte = mem_read,
        .write_bytes = mem_write,
    };
    return tdsh_terminal_readline(session, &io, "> ", line, line_size);
}

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } \
} while (0)

int main(void)
{
    char root_template[] = "/tmp/tdsh-terminal-XXXXXX";
    char *root = mkdtemp(root_template);
    CHECK(root != NULL, "mkdtemp");

    tdsh_posix_config_t cfg = TDSH_POSIX_CONFIG_DEFAULT();
    cfg.hostname = "termtest";
    cfg.default_user = "alice";
    cfg.fs_root = root;
    cfg.register_posix_commands = true;
    CHECK(tdsh_posix_init(&cfg) == 0, "posix init");

    tdsh_session_t session;
    CHECK(tdsh_session_init(&session, "alice", true) == 0, "session init");
    session.terminal_caps = TDSH_TERM_CAP_ANSI | TDSH_TERM_CAP_COLOR;

    char line[TDSH_MAX_LINE + 2U];

    static const unsigned char command_tab[] = {'v','e','r','\t','\n'};
    CHECK(run_line(&session, command_tab, sizeof(command_tab), line, sizeof(line)) >= 0,
          "command completion readline");
    CHECK(strcmp(line, "version ") == 0, "TAB command completion");

    static const unsigned char cursor_insert[] = {
        'e','c','h','o',' ','a','c', 0x1b,'[','D', 'b','\n'
    };
    CHECK(run_line(&session, cursor_insert, sizeof(cursor_insert), line, sizeof(line)) >= 0,
          "cursor insert readline");
    CHECK(strcmp(line, "echo abc") == 0, "Left arrow + insertion");

    static const unsigned char delete_key[] = {
        'e','c','h','o',' ','a','b','x','c', 0x1b,'[','D',0x1b,'[','D',0x1b,'[','3','~','\n'
    };
    CHECK(run_line(&session, delete_key, sizeof(delete_key), line, sizeof(line)) >= 0,
          "delete readline");
    CHECK(strcmp(line, "echo abc") == 0, "Delete key");

    static const unsigned char home_end[] = {
        'b','c',0x1b,'[','H','a',0x1b,'[','F','d','\n'
    };
    CHECK(run_line(&session, home_end, sizeof(home_end), line, sizeof(line)) >= 0,
          "home/end readline");
    CHECK(strcmp(line, "abcd") == 0, "Home/End keys");

    static const unsigned char history_seed[] = {'p','w','d','\n'};
    CHECK(run_line(&session, history_seed, sizeof(history_seed), line, sizeof(line)) >= 0,
          "history seed");
    CHECK(strcmp(line, "pwd") == 0, "history seed contents");

    static const unsigned char history_up[] = {0x1b,'[','A','\n'};
    CHECK(run_line(&session, history_up, sizeof(history_up), line, sizeof(line)) >= 0,
          "history up readline");
    CHECK(strcmp(line, "pwd") == 0, "Up arrow history recall");

    char real_file[TDSH_MAX_REAL_PATH];
    CHECK(tdsh_path_to_real(&session, "~/alpha.txt", real_file, sizeof(real_file), NULL, 0) == 0,
          "resolve alpha.txt");
    FILE *fp = fopen(real_file, "w");
    CHECK(fp != NULL, "create alpha.txt");
    fputs("x\n", fp);
    fclose(fp);

    tdsh_memory_stats_t before, after;
    tdsh_memory_get_stats(&before);
    static const unsigned char path_tab[] = {'c','a','t',' ','a','l','\t','\n'};
    CHECK(run_line(&session, path_tab, sizeof(path_tab), line, sizeof(line)) >= 0,
          "path completion readline");
    CHECK(strcmp(line, "cat alpha.txt ") == 0, "TAB path completion");
    tdsh_memory_get_stats(&after);
    CHECK(after.live_blocks == before.live_blocks && after.live_bytes == before.live_bytes,
          "path completion allocation cleanup");

    char history_real[TDSH_MAX_REAL_PATH];
    CHECK(tdsh_path_to_real(&session, "~/.tdsh_history", history_real, sizeof(history_real), NULL, 0) == 0,
          "history path resolve");
    CHECK(access(history_real, F_OK) == 0, "history file stays in virtual home");

    puts("PASS: portable terminal editor TAB/history/arrows/Home/End/Delete + allocation cleanup");
    return 0;
}
