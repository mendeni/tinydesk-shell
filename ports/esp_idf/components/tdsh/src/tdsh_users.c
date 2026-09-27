#include "tdsh_espidf.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/md.h"
#include "nvs.h"

#define USER_NS "ush_users"
#define USER_KEY "db"
#define BOOT_USER_KEY "boot_user"
#define USER_DB_MAGIC 0x55534855u
#define USER_DB_VERSION 1u
#define USER_MAX 12
#define USER_SALT_LEN 16
#define USER_HASH_LEN 32
#define HASH_ROUNDS 2048

static const char *TAG = "tdsh-users";

/* The factory password root gets on a fresh device (change it with passwd). */
#define DEFAULT_ROOT_PASSWORD "TinyDesk"

typedef struct {
    uint8_t used;
    char name[TDSH_USERNAME_MAX];
    uint8_t salt[USER_SALT_LEN];
    uint8_t hash[USER_HASH_LEN];
} user_entry_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    user_entry_t entries[USER_MAX];
} user_db_t;

static user_db_t s_db;
static bool s_ready;


static void ensure_user_startup_file(const char *username)
{
    if (!username || !username[0]) return;
    char dir[TDSH_MAX_PATH + sizeof(TDSH_MOUNT_POINT) + 16];
    char path[TDSH_MAX_PATH + sizeof(TDSH_MOUNT_POINT) + 32];
    if (strcmp(username, "root") == 0) {
        snprintf(dir, sizeof(dir), "%s/root", TDSH_MOUNT_POINT);
    } else {
        snprintf(dir, sizeof(dir), "%s/home/%s", TDSH_MOUNT_POINT, username);
    }
    (void)mkdir(dir, 0755);
    char mnt_dir[TDSH_MAX_PATH + sizeof(TDSH_MOUNT_POINT) + 24];
    snprintf(mnt_dir, sizeof(mnt_dir), "%s/mnt", dir);
    (void)mkdir(mnt_dir, 0755);
    snprintf(path, sizeof(path), "%s/" TDSH_STARTUP_FILE, dir);
    struct stat st;
    if (stat(path, &st) == 0) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    /* wording that fits every board and console. */
    fprintf(f, "# Startup script of %s: runs when %s's local shell starts\n", username, username);
    fprintf(f, "# (the console, or the desktop's Terminal window). One command per line,\n");
    fprintf(f, "# e.g. wificonnect to join the saved Wi-Fi network at boot.\n");
    fclose(f);
}

int tdsh_boot_user_get(char *out, size_t out_size)
{
    if (!out || out_size < 2) return -EINVAL;
    out[0] = '\0';
    nvs_handle_t h;
    esp_err_t err = nvs_open(USER_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return -ENOENT;
    size_t len = out_size;
    err = nvs_get_str(h, BOOT_USER_KEY, out, &len);
    nvs_close(h);
    if (err != ESP_OK || out[0] == '\0' || !tdsh_user_exists(out)) {
        out[0] = '\0';
        return -ENOENT;
    }
    return 0;
}

int tdsh_boot_user_set(const char *username)
{
    if (!username || !username[0] || !tdsh_user_exists(username)) return -EINVAL;
    nvs_handle_t h;
    esp_err_t err = nvs_open(USER_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return -EIO;
    err = nvs_set_str(h, BOOT_USER_KEY, username);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK ? 0 : -EIO;
}

static bool valid_username(const char *name)
{
    if (!name || !name[0] || strlen(name) >= TDSH_USERNAME_MAX) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        if (!(isalnum(*p) || *p == '_' || *p == '-' || *p == '.')) return false;
    }
    return true;
}

static int find_user(const char *name)
{
    for (int i = 0; i < USER_MAX; ++i) {
        if (s_db.entries[i].used && strcmp(s_db.entries[i].name, name) == 0) return i;
    }
    return -1;
}

static esp_err_t save_db(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(USER_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, USER_KEY, &s_db, sizeof(s_db));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static int sha256(const void *data, size_t len, uint8_t out[USER_HASH_LEN])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return -1;
    return mbedtls_md(info, (const unsigned char *)data, len, out);
}

static int make_hash(const char *password, const uint8_t salt[USER_SALT_LEN], uint8_t out[USER_HASH_LEN])
{
    if (!password) return -1;
    size_t plen = strlen(password);
    if (plen > 128) return -1;

    uint8_t initial[USER_SALT_LEN + 128];
    memcpy(initial, salt, USER_SALT_LEN);
    memcpy(initial + USER_SALT_LEN, password, plen);
    if (sha256(initial, USER_SALT_LEN + plen, out) != 0) return -1;

    uint8_t round_buf[USER_HASH_LEN + USER_SALT_LEN];
    for (unsigned i = 1; i < HASH_ROUNDS; ++i) {
        memcpy(round_buf, out, USER_HASH_LEN);
        memcpy(round_buf + USER_HASH_LEN, salt, USER_SALT_LEN);
        if (sha256(round_buf, sizeof(round_buf), out) != 0) return -1;
    }
    return 0;
}

static bool hash_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= a[i] ^ b[i];
    return diff == 0;
}

static esp_err_t set_password_idx(int idx, const char *password)
{
    if (idx < 0 || idx >= USER_MAX || !password || strlen(password) < 1) return ESP_ERR_INVALID_ARG;
    esp_fill_random(s_db.entries[idx].salt, USER_SALT_LEN);
    if (make_hash(password, s_db.entries[idx].salt, s_db.entries[idx].hash) != 0) return ESP_FAIL;
    return save_db();
}

static bool check_password(int idx, const char *password)
{
    uint8_t hash[USER_HASH_LEN];
    if (make_hash(password, s_db.entries[idx].salt, hash) != 0) return false;
    return hash_equal(hash, s_db.entries[idx].hash, USER_HASH_LEN);
}

esp_err_t tdsh_users_init(void)
{
    if (s_ready) return ESP_OK;
    memset(&s_db, 0, sizeof(s_db));

    nvs_handle_t h;
    esp_err_t err = nvs_open(USER_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    size_t len = sizeof(s_db);
    err = nvs_get_blob(h, USER_KEY, &s_db, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(s_db) || s_db.magic != USER_DB_MAGIC || s_db.version != USER_DB_VERSION) {
        memset(&s_db, 0, sizeof(s_db));
        s_db.magic = USER_DB_MAGIC;
        s_db.version = USER_DB_VERSION;
        s_db.entries[0].used = 1;
        snprintf(s_db.entries[0].name, sizeof(s_db.entries[0].name), "root");
        s_db.count = 1;
        esp_fill_random(s_db.entries[0].salt, USER_SALT_LEN);
        if (make_hash(DEFAULT_ROOT_PASSWORD, s_db.entries[0].salt, s_db.entries[0].hash) != 0) return ESP_FAIL;
        err = save_db();
        if (err != ESP_OK) return err;
        ESP_LOGW(TAG, "Created root user with the factory password '" DEFAULT_ROOT_PASSWORD "'; change it with passwd");
    }

    s_ready = true;
    for (int i = 0; i < USER_MAX; ++i) {
        if (s_db.entries[i].used) ensure_user_startup_file(s_db.entries[i].name);
    }
    return ESP_OK;
}

bool tdsh_user_exists(const char *username)
{
    return s_ready && find_user(username) >= 0;
}

bool tdsh_user_authenticate(const char *username, const char *password)
{
    int idx = find_user(username);
    if (idx < 0 || !password) return false;
    return check_password(idx, password);
}

bool tdsh_remote_access_ready(void)
{
    int idx = find_user("root");
    uint8_t hash[USER_HASH_LEN];
    if (!s_ready || idx < 0 ||
        make_hash(DEFAULT_ROOT_PASSWORD, s_db.entries[idx].salt, hash) != 0) return false;
    return !hash_equal(hash, s_db.entries[idx].hash, USER_HASH_LEN);
}

bool tdsh_user_authenticate_remote(const char *username, const char *password)
{
    return tdsh_remote_access_ready() && tdsh_user_authenticate(username, password);
}

static int read_password(const char *prompt, char *out, size_t out_size)
{
    int n = tdsh_console_readline(prompt, out, out_size, false);
    if (n <= 0) return 1;
    return 0;
}

static int remove_tree(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return errno == ENOENT ? 0 : -errno;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0 ? 0 : -errno;
    DIR *d = opendir(path);
    if (!d) return -errno;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[TDSH_MAX_PATH + sizeof(TDSH_MOUNT_POINT) + 64];
        int n = snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        if (n < 0 || n >= (int)sizeof(child)) { rc = -ENAMETOOLONG; break; }
        rc = remove_tree(child);
        if (rc != 0) break;
    }
    closedir(d);
    if (rc == 0 && rmdir(path) != 0) rc = -errno;
    return rc;
}

int tdsh_cmd_users(tdsh_session_t *session, int argc, char **argv)
{
    (void)session; (void)argc; (void)argv;
    for (int i = 0; i < USER_MAX; ++i) if (s_db.entries[i].used) printf("%s\n", s_db.entries[i].name);
    return 0;
}

int tdsh_cmd_useradd(tdsh_session_t *session, int argc, char **argv)
{
    if (strcmp(session->username, "root") != 0) { printf("tdsh: permission denied: root required\n"); return 1; }
    if (argc != 2 || !valid_username(argv[1])) { printf("usage: useradd <username>\n"); return 2; }
    if (find_user(argv[1]) >= 0) { printf("tdsh: user '%s' already exists\n", argv[1]); return 1; }

    int free_idx = -1;
    for (int i = 0; i < USER_MAX; ++i) if (!s_db.entries[i].used) { free_idx = i; break; }
    if (free_idx < 0) { printf("tdsh: user database is full\n"); return 1; }

    char p1[129], p2[129];
    printf("Creating user %s\n", argv[1]);
    if (read_password("New password: ", p1, sizeof(p1)) || read_password("Retype new password: ", p2, sizeof(p2))) return 1;
    if (strcmp(p1, p2) != 0) { printf("Passwords do not match.\n"); return 1; }

    user_entry_t *u = &s_db.entries[free_idx];
    memset(u, 0, sizeof(*u));
    u->used = 1;
    snprintf(u->name, sizeof(u->name), "%s", argv[1]);
    s_db.count++;
    esp_fill_random(u->salt, USER_SALT_LEN);
    if (make_hash(p1, u->salt, u->hash) != 0 || save_db() != ESP_OK) {
        memset(u, 0, sizeof(*u)); s_db.count--; printf("tdsh: failed to save user\n"); return 1;
    }

    char home[TDSH_MAX_PATH + sizeof(TDSH_MOUNT_POINT) + 8];
    snprintf(home, sizeof(home), "%s/home/%s", TDSH_MOUNT_POINT, argv[1]);
    (void)mkdir(home, 0755);
    ensure_user_startup_file(argv[1]);
    printf("Profile %s created. Startup: ~/" TDSH_STARTUP_FILE "\n", argv[1]);
    return 0;
}

int tdsh_cmd_userdel(tdsh_session_t *session, int argc, char **argv)
{
    if (strcmp(session->username, "root") != 0) { printf("tdsh: permission denied: root required\n"); return 1; }
    if (argc < 2 || argc > 3) { printf("usage: userdel <username> [-f]\n"); return 2; }
    if (!strcmp(argv[1], "root")) { printf("tdsh: root user cannot be deleted\n"); return 1; }
    int idx = find_user(argv[1]);
    if (idx < 0) { printf("tdsh: no user named '%s'\n", argv[1]); return 1; }

    bool force = argc == 3 && strcmp(argv[2], "-f") == 0;
    if (!force) {
        char pass[129];
        if (read_password("Password for user being deleted: ", pass, sizeof(pass))) return 1;
        if (!tdsh_user_authenticate(argv[1], pass)) { printf("Incorrect password.\n"); return 1; }
    }

    /* Remove this user's SMB mappings/credentials before the username can be reused. */
    tdsh_netmount_remove_owner(argv[1]);
    memset(&s_db.entries[idx], 0, sizeof(s_db.entries[idx]));
    if (s_db.count) s_db.count--;
    if (save_db() != ESP_OK) { printf("tdsh: failed to update user database\n"); return 1; }
    char home[TDSH_MAX_PATH + sizeof(TDSH_MOUNT_POINT) + 8];
    snprintf(home, sizeof(home), "%s/home/%s", TDSH_MOUNT_POINT, argv[1]);
    (void)remove_tree(home);
    printf("User %s deleted successfully.\n", argv[1]);
    return 0;
}

int tdsh_cmd_login(tdsh_session_t *session, int argc, char **argv)
{
    if (!session->interactive) { printf("tdsh: login requires an interactive shell\n"); return 1; }
    if (argc != 2) { printf("usage: login <username>\n"); return 2; }
    if (!tdsh_user_exists(argv[1])) { printf("No user found named '%s'.\n", argv[1]); return 1; }
    char pass[129];
    if (read_password("Password: ", pass, sizeof(pass))) return 1;
    if (!tdsh_user_authenticate(argv[1], pass)) { printf("Incorrect password.\n"); return 1; }
    if (tdsh_session_set_user(session, argv[1]) != 0) return 1;
    printf("Login successful.\n");
    if (tdsh_is_local_console_task()) {
        (void)tdsh_run_user_startup(session);
    }
    return 0;
}

int tdsh_cmd_logout(tdsh_session_t *session, int argc, char **argv)
{
    (void)argv;
    if (!session->interactive) { printf("tdsh: logout requires an interactive shell\n"); return 1; }
    if (argc != 1) { printf("usage: logout\n"); return 2; }
    session->logout_requested = true;
    printf("Logged out.\n");
    return 0;
}

int tdsh_cmd_passwd(tdsh_session_t *session, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!session->interactive) { printf("tdsh: passwd requires an interactive shell\n"); return 1; }
    int idx = find_user(session->username);
    if (idx < 0) return 1;
    char oldp[129], p1[129], p2[129];
    if (read_password("Old password: ", oldp, sizeof(oldp))) return 1;
    if (!tdsh_user_authenticate(session->username, oldp)) { printf("Incorrect old password.\n"); return 1; }
    if (read_password("New password: ", p1, sizeof(p1)) || read_password("Retype new password: ", p2, sizeof(p2))) return 1;
    if (strcmp(p1, p2) != 0) { printf("Passwords do not match.\n"); return 1; }
    if (set_password_idx(idx, p1) != ESP_OK) { printf("tdsh: failed to save password\n"); return 1; }
    printf("Password updated.\n");
    return 0;
}


static int recover_root_password(void)
{
    int root_idx = find_user("root");
    if (root_idx < 0) {
        printf("rootrecover: root account is missing\n");
        return 1;
    }

    char p1[129];
    char p2[129];
    printf("Physical USB Serial/JTAG root password recovery.\n");
    if (read_password("New root password: ", p1, sizeof(p1)) ||
        read_password("Retype new root password: ", p2, sizeof(p2))) {
        memset(p1, 0, sizeof(p1));
        memset(p2, 0, sizeof(p2));
        return 1;
    }
    if (strlen(p1) < 8) {
        printf("rootrecover: password must be at least 8 characters\n");
        memset(p1, 0, sizeof(p1));
        memset(p2, 0, sizeof(p2));
        return 1;
    }
    if (strcmp(p1, p2) != 0) {
        printf("Passwords do not match.\n");
        memset(p1, 0, sizeof(p1));
        memset(p2, 0, sizeof(p2));
        return 1;
    }

    esp_err_t err = set_password_idx(root_idx, p1);
    memset(p1, 0, sizeof(p1));
    memset(p2, 0, sizeof(p2));
    if (err != ESP_OK) {
        printf("rootrecover: failed to save root password\n");
        return 1;
    }

    printf("Root password reset. Run 'login root', then 'bootuser root' if desired.\n");
    return 0;
}

int tdsh_cmd_rootrecover(tdsh_session_t *session, int argc, char **argv)
{
    (void)argv;
    if (argc != 1) { printf("usage: rootrecover\n"); return 2; }
    if (!session || !session->interactive || !tdsh_console_begin_recovery()) {
        printf("rootrecover: physical console only; reboot locally after remote desktop use\n");
        return 1;
    }
    int result = recover_root_password();
    tdsh_console_end_recovery();
    return result;
}

int tdsh_cmd_bootuser(tdsh_session_t *session, int argc, char **argv)
{
    char current[TDSH_USERNAME_MAX];
    bool have_current = tdsh_boot_user_get(current, sizeof(current)) == 0;
    if (argc == 1) {
        printf("Boot user: %s\n", have_current ? current : "not configured");
        return 0;
    }
    if (argc != 2) { printf("usage: bootuser [username]\n"); return 2; }
    if (strcmp(session->username, "root") != 0) {
        printf("tdsh: permission denied: root required\n"); return 1;
    }
    if (!tdsh_user_exists(argv[1])) {
        printf("No user found named '%s'.\n", argv[1]); return 1;
    }
    if (tdsh_boot_user_set(argv[1]) != 0) {
        printf("tdsh: failed to save boot user\n"); return 1;
    }
    printf("Boot user set to %s. It will be used on the next reboot.\n", argv[1]);
    return 0;
}
