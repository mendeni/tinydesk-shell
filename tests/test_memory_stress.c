#include "tdsh_posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fail(const char *what, size_t iteration)
{
    fprintf(stderr, "FAIL: %s at iteration %zu\n", what, iteration);
    return 1;
}

static int assert_no_live_blocks(const char *where, size_t iteration)
{
    tdsh_memory_stats_t st;
    tdsh_memory_get_stats(&st);
    if (st.live_blocks != 0 || st.live_bytes != 0) {
        fprintf(stderr,
                "FAIL: tracked leak after %s iteration %zu: blocks=%zu bytes=%zu\n",
                where, iteration, st.live_blocks, st.live_bytes);
        return 1;
    }
    return 0;
}

int main(void)
{
    char root_template[] = "/tmp/tdsh-sdk-mem-XXXXXX";
    char *root = mkdtemp(root_template);
    if (!root) return fail("mkdtemp", 0);

    tdsh_posix_config_t cfg = TDSH_POSIX_CONFIG_DEFAULT();
    cfg.hostname = "memtest";
    cfg.default_user = "alice";
    cfg.fs_root = root;
    if (tdsh_posix_init(&cfg) != 0) return fail("posix init", 0);

    tdsh_session_t session;
    if (tdsh_session_init(&session, "alice", false) != 0) {
        return fail("session init", 0);
    }

    char real[TDSH_MAX_REAL_PATH];
    if (tdsh_path_to_real(&session, "~/stress.tdsh", real, sizeof(real), NULL, 0) != 0) {
        return fail("script path", 0);
    }
    FILE *f = fopen(real, "w");
    if (!f) return fail("script create", 0);
    fputs("A=7\n", f);
    fputs("B=5\n", f);
    fputs("C=$((A + B * 2))\n", f);
    fputs("D=$(echo capture-ok | cat)\n", f);
    fputs("if $C == 17\n", f);
    fputs("  return 0\n", f);
    fputs("else\n", f);
    fputs("  return 1\n", f);
    fputs("endif\n", f);
    fclose(f);

    /* Stress parser capture/realloc/free paths without leaving tracked blocks. */
    for (size_t i = 1; i <= 2000; ++i) {
        if (tdsh_execute_line(&session, "TMP=$(echo hello | cat)") != 0) {
            return fail("command substitution", i);
        }
        if (strcmp(tdsh_var_get(&session, "TMP"), "hello") != 0) {
            return fail("command substitution result", i);
        }
        if (assert_no_live_blocks("substitution", i)) return 1;
    }

    /* Stress the script runtime's dynamic line table and per-line strings. */
    for (size_t i = 1; i <= 1000; ++i) {
        if (tdsh_run_script_in_session(&session, "~/stress.tdsh") != 0) {
            return fail("in-session script", i);
        }
        if (assert_no_live_blocks("in-session script", i)) return 1;
    }

    /* Stress isolated session + platform worker ownership/cleanup. */
    for (size_t i = 1; i <= 500; ++i) {
        if (tdsh_run_script(&session, "~/stress.tdsh", false) != 0) {
            return fail("isolated foreground script", i);
        }
        if (assert_no_live_blocks("isolated foreground script", i)) return 1;
    }

    /* Detached worker cleanup is a separate lifecycle. Run one at a time so
     * the shell's stdio-capture implementation is not deliberately exercised
     * concurrently by this ownership test. */
    for (size_t i = 1; i <= 100; ++i) {
        if (tdsh_run_script(&session, "~/stress.tdsh", true) != 0) {
            return fail("isolated background script start", i);
        }
        bool cleaned = false;
        for (size_t wait = 0; wait < 2000; ++wait) {
            tdsh_memory_stats_t now;
            tdsh_memory_get_stats(&now);
            if (now.live_blocks == 0 && now.live_bytes == 0) {
                cleaned = true;
                break;
            }
            tdsh_sleep_ms(1);
        }
        if (!cleaned) return fail("background cleanup timeout", i);
        /* Let the detached wrapper complete its final non-tracked context free. */
        tdsh_sleep_ms(1);
    }

    tdsh_memory_stats_t st;
    tdsh_memory_get_stats(&st);
    printf("PASS: memory stress: live=%zu/%zuB peak=%zu/%zuB allocations=%zu failures=%zu\n",
           st.live_blocks, st.live_bytes, st.peak_blocks, st.peak_bytes,
           st.total_allocations, st.failed_allocations);
    return 0;
}
