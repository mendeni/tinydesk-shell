# Changelog

## Unreleased

* **Windows program `tdsh.exe`** (`ports/windows`, `examples/windows`): the
  shell as a native Windows console program, for Windows Terminal. The line
  editor, history and Tab completion work as on Linux; `ifconfig`, `ping`,
  `date`, `cal`, `tz`, `write`, `hostpath`, `capabilities`. Its files are
  in `%LOCALAPPDATA%\tdsh\rootfs`, the user is the Windows user name.
  `docs/WINDOWS_HOST.md` describes it.
* The Windows port moved here from TinyDesk, which now uses it for its
  Windows Terminal window; `tdsh_win_init_user()` sets the default user.
* Releases also contain `tinydesk-shell-windows-x64.zip`; the host workflow
  builds and tests on Windows (MinGW) too.
* The release text takes its version from `VERSION` (`{version}` in
  `RELEASE_NOTES.md`).

## 0.1.0 (2026-09-28): first release

### Releases and documentation

* Releases: a tag `v<VERSION>` builds the ESP32-C6 and ESP32 firmware and the
  Linux program and attaches them to a draft pre-release
  (`.github/workflows/release.yml`, packaged by `tools/make_release.py` with
  the same file names as TinyDesk's Shell edition). The README links the web
  installer.
* Documentation: `docs/SCRIPTING.md`, the reference for `.tdsh` scripts
  (running them, variables and quoting, `$(…)` and `$((…))`, conditions,
  loops, functions, pipes and redirection, limits, a tested example), and
  a Scripts section in the README.

### Name, licence, layout

* The project is **TinyDesk Shell** (`tdsh`): C prefix `tdsh_`, macros
  `TDSH_`, Kconfig `CONFIG_TDSH_*`, ESP-IDF component `tdsh`, headers
  `tdsh.h`, `tdsh_espidf.h`, ... Released under the MIT licence.
* Scripts use the extension `.tdsh` and the shebang `#!/bin/tdsh`; a
  directory runs its `main.tdsh`; `tdsh run <file.tdsh|directory>`.
* Per-user files: `~/.tdshrc.tdsh` (startup script), `~/.tdsh_history`,
  `~/.tdsh_tz`.
* `third_party/wolfssh/ide/` (project files for other IDEs) is not bundled:
  its paths were too long for a Windows checkout.

### Firmware and platforms

* The repository root builds the ESP32-C6 firmware; **`projects/esp32`**
  builds the classic ESP32 firmware (4 MB flash, PSRAM optional, 2.5 MB
  app + 1.4 MB `/fs`: the layout of TinyDesk's 4 MB ESP32 desktop, so a
  board can switch between the two and keep its files). Both share `main/main.c`, which registers an example
  command, `hello`.
* The ESP-IDF console can be a **UART** (`CONFIG_ESP_CONSOLE_UART`) as well
  as USB Serial/JTAG.
* Each firmware embeds its `board.conf` (not in git) or
  `board.example.conf` as the built-in board configuration
  (`cmake/board_conf.cmake`).
* Classic ESP32: libsmb2's `MD5Init`/`MD5Update`/`MD5Final` come from the
  ROM (`src/tdsh_md5_rom.c`).
* A board without Ethernet no longer logs "Ethernet start:
  ESP_ERR_NOT_SUPPORTED" every second.
* CI: `.github/workflows/firmware.yml` builds both firmwares.

### Board configuration (new)

* `tdsh_board.h` / `src/core/tdsh_board.c`: a portable `key = value` store
  for hardware settings. Built-in text (`tdsh_espidf_config_t.board_config`,
  embedded by the application) is overridden by `/fs/etc/board.conf`,
  loaded right after the file system and before any module that reads it.
  Test: `tests/test_board.c`.
* Command `board [show|get|set|unset|init]` (`set`, `unset`, `init`: root).
* Ethernet reads the `eth.*` keys; without `eth.chip = w6100` the board has
  no Ethernet and no pin is touched. `lan hw set` and `lan poll` write the
  keys. The Kconfig pin defaults are -1 (fallbacks only).
* `hwtest` reads `sd.*`, `rs485.1.*` and `rs485.2.*`; unconfigured tests
  print `[SKIP]`. Built for every chip (the LP UART test only on the
  ESP32-C6).

### Users and security

* Factory root password `TinyDesk`; nothing forces a change.
* SSH: a per-device ECDSA P-256 host key, generated on the first start and
  kept in NVS (`tdsh_ssh` / `hostkey`); `ssh hostkey [new]` shows or
  replaces it. The built-in development key is only a fallback, which
  `ssh status` flags.
* `ssh` and `ftp`: `status` for everyone; start/stop/restart for root, or
  any user on the local console.
* SFTP for every user; non-root users are confined to their home
  (`wolfssh_local/sftp_jail.c`, wolfSSH's file macros routed through
  `tdsh_sftp_allowed()`).
* Saved Wi-Fi networks have owners (database version 2; a version 1
  database is converted on first load, its networks becoming shared).
  Root's networks are shared; other users add their own.

### API for applications (ESP-IDF)

* `tdsh_espidf_run_console()` runs the local console loop in the calling
  task with its stdin/stdout (TinyDesk runs it in a desktop window);
  `tdsh_espidf_console_set_user()` / `tdsh_espidf_console_user()` switch
  the console's user.
* Structured Wi-Fi API: `tdsh_wifi_scan_list()`, `tdsh_wifi_save()`,
  `tdsh_wifi_forget()`, `tdsh_wifi_connect_saved()`,
  `tdsh_wifi_disconnect_now()`.
* `tdsh_time_auto()`, `tdsh_time_set_auto()` (automatic SNTP time on/off,
  NVS `ush_time`/`auto`), `tdsh_time_sync_now()`.
* `tdsh_ssh_is_running()` / `tdsh_ssh_set_running()`,
  `tdsh_ftp_is_running()` / `tdsh_ftp_set_running()`.
* Script worker tasks inherit the caller's stdio (background workers only
  from the local console) and hand the global streams back before exiting.

### Fixes

* `ssh stop` works: the listening socket gets a 0.5 s `SO_RCVTIMEO`, so
  `accept()` returns and sees the stop (before, the server task stayed
  until the next client and `ssh start` failed).
* The SFTP start directory is set for password logins too (FileZilla could
  not open `/`).
* A half-done W6100 setup is undone (`eth_setup_undo()`): without a chip,
  every network retry leaked a driver task and an SPI device.
* The 64 KB SSH heap arena is no longer a static buffer: it is allocated on
  the first `ssh start`, from PSRAM when the chip has it (the start check
  then asks for 64 KB of internal RAM instead of 96 KB), else from internal
  RAM (the check adds the arena's 64 KB). Boards without PSRAM keep 64 KB of
  static RAM for everything else; `ssh status` shows where the heap is.
* The startup-file template no longer mentions a USB Serial/JTAG console on
  every board.
