#ifndef TDSH_WOLFSSL_USER_SETTINGS_H
#define TDSH_WOLFSSL_USER_SETTINGS_H

/*
 * tdsh ESP32 wolfCrypt/wolfSSH configuration.
 *
 * This profile intentionally follows the shape of wolfSSL's maintained
 * ESP32 wolfSSH example rather than using the earlier minimal custom profile.
 * Important differences from V0.3.8:
 *   - SINGLE_THREADED wolfCrypt (one SSH server task services one client)
 *   - USE_FAST_MATH, matching the ESP32 wolfSSH example
 *   - original ASN.1 decoder (WOLFSSL_ASN_TEMPLATE is intentionally absent)
 *   - P-256 ECC host key / ECDH only; RSA and finite-field DH disabled
 *   - filesystem remains enabled because tdsh needs SFTP on LittleFS
 *   - ESP32 crypto HW remains disabled until runtime is stable
 */

#include "sdkconfig.h"

/* Keep the platform selection deterministic. */
#ifdef WOLFSSL_ESPIDF
#undef WOLFSSL_ESPIDF
#endif
#define WOLFSSL_ESPIDF

#ifdef WOLFSSL_ESP32
#undef WOLFSSL_ESP32
#endif
#define WOLFSSL_ESP32

#define ESP_ENABLE_WOLFSSH
#define WOLFSSL_WOLFSSH
#define WOLFCRYPT_ONLY

/* tdsh's current server model has one wolfSSH worker task and one active
 * connection at a time. Avoid wolfCrypt's global mutex setup entirely. */
#define SINGLE_THREADED

/* Embedded wolfSSH defaults. */
#define BENCH_EMBEDDED
#define DEFAULT_WINDOW_SZ 2000
#define WOLFSSH_TERM
#define WOLFSSL_KEY_GEN
#define WOLFSSL_PUBLIC_MP
#define WOLFSSL_SMALL_STACK
#define USE_FAST_MATH

/* SSH crypto. P-256/SHA-256 are the primary path. */
#define HAVE_ECC
#define HAVE_CURVE25519
#define CURVE25519_SMALL
#define HAVE_HKDF
#define HAVE_AEAD
#define HAVE_AESGCM
#define WOLFSSL_AES_COUNTER
#define WOLFSSL_SHA224
#define WOLFSSL_SHA384
#define WOLFSSL_SHA512

/* IMPORTANT: wolfSSL 5.8.2 defaults to WOLFSSL_ASN_TEMPLATE when neither
 * decoder macro is selected. Explicitly select the original decoder for the
 * SEC1 P-256 host key; merely omitting WOLFSSL_ASN_TEMPLATE is not enough. */
#define WOLFSSL_ASN_ORIGINAL

/* We do not use RSA or classic finite-field Diffie-Hellman. */
#define NO_RSA
#define NO_DH
#define NO_DSA
#define NO_RC4
#define NO_MD4
#define WOLFSSH_NO_RSA
#define WOLFSSH_NO_DH
#define WOLFSSH_NO_ED25519

/* Keep only the NIST P-256 ECDSA/ECDH family needed by the bundled host key.
 * This reduces code and avoids advertising curves we have not validated. */
#define WOLFSSH_NO_ECDSA_SHA2_NISTP384
#define WOLFSSH_NO_ECDSA_SHA2_NISTP521
#define WOLFSSH_NO_ECDH_SHA2_NISTP384
#define WOLFSSH_NO_ECDH_SHA2_NISTP521

/* SFTP needs ESP-IDF VFS/LittleFS. Never allow wolfSSL/wolfSSH to disable the
 * filesystem merely because their UART-only example does. */
#ifdef NO_FILESYSTEM
#undef NO_FILESYSTEM
#endif
#ifdef WOLFSSH_NO_FILESYSTEM
#undef WOLFSSH_NO_FILESYSTEM
#endif

/* First make the software path stable. Hardware crypto can be enabled later. */
#define NO_ESP32_CRYPT
#define NO_WOLFSSL_ESP32_CRYPT_HASH
#define NO_WOLFSSL_ESP32_CRYPT_HASH_SHA
#define NO_WOLFSSL_ESP32_CRYPT_HASH_SHA224
#define NO_WOLFSSL_ESP32_CRYPT_HASH_SHA256
#define NO_WOLFSSL_ESP32_CRYPT_HASH_SHA384
#define NO_WOLFSSL_ESP32_CRYPT_HASH_SHA512
#define NO_WOLFSSL_ESP32_CRYPT_AES
#define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI
#define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI_MP_MUL
#define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI_MULMOD
#define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI_EXPTMOD

#endif /* TDSH_WOLFSSL_USER_SETTINGS_H */
