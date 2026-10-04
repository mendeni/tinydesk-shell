#define _GNU_SOURCE
#include "tdsh_posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(cond, msg)                        \
    do                                          \
    {                                           \
        if (!(cond))                            \
        {                                       \
            fprintf(stderr, "FAIL: %s\n", msg); \
            return 1;                           \
        }                                       \
    } while (0)

int main(void)
{
    char root_template[] = "/tmp/tdsh-root-XXXXXX";
    char host_template[] = "/tmp/tdsh-hosthome-XXXXXX";
    char *root = mkdtemp(root_template);
    char *host_home = mkdtemp(host_template);
    CHECK(root && host_home, "mkdtemp");

    tdsh_posix_config_t cfg = TDSH_POSIX_CONFIG_DEFAULT();
    cfg.hostname = "maptest";
    cfg.default_user = "alice";
    cfg.fs_root = root;
    cfg.host_home = host_home;
    /* Important: default is sandboxed. Do not enable host-home mapping. */
    CHECK(cfg.map_default_user_home_to_host_home == false, "sandbox is the default");
    CHECK(tdsh_posix_init(&cfg) == 0, "posix init");

    tdsh_session_t session;
    CHECK(tdsh_session_init(&session, "alice", false) == 0, "session init");

    char real[TDSH_MAX_REAL_PATH];
    CHECK(tdsh_path_to_real(&session, "~/isolated.txt", real, sizeof(real), NULL, 0) == 0,
          "resolve virtual home file");

    char expected[TDSH_MAX_REAL_PATH];
    snprintf(expected, sizeof(expected), "%s/home/alice/isolated.txt", root);
    if (strcmp(real, expected) != 0)
    {
        fprintf(stderr, "FAIL: expected %s got %s\n", expected, real);
        return 1;
    }

    char forbidden[TDSH_MAX_REAL_PATH];
    snprintf(forbidden, sizeof(forbidden), "%s/isolated.txt", host_home);
    CHECK(strcmp(real, forbidden) != 0, "TinyDesk Shell home must not be host $HOME");

    FILE *fp = fopen(real, "w");
    CHECK(fp != NULL, "create sandboxed file");
    fputs("isolated\n", fp);
    fclose(fp);
    CHECK(access(expected, F_OK) == 0, "file exists in sandbox");
    CHECK(access(forbidden, F_OK) != 0, "file absent from host home");

    puts("PASS: POSIX TinyDesk Shell home is isolated from host home by default");
    return 0;
}
