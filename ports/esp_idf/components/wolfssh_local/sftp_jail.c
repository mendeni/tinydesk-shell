/*
 * sftp_jail.c - keep a non-root SFTP user inside their home directory.
 *
 *
 * wolfSSH's SFTP server has a "default path" but no confinement: any
 * absolute path reaches the whole filesystem. The file macros in
 * include/wolfssh/port.h are routed through tdsh_sftp_allowed(), which
 * checks the calling task's jail (set by the SSH client task for the user
 * that logged in). Paths are normalised first, so "..", "." and "//"
 * cannot escape. Root is not jailed.
 */
#include "tdsh_sftp_jail.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SLOTS 4
#define ROOT_MAX 160

static struct {
    TaskHandle_t task;
    char root[ROOT_MAX];
} s_slots[SLOTS];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void tdsh_sftp_jail_enter(const char *root)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    tdsh_sftp_jail_leave();
    if (!root) return;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < SLOTS; i++) {
        if (s_slots[i].task) continue;
        s_slots[i].task = me;
        snprintf(s_slots[i].root, sizeof(s_slots[i].root), "%s", root);
        break;
    }
    taskEXIT_CRITICAL(&s_mux);
}

void tdsh_sftp_jail_leave(void)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < SLOTS; i++)
        if (s_slots[i].task == me) s_slots[i].task = NULL;
    taskEXIT_CRITICAL(&s_mux);
}

/* Copy of the calling task's jail root, or false when it has none. */
static bool my_root(char *out, size_t cap)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    bool found = false;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < SLOTS && !found; i++) {
        if (s_slots[i].task != me) continue;
        snprintf(out, cap, "%s", s_slots[i].root);
        found = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    return found;
}

/* Resolve ".", ".." and repeated slashes of an absolute path. */
static bool normalise(const char *in, char *out, size_t cap)
{
    if (!in || in[0] != '/') return false;
    size_t len = 0;
    out[0] = '\0';
    const char *p = in;
    while (*p) {
        while (*p == '/') p++;
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - seg);
        if (n == 0 || (n == 1 && seg[0] == '.')) continue;
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            while (len > 0 && out[len - 1] != '/') len--;    /* drop last part */
            if (len > 0) len--;                               /* and its slash */
            out[len] = '\0';
            continue;
        }
        if (len + 1 + n + 1 > cap) return false;
        out[len++] = '/';
        memcpy(out + len, seg, n);
        len += n;
        out[len] = '\0';
    }
    if (len == 0) {
        if (cap < 2) return false;
        out[0] = '/';
        out[1] = '\0';
    }
    return true;
}

bool tdsh_sftp_allowed(const char *path, bool metadata_only)
{
    char root[ROOT_MAX], norm[ROOT_MAX + 96];
    if (!my_root(root, sizeof(root))) return true;       /* not jailed */
    if (!normalise(path, norm, sizeof(norm))) return false;
    size_t rl = strlen(root);
    if (strncmp(norm, root, rl) == 0 && (norm[rl] == '\0' || norm[rl] == '/')) return true;
    /* Looking at the directories above the home (to show where it is) is
     * harmless; listing or changing them is not. */
    if (metadata_only) {
        size_t nl = strlen(norm);
        if (strcmp(norm, "/") == 0) return true;
        if (nl < rl && strncmp(root, norm, nl) == 0 && root[nl] == '/') return true;
    }
    return false;
}
