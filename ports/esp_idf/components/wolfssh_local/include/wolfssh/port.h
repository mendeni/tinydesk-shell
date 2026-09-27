#ifndef TDSH_WOLFSSH_PORT_OVERLAY_H
#define TDSH_WOLFSSH_PORT_OVERLAY_H

/*
 * ESP-IDF / LittleFS compatibility overlay for wolfSSH v1.5.0.
 *
 * wolfSSH's generic POSIX SFTP port uses chmod() and fchmod(). ESP-IDF 5.3.1
 * does not provide usable POSIX mode semantics for LittleFS, and resolving
 * fchmod through Newlib's libnosys pulls dir.o which conflicts with ESP-IDF
 * VFS implementations of opendir/readdir/mkdir/getcwd/etc.
 *
 * Include the real upstream port first, then override only the permission
 * mutators. SFTP mode requests become successful no-ops, which is appropriate
 * for a filesystem that does not implement Unix permission bits.
 *
 * tinydesk patch: the path-taking file macros also go through
 * tdsh_sftp_allowed(), which keeps a non-root SFTP user inside their home
 * (see sftp_jail.c). Denied operations fail with EACCES.
 */
#include_next <wolfssh/port.h>

#ifdef ESP_PLATFORM
    #ifdef WCHMOD
        #undef WCHMOD
    #endif
    #define WCHMOD(fs, f, m) \
        ((void)(fs), (void)(f), (void)(m), 0)

    #ifdef WFCHMOD
        #undef WFCHMOD
    #endif
    #define WFCHMOD(fs, fd, m) \
        ((void)(fs), (void)(fd), (void)(m), 0)

    #if defined(WOLFSSH_SFTP) || defined(WOLFSSH_SCP)
        #include <errno.h>
        #include "tdsh_sftp_jail.h"

        #define TDSH_JAIL_DENY(ret) (errno = EACCES, (ret))

        #undef WOPEN
        #define WOPEN(fs, f, m, p) \
            ((void)(fs), tdsh_sftp_allowed((f), false) ? open((f), (m), (p)) : TDSH_JAIL_DENY(-1))

        #undef WFOPEN
        #define WFOPEN(fs, f, fn, m) \
            ((void)(fs), tdsh_sftp_allowed((fn), false) ? wfopen((f), (fn), (m)) \
                                                          : (*(f) = NULL, TDSH_JAIL_DENY(1)))

        #undef WSTAT
        #define WSTAT(fs, p, b) \
            ((void)(fs), tdsh_sftp_allowed((p), true) ? stat((p), (b)) : TDSH_JAIL_DENY(-1))

        #undef WLSTAT
        #define WLSTAT(fs, p, b) \
            ((void)(fs), tdsh_sftp_allowed((p), true) ? stat((p), (b)) : TDSH_JAIL_DENY(-1))

        #undef WREMOVE
        #define WREMOVE(fs, d) \
            ((void)(fs), tdsh_sftp_allowed((d), false) ? remove((d)) : TDSH_JAIL_DENY(-1))

        #undef WRENAME
        #define WRENAME(fs, o, n) \
            ((void)(fs), tdsh_sftp_allowed((o), false) && tdsh_sftp_allowed((n), false) \
                 ? rename((o), (n)) : TDSH_JAIL_DENY(-1))

        #undef WMKDIR
        #define WMKDIR(fs, p, m) \
            ((void)(fs), tdsh_sftp_allowed((p), false) ? mkdir((p), (m)) : TDSH_JAIL_DENY(-1))

        #undef WRMDIR
        #define WRMDIR(fs, d) \
            ((void)(fs), tdsh_sftp_allowed((d), false) ? rmdir((d)) : TDSH_JAIL_DENY(-1))

        #ifndef NO_WOLFSSH_DIR
            #undef WOPENDIR
            #define WOPENDIR(fs, h, c, d) \
                ((void)(fs), (void)(h), tdsh_sftp_allowed((d), false) \
                     ? ((*(c) = opendir((d))) == NULL) : (*(c) = NULL, TDSH_JAIL_DENY(1)))
        #endif
    #endif
#endif

#endif /* TDSH_WOLFSSH_PORT_OVERLAY_H */
