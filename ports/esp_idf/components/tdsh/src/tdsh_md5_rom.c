/*
 * tdsh_md5_rom.c - MD5Init/MD5Update/MD5Final for libsmb2 on the classic
 * ESP32 (SMB mounts, `netmount`).
 *
 * libsmb2 leaves its own md5.c out on ESP_PLATFORM and expects these names
 * from the ROM. The ESP32-C6's ROM tables export them; the ESP32's export the
 * same ROM code only as esp_rom_md5_*. libsmb2's struct MD5Context has the
 * ROM context's layout (4 + 2 + 16 words).
 */
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32
#include <stddef.h>
#include <stdint.h>

#include "esp_rom_md5.h"

_Static_assert(sizeof(md5_context_t) == 88, "MD5 context layout");

void MD5Init(md5_context_t *ctx)
{
    esp_rom_md5_init(ctx);
}
void MD5Update(md5_context_t *ctx, const unsigned char *buf, unsigned len)
{
    esp_rom_md5_update(ctx, buf, len);
}
void MD5Final(unsigned char digest[16], md5_context_t *ctx)
{
    esp_rom_md5_final(digest, ctx);
}
#endif
