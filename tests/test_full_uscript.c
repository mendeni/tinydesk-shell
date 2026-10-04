#include "tdsh_posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TDSH_SOURCE_DIR
#error TDSH_SOURCE_DIR must be defined by the build
#endif

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in)
        return -1;
    FILE *out = fopen(dst, "wb");
    if (!out)
    {
        fclose(in);
        return -1;
    }
    char buf[4096];
    int rc = 0;
    for (;;)
    {
        size_t n = fread(buf, 1, sizeof(buf), in);
        if (n && fwrite(buf, 1, n, out) != n)
        {
            rc = -1;
            break;
        }
        if (n < sizeof(buf))
        {
            if (ferror(in))
                rc = -1;
            break;
        }
    }
    fclose(out);
    fclose(in);
    return rc;
}

int main(void)
{
    char root_template[] = "/tmp/tdsh-sdk-full-XXXXXX";
    char *root = mkdtemp(root_template);
    if (!root)
        return 1;

    tdsh_posix_config_t cfg = TDSH_POSIX_CONFIG_DEFAULT();
    cfg.hostname = "fulltest";
    cfg.default_user = "developer";
    cfg.fs_root = root;
    if (tdsh_posix_init(&cfg) != 0)
        return 2;

    tdsh_session_t session;
    if (tdsh_session_init(&session, "developer", false) != 0)
        return 3;

    char dst[TDSH_MAX_REAL_PATH];
    if (tdsh_path_to_real(&session, "~/test.tdsh", dst, sizeof(dst), NULL, 0) != 0)
        return 4;

    char src[TDSH_MAX_REAL_PATH * 2];
    snprintf(src, sizeof(src), "%s/tests/scripts/test_full_uscript_1_1.tdsh", TDSH_SOURCE_DIR);
    if (copy_file(src, dst) != 0)
        return 5;

    int rc = tdsh_run_script(&session, "~/test.tdsh", false);
    if (rc != 0)
    {
        fprintf(stderr, "full uScript regression returned %d\n", rc);
        return 6;
    }

    tdsh_memory_stats_t st;
    tdsh_memory_get_stats(&st);
    if (st.live_blocks != 0 || st.live_bytes != 0)
    {
        fprintf(stderr, "tracked allocations remain after full regression: %zu blocks / %zu bytes\n",
                st.live_blocks, st.live_bytes);
        return 7;
    }

    printf("PASS: full uScript 1.1 regression\n");
    return 0;
}
