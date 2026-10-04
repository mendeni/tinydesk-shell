#include "tdsh_posix.h"

#include <stdio.h>

static int cmd_app(tdsh_session_t *session, int argc, char **argv)
{
    (void)session;
    printf("application command reached; argc=%d\n", argc);
    for (int i = 1; i < argc; ++i)
        printf("arg[%d]=%s\n", i, argv[i]);
    return 0;
}

static const tdsh_command_t app_commands[] = {
    {"app", "app [args ...]", "Example SDK application command", cmd_app, 0},
};

int main(void)
{
    tdsh_posix_config_t cfg = TDSH_POSIX_CONFIG_DEFAULT();

    int rc = tdsh_posix_init(&cfg);
    if (rc != 0)
    {
        fprintf(stderr, "tdsh_posix_init failed: %d\n", rc);
        return 1;
    }
    rc = tdsh_register_commands(app_commands,
                                sizeof(app_commands) / sizeof(app_commands[0]));
    if (rc != 0)
    {
        fprintf(stderr, "app command registration failed: %d\n", rc);
        return 1;
    }
    return tdsh_posix_run_interactive();
}
