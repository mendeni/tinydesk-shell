/*
 * test_terminal_wrap.c - the line editor on a narrow terminal: lines that
 * wrap over several rows, editing across the wrap, the right margin, UTF-8,
 * a coloured prompt, and asking the terminal for its width.
 *
 * The editor's output goes into a small VT emulator (deferred wrap, CR, LF,
 * CSI A/B/C/D/J/K/H, ESC[6n), and the tests check the screen it shows.
 */
#include "tdsh.h"
#include "tdsh_terminal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define W 20
#define H 12

/* ------------------------------------------------------------ screen */

typedef struct
{
    uint32_t cell[H][W];
    int row, col;
    bool pending_wrap;
    /* parser */
    int esc; /* 0 text, 1 after ESC, 2 in CSI */
    char csi[32];
    size_t csi_len;
    uint32_t cp;
    int cp_left;
    /* input */
    const char *keys;
    size_t keys_len, keys_pos;
    char reply[32];
    size_t reply_len, reply_pos;
    /* behaviour */
    bool give_columns;     /* io.columns reports W */
    bool answers;          /* answers ESC[6n */
    bool keys_while_asked; /* keys arrive before the answer */
    bool type_ahead;       /* keys are already waiting when the line starts */
    int queries;
} term_t;

static void clear_cells(term_t *t, int r0, int c0, int r1, int c1)
{
    for (int r = r0; r <= r1; r++)
        for (int c = (r == r0 ? c0 : 0); c <= (r == r1 ? c1 : W - 1); c++)
            t->cell[r][c] = ' ';
}

static void line_feed(term_t *t)
{
    if (t->row < H - 1)
    {
        t->row++;
        return;
    }
    memmove(t->cell[0], t->cell[1], sizeof(t->cell[0]) * (H - 1));
    clear_cells(t, H - 1, 0, H - 1, W - 1);
}

static void put_char(term_t *t, uint32_t cp)
{
    if (t->pending_wrap)
    {
        t->col = 0;
        line_feed(t);
        t->pending_wrap = false;
    }
    t->cell[t->row][t->col] = cp;
    if (t->col == W - 1)
        t->pending_wrap = true;
    else
        t->col++;
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi
                                : v;
}

static void do_csi(term_t *t, char final)
{
    int p[4] = {0, 0, 0, 0}, np = 0;
    t->csi[t->csi_len] = '\0';
    for (const char *s = t->csi; *s && np < 4; s++)
    {
        if (*s >= '0' && *s <= '9')
            p[np] = p[np] * 10 + (*s - '0');
        else if (*s == ';')
            np++;
    }
    int n = p[0] ? p[0] : 1;
    t->pending_wrap = false;
    switch (final)
    {
    case 'A':
        t->row = clamp(t->row - n, 0, H - 1);
        break;
    case 'B':
        t->row = clamp(t->row + n, 0, H - 1);
        break;
    case 'C':
        t->col = clamp(t->col + n, 0, W - 1);
        break;
    case 'D':
        t->col = clamp(t->col - n, 0, W - 1);
        break;
    case 'H':
        t->row = clamp((p[0] ? p[0] : 1) - 1, 0, H - 1);
        t->col = clamp((p[1] ? p[1] : 1) - 1, 0, W - 1);
        break;
    case 'J':
        if (p[0] == 2)
            clear_cells(t, 0, 0, H - 1, W - 1);
        else
            clear_cells(t, t->row, t->col, H - 1, W - 1);
        break;
    case 'K':
        if (p[0] == 2)
            clear_cells(t, t->row, 0, t->row, W - 1);
        else
            clear_cells(t, t->row, t->col, t->row, W - 1);
        break;
    case 'n':
        if (p[0] == 6)
        {
            t->queries++;
            if (t->answers)
            {
                t->reply_len = (size_t)snprintf(t->reply, sizeof(t->reply), "\033[%d;%dR", t->row + 1, t->col + 1);
                t->reply_pos = 0;
            }
        }
        break;
    default:
        break; /* m and others: no effect on the cells */
    }
}

static void feed(term_t *t, uint8_t b)
{
    if (t->esc == 1)
    {
        t->esc = b == '[' ? 2 : 0;
        t->csi_len = 0;
        return;
    }
    if (t->esc == 2)
    {
        if (b >= 0x40 && b <= 0x7E)
        {
            t->esc = 0;
            do_csi(t, (char)b);
        }
        else if (t->csi_len + 1 < sizeof(t->csi))
            t->csi[t->csi_len++] = (char)b;
        return;
    }
    if (t->cp_left > 0)
    {
        t->cp = (t->cp << 6) | (b & 0x3F);
        if (--t->cp_left == 0)
            put_char(t, t->cp);
        return;
    }
    if (b == 0x1B)
        t->esc = 1;
    else if (b == '\r')
    {
        t->col = 0;
        t->pending_wrap = false;
    }
    else if (b == '\n')
    {
        line_feed(t);
        t->pending_wrap = false;
    }
    else if (b == '\a')
        ;
    else if (b >= 0xF0)
        t->cp = b & 0x07, t->cp_left = 3;
    else if (b >= 0xE0)
        t->cp = b & 0x0F, t->cp_left = 2;
    else if (b >= 0xC0)
        t->cp = b & 0x1F, t->cp_left = 1;
    else if (b >= 0x20)
        put_char(t, b);
}

/* ------------------------------------------------------------ io */

static int t_write(void *ctx, const void *data, size_t len)
{
    term_t *t = ctx;
    for (size_t i = 0; i < len; i++)
        feed(t, ((const uint8_t *)data)[i]);
    return 0;
}

static int t_read(void *ctx, uint8_t *out)
{
    term_t *t = ctx;
    if (t->reply_pos < t->reply_len)
    {
        *out = (uint8_t)t->reply[t->reply_pos++];
        return 0;
    }
    if (t->keys_pos >= t->keys_len)
        return -EIO; /* end of the keys: readline returns with the line */
    *out = (uint8_t)t->keys[t->keys_pos++];
    return 0;
}

static int t_read_timeout(void *ctx, uint8_t *out, unsigned timeout_ms)
{
    term_t *t = ctx;
    if (t->type_ahead && t->keys_pos < t->keys_len)
    {
        *out = (uint8_t)t->keys[t->keys_pos++];
        return 0;
    }
    /* keys_while_asked: nothing is waiting when the editor checks (timeout
     * 0), then three keys arrive before the answer. */
    if (t->keys_while_asked && timeout_ms > 0 && t->keys_pos < t->keys_len && t->keys_pos < 3)
    {
        *out = (uint8_t)t->keys[t->keys_pos++];
        return 0;
    }
    if (t->reply_pos < t->reply_len)
    {
        *out = (uint8_t)t->reply[t->reply_pos++];
        return 0;
    }
    return -ETIMEDOUT;
}

static int t_columns(void *ctx)
{
    term_t *t = ctx;
    return t->give_columns ? W : 0;
}

/* ------------------------------------------------------------ checks */

static int failures;

#define CHECK(cond, ...)                                         \
    do                                                           \
    {                                                            \
        if (!(cond))                                             \
        {                                                        \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fputc('\n', stderr);                                 \
            failures++;                                          \
        }                                                        \
    } while (0)

static void term_init(term_t *t)
{
    memset(t, 0, sizeof(*t));
    clear_cells(t, 0, 0, H - 1, W - 1);
    t->give_columns = true;
}

static char s_line[TDSH_MAX_LINE + 2U];

static int edit(term_t *t, const char *prompt, const char *keys, size_t keys_len)
{
    static tdsh_session_t session; /* history and completion are not used */
    t->keys = keys;
    t->keys_len = keys_len;
    t->keys_pos = 0;
    tdsh_terminal_io_t io = {
        .context = t,
        .read_byte = t_read,
        .write_bytes = t_write,
        .columns = t_columns,
        .read_byte_timeout = t_read_timeout,
    };
    return tdsh_terminal_readline(&session, &io, prompt, s_line, sizeof(s_line));
}

/* Code points of a UTF-8 string, skipping escape sequences. */
static size_t to_cells(const char *s, uint32_t *out, size_t cap)
{
    size_t n = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (*p && n < cap)
    {
        if (*p == 0x1B)
        {
            p++;
            if (*p == '[')
            {
                p++;
                while (*p && (*p < 0x40 || *p > 0x7E))
                    p++;
                if (*p)
                    p++;
            }
            continue;
        }
        uint32_t cp = *p++;
        int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2
                                : cp >= 0xC0   ? 1
                                               : 0;
        if (more)
            cp &= 0x3F >> more;
        while (more-- > 0 && *p)
            cp = (cp << 6) | (*p++ & 0x3F);
        out[n++] = cp;
    }
    return n;
}

/* The screen from row `top` shows prompt + line wrapped at W, every other
 * row up to the bottom is empty, and the cursor is at cell `cursor_cell`. */
static void expect_screen(const term_t *t, int top, const char *prompt, const char *line, size_t cursor_cell)
{
    uint32_t text[512];
    size_t n = to_cells(prompt, text, 512);
    n += to_cells(line, text + n, 512 - n);
    for (int r = top; r < H; r++)
    {
        for (int c = 0; c < W; c++)
        {
            size_t i = (size_t)(r - top) * W + (size_t)c;
            uint32_t want = i < n ? text[i] : ' ';
            if (t->cell[r][c] != want)
            {
                CHECK(false, "row %d col %d is U+%04X, expected U+%04X", r, c, (unsigned)t->cell[r][c], (unsigned)want);
                return;
            }
        }
    }
    int want_row = top + (int)(cursor_cell / W), want_col = (int)(cursor_cell % W);
    CHECK(t->row == want_row && t->col == want_col, "cursor at %d,%d, expected %d,%d", t->row, t->col, want_row, want_col);
}

#define KEYS(s) s, sizeof(s) - 1

/* ------------------------------------------------------------ tests */

static const char *letters(size_t n)
{
    static char s[128];
    for (size_t i = 0; i < n; i++)
        s[i] = (char)('a' + i % 26);
    s[n] = '\0';
    return s;
}

static void test_long_line_wraps(void)
{
    term_t t;
    term_init(&t);
    edit(&t, "> ", letters(50), 50);
    CHECK(strcmp(s_line, letters(50)) == 0, "line \"%s\"", s_line);
    expect_screen(&t, 0, "> ", s_line, 52);
}

static void test_backspace_across_the_wrap(void)
{
    /* 50 letters, then 15 Backspaces: 37 cells left, the third row must be
     * empty again (the old editor left it on the screen). */
    char keys[128];
    memcpy(keys, letters(50), 50);
    memset(keys + 50, 0x7F, 15);
    term_t t;
    term_init(&t);
    edit(&t, "> ", keys, 65);
    CHECK(strcmp(s_line, letters(35)) == 0, "line \"%s\"", s_line);
    expect_screen(&t, 0, "> ", s_line, 37);
}

static void test_home_end_and_insert_across_rows(void)
{
    char keys[256];
    size_t k = 0;
    memcpy(keys, letters(50), 50);
    k = 50;
    memcpy(keys + k, "\033[H", 3), k += 3; /* Home: row 0, after the prompt */
    keys[k++] = 'X';
    memcpy(keys + k, "\033[F", 3), k += 3; /* End */
    for (int i = 0; i < 33; i++)           /* Left back over a row boundary */
        memcpy(keys + k, "\033[D", 3), k += 3;
    keys[k++] = 'Y';
    keys[k++] = 0x7F; /* Backspace the Y again: the cursor is at the start of row 1 ... */
    keys[k++] = 0x7F; /* ... and the letter before it, at the end of row 0 */
    term_t t;
    term_init(&t);
    edit(&t, "> ", keys, k);

    /* X + 50 letters; 33 Lefts from the end leave the cursor at index 18
     * (cell 20, the start of row 1); Y goes in and out again, then line[17]
     * goes and the cursor is at cell 19, the end of row 0. */
    char want[64];
    snprintf(want, sizeof(want), "X%s", letters(50));
    memmove(want + 17, want + 18, strlen(want + 18) + 1);
    CHECK(strcmp(s_line, want) == 0, "line \"%s\", expected \"%s\"", s_line, want);
    expect_screen(&t, 0, "> ", s_line, 2 + 17);
}

static void test_right_margin(void)
{
    /* 18 letters after "> " fill the first row exactly: the cursor goes to
     * the start of the second row, and the next letter appears there. */
    term_t t;
    term_init(&t);
    edit(&t, "> ", letters(18), 18);
    expect_screen(&t, 0, "> ", s_line, 20);

    term_init(&t);
    edit(&t, "> ", letters(19), 19);
    expect_screen(&t, 0, "> ", s_line, 21);

    /* Backspace from the second row back to the margin. */
    char keys[32];
    memcpy(keys, letters(19), 19);
    keys[19] = 0x7F;
    term_init(&t);
    edit(&t, "> ", keys, 20);
    expect_screen(&t, 0, "> ", s_line, 20);
}

static void test_utf8_counts_code_points(void)
{
    /* "café ☕ 中文" is 9 code points in 16 bytes; then 30 more letters. */
    static const char text[] = "caf\xc3\xa9 \xe2\x98\x95 \xe4\xb8\xad\xe6\x96\x87";
    char keys[256];
    size_t k = strlen(text);
    memcpy(keys, text, k);
    memcpy(keys + k, letters(30), 30), k += 30;
    /* Left 31 times: before the last code point of text; Backspace removes
     * 中 (3 bytes) as one character. */
    for (int i = 0; i < 31; i++)
        memcpy(keys + k, "\033[D", 3), k += 3;
    keys[k++] = 0x7F;
    term_t t;
    term_init(&t);
    edit(&t, "> ", keys, k);
    char want[128];
    snprintf(want, sizeof(want), "caf\xc3\xa9 \xe2\x98\x95 \xe6\x96\x87%s", letters(30));
    CHECK(strcmp(s_line, want) == 0, "line \"%s\"", s_line);
    expect_screen(&t, 0, "> ", s_line, 2 + 7);
}

static void test_coloured_prompt(void)
{
    static const char prompt[] = "\033[1;32muser\033[0m:\033[1;34m~\033[0m$ ";
    term_t t;
    term_init(&t);
    edit(&t, prompt, letters(30), 30);
    expect_screen(&t, 0, prompt, s_line, 8 + 30);
}

static void test_width_asked_from_the_terminal(void)
{
    term_t t;
    term_init(&t);
    t.give_columns = false;
    t.answers = true;
    edit(&t, "> ", letters(50), 50);
    CHECK(t.queries == 2, "%d queries", t.queries);
    CHECK(strcmp(s_line, letters(50)) == 0, "line \"%s\"", s_line);
    expect_screen(&t, 0, "> ", s_line, 52);
}

static void test_unanswered_width_query(void)
{
    /* No answer: 80 columns are assumed, nothing stray ends up in the
     * line, and the prompt is where it was asked from. */
    term_t t;
    term_init(&t);
    t.give_columns = false;
    t.answers = false;
    edit(&t, "> ", KEYS("hello"));
    CHECK(t.queries == 1, "%d queries", t.queries);
    CHECK(strcmp(s_line, "hello") == 0, "line \"%s\"", s_line);
    expect_screen(&t, 0, "> ", "hello", 7);
}

static void test_keys_typed_while_asking(void)
{
    /* Keys that arrive before the answer are typed, in order. */
    term_t t;
    term_init(&t);
    t.give_columns = false;
    t.answers = true;
    t.keys_while_asked = true;
    edit(&t, "> ", letters(30), 30);
    CHECK(t.queries == 2, "%d queries", t.queries);
    CHECK(strcmp(s_line, letters(30)) == 0, "line \"%s\"", s_line);
    expect_screen(&t, 0, "> ", s_line, 32);
}

static void test_type_ahead_skips_the_query(void)
{
    /* Keys already waiting (a paste): nothing is asked, the width learned
     * before (20, from the tests above) is used, and the keys are intact. */
    term_t t;
    term_init(&t);
    t.give_columns = false;
    t.answers = true;
    t.type_ahead = true;
    edit(&t, "> ", letters(30), 30);
    CHECK(t.queries == 0, "%d queries", t.queries);
    CHECK(strcmp(s_line, letters(30)) == 0, "line \"%s\"", s_line);
    expect_screen(&t, 0, "> ", s_line, 32);
}

static void test_no_timed_read_no_query(void)
{
    /* Without columns and a timed read the editor must not ask. */
    term_t t;
    term_init(&t);
    t.give_columns = false;
    t.answers = true;
    static tdsh_session_t session;
    tdsh_terminal_io_t io = {.context = &t, .read_byte = t_read, .write_bytes = t_write};
    t.keys = "abc";
    t.keys_len = 3;
    tdsh_terminal_readline(&session, &io, "> ", s_line, sizeof(s_line));
    CHECK(t.queries == 0, "%d queries", t.queries);
    CHECK(strcmp(s_line, "abc") == 0, "line \"%s\"", s_line);
}

static void test_ctrl_c_below_a_wrapped_line(void)
{
    /* Ctrl+C with the cursor on the first row: ^C goes after the text,
     * and the cursor to the next row. */
    char keys[64];
    memcpy(keys, letters(30), 30);
    memcpy(keys + 30, "\033[H\x03", 4);
    term_t t;
    term_init(&t);
    int rc = edit(&t, "> ", keys, 34);
    CHECK(rc == 0 && s_line[0] == '\0', "rc %d, line \"%s\"", rc, s_line);
    CHECK(t.cell[1][12] == '^' && t.cell[1][13] == 'C', "^C not after the text");
    CHECK(t.row == 2 && t.col == 0, "cursor at %d,%d", t.row, t.col);
}

int main(void)
{
    test_long_line_wraps();
    test_backspace_across_the_wrap();
    test_home_end_and_insert_across_rows();
    test_right_margin();
    test_utf8_counts_code_points();
    test_coloured_prompt();
    test_width_asked_from_the_terminal();
    test_unanswered_width_query();
    test_keys_typed_while_asking();
    test_type_ahead_skips_the_query();
    test_no_timed_read_no_query();
    test_ctrl_c_below_a_wrapped_line();
    if (failures)
    {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("PASS: line editor on a 20-column terminal: wrapping, editing across rows, margin, UTF-8, prompt colours, width query");
    return 0;
}
