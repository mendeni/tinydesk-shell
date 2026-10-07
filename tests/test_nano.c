/*
 * test_nano.c - nano with UTF-8 text: the cursor moves, deletes and counts
 * columns by character, never splitting a multibyte character.
 *
 * nano reads keys from stdin and draws on stdout; both are files here. The
 * results are checked in the saved file and in what nano drew.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "tdsh.h"

int tdsh_posix_nano_impl(tdsh_session_t *session, int argc, char **argv);

static int failures;

#define CHECK(cond, what)                                         \
    do                                                            \
    {                                                             \
        if (!(cond))                                              \
        {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); \
            failures++;                                           \
        }                                                         \
    } while (0)

#define ROOT    "nano_test_root"
#define SIZE    "\033[24;80R" /* the terminal's answer to nano's size query */
#define LEFT    "\033[D"
#define RIGHT   "\033[C"
#define UP      "\033[A"
#define DOWN    "\033[B"
#define DEL     "\033[3~"
#define BKSP    "\x7f"
#define SAVE    "\x0f"
#define EXIT    "\x18"
#define CUR_POS "\x03"

static tdsh_session_t s_session;
static char s_screen[65536];

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f)
    {
        fputs(text, f);
        fclose(f);
    }
}

static const char *read_file(const char *path)
{
    static char buf[4096];
    buf[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f)
        return buf;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Run nano on `name` (in root's home) with these keys; returns its screen output. */
static const char *run_nano(const char *name, const char *keys)
{
    write_file("nano_test_keys.bin", keys);
    FILE *in = fopen("nano_test_keys.bin", "rb");
    FILE *out = fopen("nano_test_screen.txt", "w+b");
    s_screen[0] = '\0';
    if (!in || !out)
        return s_screen;

    FILE *old_in = stdin;
    FILE *old_out = stdout;
    stdin = in;
    stdout = out;
    char *argv[] = {"nano", (char *)name, NULL};
    tdsh_posix_nano_impl(&s_session, 2, argv);
    fflush(stdout);
    stdin = old_in;
    stdout = old_out;

    rewind(out);
    size_t n = fread(s_screen, 1, sizeof(s_screen) - 1, out);
    s_screen[n] = '\0';
    fclose(in);
    fclose(out);
    remove("nano_test_keys.bin");
    remove("nano_test_screen.txt");
    return s_screen;
}

/* Every multibyte sequence complete: nano never drew half a character. */
static int utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p)
    {
        int n = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1
                            : (*p & 0xF0) == 0xE0   ? 2
                            : (*p & 0xF8) == 0xF0   ? 3
                                                    : -1;
        if (n < 0)
            return 0;
        p++;
        while (n-- > 0)
        {
            if ((*p & 0xC0) != 0x80)
                return 0;
            p++;
        }
    }
    return 1;
}

int main(void)
{
    tdsh_core_config_t cfg = TDSH_CORE_CONFIG_DEFAULT();
    cfg.fs_root = ROOT;
    CHECK(tdsh_core_init(&cfg) == 0, "core starts");
    mkdir(ROOT, 0755);
    mkdir(ROOT "/root", 0755);
    remove(ROOT "/root/cyr.txt"); /* new files, also on a second run */
    remove(ROOT "/root/col.txt");
    CHECK(tdsh_session_init(&s_session, "root", true) == 0, "root session");

    /* Typing Cyrillic, then Left, Backspace and Delete by character:
     * "Привет", two Left (before "е"), Backspace (removes "в"), "X",
     * Delete (removes "е"). */
    const char *screen = run_nano("cyr.txt", SIZE "Привет" LEFT LEFT BKSP "X" DEL SAVE EXIT);
    CHECK(strcmp(read_file(ROOT "/root/cyr.txt"), "ПриXт") == 0, "Left, Backspace, Delete by character");
    CHECK(utf8_valid(screen), "only whole characters drawn while typing and deleting");
    CHECK(strstr(screen, "ПриXт") != NULL, "the line drawn whole");

    /* Up and Down keep the character column. */
    write_file(ROOT "/root/cols.txt", "日本語\nabcdef");
    screen = run_nano("cols.txt", SIZE RIGHT RIGHT DOWN "Z" UP "!" SAVE EXIT);
    CHECK(strcmp(read_file(ROOT "/root/cols.txt"), "日本語!\nabZcdef") == 0,
          "Down after two characters lands after two characters; Up to the shorter line's end");
    CHECK(utf8_valid(screen), "valid UTF-8 on screen");

    /* The column in Ctrl+C and the status line counts characters. */
    screen = run_nano("col.txt", SIZE "éé" CUR_POS SAVE EXIT);
    CHECK(strstr(screen, "line 1/1, column 3") != NULL, "Ctrl+C: column 3 after two characters");
    screen = run_nano("col.txt", SIZE RIGHT RIGHT SAVE EXIT);
    CHECK(strstr(screen, "Col 3") != NULL, "status line: Col 3 after two characters");

    /* An invalid byte is one character, drawn as '?', and deleted whole. */
    write_file(ROOT "/root/bad.txt", "a\xff"
                                     "b");
    screen = run_nano("bad.txt", SIZE RIGHT DEL SAVE EXIT);
    CHECK(strcmp(read_file(ROOT "/root/bad.txt"), "ab") == 0, "Delete removes the invalid byte");
    CHECK(utf8_valid(screen), "an invalid byte is drawn as '?'");

    /* A long line scrolls by character and is never cut inside one. */
    char long_line[1024] = "";
    for (int i = 0; i < 120; ++i)
        strcat(long_line, i % 2 ? "щ" : "ж");
    write_file(ROOT "/root/long.txt", long_line);
    screen = run_nano("long.txt", SIZE "\033[F"
                                       "!" SAVE EXIT);
    strcat(long_line, "!");
    CHECK(strcmp(read_file(ROOT "/root/long.txt"), long_line) == 0, "End goes past 120 characters");
    CHECK(utf8_valid(screen), "a scrolled line is drawn in whole characters");

    /* Search for a UTF-8 word, then edit there. */
    write_file(ROOT "/root/find.txt", "один два три");
    screen = run_nano("find.txt", SIZE "\x17"
                                       "два\r" DEL SAVE EXIT);
    CHECK(strcmp(read_file(ROOT "/root/find.txt"), "один ва три") == 0, "search finds a UTF-8 word");

    if (failures)
        printf("%d failure(s)\n", failures);
    else
        printf("nano tests passed\n");
    return failures ? 1 : 0;
}
