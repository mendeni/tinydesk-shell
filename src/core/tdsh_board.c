/*
 * tdsh_board.c - board configuration (see tdsh_board.h).
 *
 * Portable C: standard library only, so the parser is tested on the host.
 */
#include "tdsh_board.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    char key[TDSH_BOARD_KEY_MAX];
    char value[TDSH_BOARD_VALUE_MAX];
    tdsh_board_origin_t origin;
} entry_t;

static entry_t *s_entries;
static int s_count, s_cap;
static const char *s_builtin;
static char s_file[160];

/* ------------------------------------------------------------ parsing */

static bool key_char(int c)
{
    return islower(c) || isdigit(c) || c == '.' || c == '_' || c == '-';
}

int tdsh_board_parse(const char *text,
                     void (*fn)(const char *key, const char *value, int line, void *user),
                     void *user)
{
    int first_bad = 0;
    int line_no = 0;
    const char *p = text ? text : "";
    while (*p)
    {
        line_no++;
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char line[TDSH_BOARD_KEY_MAX + TDSH_BOARD_VALUE_MAX + 64];
        bool too_long = len >= sizeof(line);
        if (too_long)
            len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        p = end ? end + 1 : p + strlen(p);

        char *hash = strchr(line, '#');                 /* comments */
        if (hash)
            *hash = '\0';
        char *s = line;
        while (isspace((unsigned char)*s))
            s++;
        char *e = s + strlen(s);
        while (e > s && isspace((unsigned char)e[-1]))
            *--e = '\0';
        if (!*s)
            continue;                              /* blank or comment */

        char *eq = strchr(s, '=');
        bool ok = eq != NULL && !too_long;
        char key[TDSH_BOARD_KEY_MAX] = "";
        char value[TDSH_BOARD_VALUE_MAX] = "";
        if (ok)
        {
            char *ke = eq;
            while (ke > s && isspace((unsigned char)ke[-1]))
                ke--;
            size_t kl = (size_t)(ke - s);
            ok = kl > 0 && kl < sizeof(key);
            for (size_t i = 0; ok && i < kl; i++)
                ok = key_char((unsigned char)s[i]);
            if (ok)
            {
                memcpy(key, s, kl);
                char *v = eq + 1;
                while (isspace((unsigned char)*v))
                    v++;
                ok = strlen(v) < sizeof(value);
                if (ok)
                    snprintf(value, sizeof(value), "%s", v);
            }
        }
        if (!ok)
        {
            if (!first_bad)
                first_bad = line_no;
            continue;
        }
        if (fn)
            fn(key, value, line_no, user);
    }
    return first_bad;
}

/* ------------------------------------------------------------ the table */

static entry_t *find(const char *key)
{
    for (int i = 0; i < s_count; i++)
        if (strcmp(s_entries[i].key, key) == 0)
            return &s_entries[i];
    return NULL;
}

static void remove_key(const char *key)
{
    entry_t *e = find(key);
    if (!e)
        return;
    int i = (int)(e - s_entries);
    memmove(&s_entries[i], &s_entries[i + 1], sizeof(entry_t) * (size_t)(s_count - i - 1));
    s_count--;
}

static void put(const char *key, const char *value, tdsh_board_origin_t origin)
{
    if (!value[0])
    {                                    /* "key =" unsets */
        remove_key(key);
        return;
    }
    entry_t *e = find(key);
    if (!e)
    {
        if (s_count == s_cap)
        {
            int cap = s_cap ? s_cap * 2 : 16;
            entry_t *grown = realloc(s_entries, sizeof(entry_t) * (size_t)cap);
            if (!grown)
                return;
            s_entries = grown;
            s_cap = cap;
        }
        e = &s_entries[s_count++];
        snprintf(e->key, sizeof(e->key), "%s", key);
    }
    snprintf(e->value, sizeof(e->value), "%s", value);
    e->origin = origin;
}

static void put_builtin(const char *key, const char *value, int line, void *user)
{
    (void)line;
    (void)user;
    put(key, value, TDSH_BOARD_BUILTIN);
}

static void put_file(const char *key, const char *value, int line, void *user)
{
    (void)line;
    (void)user;
    put(key, value, TDSH_BOARD_FILE);
}

/* The whole file as a string (NULL if missing or unreadable). */
static char *read_file(const char *path)
{
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f)
        return NULL;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    char chunk[256];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
    {
        if (len + n + 1 > cap)
        {
            size_t nc = cap ? cap * 2 : 1024;
            while (nc < len + n + 1)
                nc *= 2;
            char *g = realloc(buf, nc);
            if (!g)
            {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = g;
            cap = nc;
        }
        memcpy(buf + len, chunk, n);
        len += n;
    }
    fclose(f);
    if (!buf)
        buf = calloc(1, 1);
    else
        buf[len] = '\0';
    return buf;
}

int tdsh_board_load(const char *builtin_text, const char *file_path)
{
    s_count = 0;
    s_builtin = builtin_text;
    snprintf(s_file, sizeof(s_file), "%s", file_path ? file_path : "");
    (void)tdsh_board_parse(builtin_text, put_builtin, NULL);
    char *text = read_file(file_path);
    int bad = text ? tdsh_board_parse(text, put_file, NULL) : 0;
    free(text);
    return bad;
}

const char *tdsh_board_get(const char *key)
{
    entry_t *e = key ? find(key) : NULL;
    return e ? e->value : NULL;
}

int tdsh_board_int(const char *key, int def)
{
    const char *v = tdsh_board_get(key);
    if (!v)
        return def;
    char *end = NULL;
    long n = strtol(v, &end, 0);
    return end && end != v && *end == '\0' ? (int)n : def;
}

bool tdsh_board_bool(const char *key, bool def)
{
    const char *v = tdsh_board_get(key);
    if (!v)
        return def;
    if (!strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "on") || !strcmp(v, "1"))
        return true;
    if (!strcmp(v, "no") || !strcmp(v, "false") || !strcmp(v, "off") || !strcmp(v, "0"))
        return false;
    return def;
}

tdsh_board_origin_t tdsh_board_origin(const char *key)
{
    entry_t *e = key ? find(key) : NULL;
    return e ? e->origin : TDSH_BOARD_UNSET;
}

int tdsh_board_count(void)
{
    return s_count;
}

bool tdsh_board_at(int index, const char **key, const char **value, tdsh_board_origin_t *origin)
{
    if (index < 0 || index >= s_count)
        return false;
    if (key)
        *key = s_entries[index].key;
    if (value)
        *value = s_entries[index].value;
    if (origin)
        *origin = s_entries[index].origin;
    return true;
}

const char *tdsh_board_builtin(void)
{
    return s_builtin;
}
const char *tdsh_board_file(void)
{
    return s_file;
}

/* ------------------------------------------------------------ editing */

/* Does this line of the file set `key`? */
static bool line_sets(const char *line, const char *key)
{
    while (isspace((unsigned char)*line))
        line++;
    size_t kl = strlen(key);
    if (strncmp(line, key, kl) != 0)
        return false;
    line += kl;
    while (*line == ' ' || *line == '\t')
        line++;
    return *line == '=';
}

int tdsh_board_set(const char *key, const char *value)
{
    if (!key || !key[0] || strlen(key) >= TDSH_BOARD_KEY_MAX)
        return -EINVAL;
    for (const char *k = key; *k; k++)
        if (!key_char((unsigned char)*k))
            return -EINVAL;
    if (value && (strlen(value) >= TDSH_BOARD_VALUE_MAX || strchr(value, '\n') || strchr(value, '#')))
        return -EINVAL;
    if (!s_file[0])
        return -ENOENT;

    char *old = read_file(s_file);
    char tmp[sizeof(s_file) + 4];
    snprintf(tmp, sizeof(tmp), "%s.new", s_file);
    FILE *out = fopen(tmp, "wb");
    if (!out)
    {
        free(old);
        return -errno;
    }
    bool done = false;
    const char *p = old ? old : "";
    while (*p)
    {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char line[TDSH_BOARD_KEY_MAX + TDSH_BOARD_VALUE_MAX + 64];
        size_t cl = len < sizeof(line) ? len : sizeof(line) - 1;
        memcpy(line, p, cl);
        line[cl] = '\0';
        if (line_sets(line, key))
        {
            if (!done && value && value[0])
                fprintf(out, "%s = %s\n", key, value);
            done = true;                                /* drop repeats of the key */
        }
        else
        {
            fwrite(p, 1, len, out);
            fputc('\n', out);
        }
        p = end ? end + 1 : p + len;
    }
    if (!done && value && value[0])
        fprintf(out, "%s = %s\n", key, value);
    free(old);
    bool write_ok = fflush(out) == 0;
    if (fclose(out) != 0)
        write_ok = false;
    if (!write_ok)
    {
        remove(tmp);
        return -EIO;
    }
    remove(s_file);
    if (rename(tmp, s_file) != 0)
        return -errno;
    /* Reload both sources: a removed key falls back to its built-in value. */
    char file[sizeof(s_file)];
    snprintf(file, sizeof(file), "%s", s_file);
    (void)tdsh_board_load(s_builtin, file);
    return 0;
}

int tdsh_board_unsaved(void)
{
    int n = 0;
    for (int i = 0; i < s_count; i++)
        if (s_entries[i].origin == TDSH_BOARD_BUILTIN)
            n++;
    return n;
}

int tdsh_board_save_builtin(void)
{
    /* One key at a time through tdsh_board_set(), which keeps the file's
     * comments and other lines; it reloads the table, so look again each
     * time. Every round moves one key to the file, so at most the number
     * of keys rounds. */
    int saved = 0;
    for (int round = tdsh_board_count(); round >= 0; round--)
    {
        int i = 0;
        while (i < s_count && s_entries[i].origin != TDSH_BOARD_BUILTIN)
            i++;
        if (i == s_count)
            return saved;
        char key[TDSH_BOARD_KEY_MAX], value[TDSH_BOARD_VALUE_MAX];
        snprintf(key, sizeof(key), "%s", s_entries[i].key);
        snprintf(value, sizeof(value), "%s", s_entries[i].value);
        int rc = tdsh_board_set(key, value);
        if (rc != 0)
            return rc;
        saved++;
    }
    return -EIO;
}
