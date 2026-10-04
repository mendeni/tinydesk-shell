#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "tdsh_espidf.h"

static const char *TAG = "embed-example";

/* Replace these with your real product services. The important point is that
 * commands call application APIs; TinyDesk Shell does not duplicate machine logic. */
static bool machine_is_running(void)
{
    return false;
}
static float machine_power_kw(void)
{
    return 0.0f;
}

static int cmd_machine(tdsh_session_t *session, int argc, char **argv)
{
    (void)session;
    if (argc == 2 && strcmp(argv[1], "status") == 0)
    {
        printf("state: %s\n", machine_is_running() ? "running" : "stopped");
        printf("power: %.1f kW\n", (double)machine_power_kw());
        return 0;
    }
    printf("usage: machine status\n");
    return 2;
}

static const tdsh_command_t s_app_commands[] = {
    {
        .name = "machine",
        .usage = "machine status",
        .help = "Inspect product machine service",
        .fn = cmd_machine,
        .flags = 0,
    },
};

void app_main(void)
{
    /* In a real product firmware these are normally already done by the product
     * startup sequence. Do them only once. */
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK)
    {
        /* Do not erase production NVS automatically. Let the product's
         * existing migration/recovery policy decide what to do. */
        ESP_LOGE(TAG, "NVS initialization requires product recovery/migration: %s",
                 esp_err_to_name(err));
        return;
    }

    /* Start your existing product drivers/services first:
     *   filesystem / network manager / Modbus / MQTT / logger / product transport
     * Do not let both the application and TinyDesk Shell create the same esp_netif. */

    tdsh_espidf_config_t shell_cfg = TDSH_ESP_IDF_CONFIG_DEFAULT();
    shell_cfg.hostname = "mydevice";
    shell_cfg.default_user = "root";
    shell_cfg.init_network_manager = false;          /* product firmware owns networking. */
    shell_cfg.register_network_commands = false;    /* Avoid a second network control plane. */
    shell_cfg.register_remote_server_commands = false;
    shell_cfg.register_hardware_commands = false;   /* Avoid generic GPIO/hwtest on a live machine. */

    ESP_ERROR_CHECK(tdsh_espidf_init(&shell_cfg));
    int rc = tdsh_register_commands(s_app_commands,
                                    sizeof(s_app_commands) / sizeof(s_app_commands[0]));
    if (rc != 0)
    {
        ESP_LOGE(TAG, "product command registration failed: %d", rc);
        return;
    }
    ESP_ERROR_CHECK(tdsh_espidf_start());
}
