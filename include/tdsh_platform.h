#ifndef TDSH_PLATFORM_H
#define TDSH_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef int (*tdsh_worker_fn_t)(void *arg);
    typedef void (*tdsh_worker_cleanup_fn_t)(void *arg);

    typedef struct tdsh_platform_api
    {
        const char *name;
        void *context;

        uint64_t (*monotonic_ms)(void *context);
        void (*sleep_ms)(void *context, uint32_t ms);
        void (*yield)(void *context);
        int (*random_bytes)(void *context, void *buffer, size_t length);

        void *(*malloc_fn)(void *context, size_t size);
        void *(*calloc_fn)(void *context, size_t count, size_t size);
        void *(*realloc_fn)(void *context, void *ptr, size_t size);
        void (*free_fn)(void *context, void *ptr);

    /*
     * Execute worker(arg) using a platform-owned thread/task wrapper.
     *
     * background=false: block until completion and write worker status to result_out.
     * background=true: return after successful start; worker/cleanup happen later.
     *
     * Ownership contract:
     * - On return 0, the platform owns arg and MUST call cleanup(arg) exactly once
     *   after worker returns (for foreground or background execution).
     * - On non-zero return, ownership remains with the caller and cleanup MUST NOT
     *   be called by the platform.
     */
        int (*worker_run)(void *context,
                          const char *name,
                          size_t stack_bytes,
                          int priority,
                          bool background,
                          tdsh_worker_fn_t worker,
                          void *arg,
                          tdsh_worker_cleanup_fn_t cleanup,
                          int *result_out);
    } tdsh_platform_api_t;

#ifdef __cplusplus
}
#endif

#endif /* TDSH_PLATFORM_H */
