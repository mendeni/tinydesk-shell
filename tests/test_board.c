/*
 * test_board.c - the board configuration parser and store (tdsh_board.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tdsh_board.h"

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

typedef struct
{
    int n;
    char keys[8][TDSH_BOARD_KEY_MAX];
    char values[8][TDSH_BOARD_VALUE_MAX];
} seen_t;

static void collect(const char *key, const char *value, int line, void *user)
{
    (void)line;
    seen_t *s = user;
    if (s->n < 8)
    {
        snprintf(s->keys[s->n], sizeof(s->keys[0]), "%s", key);
        snprintf(s->values[s->n], sizeof(s->values[0]), "%s", value);
        s->n++;
    }
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
}

static char *read_all(const char *path)
{
    static char buf[4096];
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

int main(void)
{
    /* Parsing: comments, spaces, CRLF, an unset, a bad line. */
    seen_t s = {0};
    int bad = tdsh_board_parse("# header\n"
                               "  rs485.1.tx = 16   # trailing comment\r\n"
                               "\n"
                               "eth.chip=w6100\n"
                               "not a setting\n"
                               "Bad.Key = 1\n"
                               "sd.cs =\n",
                               collect, &s);
    CHECK(bad == 5, "first bad line is reported");
    CHECK(s.n == 3, "three settings");
    CHECK(!strcmp(s.keys[0], "rs485.1.tx") && !strcmp(s.values[0], "16"), "spaces and comments trimmed");
    CHECK(!strcmp(s.keys[1], "eth.chip") && !strcmp(s.values[1], "w6100"), "no spaces around =");
    CHECK(!strcmp(s.keys[2], "sd.cs") && s.values[2][0] == '\0', "empty value = unset");
    CHECK(tdsh_board_parse(NULL, collect, &s) == 0, "NULL text is empty");

    /* Loading: the device file overrides the built-in text. */
    const char *path = "test_board.conf";
    const char *builtin = "rs485.1.uart = 1\nrs485.1.tx = 16\neth.chip = none\nflag = yes\n";
    write_file(path, "# my board\nrs485.1.tx = 20\neth.chip = w6100\nflag =\nhex = 0x10\n");
    CHECK(tdsh_board_load(builtin, path) == 0, "load ok");
    CHECK(tdsh_board_int("rs485.1.uart", -1) == 1, "built-in value");
    CHECK(tdsh_board_int("rs485.1.tx", -1) == 20, "file overrides built-in");
    CHECK(tdsh_board_origin("rs485.1.tx") == TDSH_BOARD_FILE, "origin file");
    CHECK(tdsh_board_origin("rs485.1.uart") == TDSH_BOARD_BUILTIN, "origin built-in");
    CHECK(!strcmp(tdsh_board_get("eth.chip"), "w6100"), "string value");
    CHECK(tdsh_board_get("flag") == NULL, "file can unset a built-in key");
    CHECK(tdsh_board_bool("flag", true) == true, "missing bool gives default");
    CHECK(tdsh_board_int("hex", 0) == 16, "hex numbers");
    CHECK(tdsh_board_int("missing", -1) == -1, "missing int gives default");
    CHECK(tdsh_board_int("eth.chip", -7) == -7, "non-number gives default");
    CHECK(tdsh_board_count() == 4, "four settings after the unset");

    /* Setting: the file keeps its comment, the key is replaced in place. */
    CHECK(tdsh_board_set("rs485.1.tx", "30") == 0, "set existing");
    CHECK(tdsh_board_set("sd.cs", "22") == 0, "set new");
    CHECK(tdsh_board_int("rs485.1.tx", -1) == 30 && tdsh_board_int("sd.cs", -1) == 22, "set values");
    char *text = read_all(path);
    CHECK(text && strstr(text, "# my board\n") == text, "comment kept");
    CHECK(text && strstr(text, "rs485.1.tx = 30\n") && !strstr(text, "rs485.1.tx = 20"), "replaced in place");
    CHECK(text && strstr(text, "sd.cs = 22\n"), "appended");

    /* Unsetting a file key falls back to the built-in value. */
    CHECK(tdsh_board_set("rs485.1.tx", NULL) == 0, "unset");
    CHECK(tdsh_board_int("rs485.1.tx", -1) == 16, "back to the built-in value");
    CHECK(tdsh_board_origin("rs485.1.tx") == TDSH_BOARD_BUILTIN, "origin back to built-in");

    /* Bad input is refused. */
    CHECK(tdsh_board_set("Bad Key", "1") != 0, "bad key refused");
    CHECK(tdsh_board_set("ok.key", "a # b") != 0, "value with # refused");

    /* Saving the built-in settings: the two that come only from the
     * firmware go into the file; the file's own (and its unset "flag")
     * stay as they are, and nothing changes value. */
    CHECK(tdsh_board_unsaved() == 2, "two settings only built in");
    CHECK(tdsh_board_save_builtin() == 2, "two saved");
    CHECK(tdsh_board_unsaved() == 0, "none left");
    CHECK(tdsh_board_origin("rs485.1.uart") == TDSH_BOARD_FILE && tdsh_board_int("rs485.1.uart", -1) == 1, "uart saved");
    CHECK(tdsh_board_origin("rs485.1.tx") == TDSH_BOARD_FILE && tdsh_board_int("rs485.1.tx", -1) == 16, "tx saved");
    CHECK(tdsh_board_get("flag") == NULL, "a key the file unsets stays unset");
    CHECK(!strcmp(tdsh_board_get("eth.chip"), "w6100") && tdsh_board_int("sd.cs", -1) == 22, "file values kept");
    text = read_all(path); /* a static buffer */
    CHECK(text && strstr(text, "# my board\n") == text, "comment kept on save");
    CHECK(text && strstr(text, "rs485.1.uart = 1\n") && strstr(text, "rs485.1.tx = 16\n"), "written to the file");
    CHECK(tdsh_board_save_builtin() == 0, "saving again writes nothing");
    /* What the file now holds survives without the built-in text (a
     * firmware built without it). */
    CHECK(tdsh_board_load(NULL, path) == 0 && tdsh_board_int("rs485.1.uart", -1) == 1 &&
              tdsh_board_int("rs485.1.tx", -1) == 16 && tdsh_board_get("flag") == NULL,
          "settings kept without the built-in text");
    CHECK(tdsh_board_load(builtin, path) == 0, "reload with the built-in text");

    /* Numbers are decimal or 0x hex, never octal. */
    write_file(path, "a = 08\nb = 010\nc = 0x1F\nd = -1\ne = 0X10\nf = 12abc\n");
    CHECK(tdsh_board_load(NULL, path) == 0, "load numbers");
    CHECK(tdsh_board_int("a", -9) == 8, "08 is 8");
    CHECK(tdsh_board_int("b", -9) == 10, "010 is 10, not octal 8");
    CHECK(tdsh_board_int("c", -9) == 31 && tdsh_board_int("e", -9) == 16, "0x hex");
    CHECK(tdsh_board_int("d", 5) == -1, "-1");
    CHECK(tdsh_board_int("f", -9) == -9, "trailing junk gives the default");

    /* No device file at all. */
    remove(path);
    CHECK(tdsh_board_load(builtin, path) == 0, "missing file is fine");
    CHECK(tdsh_board_int("rs485.1.tx", -1) == 16 && tdsh_board_bool("flag", false), "built-in only");

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "ok", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
