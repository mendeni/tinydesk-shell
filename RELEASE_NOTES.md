# TinyDesk Shell developer preview

A Unix-like shell for microcontrollers, with users, networking and SSH, that
also runs on a PC. It is the shell inside
[TinyDesk](https://github.com/schikani/tinydesk) and works on its own too.

This release is a developer preview: expect rough edges, and please report
what breaks.

## Install

* **ESP boards, from the browser:** <https://schikani.github.io/tinydesk-docs/install/#shell/esp32c6>
  (Chrome or Edge on a computer). Choose *TinyDesk Shell* and your board.
* **Without the browser:** flash a `*-factory.bin` below at offset 0:

  ```bash
  esptool.py --chip esp32c6 write_flash 0x0 tinydesk-shell-{version}-esp32c6-factory.bin
  esptool.py --chip esp32 write_flash 0x0 tinydesk-shell-{version}-esp32-factory.bin
  ```

* **Linux (x86_64, built on Ubuntu 22.04):** `tinydesk-shell-linux-x86_64.tar.gz`,
  then `./tdsh`. Its files live in `~/.local/share/tdsh/rootfs`.

| File | Board |
| --- | --- |
| `tinydesk-shell-{version}-esp32c6-factory.bin` | ESP32-C6 with 8 MB flash, built-in USB |
| `tinydesk-shell-{version}-esp32-factory.bin` | any ESP32 with 4 MB flash or more; PSRAM is used when present |

Check the files against `SHA256SUMS.txt`. The firmware is not signed. A
factory image erases users, Wi-Fi networks and the SSH host key.

## Before connecting

Open the board's serial port in a UTF-8 terminal (115200 baud on the
ESP32, any speed on the ESP32-C6's USB port), or the web terminal at
<https://schikani.github.io/tinydesk-docs/console/>. You start as root:
run `passwd` locally (initial password: `TinyDesk`) before enabling remote
access. `help` lists the commands; `.tdsh` scripts are described in
`docs/SCRIPTING.md`.

Changes: see `CHANGELOG.md`.
