#include "tdsh_console_access.h"
#include <stdio.h>

/* Exercise the actual shared-console policy, including both takeover orders.
 * Avoid assert(): release builds must execute every security check. */
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "line %d: %s\n", __LINE__, #expr); return 1; } } while (0)
int main(void)
{
    tdsh_console_access_t access = TDSH_ACCESS_LOCAL;
    CHECK(tdsh_access_is_physical(&access));
    CHECK(tdsh_access_begin_recovery(&access));
    CHECK(!tdsh_access_begin_recovery(&access));
    CHECK(!tdsh_access_mark_remote(&access)); /* takeover during password prompt */
    tdsh_access_end_recovery(&access);
    CHECK(tdsh_access_begin_recovery(&access)); /* retry after failed/cancelled recovery */
    tdsh_access_end_recovery(&access);
    CHECK(tdsh_access_mark_remote(&access));
    CHECK(!tdsh_access_is_physical(&access));
    CHECK(!tdsh_access_begin_recovery(&access));
    CHECK(tdsh_access_mark_remote(&access)); /* reconnect */
    tdsh_access_end_recovery(&access); /* cannot accidentally restore trust */
    CHECK(!tdsh_access_begin_recovery(&access)); /* queued input after disconnect */
    CHECK(!tdsh_access_is_physical(&access));
    puts("Console recovery and takeover checks passed");
    return 0;
}
