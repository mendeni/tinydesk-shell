/*
 * tdsh_board.h - board configuration: GPIO pins and other hardware
 * settings of the board the firmware runs on, kept out of the source code.
 *
 * Settings are "key = value" lines ('#' starts a comment), for example
 *
 *     rs485.1.uart = 1
 *     rs485.1.tx   = 16
 *     eth.chip     = w6100
 *
 * Where a value comes from, highest first:
 *   1. the device file (tdsh_board_file(), /fs/etc/board.conf on ESP-IDF):
 *      edit it on the board, then restart;
 *   2. the configuration built into the firmware (the application passes
 *      its text to tdsh_board_load(); tinydesk embeds its port's
 *      board.conf, or board.example.conf when there is none);
 *   3. the default the caller of tdsh_board_int() / _str() gives, which
 *      normally means "not configured".
 *
 * Keys are lower case: letters, digits, '.', '_' and '-'. Every module that
 * needs hardware reads its own keys (documented next to the module and in
 * board.example.conf), so a new module adds keys, not code here.
 *
 * Load once at start-up, before the modules that use it; the getters may
 * then be called from any task (the table does not change until the next
 * load or set).
 */
#ifndef TDSH_BOARD_H
#define TDSH_BOARD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TDSH_BOARD_KEY_MAX   40      /* longest key + 1 */
#define TDSH_BOARD_VALUE_MAX 96    /* longest value + 1 */

typedef enum
{
    TDSH_BOARD_UNSET = 0,          /* not in either configuration */
    TDSH_BOARD_BUILTIN,            /* from the firmware's built-in configuration */
    TDSH_BOARD_FILE,               /* from the device file */
} tdsh_board_origin_t;

/* Parse configuration text: fn() gets every setting in order (an empty value
 * means "unset this key"). Returns 0, or the number of the first line that is
 * not "key = value", a comment or blank (those lines are skipped). Needs no
 * memory and works anywhere (used by the host tests). */
int tdsh_board_parse(const char *text,
                     void (*fn)(const char *key, const char *value, int line, void *user),
                     void *user);

/* Load the built-in text (may be NULL) and then the device file at
 * file_path (may be NULL or missing). Returns 0, or the first bad line of
 * the device file (the rest still loads). Replaces anything loaded before. */
int tdsh_board_load(const char *builtin_text, const char *file_path);

/* Values; NULL / def when the key is not set. */
const char *tdsh_board_get(const char *key);
int tdsh_board_int(const char *key, int def);
bool tdsh_board_bool(const char *key, bool def);   /* yes/no, true/false, on/off, 1/0 */
tdsh_board_origin_t tdsh_board_origin(const char *key);

/* All loaded settings in load order, for listing. */
int tdsh_board_count(void);
bool tdsh_board_at(int index, const char **key, const char **value, tdsh_board_origin_t *origin);

/* The built-in text and the device file path given to the last load. */
const char *tdsh_board_builtin(void);
const char *tdsh_board_file(void);

/* Change a setting in the device file (value NULL or "" removes the line),
 * keeping its comments and the other lines, and in the loaded table.
 * Modules read their pins at start-up, so a change applies after a
 * restart. Returns 0 or a negative errno. */
int tdsh_board_set(const char *key, const char *value);

/* Settings that come only from the configuration built into the firmware.
 * Firmware built without them (an official release) would lose them, so
 * save them in the device file before installing it. */
int tdsh_board_unsaved(void);

/* Write every setting that comes from the built-in configuration into the
 * device file (with tdsh_board_set(): the file's comments and other lines
 * stay, settings the file changes or removes stay as the file has them).
 * Returns how many were written, or a negative errno. */
int tdsh_board_save_builtin(void);

#ifdef __cplusplus
}
#endif

#endif
