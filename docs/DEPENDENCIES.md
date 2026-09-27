# Build dependencies

| Component | Version | Delivery |
| --- | --- | --- |
| ESP-IDF | 5.3.1 | Select installed Espressif SDK/toolchain on Windows |
| wolfSSH | 1.5.0 | Bundled `third_party/wolfssh` and local adapter |
| wolfSSL | 5.8.2~1 | Bundled `managed_components/wolfssl__wolfssl` |
| LittleFS ESP component | 1.19.1 | Bundled `managed_components/joltwallet__littlefs` |
| W6100 | 1.0.0 | Bundled `managed_components/espressif__w6100` |
| WIZnet common | 1.0.0 | Bundled `managed_components/espressif__wiznet_common` |
| libsmb2 | 3.0.1 | Bundled `managed_components/sahlberg__libsmb2` |

Managed component sources come from the user's supplied working project. The SDK
manifest pins the same versions, including WIZnet common so a transitive dependency
update does not silently change the known-working driver family.

Component Manager may contact the registry during configuration to validate/resolve
metadata. Keep internet access available for the first configuration. The toolchain
and Python dependencies are installed through ESP-IDF, not embedded in this ZIP.
Do not manually edit managed components; compatibility adjustments are applied by
the local adapter and `tdsh_espidf_project_setup.cmake`.
