/*
 * tdsh_sftp_jail.h - confine an SFTP session to a directory (tinydesk
 * patch). Used by the SSH client task and by the wolfSSH file macros.
 */
#ifndef TDSH_SFTP_JAIL_H
#define TDSH_SFTP_JAIL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Confine file access of the calling task to `root` (a real path such as
 * "/fs/home/bob"); NULL removes any confinement. */
    void tdsh_sftp_jail_enter(const char *root);
    void tdsh_sftp_jail_leave(void);

/* True if the calling task may use `path`. metadata_only also allows
 * stat() of the directories above the jail root. */
    bool tdsh_sftp_allowed(const char *path, bool metadata_only);

#ifdef __cplusplus
}
#endif

#endif
