#include "tdsh_posix.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

static int cmd_probe(tdsh_session_t *session, int argc, char **argv)
{
    (void)argc; (void)argv;
    return tdsh_var_set(session, "PROBE", "ok") == 0 ? 0 : 1;
}

int main(void)
{
    char root_template[] = "/tmp/tdsh-sdk-test-XXXXXX";
    char *root = mkdtemp(root_template);
    CHECK(root != NULL, "mkdtemp");
    if (!root) return 1;

    tdsh_posix_config_t cfg = TDSH_POSIX_CONFIG_DEFAULT();
    cfg.hostname = "testhost";
    cfg.default_user = "alice";
    cfg.fs_root = root;
    CHECK(tdsh_posix_init(&cfg) == 0, "posix init");

    tdsh_session_t s;
    CHECK(tdsh_session_init(&s, "alice", false) == 0, "session init");
    CHECK(strcmp(s.home, "/home/alice") == 0, "home path");
    CHECK(tdsh_var_set(&s, "A", "7") == 0, "var set");
    CHECK(strcmp(tdsh_var_get(&s, "A"), "7") == 0, "var get");
    CHECK(tdsh_var_set(&s, "PWD", "bad") == -EPERM, "protected var");

    char logical[TDSH_MAX_PATH];
    CHECK(tdsh_path_normalize(&s, "../../root", logical, sizeof(logical)) == 0, "jail normalize");
    CHECK(strncmp(logical, "/home/alice", strlen("/home/alice")) == 0, "jail clamp");
    CHECK(tdsh_path_normalize(&s, "/root", logical, sizeof(logical)) == -EACCES, "root denied");

    static const tdsh_command_t probe = {
        "probe", "probe", "test command", cmd_probe, 0
    };
    CHECK(tdsh_register_command(&probe) == 0, "custom command register");
    CHECK(tdsh_execute_line(&s, "probe") == 0, "custom command execute");
    CHECK(strcmp(tdsh_var_get(&s, "PROBE"), "ok") == 0, "command changed session");

    CHECK(tdsh_execute_line(&s, "X=$((7 + 5 * 2))") == 0, "arithmetic assignment");
    CHECK(strcmp(tdsh_var_get(&s, "X"), "17") == 0, "arithmetic result");
    CHECK(tdsh_execute_line(&s, "Y=$(echo hello)") == 0, "command substitution");
    CHECK(strcmp(tdsh_var_get(&s, "Y"), "hello") == 0, "substitution result");

    char real[TDSH_MAX_REAL_PATH];
    CHECK(tdsh_path_to_real(&s, "~/scope.tdsh", real, sizeof(real), NULL, 0) == 0, "script real path");
    FILE *f = fopen(real, "w");
    CHECK(f != NULL, "create script");
    if (f) {
        fputs("TEMP=inside\n", f);
        fputs("I=0\n", f);
        fputs("while $I < 3\n", f);
        fputs("  I=$((I + 1))\n", f);
        fputs("endwhile\n", f);
        fputs("return 0\n", f);
        fclose(f);
        CHECK(tdsh_run_script(&s, "~/scope.tdsh", false) == 0, "isolated script");
        CHECK(tdsh_var_get(&s, "TEMP") == NULL, "script variable isolation");
    }

    tdsh_memory_stats_t st;
    tdsh_memory_get_stats(&st);
    CHECK(st.live_blocks == 0, "no live tracked core allocations after test");

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS: portable core/session/parser/script/path/memory tests\n");
    return 0;
}
