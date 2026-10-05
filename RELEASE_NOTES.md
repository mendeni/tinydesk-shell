# TinyDesk Shell developer preview

A Unix-like shell for microcontrollers, with users, networking and SSH, that
also runs on a PC. It is the shell inside
[TinyDesk](https://github.com/schikani/tinydesk) and works on its own too.

This release is a developer preview: expect rough edges, and please report
what breaks.

## New in 0.1.4

* **Long command lines:** a line longer than the terminal is wide wraps
  over several rows and stays editable: typing, Backspace, Delete, the
  arrows, Home/End and history redraw every row. Before, text was left
  behind or doubled and the cursor could not move back across the wrap.
  The shell learns the width from the terminal (asked once per line on a
  serial console), the SSH client, the Windows console or the Linux
  terminal; UTF-8 characters count as one column each.
* **`board save`** copies the pins built into the firmware into
  `/etc/board.conf`, so firmware built without them, such as an official
  release, keeps them. `board show` says when that is needed.
* **Tab completion** skips names too long for a shell path.
* **For contributors:** `CONTRIBUTING.md`, and a pre-commit hook that
  formats C files with clang-format 16 (`pip install pre-commit`, then
  `pre-commit install`). The sources are now formatted with it.
* **For programs that embed the shell:** `tdsh_terminal_io_t` has two
  new optional members, `columns()` and `read_byte_timeout()`; code that
  fills the struct by position must name them (designated initializers
  are fine).

Also since 0.1.0: nano follows the terminal's size and the factory
password is named where it is needed (0.1.3), the SD card at `/sd`
(`sd mount`, 0.1.2), `ping -c`, and `tdsh.exe`, a native Windows program
(`tinydesk-shell-windows-x64.zip`).

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
* **Windows (x64, Windows 10 or later):** `tinydesk-shell-windows-x64.zip`, then
  `tdsh.exe` in Windows Terminal. Its files live in `%LOCALAPPDATA%\tdsh\rootfs`.

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
