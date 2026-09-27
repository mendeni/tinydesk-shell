#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "tdsh.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * TinyDesk Shell parser + uScript expansion layer.
 *
 * Interactive and .tdsh commands share this parser, so the following work in
 * scripts and at the prompt:
 *   $VAR / ${VAR} / $?
 *   $(command)
 *   $((integer expression))
 *   single + double quotes
 *   ;  &&  ||
 *   memory-backed pipelines: command | command
 *   redirection: >  >>  <
 *   wildcard expansion: *  ?
 *
 * Script-only control flow (if/while/for/functions/...) lives in
 * tdsh_script.c.
 */

#define USCRIPT_PIPE_MAX_STAGES      8
#define USCRIPT_CAPTURE_INITIAL      256U
#define USCRIPT_CAPTURE_MAX          8192U

static bool is_var_name_start(char c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static bool is_var_name_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static int append_char(char *out, size_t out_size, size_t *out_len, char c)
{
    if (*out_len + 1U >= out_size) return -ENOSPC;
    out[(*out_len)++] = c;
    out[*out_len] = '\0';
    return 0;
}

static int append_n(char *out, size_t out_size, size_t *out_len,
                    const char *text, size_t n)
{
    if (*out_len + n >= out_size) return -ENOSPC;
    if (n) memcpy(out + *out_len, text, n);
    *out_len += n;
    out[*out_len] = '\0';
    return 0;
}

static int append_text(char *out, size_t out_size, size_t *out_len,
                       const char *text)
{
    return append_n(out, out_size, out_len, text, strlen(text));
}

static void trim_inplace(char *text)
{
    if (!text) return;
    char *start = text;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != text) memmove(text, start, strlen(start) + 1U);

    size_t len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1U])) {
        text[--len] = '\0';
    }
}

/* Return the matching ')' for input[open_index] == '(' while honoring quotes. */
static int find_matching_paren(const char *input, size_t open_index, size_t *end_out)
{
    if (!input || input[open_index] != '(' || !end_out) return -EINVAL;

    unsigned depth = 1;
    bool single = false;
    bool dbl = false;
    bool escaped = false;

    for (size_t i = open_index + 1U; input[i]; ++i) {
        char c = input[i];

        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\' && !single) {
            escaped = true;
            continue;
        }
        if (!dbl && c == '\'') {
            single = !single;
            continue;
        }
        if (!single && c == '"') {
            dbl = !dbl;
            continue;
        }
        if (single || dbl) continue;

        if (c == '(') {
            depth++;
        } else if (c == ')') {
            if (--depth == 0) {
                *end_out = i;
                return 0;
            }
        }
    }
    return -EINVAL;
}

/* ---------- Integer expression evaluator ---------- */

typedef struct {
    tdsh_session_t *session;
    const char *p;
    bool ok;
} expr_parser_t;

static void expr_ws(expr_parser_t *ep)
{
    while (isspace((unsigned char)*ep->p)) ep->p++;
}

static bool expr_match(expr_parser_t *ep, const char *op)
{
    expr_ws(ep);
    size_t n = strlen(op);
    if (strncmp(ep->p, op, n) == 0) {
        ep->p += n;
        return true;
    }
    return false;
}

static int64_t string_to_int(const char *value, bool *ok)
{
    if (!value || !value[0]) {
        *ok = false;
        return 0;
    }
    char *end = NULL;
    errno = 0;
    long long v = strtoll(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0') {
        *ok = false;
        return 0;
    }
    *ok = true;
    return (int64_t)v;
}

static const char *resolve_named_value(tdsh_session_t *session,
                                       const char *name,
                                       char *special, size_t special_size)
{
    if (strcmp(name, "?") == 0) {
        snprintf(special, special_size, "%d", session ? session->last_status : 0);
        return special;
    }

    if (tdsh_script_special_var(session, name, special, special_size)) {
        return special;
    }

    return tdsh_var_get(session, name);
}

static int64_t expr_or(expr_parser_t *ep);

static int64_t expr_primary(expr_parser_t *ep)
{
    expr_ws(ep);

    if (*ep->p == '(') {
        ep->p++;
        int64_t v = expr_or(ep);
        expr_ws(ep);
        if (*ep->p != ')') {
            ep->ok = false;
            return 0;
        }
        ep->p++;
        return v;
    }

    if (*ep->p == '$') {
        ep->p++;
        char name[TDSH_VAR_NAME_MAX];
        size_t n = 0;

        if (*ep->p == '{') {
            ep->p++;
            while (*ep->p && *ep->p != '}' && n + 1U < sizeof(name)) {
                name[n++] = *ep->p++;
            }
            if (*ep->p != '}') {
                ep->ok = false;
                return 0;
            }
            ep->p++;
        } else if (*ep->p == '?' || *ep->p == '#') {
            name[n++] = *ep->p++;
        } else if (isdigit((unsigned char)*ep->p)) {
            while (isdigit((unsigned char)*ep->p) && n + 1U < sizeof(name)) {
                name[n++] = *ep->p++;
            }
        } else if (is_var_name_start(*ep->p)) {
            while (is_var_name_char(*ep->p) && n + 1U < sizeof(name)) {
                name[n++] = *ep->p++;
            }
        } else {
            ep->ok = false;
            return 0;
        }

        name[n] = '\0';
        char special[64];
        const char *value = resolve_named_value(ep->session, name,
                                                special, sizeof(special));
        bool ok = false;
        int64_t v = string_to_int(value ? value : "0", &ok);
        if (!ok) ep->ok = false;
        return v;
    }

    if (is_var_name_start(*ep->p)) {
        char name[TDSH_VAR_NAME_MAX];
        size_t n = 0;
        while (is_var_name_char(*ep->p) && n + 1U < sizeof(name)) {
            name[n++] = *ep->p++;
        }
        name[n] = '\0';

        if (strcmp(name, "true") == 0) return 1;
        if (strcmp(name, "false") == 0) return 0;

        char special[64];
        const char *value = resolve_named_value(ep->session, name,
                                                special, sizeof(special));
        bool ok = false;
        int64_t v = string_to_int(value ? value : "0", &ok);
        if (!ok) ep->ok = false;
        return v;
    }

    char *end = NULL;
    errno = 0;
    long long value = strtoll(ep->p, &end, 0);
    if (end == ep->p || errno != 0) {
        ep->ok = false;
        return 0;
    }
    ep->p = end;
    return (int64_t)value;
}

static int64_t expr_unary(expr_parser_t *ep)
{
    expr_ws(ep);
    if (expr_match(ep, "!")) return !expr_unary(ep);
    if (expr_match(ep, "+")) return expr_unary(ep);
    if (expr_match(ep, "-")) return -expr_unary(ep);
    return expr_primary(ep);
}

static int64_t expr_mul(expr_parser_t *ep)
{
    int64_t v = expr_unary(ep);
    while (ep->ok) {
        if (expr_match(ep, "*")) {
            v *= expr_unary(ep);
        } else if (expr_match(ep, "/")) {
            int64_t rhs = expr_unary(ep);
            if (rhs == 0) { ep->ok = false; return 0; }
            v /= rhs;
        } else if (expr_match(ep, "%")) {
            int64_t rhs = expr_unary(ep);
            if (rhs == 0) { ep->ok = false; return 0; }
            v %= rhs;
        } else break;
    }
    return v;
}

static int64_t expr_add(expr_parser_t *ep)
{
    int64_t v = expr_mul(ep);
    while (ep->ok) {
        if (expr_match(ep, "+")) v += expr_mul(ep);
        else if (expr_match(ep, "-")) v -= expr_mul(ep);
        else break;
    }
    return v;
}

static int64_t expr_rel(expr_parser_t *ep)
{
    int64_t v = expr_add(ep);
    while (ep->ok) {
        if (expr_match(ep, "<=")) v = (v <= expr_add(ep));
        else if (expr_match(ep, ">=")) v = (v >= expr_add(ep));
        else if (expr_match(ep, "<")) v = (v < expr_add(ep));
        else if (expr_match(ep, ">")) v = (v > expr_add(ep));
        else break;
    }
    return v;
}

static int64_t expr_eq(expr_parser_t *ep)
{
    int64_t v = expr_rel(ep);
    while (ep->ok) {
        if (expr_match(ep, "==")) v = (v == expr_rel(ep));
        else if (expr_match(ep, "!=")) v = (v != expr_rel(ep));
        else break;
    }
    return v;
}

static int64_t expr_and(expr_parser_t *ep)
{
    int64_t v = expr_eq(ep);
    while (ep->ok && expr_match(ep, "&&")) {
        int64_t rhs = expr_eq(ep);
        v = (v && rhs);
    }
    return v;
}

static int64_t expr_or(expr_parser_t *ep)
{
    int64_t v = expr_and(ep);
    while (ep->ok && expr_match(ep, "||")) {
        int64_t rhs = expr_and(ep);
        v = (v || rhs);
    }
    return v;
}

int tdsh_eval_int_expr(tdsh_session_t *session, const char *expr,
                         int64_t *value_out)
{
    if (!session || !expr || !value_out) return -EINVAL;
    expr_parser_t ep = {.session = session, .p = expr, .ok = true};
    int64_t value = expr_or(&ep);
    expr_ws(&ep);
    if (!ep.ok || *ep.p != '\0') return -EINVAL;
    *value_out = value;
    return 0;
}

/* ---------- Task-local memory stream capture ---------- */

typedef struct {
    const char *input;
    size_t input_len;
    size_t input_pos;
    char *output;
    size_t output_len;
    size_t output_cap;
    bool overflow;
} memio_t;

static int memio_read(void *cookie, char *buf, int len)
{
    memio_t *io = (memio_t *)cookie;
    if (!io || !buf || len <= 0) return 0;
    if (io->input_pos >= io->input_len) return 0;

    size_t remain = io->input_len - io->input_pos;
    size_t n = remain < (size_t)len ? remain : (size_t)len;
    memcpy(buf, io->input + io->input_pos, n);
    io->input_pos += n;
    return (int)n;
}

static int memio_write(void *cookie, const char *buf, int len)
{
    memio_t *io = (memio_t *)cookie;
    if (!io || !buf || len <= 0) return 0;

    size_t need = io->output_len + (size_t)len + 1U;
    if (need > USCRIPT_CAPTURE_MAX + 1U) {
        io->overflow = true;
        errno = ENOSPC;
        return -1;
    }

    if (need > io->output_cap) {
        size_t cap = io->output_cap ? io->output_cap : USCRIPT_CAPTURE_INITIAL;
        while (cap < need && cap < USCRIPT_CAPTURE_MAX + 1U) cap *= 2U;
        if (cap > USCRIPT_CAPTURE_MAX + 1U) cap = USCRIPT_CAPTURE_MAX + 1U;
        if (cap < need) {
            io->overflow = true;
            errno = ENOSPC;
            return -1;
        }
        char *p = tdsh_realloc(io->output, cap);
        if (!p) {
            errno = ENOMEM;
            return -1;
        }
        io->output = p;
        io->output_cap = cap;
    }

    memcpy(io->output + io->output_len, buf, (size_t)len);
    io->output_len += (size_t)len;
    io->output[io->output_len] = '\0';
    return len;
}


#if defined(__GLIBC__)
static ssize_t memio_glibc_read(void *cookie, char *buf, size_t len)
{
    if (len > (size_t)INT_MAX) len = (size_t)INT_MAX;
    return (ssize_t)memio_read(cookie, buf, (int)len);
}

static ssize_t memio_glibc_write(void *cookie, const char *buf, size_t len)
{
    if (len > (size_t)INT_MAX) len = (size_t)INT_MAX;
    return (ssize_t)memio_write(cookie, buf, (int)len);
}
#endif

static FILE *memio_open_stream(memio_t *io)
{
#if defined(__GLIBC__)
    cookie_io_functions_t fns = {
        .read = memio_glibc_read,
        .write = memio_glibc_write,
        .seek = NULL,
        .close = NULL,
    };
    return fopencookie(io, "w+", fns);
#else
    /* ESP-IDF/newlib provides BSD funopen(). */
    return funopen(io, memio_read, memio_write, NULL, NULL);
#endif
}

static int capture_execute(tdsh_session_t *session, const char *command,
                           const char *input, char **output_out,
                           int *status_out)
{
    if (!session || !command || !output_out || !status_out) return -EINVAL;

    memio_t io = {
        .input = input ? input : "",
        .input_len = input ? strlen(input) : 0,
    };

    FILE *stream = memio_open_stream(&io);
    if (!stream) return -errno;
    setvbuf(stream, NULL, _IONBF, 0);

    FILE *old_in = stdin;
    FILE *old_out = stdout;
    stdin = stream;
    stdout = stream;

    int status = tdsh_execute_line(session, command);
    fflush(stdout);

    stdin = old_in;
    stdout = old_out;
    fclose(stream);

    if (io.overflow) {
        tdsh_free(io.output);
        return -ENOSPC;
    }

    if (!io.output) {
        io.output = tdsh_strdup("");
        if (!io.output) return -ENOMEM;
    }

    *output_out = io.output;
    *status_out = status;
    return 0;
}

/*
 * Execute command substitution in a heap-allocated session clone.
 *
 * tdsh_session_t is intentionally fairly large because it carries the
 * per-session variable table.  Copying it as an automatic/local variable
 * consumed several KiB of the uScript task stack for every $(...) or
 * command-substitution expansion. On ESP32-C6 this combined badly with nested parser /
 * function / control-flow frames and caused a stack-protection fault.
 *
 * Keeping the clone on the heap preserves shell-like subshell semantics:
 * variable changes made by the substituted command do not leak back into
 * the caller, while script_runtime is still copied so $1/$# and uScript
 * functions remain visible inside the substitution.
 */
static int capture_execute_subshell(tdsh_session_t *session,
                                    const char *command,
                                    const char *input,
                                    char **output_out,
                                    int *status_out)
{
    if (!session) return -EINVAL;

    tdsh_session_t *sub = tdsh_malloc(sizeof(*sub));
    if (!sub) return -ENOMEM;

    memcpy(sub, session, sizeof(*sub));
    int rc = capture_execute(sub, command, input, output_out, status_out);
    tdsh_free(sub);
    return rc;
}

static void normalize_substitution_output(char *text)
{
    if (!text) return;
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1U] == '\n' || text[len - 1U] == '\r')) {
        text[--len] = '\0';
    }
    for (size_t i = 0; text[i]; ++i) {
        if (text[i] == '\n' || text[i] == '\r') text[i] = ' ';
    }
}

/* ---------- Shell wildcard expansion ---------- */

static bool glob_pattern_match(const char *pattern, const char *text)
{
    if (!pattern || !text) return false;

    while (*pattern) {
        if (*pattern == '*') {
            while (*pattern == '*') pattern++;
            if (!*pattern) return true;
            for (const char *p = text; ; ++p) {
                if (glob_pattern_match(pattern, p)) return true;
                if (!*p) break;
            }
            return false;
        }

        if (*pattern == '?') {
            if (!*text) return false;
            pattern++;
            text++;
            continue;
        }

        if (*pattern != *text) return false;
        pattern++;
        text++;
    }

    return *text == '\0';
}

static int string_ptr_compare(const void *a, const void *b)
{
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

static bool contains_glob_chars(const char *text)
{
    return text && (strchr(text, '*') != NULL || strchr(text, '?') != NULL);
}

/*
 * Expand one shell word containing an unquoted '*' or '?'.
 *
 * To keep the embedded implementation predictable, wildcard metacharacters
 * are supported in the final path component. Examples:
 *
 *     *.txt
 *     logs/<star>.bin
 *     ~/mnt/pc/<star>.tdsh
 *
 * If the directory portion itself contains '*' or '?', or if no entry
 * matches, the original word is retained (the default POSIX-shell behavior
 * when nullglob is not enabled).
 */
static int append_glob_expansion(tdsh_session_t *session,
                                 const char *token,
                                 char *storage,
                                 size_t storage_size,
                                 size_t *out_len,
                                 char **argv,
                                 int *argc,
                                 bool *expanded_out)
{
    if (!session || !token || !storage || !out_len ||
        !argv || !argc || !expanded_out) {
        return -EINVAL;
    }

    *expanded_out = false;

    const char *slash = strrchr(token, '/');
    const char *pattern = slash ? slash + 1 : token;
    if (!contains_glob_chars(pattern)) return 0;

    char dir_expr[TDSH_MAX_PATH];
    char result_prefix[TDSH_MAX_PATH];
    result_prefix[0] = '\0';

    if (slash) {
        size_t dir_len = (size_t)(slash - token);
        size_t prefix_len = dir_len + 1U;
        if (dir_len == 0) {
            snprintf(dir_expr, sizeof(dir_expr), "/");
        } else {
            if (dir_len >= sizeof(dir_expr)) return -ENAMETOOLONG;
            memcpy(dir_expr, token, dir_len);
            dir_expr[dir_len] = '\0';
        }

        if (prefix_len >= sizeof(result_prefix)) return -ENAMETOOLONG;
        memcpy(result_prefix, token, prefix_len);
        result_prefix[prefix_len] = '\0';
    } else {
        snprintf(dir_expr, sizeof(dir_expr), ".");
    }

    /* Recursive wildcard directory components are intentionally deferred. */
    if (contains_glob_chars(dir_expr)) return 0;

    char real_dir[TDSH_MAX_REAL_PATH];
    if (tdsh_path_to_real(session, dir_expr,
                            real_dir, sizeof(real_dir),
                            NULL, 0) != 0) {
        return 0; /* unmatched pattern stays literal */
    }

    DIR *dir = opendir(real_dir);
    if (!dir) return 0; /* unmatched/inaccessible pattern stays literal */

    char *matches[TDSH_MAX_ARGS];
    size_t match_count = 0;
    memset(matches, 0, sizeof(matches));

    bool allow_hidden = pattern[0] == '.';
    struct dirent *entry;
    int rc = 0;

    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (!allow_hidden && name[0] == '.') continue;
        if (!glob_pattern_match(pattern, name)) continue;

        if (match_count >= TDSH_MAX_ARGS) {
            rc = -E2BIG;
            break;
        }

        size_t prefix_len = strlen(result_prefix);
        size_t name_len = strlen(name);
        size_t total = prefix_len + name_len;
        if (total >= TDSH_MAX_PATH) {
            rc = -ENAMETOOLONG;
            break;
        }

        char *item = tdsh_malloc(total + 1U);
        if (!item) {
            rc = -ENOMEM;
            break;
        }
        memcpy(item, result_prefix, prefix_len);
        memcpy(item + prefix_len, name, name_len + 1U);
        matches[match_count++] = item;
    }

    closedir(dir);

    if (rc == 0 && match_count > 1U) {
        qsort(matches, match_count, sizeof(matches[0]), string_ptr_compare);
    }

    if (rc == 0 && match_count > 0U) {
        if (*argc + (int)match_count > TDSH_MAX_ARGS) {
            rc = -E2BIG;
        } else {
            for (size_t i = 0; i < match_count; ++i) {
                argv[(*argc)++] = storage + *out_len;
                rc = append_text(storage, storage_size, out_len, matches[i]);
                if (rc == 0) {
                    rc = append_char(storage, storage_size, out_len, '\0');
                }
                if (rc != 0) break;
            }
            if (rc == 0) *expanded_out = true;
        }
    }

    for (size_t i = 0; i < match_count; ++i) tdsh_free(matches[i]);
    return rc;
}

static int finalize_parsed_token(tdsh_session_t *session,
                                 char *storage,
                                 size_t storage_size,
                                 size_t *out_len,
                                 char **argv,
                                 int *argc,
                                 char *token_start,
                                 bool wildcard_unquoted)
{
    int rc = append_char(storage, storage_size, out_len, '\0');
    if (rc != 0) return rc;

    if (!wildcard_unquoted || !contains_glob_chars(token_start)) {
        return 0;
    }

    /*
     * The raw token is already present as argv[argc-1]. If expansion succeeds,
     * replace that one argv entry with all sorted matches.
     */
    int original_index = *argc - 1;
    bool expanded = false;
    *argc = original_index;
    rc = append_glob_expansion(session, token_start,
                               storage, storage_size, out_len,
                               argv, argc, &expanded);
    if (rc != 0) return rc;

    if (!expanded) {
        argv[(*argc)++] = token_start;
    }
    return 0;
}

/* ---------- Generic stdin/stdout redirection ---------- */

typedef struct {
    bool has_input;
    bool has_output;
    bool append_output;
    char input_path[TDSH_MAX_PATH];
    char output_path[TDSH_MAX_PATH];
} shell_redir_t;

/* Find the end of one shell word while preserving quotes/substitutions. */
static int scan_redirection_word(const char *text, size_t start, size_t *end_out)
{
    if (!text || !end_out) return -EINVAL;

    bool single = false;
    bool dbl = false;
    bool escaped = false;

    size_t i = start;
    for (; text[i]; ++i) {
        char c = text[i];

        if (escaped) {
            escaped = false;
            continue;
        }
        if (!single && c == '\\') {
            escaped = true;
            continue;
        }
        if (!dbl && c == '\'') {
            single = !single;
            continue;
        }
        if (!single && c == '"') {
            dbl = !dbl;
            continue;
        }

        if (!single && !dbl && c == '$' && text[i + 1U] == '(') {
            size_t end = 0;
            if (find_matching_paren(text, i + 1U, &end) != 0) return -EINVAL;
            i = end;
            continue;
        }

        if (!single && !dbl &&
            (isspace((unsigned char)c) || c == '<' || c == '>')) {
            break;
        }
    }

    if (single || dbl || escaped) return -EINVAL;
    *end_out = i;
    return 0;
}

static int expand_redirection_target(tdsh_session_t *session,
                                     const char *expr,
                                     char *out,
                                     size_t out_size)
{
    char storage[TDSH_MAX_LINE + 1];
    char *argv[TDSH_MAX_ARGS];
    int argc = 0;

    int rc = tdsh_parse_words(session, expr,
                                storage, sizeof(storage),
                                argv, &argc);
    if (rc != 0) return rc;
    if (argc != 1) return argc > 1 ? -E2BIG : -EINVAL;

    int written = snprintf(out, out_size, "%s", argv[0]);
    return (written < 0 || (size_t)written >= out_size)
               ? -ENAMETOOLONG
               : 0;
}

/*
 * Remove top-level <, > and >> operators from a command segment while
 * expanding and recording their targets. Operators inside quotes or $(...)
 * remain ordinary text.
 */
static int extract_redirections(tdsh_session_t *session,
                                const char *segment,
                                char *clean,
                                size_t clean_size,
                                shell_redir_t *redir)
{
    if (!session || !segment || !clean || clean_size == 0 || !redir) {
        return -EINVAL;
    }

    memset(redir, 0, sizeof(*redir));

    size_t out_len = 0;
    bool single = false;
    bool dbl = false;
    bool escaped = false;

    for (size_t i = 0; segment[i]; ++i) {
        char c = segment[i];

        if (escaped) {
            if (out_len + 1U >= clean_size) return -ENOSPC;
            clean[out_len++] = c;
            escaped = false;
            continue;
        }

        if (!single && c == '\\') {
            if (out_len + 1U >= clean_size) return -ENOSPC;
            clean[out_len++] = c;
            escaped = true;
            continue;
        }

        if (!dbl && c == '\'') {
            single = !single;
            if (out_len + 1U >= clean_size) return -ENOSPC;
            clean[out_len++] = c;
            continue;
        }

        if (!single && c == '"') {
            dbl = !dbl;
            if (out_len + 1U >= clean_size) return -ENOSPC;
            clean[out_len++] = c;
            continue;
        }

        if (!single && !dbl && c == '$' && segment[i + 1U] == '(') {
            size_t end = 0;
            if (find_matching_paren(segment, i + 1U, &end) != 0) return -EINVAL;
            size_t n = end - i + 1U;
            if (out_len + n >= clean_size) return -ENOSPC;
            memcpy(clean + out_len, segment + i, n);
            out_len += n;
            i = end;
            continue;
        }

        if (!single && !dbl && (c == '<' || c == '>')) {
            bool is_input = c == '<';
            bool append = (!is_input && segment[i + 1U] == '>');
            if (append) i++;

            if ((is_input && redir->has_input) ||
                (!is_input && redir->has_output)) {
                return -EINVAL; /* one input and one output redirection max */
            }

            size_t j = i + 1U;
            while (segment[j] && isspace((unsigned char)segment[j])) j++;
            if (!segment[j]) return -EINVAL;

            size_t end = 0;
            int rc = scan_redirection_word(segment, j, &end);
            if (rc != 0 || end == j) return -EINVAL;

            size_t expr_len = end - j;
            if (expr_len > TDSH_MAX_LINE) return -ENAMETOOLONG;

            char expr[TDSH_MAX_LINE + 1];
            memcpy(expr, segment + j, expr_len);
            expr[expr_len] = '\0';

            char *target = is_input ? redir->input_path : redir->output_path;
            rc = expand_redirection_target(session, expr,
                                           target, TDSH_MAX_PATH);
            if (rc != 0) return rc;

            if (is_input) {
                redir->has_input = true;
            } else {
                redir->has_output = true;
                redir->append_output = append;
            }

            /* Leave a separator so removing redirection never joins words. */
            if (out_len > 0 && !isspace((unsigned char)clean[out_len - 1U])) {
                if (out_len + 1U >= clean_size) return -ENOSPC;
                clean[out_len++] = ' ';
            }

            i = end > 0 ? end - 1U : end;
            continue;
        }

        if (out_len + 1U >= clean_size) return -ENOSPC;
        clean[out_len++] = c;
    }

    if (single || dbl || escaped) return -EINVAL;

    clean[out_len] = '\0';
    trim_inplace(clean);
    return 0;
}

static int open_shell_redirections(tdsh_session_t *session,
                                   const shell_redir_t *redir,
                                   FILE **input_file,
                                   FILE **output_file)
{
    *input_file = NULL;
    *output_file = NULL;

    if (redir->has_input) {
        char real[TDSH_MAX_REAL_PATH];
        int rc = tdsh_path_to_real(session, redir->input_path,
                                     real, sizeof(real), NULL, 0);
        if (rc != 0) return rc;

        *input_file = fopen(real, "rb");
        if (!*input_file) return -errno;
    }

    if (redir->has_output) {
        char real[TDSH_MAX_REAL_PATH];
        int rc = tdsh_path_to_real(session, redir->output_path,
                                     real, sizeof(real), NULL, 0);
        if (rc != 0) {
            if (*input_file) fclose(*input_file);
            *input_file = NULL;
            return rc;
        }

        *output_file = fopen(real, redir->append_output ? "ab" : "wb");
        if (!*output_file) {
            int saved = errno;
            if (*input_file) fclose(*input_file);
            *input_file = NULL;
            return -saved;
        }
    }

    return 0;
}

int tdsh_parse_words(tdsh_session_t *session,
                       const char *input,
                       char *storage, size_t storage_size,
                       char **argv, int *argc_out)
{
    if (!session || !input || !storage || !argv || !argc_out) return -EINVAL;

    size_t out_len = 0;
    int argc = 0;
    bool dbl = false;
    bool single = false;
    bool escaped = false;
    bool token_started = false;
    bool token_wildcard_unquoted = false;
    char *token_start = NULL;
    storage[0] = '\0';

#define START_TOKEN() do { \
    if (!token_started) { \
        if (argc >= TDSH_MAX_ARGS) return -E2BIG; \
        token_start = storage + out_len; \
        argv[argc++] = token_start; \
        token_started = true; \
        token_wildcard_unquoted = false; \
    } \
} while (0)

#define FINISH_TOKEN() do { \
    if (token_started) { \
        int _rc = finalize_parsed_token(session, \
                                        storage, storage_size, &out_len, \
                                        argv, &argc, token_start, \
                                        token_wildcard_unquoted); \
        if (_rc != 0) return _rc; \
        token_started = false; \
        token_start = NULL; \
        token_wildcard_unquoted = false; \
    } \
} while (0)

    for (size_t i = 0;; ++i) {
        char c = input[i];
        bool at_end = (c == '\0');

        if (escaped && !at_end) {
            START_TOKEN();
            int rc = append_char(storage, storage_size, &out_len, c);
            if (rc != 0) return rc;
            escaped = false;
            continue;
        }

        if (!single && !at_end && c == '\\') {
            START_TOKEN();
            escaped = true;
            continue;
        }

        if (!dbl && !at_end && c == '\'') {
            START_TOKEN();
            single = !single;
            continue;
        }

        if (!single && !at_end && c == '"') {
            START_TOKEN();
            dbl = !dbl;
            continue;
        }

        if (!single && !at_end && c == '$') {
            /* Arithmetic expansion: $(( expression )) */
            if (input[i + 1U] == '(' && input[i + 2U] == '(') {
                size_t end = 0;
                if (find_matching_paren(input, i + 1U, &end) != 0 ||
                    end <= i + 3U || input[end - 1U] != ')') {
                    return -EINVAL;
                }
                size_t expr_len = end - i - 4U;
                char expr[TDSH_MAX_LINE + 1];
                if (expr_len >= sizeof(expr)) return -ENOSPC;
                memcpy(expr, input + i + 3U, expr_len);
                expr[expr_len] = '\0';

                int64_t value = 0;
                int rc = tdsh_eval_int_expr(session, expr, &value);
                if (rc != 0) return rc;
                char number[32];
                snprintf(number, sizeof(number), "%lld", (long long)value);
                START_TOKEN();
                rc = append_text(storage, storage_size, &out_len, number);
                if (rc != 0) return rc;
                i = end;
                continue;
            }

            /* Modern command substitution: $( command ) */
            if (input[i + 1U] == '(') {
                size_t end = 0;
                if (find_matching_paren(input, i + 1U, &end) != 0) return -EINVAL;
                size_t n = end - i - 2U;
                char *inner = tdsh_malloc(n + 1U);
                if (!inner) return -ENOMEM;
                memcpy(inner, input + i + 2U, n);
                inner[n] = '\0';

                char *captured = NULL;
                int sub_status = 0;
                int rc = capture_execute_subshell(session, inner, NULL,
                                                  &captured, &sub_status);
                tdsh_free(inner);
                if (rc != 0) return rc;
                normalize_substitution_output(captured);
                START_TOKEN();
                if (!dbl && contains_glob_chars(captured)) {
                    token_wildcard_unquoted = true;
                }
                rc = append_text(storage, storage_size, &out_len, captured);
                tdsh_free(captured);
                if (rc != 0) return rc;
                i = end;
                continue;
            }

            char name[TDSH_VAR_NAME_MAX];
            size_t nlen = 0;
            size_t end = i;

            if (input[i + 1U] == '{') {
                size_t j = i + 2U;
                while (input[j] && input[j] != '}' && nlen + 1U < sizeof(name)) {
                    name[nlen++] = input[j++];
                }
                if (input[j] != '}') return -EINVAL;
                end = j;
            } else if (input[i + 1U] == '?' || input[i + 1U] == '#') {
                name[nlen++] = input[i + 1U];
                end = i + 1U;
            } else if (isdigit((unsigned char)input[i + 1U])) {
                size_t j = i + 1U;
                while (isdigit((unsigned char)input[j]) && nlen + 1U < sizeof(name)) {
                    name[nlen++] = input[j++];
                }
                end = j - 1U;
            } else if (is_var_name_start(input[i + 1U])) {
                size_t j = i + 1U;
                while (input[j] && is_var_name_char(input[j]) &&
                       nlen + 1U < sizeof(name)) {
                    name[nlen++] = input[j++];
                }
                end = j - 1U;
            }

            if (nlen > 0) {
                name[nlen] = '\0';
                char special[64];
                const char *value = resolve_named_value(session, name,
                                                        special, sizeof(special));
                START_TOKEN();
                if (!dbl && contains_glob_chars(value ? value : "")) {
                    token_wildcard_unquoted = true;
                }
                int rc = append_text(storage, storage_size, &out_len,
                                     value ? value : "");
                if (rc != 0) return rc;
                i = end;
                continue;
            }
        }

        if (at_end || (!single && !dbl && isspace((unsigned char)c))) {
            FINISH_TOKEN();
            if (at_end) break;
            continue;
        }

        START_TOKEN();
        if (!single && !dbl && (c == '*' || c == '?')) {
            token_wildcard_unquoted = true;
        }
        int rc = append_char(storage, storage_size, &out_len, c);
        if (rc != 0) return rc;
    }

#undef FINISH_TOKEN
#undef START_TOKEN

    if (dbl || single || escaped) return -EINVAL;
    *argc_out = argc;
    return 0;
}

static bool valid_assignment(const char *arg, const char **eq_out)
{
    const char *eq = strchr(arg, '=');
    if (!eq || eq == arg || !is_var_name_start(arg[0])) return false;
    for (const char *p = arg + 1; p < eq; ++p) {
        if (!is_var_name_char(*p)) return false;
    }
    if (eq_out) *eq_out = eq;
    return true;
}

static int set_assignment(tdsh_session_t *session, const char *arg)
{
    const char *eq = NULL;
    if (!valid_assignment(arg, &eq)) return -EINVAL;
    size_t name_len = (size_t)(eq - arg);
    char name[TDSH_VAR_NAME_MAX];
    if (name_len >= sizeof(name)) return -ENOSPC;
    memcpy(name, arg, name_len);
    name[name_len] = '\0';
    return tdsh_var_set(session, name, eq + 1U);
}

static int execute_segment(tdsh_session_t *session, const char *segment)
{
    char clean[TDSH_MAX_LINE + 1];
    shell_redir_t redir;

    int rc = extract_redirections(session, segment,
                                  clean, sizeof(clean), &redir);
    if (rc != 0) {
        printf("tdsh: redirection parse error: %s\n",
               strerror(rc < 0 ? -rc : rc));
        return 2;
    }

    char storage[TDSH_MAX_LINE + 1];
    char *argv[TDSH_MAX_ARGS];
    int argc = 0;

    rc = tdsh_parse_words(session, clean, storage, sizeof(storage),
                            argv, &argc);
    if (rc != 0) {
        printf("tdsh: parse error (%d)\n", rc);
        return 2;
    }

    FILE *redir_in = NULL;
    FILE *redir_out = NULL;
    FILE *old_in = stdin;
    FILE *old_out = stdout;

    rc = open_shell_redirections(session, &redir, &redir_in, &redir_out);
    if (rc != 0) {
        printf("tdsh: redirection: %s\n", strerror(-rc));
        return 1;
    }

    if (redir_in) stdin = redir_in;
    if (redir_out) {
        stdout = redir_out;
        setvbuf(stdout, NULL, _IONBF, 0);
    }

    int status = 0;

    /*
     * A redirection-only command is useful for creating/truncating a file:
     *     > empty.txt
     */
    if (argc == 0) {
        status = 0;
        goto done;
    }

    bool assignments = true;
    for (int i = 0; i < argc; ++i) {
        if (!valid_assignment(argv[i], NULL)) {
            assignments = false;
            break;
        }
    }

    if (assignments) {
        for (int i = 0; i < argc; ++i) {
            rc = set_assignment(session, argv[i]);
            if (rc != 0) {
                printf("tdsh: cannot set variable (%d)\n", rc);
                status = 1;
                goto done;
            }
        }
        status = 0;
        goto done;
    }

    /* Lightweight language built-ins that do not need command-table entries. */
    if (strcmp(argv[0], "true") == 0 && argc == 1) {
        status = 0;
        goto done;
    }
    if (strcmp(argv[0], "false") == 0 && argc == 1) {
        status = 1;
        goto done;
    }

    /* Shell-compatible stdin passthrough used by pipes and '< file'. */
    if (strcmp(argv[0], "cat") == 0 && argc == 1) {
        char buf[256];
        size_t n;
        status = 0;
        while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) {
            if (fwrite(buf, 1, n, stdout) != n) {
                status = 1;
                break;
            }
        }
        if (ferror(stdin)) status = 1;
        goto done;
    }

    if (strcmp(argv[0], "export") == 0) {
        if (argc < 2) {
            printf("usage: export NAME=value [...]\n");
            status = 2;
            goto done;
        }
        status = 0;
        for (int i = 1; i < argc; ++i) {
            if (valid_assignment(argv[i], NULL)) {
                rc = set_assignment(session, argv[i]);
            } else {
                const char *value = tdsh_var_get(session, argv[i]);
                rc = value ? 0 : -ENOENT;
            }
            if (rc != 0) {
                printf("export: invalid variable: %s\n", argv[i]);
                status = 1;
                break;
            }
        }
        goto done;
    }

    {
        int function_status = 0;
        if (tdsh_script_try_function(session, argc, argv, &function_status)) {
            status = function_status;
        } else {
            status = tdsh_execute_argv(session, argc, argv);
        }
    }

done:
    if (redir_out) fflush(redir_out);

    stdin = old_in;
    stdout = old_out;

    if (redir_out) fclose(redir_out);
    if (redir_in) fclose(redir_in);

    return status;
}

/* Detect a top-level separator while treating quotes/substitutions as opaque. */
static int split_pipeline(char *buffer, char **stages, int *count_out)
{
    int count = 1;
    stages[0] = buffer;
    bool single = false, dbl = false, escaped = false;

    for (size_t i = 0; buffer[i]; ++i) {
        char c = buffer[i];
        if (escaped) { escaped = false; continue; }
        if (c == '\\' && !single) { escaped = true; continue; }
        if (!dbl && c == '\'') { single = !single; continue; }
        if (!single && c == '"') { dbl = !dbl; continue; }
        if (single) continue;

        if (c == '$' && buffer[i + 1U] == '(') {
            size_t end = 0;
            if (find_matching_paren(buffer, i + 1U, &end) != 0) return -EINVAL;
            i = end;
            continue;
        }

        if (!dbl && c == '|' && buffer[i + 1U] != '|') {
            if (count >= USCRIPT_PIPE_MAX_STAGES) return -E2BIG;
            buffer[i] = '\0';
            stages[count++] = buffer + i + 1U;
        }
    }

    if (single || dbl || escaped) return -EINVAL;
    for (int i = 0; i < count; ++i) trim_inplace(stages[i]);
    *count_out = count;
    return 0;
}

static int execute_pipeline(tdsh_session_t *session, const char *segment)
{
    char buffer[TDSH_MAX_LINE + 1];
    if (snprintf(buffer, sizeof(buffer), "%s", segment) >= (int)sizeof(buffer)) {
        return 2;
    }

    char *stages[USCRIPT_PIPE_MAX_STAGES];
    int count = 0;
    int rc = split_pipeline(buffer, stages, &count);
    if (rc != 0) {
        printf("tdsh: pipeline parse error (%d)\n", rc);
        return 2;
    }
    if (count == 1) return execute_segment(session, stages[0]);

    char *input = NULL;
    int status = 0;
    for (int i = 0; i < count; ++i) {
        if (!stages[i][0]) {
            tdsh_free(input);
            printf("tdsh: empty pipeline stage\n");
            return 2;
        }

        char *output = NULL;
        rc = capture_execute(session, stages[i], input, &output, &status);
        tdsh_free(input);
        input = NULL;
        if (rc != 0) {
            printf("tdsh: pipeline capture failed: %s\n", strerror(-rc));
            tdsh_free(output);
            return 1;
        }
        input = output;
    }

    if (input && input[0]) fputs(input, stdout);
    tdsh_free(input);
    return status;
}

typedef enum {
    OP_ALWAYS = 0,
    OP_AND,
    OP_OR,
} pending_op_t;

int tdsh_execute_line(tdsh_session_t *session, const char *line)
{
    if (!session || !line) return 2;

    while (*line && isspace((unsigned char)*line)) line++;
    if (*line == '\0' || *line == '#') return 0;

    char segment[TDSH_MAX_LINE + 1];
    size_t seg_len = 0;
    bool single = false, dbl = false, escaped = false;
    pending_op_t op = OP_ALWAYS;
    int last_status = session->last_status;
    bool executed_any = false;

    for (size_t i = 0;; ++i) {
        char c = line[i];
        bool at_end = (c == '\0');

        if (escaped && !at_end) {
            if (seg_len >= TDSH_MAX_LINE) return 2;
            segment[seg_len++] = c;
            escaped = false;
            continue;
        }
        if (!single && !at_end && c == '\\') {
            if (seg_len >= TDSH_MAX_LINE) return 2;
            segment[seg_len++] = c;
            escaped = true;
            continue;
        }
        if (!dbl && !at_end && c == '\'') {
            single = !single;
            if (seg_len >= TDSH_MAX_LINE) return 2;
            segment[seg_len++] = c;
            continue;
        }
        if (!single && !at_end && c == '"') {
            dbl = !dbl;
            if (seg_len >= TDSH_MAX_LINE) return 2;
            segment[seg_len++] = c;
            continue;
        }

        if (!single && !at_end && c == '$' && line[i + 1U] == '(') {
            size_t end = 0;
            if (find_matching_paren(line, i + 1U, &end) != 0) {
                printf("tdsh: unmatched command/arithmetic substitution\n");
                return 2;
            }
            size_t n = end - i + 1U;
            if (seg_len + n > TDSH_MAX_LINE) return 2;
            memcpy(segment + seg_len, line + i, n);
            seg_len += n;
            i = end;
            continue;
        }

        bool comment = (!at_end && !single && !dbl && c == '#' &&
                        (i == 0 || isspace((unsigned char)line[i - 1U])));
        if (comment) {
            c = '\0';
            at_end = true;
        }

        bool semicolon = (!at_end && !single && !dbl && c == ';');
        bool and_op = (!at_end && !single && !dbl && c == '&' && line[i + 1U] == '&');
        bool or_op = (!at_end && !single && !dbl && c == '|' && line[i + 1U] == '|');

        if (at_end || semicolon || and_op || or_op) {
            segment[seg_len] = '\0';
            trim_inplace(segment);

            if (segment[0]) {
                bool run = (op == OP_ALWAYS) ||
                           (op == OP_AND && last_status == 0) ||
                           (op == OP_OR && last_status != 0);
                if (run) {
                    last_status = execute_pipeline(session, segment);
                    session->last_status = last_status;
                    executed_any = true;
                    if (session->logout_requested) return last_status;
                }
            }

            seg_len = 0;
            if (at_end) break;
            op = and_op ? OP_AND : (or_op ? OP_OR : OP_ALWAYS);
            if (and_op || or_op) i++;
            continue;
        }

        if (seg_len >= TDSH_MAX_LINE) {
            printf("tdsh: command too long\n");
            return 2;
        }
        segment[seg_len++] = c;
    }

    if (single || dbl || escaped) {
        printf("tdsh: unmatched quote or escape\n");
        session->last_status = 2;
        return 2;
    }

    if (!executed_any) return 0;
    return last_status;
}
