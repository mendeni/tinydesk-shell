# Embedding the shell in an existing application

An existing product firmware can embed TinyDesk Shell as a component; it should not become a fork of the shell.

```text
Product application
  network manager
  Modbus master/slave
  MQTT worker
  logger / data model / OTA
       |
       +---- product command wrappers ----> TinyDesk Shell
```

If the application already initializes Wi-Fi/Ethernet:

```c
tdsh_espidf_config_t cfg = TDSH_ESP_IDF_CONFIG_DEFAULT();
cfg.init_network_manager = false;
cfg.register_network_commands = false;
cfg.register_remote_server_commands = false; /* enable deliberately later */
cfg.register_hardware_commands = false;      /* safer on a live machine */
```

This avoids duplicate default-netif ownership. Register thin commands that call
existing service APIs; do not move polling/reconnect/safety loops into shell
handlers.

Suggested migration:
1. integrate shell without changing existing product services;
2. keep shell network ownership disabled;
3. add `machine`, `mqtt`, `modbus`, `logger` wrapper commands;
4. rerun uScript + hardware tests;
5. compare heap minimum/largest block before/after repeated scripts;
6. only then extract generic product services into reusable modules.
