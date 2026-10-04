#define _GNU_SOURCE
#include "tdsh_posix.h"
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
int tdsh_posix_nano_impl(tdsh_session_t *session, int argc, char **argv);

static int usage(const char *name)
{
    const tdsh_command_t *c = tdsh_command_find(name);
    if (c)
        printf("usage: %s\n", c->usage);
    return 2;
}

static int cmd_hostpath(tdsh_session_t *s, int argc, char **argv)
{
    if (argc > 2)
        return usage("hostpath");
    char real[TDSH_MAX_REAL_PATH], logical[TDSH_MAX_PATH];
    const char *p = argc == 2 ? argv[1] : s->cwd;
    int rc = tdsh_path_to_real(s, p, real, sizeof(real), logical, sizeof(logical));
    if (rc)
    {
        printf("hostpath: unable to resolve '%s' (%d)\n", p, rc);
        return 1;
    }
    printf("%s -> %s\n", logical, real);
    return 0;
}

static int cmd_write(tdsh_session_t *s, int argc, char **argv)
{
    if (argc < 2)
        return usage("write");
    char real[TDSH_MAX_REAL_PATH], logical[TDSH_MAX_PATH];
    int rc = tdsh_path_to_real(s, argv[1], real, sizeof(real), logical, sizeof(logical));
    if (rc)
    {
        printf("write: invalid path\n");
        return 1;
    }
    FILE *f = fopen(real, "w");
    if (!f)
    {
        printf("write: %s: %s\n", logical, strerror(errno));
        return 1;
    }
    if (argc > 2)
    {
        for (int i = 2; i < argc; i++)
            fprintf(f, "%s%s", argv[i], i + 1 < argc ? " " : "");
        fputc('\n', f);
        fclose(f);
        return 0;
    }
    if (!s->interactive)
    {
        fclose(f);
        printf("write: interactive terminal required\n");
        return 1;
    }
    fclose(f);
    printf("TinyDesk Shell line writer: %s\nEnter lines; a line containing only .save finishes. .quit discards.\n", logical);
    char tmp[TDSH_MAX_REAL_PATH + 16];
    snprintf(tmp, sizeof(tmp), "%s.tmp", real);
    FILE *out = fopen(tmp, "w");
    if (!out)
    {
        printf("write: %s\n", strerror(errno));
        return 1;
    }
    char line[TDSH_MAX_LINE + 2];
    for (;;)
    {
        fputs("write> ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin))
        {
            fclose(out);
            unlink(tmp);
            return 1;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (!strcmp(line, ".quit"))
        {
            fclose(out);
            unlink(tmp);
            puts("Changes discarded.");
            return 0;
        }
        if (!strcmp(line, ".save"))
        {
            fclose(out);
            if (rename(tmp, real) != 0)
            {
                printf("write: save: %s\n", strerror(errno));
                unlink(tmp);
                return 1;
            }
            printf("Saved %s\n", logical);
            return 0;
        }
        fprintf(out, "%s\n", line);
        fflush(out);
    }
}

static long tz_offset(tdsh_session_t *s)
{
    char real[TDSH_MAX_REAL_PATH];
    if (tdsh_path_to_real(s, "~/.tdsh_tz", real, sizeof(real), NULL, 0) != 0)
        return 0;
    FILE *f = fopen(real, "r");
    if (!f)
        return 0;
    long v = 0;
    if (fscanf(f, "%ld", &v) != 1)
        v = 0;
    fclose(f);
    return v;
}
static int cmd_tz(tdsh_session_t *s, int argc, char **argv)
{
    if (argc > 2)
        return usage("tz");
    if (argc == 2)
    {
        int sign = 1, h = 0, m = 0;
        const char *p = argv[1];
        if (*p == '-')
        {
            sign = -1;
            p++;
        }
        else if (*p == '+')
            p++;
        if (sscanf(p, "%d:%d", &h, &m) != 2 || h > 14 || m > 59)
        {
            puts("tz: expected [+|-]HH:MM");
            return 1;
        }
        long off = sign * (h * 3600L + m * 60L);
        char real[TDSH_MAX_REAL_PATH];
        if (tdsh_path_to_real(s, "~/.tdsh_tz", real, sizeof(real), NULL, 0) != 0)
            return 1;
        FILE *f = fopen(real, "w");
        if (!f)
            return 1;
        fprintf(f, "%ld\n", off);
        fclose(f);
    }
    long off = tz_offset(s);
    long a = labs(off);
    printf("UTC%c%02ld:%02ld\n", off < 0 ? '-' : '+', a / 3600, (a % 3600) / 60);
    return 0;
}
static int local_tm(tdsh_session_t *s, struct tm *out)
{
    time_t t = time(NULL) + (time_t)tz_offset(s);
    return gmtime_r(&t, out) ? 0 : 1;
}
static int cmd_date(tdsh_session_t *s, int argc, char **argv)
{
    (void)argv;
    if (argc != 1)
        return usage("date");
    struct tm tm;
    if (local_tm(s, &tm))
        return 1;
    char b[96];
    strftime(b, sizeof(b), "%a %d-%m-%Y %H:%M:%S", &tm);
    printf("%s\n", b);
    return 0;
}
static int cmd_cal(tdsh_session_t *s, int argc, char **argv)
{
    (void)argv;
    if (argc != 1)
        return usage("cal");
    struct tm tm;
    if (local_tm(s, &tm))
        return 1;
    int y = tm.tm_year + 1900, m = tm.tm_mon + 1;
    struct tm first = {.tm_year = y - 1900, .tm_mon = m - 1, .tm_mday = 1};
    mktime(&first);
    int days = 31;
    if (m == 4 || m == 6 || m == 9 || m == 11)
        days = 30;
    else if (m == 2)
    {
        int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        days = leap ? 29 : 28;
    }
    static const char *mn[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    printf("     %s %d\nSu Mo Tu We Th Fr Sa\n", mn[m - 1], y);
    for (int i = 0; i < first.tm_wday; i++)
        printf("   ");
    for (int d = 1; d <= days; d++)
    {
        printf("%2d%c", d, ((first.tm_wday + d) % 7) == 0 ? '\n' : ' ');
    }
    if ((first.tm_wday + days) % 7)
        putchar('\n');
    return 0;
}

static int cmd_users(tdsh_session_t *s, int argc, char **argv)
{
    (void)argv;
    if (argc != 1)
        return usage("users");
    printf("%s  (active POSIX SDK session)\n", s->username);
    puts("Note: persistent multi-user authentication is provided by platform/auth modules; the POSIX preview does not impersonate Linux users.");
    return 0;
}
static int cmd_auth_unavailable(tdsh_session_t *s, int argc, char **argv)
{
    (void)s;
    (void)argc;
    printf("%s: persistent TinyDesk Shell auth backend is not enabled in the POSIX host preview\n", argv[0]);
    return 1;
}

static int cmd_ifconfig(tdsh_session_t *s, int argc, char **argv)
{
    (void)s;
    (void)argv;
    if (argc != 1)
        return usage("ifconfig");
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) != 0)
    {
        perror("ifconfig");
        return 1;
    }
    for (struct ifaddrs *i = ifs; i; i = i->ifa_next)
    {
        if (!i->ifa_addr)
            continue;
        int fam = i->ifa_addr->sa_family;
        if (fam != AF_INET && fam != AF_INET6)
            continue;
        char a[INET6_ADDRSTRLEN];
        void *src = fam == AF_INET ? (void *)&((struct sockaddr_in *)i->ifa_addr)->sin_addr : (void *)&((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr;
        if (inet_ntop(fam, src, a, sizeof(a)))
            printf("%-10s %s %s\n", i->ifa_name, fam == AF_INET ? "inet " : "inet6", a);
    }
    freeifaddrs(ifs);
    return 0;
}
static int cmd_ping(tdsh_session_t *s, int argc, char **argv)
{
    (void)s;
    if (argc < 2)
        return usage("ping");
    char *av[8];
    int n = 0;
    int has_c = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-c"))
            has_c = 1;
    av[n++] = "ping";
    if (!has_c)
    {
        av[n++] = "-c";
        av[n++] = "4";
    }
    for (int i = 1; i < argc && n < 7; i++)
        av[n++] = argv[i];
    av[n] = NULL;
    pid_t pid;
    int rc = posix_spawnp(&pid, "ping", NULL, NULL, av, environ);
    if (rc)
    {
        printf("ping: %s\n", strerror(rc));
        return 1;
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0)
        return 1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}
static int cmd_caps(tdsh_session_t *s, int argc, char **argv)
{
    (void)s;
    (void)argv;
    if (argc != 1)
        return usage("capabilities");
    puts("POSIX host capabilities:\n  portable shell/uScript/filesystem: yes\n  colors/ANSI terminal: auto-detected\n  TAB/history/cursor editing: yes\n  isolated TinyDesk Shell filesystem: yes (default)\n  write/nano: yes\n  date/tz/cal: yes\n  ifconfig/ping: yes\n  persistent TinyDesk Shell auth: not yet enabled\n  ESP Wi-Fi/W6100/GPIO/hwtest/OTA: ESP-IDF port only\n  SSH/FTP/SMB servers: optional platform modules");
    return 0;
}

static int cmd_nano(tdsh_session_t *s, int argc, char **argv)
{
    struct termios oldt, raw;
    bool changed = false;
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &oldt) == 0)
    {
        raw = oldt;
        cfmakeraw(&raw);
        raw.c_oflag |= OPOST;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0)
            changed = true;
    }
    int rc = tdsh_posix_nano_impl(s, argc, argv);
    if (changed)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
    return rc;
}

static const tdsh_command_t cmds[] = {
    {"write", "write <file> [text ...]", "Line writer or direct file writer", cmd_write, 0},
    {"nano", "nano <file>", "Full-screen VT100 nano-style editor", cmd_nano, TDSH_CMD_INTERACTIVE},
    {"users", "users", "List TinyDesk Shell users/session identity", cmd_users, 0},
    {"useradd", "useradd <username>", "Create a TinyDesk Shell user (auth module)", cmd_auth_unavailable, TDSH_CMD_ROOT_ONLY},
    {"userdel", "userdel <username> [-f]", "Delete a TinyDesk Shell user (auth module)", cmd_auth_unavailable, TDSH_CMD_ROOT_ONLY},
    {"login", "login <username>", "Login as another TinyDesk Shell user (auth module)", cmd_auth_unavailable, TDSH_CMD_INTERACTIVE},
    {"logout", "logout", "Logout current TinyDesk Shell session (auth module)", cmd_auth_unavailable, TDSH_CMD_INTERACTIVE},
    {"passwd", "passwd", "Change TinyDesk Shell password (auth module)", cmd_auth_unavailable, TDSH_CMD_INTERACTIVE},
    {"bootuser", "bootuser [username]", "Show/set boot identity (platform auth module)", cmd_auth_unavailable, 0},
    {"tz", "tz [[+|-]HH:MM]", "Show or set per-user timezone offset", cmd_tz, 0},
    {"date", "date", "Show current date/time using TinyDesk Shell timezone", cmd_date, 0},
    {"cal", "cal", "Print current month calendar", cmd_cal, 0},
    {"ifconfig", "ifconfig", "Show POSIX network interfaces", cmd_ifconfig, 0},
    {"ping", "ping [-c count] <host/address ...>", "Send ICMP echo using host ping utility", cmd_ping, 0},
    {"hostpath", "hostpath [path]", "Show real host path for a TinyDesk Shell path", cmd_hostpath, 0},
    {"capabilities", "capabilities", "Show capabilities of this platform port", cmd_caps, 0},
};
int tdsh_posix_register_commands(void)
{
    return tdsh_register_commands(cmds, sizeof(cmds) / sizeof(cmds[0]));
}
