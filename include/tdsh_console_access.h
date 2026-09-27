#ifndef TDSH_CONSOLE_ACCESS_H
#define TDSH_CONSOLE_ACCESS_H

#include <stdbool.h>
#include <stdatomic.h>

/* A shared desktop cannot regain physical trust merely by disconnecting:
 * remote commands and input may still be queued. Only a reboot resets this.
 * Recovery holds a lease so a remote takeover cannot supply its passwords. */
enum { TDSH_ACCESS_LOCAL, TDSH_ACCESS_RECOVERY, TDSH_ACCESS_REMOTE };
typedef atomic_int tdsh_console_access_t;

static inline bool tdsh_access_is_physical(tdsh_console_access_t *access)
{
    return atomic_load(access) != TDSH_ACCESS_REMOTE;
}

static inline bool tdsh_access_begin_recovery(tdsh_console_access_t *access)
{
    int expected = TDSH_ACCESS_LOCAL;
    return atomic_compare_exchange_strong(access, &expected, TDSH_ACCESS_RECOVERY);
}

static inline void tdsh_access_end_recovery(tdsh_console_access_t *access)
{
    int expected = TDSH_ACCESS_RECOVERY;
    (void)atomic_compare_exchange_strong(access, &expected, TDSH_ACCESS_LOCAL);
}

static inline bool tdsh_access_mark_remote(tdsh_console_access_t *access)
{
    int expected = TDSH_ACCESS_LOCAL;
    return atomic_compare_exchange_strong(access, &expected, TDSH_ACCESS_REMOTE)
        || expected == TDSH_ACCESS_REMOTE;
}
#endif
