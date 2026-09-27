# Embedding the shell in an existing application pattern

This example shows the ownership boundary, not a replacement product firmware.

- product owns its existing networking, Modbus, MQTT, logger and machine tasks.
- TinyDesk Shell owns its console/session/parser/script runtime.
- Set `init_network_manager=false` when product already creates ESP-NETIF/Wi-Fi/Ethernet objects.
- Register thin shell commands that call product service APIs. Never duplicate the
  production control logic inside command handlers.
- Keep machine protection and time-critical control in the dsPIC/native machine
  services; shell commands are a control/diagnostic plane.

Merge the shell initialization into the existing `app_main()` rather than
replacing the product startup sequence wholesale.
