#include "tdsh_espidf.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"

/*
 * ESP-IDF 5.3.1's ESP_PING_DEFAULT_CONFIG() uses ESP_TASK_PING_STACK,
 * which is only 2048 bytes (+ TASK_EXTRA_STACK_SIZE). That is marginal
 * once our callbacks perform several esp_ping_get_profile() calls and
 * formatted console output. Give the dedicated ping worker enough room.
 *
 * ESP-IDF FreeRTOS task stack sizes are specified in bytes.
 */
#define TDSH_PING_TASK_STACK_SIZE 6144U

typedef struct {
    SemaphoreHandle_t done;
} ping_ctx_t;

static void on_success(esp_ping_handle_t hdl, void *args)
{
    (void)args;

    uint8_t ttl = 0;
    uint16_t seqno = 0;
    uint32_t elapsed = 0;
    uint32_t len = 0;
    ip_addr_t addr;
    memset(&addr, 0, sizeof(addr));

    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO,
                               &seqno, sizeof(seqno));
    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_TTL,
                               &ttl, sizeof(ttl));
    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR,
                               &addr, sizeof(addr));
    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE,
                               &len, sizeof(len));
    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP,
                               &elapsed, sizeof(elapsed));

    printf("%" PRIu32 " bytes from %s icmp_seq=%" PRIu16
           " ttl=%u time=%" PRIu32 " ms\n",
           len,
           ipaddr_ntoa(&addr),
           seqno,
           (unsigned)ttl,
           elapsed);
}

static void on_timeout(esp_ping_handle_t hdl, void *args)
{
    (void)args;

    uint16_t seqno = 0;
    ip_addr_t addr;
    memset(&addr, 0, sizeof(addr));

    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO,
                               &seqno, sizeof(seqno));
    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR,
                               &addr, sizeof(addr));

    printf("From %s icmp_seq=%" PRIu16 " timeout\n",
           ipaddr_ntoa(&addr), seqno);
}

static void on_end(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t *ctx = (ping_ctx_t *)args;

    uint32_t tx = 0;
    uint32_t rx = 0;
    uint32_t duration = 0;
    uint32_t loss = 0;
    ip_addr_t addr;
    memset(&addr, 0, sizeof(addr));

    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST,
                               &tx, sizeof(tx));
    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY,
                               &rx, sizeof(rx));
    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION,
                               &duration, sizeof(duration));
    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR,
                               &addr, sizeof(addr));

    /* Integer-only calculation: avoids floating-point work in ping task. */
    if (tx > 0) {
        const uint32_t lost = (tx >= rx) ? (tx - rx) : 0;
        loss = (lost * 100U) / tx;
    }

    printf("--- %s ping statistics ---\n", ipaddr_ntoa(&addr));
    printf("%" PRIu32 " packets transmitted, %" PRIu32
           " received, %" PRIu32 "%% packet loss, time %" PRIu32 "ms\n",
           tx, rx, loss, duration);

    if (ctx != NULL && ctx->done != NULL) {
        xSemaphoreGive(ctx->done);
    }
}

static int resolve_target(const char *host, ip_addr_t *target)
{
    memset(target, 0, sizeof(*target));

    if (ipaddr_aton(host, target)) {
        return 0;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL) {
        return -1;
    }

    if (res->ai_family == AF_INET) {
        const struct in_addr a =
            ((const struct sockaddr_in *)res->ai_addr)->sin_addr;
        inet_addr_to_ip4addr(ip_2_ip4(target), &a);
    } else if (res->ai_family == AF_INET6) {
        const struct in6_addr a6 =
            ((const struct sockaddr_in6 *)res->ai_addr)->sin6_addr;
        inet6_addr_to_ip6addr(ip_2_ip6(target), &a6);
    } else {
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    return 0;
}

static int ping_one(const char *host)
{
    ip_addr_t target;
    if (resolve_target(host, &target) != 0) {
        printf("ping: unknown host %s\n", host);
        return 1;
    }

    ping_ctx_t ctx = {
        .done = xSemaphoreCreateBinary(),
    };

    if (ctx.done == NULL) {
        printf("ping: cannot allocate completion semaphore\n");
        return 1;
    }

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = 4;
    cfg.interval_ms = 1000;
    cfg.timeout_ms = 1000;
    cfg.task_stack_size = TDSH_PING_TASK_STACK_SIZE;

    esp_ping_callbacks_t cb = {
        .cb_args = &ctx,
        .on_ping_success = on_success,
        .on_ping_timeout = on_timeout,
        .on_ping_end = on_end,
    };

    esp_ping_handle_t handle = NULL;
    esp_err_t err = esp_ping_new_session(&cfg, &cb, &handle);
    if (err != ESP_OK) {
        printf("ping: cannot create session: %s\n", esp_err_to_name(err));
        vSemaphoreDelete(ctx.done);
        return 1;
    }

    printf("PING %s (%s)\n", host, ipaddr_ntoa(&target));

    err = esp_ping_start(handle);
    if (err != ESP_OK) {
        printf("ping: start failed: %s\n", esp_err_to_name(err));
        (void)esp_ping_delete_session(handle);
        vSemaphoreDelete(ctx.done);
        return 1;
    }

    const BaseType_t finished =
        xSemaphoreTake(ctx.done, pdMS_TO_TICKS(12000));

    if (finished != pdTRUE) {
        printf("ping: session timed out waiting for completion\n");
    }

    (void)esp_ping_stop(handle);
    (void)esp_ping_delete_session(handle);
    vSemaphoreDelete(ctx.done);

    return (finished == pdTRUE) ? 0 : 1;
}

int tdsh_cmd_ping(tdsh_session_t *session, int argc, char **argv)
{
    (void)session;

    if (argc < 2) {
        printf("usage: ping <host/address ...>\n");
        return 2;
    }

    if (!tdsh_network_is_online()) {
        printf("ping: no network interface is connected.\n");
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        if (ping_one(argv[i]) != 0) {
            rc = 1;
        }
    }

    return rc;
}
