# TinyDesk standards

This guide applies to both repositories, `tinydesk` and `tinydesk-shell`. It
lives in `tinydesk-shell`, where ports start; TinyDesk's CONTRIBUTING.md links
to it. It also says what a community port should do so that its code,
commands and releases sit cleanly next to the official ones and can later
move upstream without renaming.

It describes how the code is already written wherever the code is consistent.
Where the two repositories differ today, the rule below is the one new code
follows; the existing differences are listed at the end.

**Must** means a pull request will not be merged without it (or a port will
not be listed as a community port). **Should** means the rule is expected but
can be broken with a reason given in the pull request.

Applies from 0.1.4.

## Contents

1. [Names](#1-names)
2. [Ports](#2-ports)
3. [C code](#3-c-code)
4. [Shell commands and scripts](#4-shell-commands-and-scripts)
5. [Board configuration](#5-board-configuration)
6. [Files on the device](#6-files-on-the-device)
7. [Versions and releases](#7-versions-and-releases)
8. [Commits and pull requests](#8-commits-and-pull-requests)
9. [Not standardised yet](#9-not-standardised-yet)
10. [Known deviations](#10-known-deviations)

## 1. Names

### Projects

| Name | Repository | Short name | C prefix |
| --- | --- | --- | --- |
| TinyDesk | `tinydesk-project/tinydesk` | `tinydesk` | `td_`, `TD_` |
| TinyDesk Shell | `tinydesk-project/tinydesk-shell` | `tdsh` | `tdsh_`, `TDSH_` |

"TinyDesk" and "TinyDesk Shell" name the official projects only. A port
must not use either as the start of its own name, so nobody mistakes it for
an official build. Describe a port as "*Name*, a port of TinyDesk Shell to
*platform*". TinyTang is a good example.

### Platform and board identifiers

Identifiers are lowercase and appear in directory names, release file names
and what the `platform` command prints.

| Kind | Form | Examples |
| --- | --- | --- |
| Chip | the vendor's part name, no separators | `esp32`, `esp32c6`, `bl616` |
| SDK or OS | its usual name, `_` between words | `esp_idf`, `posix`, `windows` |
| Board variant | chip, then `-` and what differs | `esp32-4mb` |

### Port prefix

Every port picks one short lowercase prefix (2 to 8 letters, for example
`tang`) and uses it for everything it owns:

| What | Form | Example |
| --- | --- | --- |
| C functions, variables, types | `<prefix>_` | `tang_osd_set()` |
| Macros, enum constants | `<PREFIX>_` | `TANG_...` |
| Port-only source files | `<prefix>_<feature>.c` | `tang_osd.c` |
| Build switches (CMake, Kconfig, environment) | `<PREFIX>_` or the port name in capitals | `TINYTANG_MIN` |
| Board configuration keys | `<prefix>.` | `tang.osd.rows` |
| Configuration on the device | `/etc/<prefix>/` or `/etc/<prefix>.conf` | `/etc/tang/` |

The prefix is recorded in the Prefix column of the Community ports table in
the TinyDesk Shell README, so two ports never choose the same one. `td`,
`tdsh`, `tinydesk` and any platform identifier cannot be chosen.

## 2. Ports

### Layout

A port's platform code goes in `ports/<platform>/`, where `<platform>` is the
chip identifier if the code is specific to one chip (`ports/bl616`) or the SDK
identifier if it serves several chips (`ports/esp_idf`).

New files that implement an interface defined upstream are named
`<upstream prefix>_<interface>[_<detail>]_<platform>.c`:

| Interface | Defined by | File |
| --- | --- | --- |
| TinyDesk Shell platform | `tdsh_platform.h` | `tdsh_platform_<platform>.c` |
| TinyDesk Shell file system | the standard C calls (PORTING.md) | `tdsh_fs_<platform>.c` |
| TinyDesk hardware layer | `td_hal.h` | `td_hal_<platform>.c` |
| TinyDesk to shell bridge | `ports/common/tdsh_bridge.h` | `tdsh_bridge_<platform>.c` |

Everything else in the port uses the port prefix. The existing ports were
written before this rule and use other names (section 10).

### Names inside interface files

A port must not define new symbols starting with `td_` or `tdsh_`, with one
exception: functions the application calls to start the port are named
`<upstream prefix>_<platform>_<verb>`, for example `tdsh_bl616_init()`.
Upstream will never define `td_<platform>_*` or `tdsh_<platform>_*` names
for a platform that has a listed community port. This is what lets a
platform layer move into `tinydesk-shell` later without renaming anything.

Everything that is not the public entry point is `static` and starts with
`s_`, as in the core.

### Upstream code

- The core (`src/` and `include/` of both repositories) must build unchanged
  for the port. Target-specific `#if` lines belong in `ports/`, never in the
  core. If the core needs a hook, open an issue or pull request upstream.
- Track upstream as git submodules, checked out at a release tag
  (TinyDesk itself keeps TinyDesk Shell in `third_party/tdsh`).
- Never commit edits inside a submodule. A change you need before upstream
  has it is carried as a patch file in `third_party/patches/`, applied by an
  idempotent script before the build, and listed in `THIRD_PARTY.md` with the
  upstream commit it applies to. Each carried patch to TinyDesk or TinyDesk
  Shell should have a matching upstream issue or pull request.
- The host tests of both repositories must pass against the commit the port
  pins (`ctest` in each repository; see CONTRIBUTING.md).

### What a port reports

The platform name is `<platform>/<threading>`, followed for a port by
`, <port name> <version or build id>`. The `platform` command prints it with
the TinyDesk Shell version, and `version` prints it after the version:

```
# platform
platform: esp-idf/freertos
TinyDesk Shell: 0.1.4
# version
TinyDesk Shell 0.1.4 (esp-idf/freertos)
```

A port would print `bl616/freertos, tinytang <build id>`.

### To be listed as a community port

The README of the port must state:

- the platform and board;
- the TinyDesk and TinyDesk Shell versions it is built against;
- its prefix;
- what it adds, and what of upstream it leaves out or changes;
- how to build and flash it;
- its licence, compatible with MIT, and the licences of what it bundles.

## 3. C code

### Language and warnings

- C11. The TinyDesk core, protocols and apps build without compiler
  extensions (`C_EXTENSIONS OFF`).
- No warnings with `-Wall -Wextra -Wpedantic` on the host build. TinyDesk's
  own code also builds with `-Werror`.
- Every host test passes (`ctest --test-dir <build directory>`).

### Format

- Format with clang-format 16 and the `.clang-format` of the repository.
  `pre-commit install` does it on every commit.
- Braces on their own line (Allman), for functions, `if`, `else`, loops,
  `switch` and `case`.
- No body on the same line as its `if`, loop, `case` or function, even for
  one statement.
- Four spaces, no tabs. `int *p`, not `int* p`.
- Comments use `/* */`. Every new file starts with a comment that names the
  file and says in one line what it is for:

  ```c
  /*
   * td_hal.h - the hardware abstraction layer.
   */
  ```

### Naming

| What | Form | Example |
| --- | --- | --- |
| Functions, variables, struct members | `lower_snake_case` | `td_full_redraw()` |
| Public functions | project prefix, then module | `td_wm_dispatch()`, `tdsh_register_command()` |
| Types | `_t` suffix | `td_event_t` |
| Struct tags, when a struct needs one | the type name without `_t` | `typedef struct td_hal { ... } td_hal_t;` |
| Macros, enum constants | `UPPER_SNAKE_CASE`, with a group | `TD_KEY_UP`, `TD_EV_MOUSE` |
| File-scope (`static`) variables | `s_` prefix | `s_wins`, `s_dirty` |
| Include guards | file name in capitals with `_H` | `TD_HAL_H` |
| Context pointer in callbacks | `ctx` | `int (*read_byte)(void *ctx)` |
| Desktop app registration | `<prefix>_<app>_register()` | `td_editor_register()` |

Public headers live in `include/tinydesk/td_*.h` (TinyDesk) and
`include/tdsh*.h` (TinyDesk Shell).

### Sizes and limits

- Limits that size a public buffer, a pool or a part of the RAM budget are
  macros in `td_config.h` or `tdsh.h`, wrapped in `#ifndef` so a build can
  override them (`-DTD_MAX_COLS=100`). Ports set them from the build system,
  never by editing the header.
- Sizes private to one module may be macros in that file.
- Comment each limit with what it costs in RAM.
- Features that need large buffers allocate them while in use and free them
  afterwards (a desktop app while its window is open).

### Portability

The code runs on 32-bit chips today. New code should not make that worse:

- Use `size_t` for sizes and the fixed-width types (`int32_t`, `uint32_t`)
  for values that must have a given range; do not assume `int` is wider than
  16 bits in new interfaces.
- Never keep a pointer in an integer other than `uintptr_t`.
- No casts that can truncate (`size_t` into `int`) without a range check.
- Constant tables and strings are `const`, so targets with separate program
  memory can keep them in flash.
- TinyDesk Shell's core allocates through `tdsh_malloc()` and the other
  allocator hooks, so a port can supply its own allocator. TinyDesk uses the
  C library's `malloc()`.

### Hardware

No pin, UART or SPI host number appears in code. They come from board
configuration (section 5).

## 4. Shell commands and scripts

### Command names

- Lowercase letters and digits, no separators: `wificonnect`, `hwtest`,
  `bootuser`.
- One command per subject, with verbs as subcommands:
  `sd mount`, `mqtt status`, `ota install`, `board save`, `tdsh run`.
- The state of a service or a job is shown by `status` (`ota status`,
  `sd status`); settings are listed by `show` (`board show`).
- Options are `-x` for one letter and `--word` for longer ones (`ping -c 4`,
  `ota install -f`, `tdsh run file --bg`).
- Every command is registered with a usage line and a one-line description,
  which `help` shows.

### Names reserved by upstream

A port must not register a command with these names unless it does what the
upstream command does, with the same options:

- every command upstream ships on any platform (run `help` on the ESP32,
  POSIX and Windows builds; FUNCTIONS.md lists them);
- the peripheral and subsystem names `gpio`, `spi`, `i2c`, `uart`, `adc`,
  `pwm`, `usb`, `sd`, `eth`, `wifi`, `ota`;
- `peek` and `poke`: the core registers them when a port lists memory
  regions (`mem_regions` in `tdsh_platform_api_t`, see docs/PORTING.md). A
  port provides the regions, not commands of its own with these names.

A port command whose name is generic enough that upstream might one day want
it should carry the prefix (`tangput`, `tangflash`). If upstream later adds a
command with the same name as a port's, the port renames its command.

### Output and errors

- Plain text.
- Errors start with the command name: `ls: invalid path: /foo`.
- Exit status 0 for success, non-zero for failure, so scripts can test `$?`.

### Scripts

- Script files end in `.tdsh` and start with `#!/bin/tdsh`.
- A folder run with `tdsh run` runs its `main.tdsh`.
- Each user's startup script is `~/.tdshrc.tdsh`.

## 5. Board configuration

Pins and optional hardware are described in `board.conf`, never in code.

- One `key = value` per line, `#` starts a comment, an empty value unsets the
  key.
- Keys are lowercase, separated by dots: `<device>[.<number>].<field>`
  (`rs485.1.tx`, `eth.cs`, `sd.cs`).
- Numbers are decimal, or hexadecimal after `0x`. `-1` means "not
  connected"; a missing key means "this board does not have it".
- A new key is added, commented out and explained, to every
  `board.example.conf` in the repository.
- `board.conf` itself is in `.gitignore`.
- On the device, `/etc/board.conf` overrides the settings built into the
  firmware. Official firmware has none built in; `board save` copies a
  custom build's settings into `/etc/board.conf` before an official update.
- Port-only keys start with the port prefix (`tang.`). Hardware upstream
  already knows (RS-485, Ethernet, SD card) uses the upstream keys.

## 6. Files on the device

| Path | Use |
| --- | --- |
| `/etc/` | system configuration (`/etc/board.conf`) |
| `/etc/<prefix>/` or `/etc/<prefix>.conf` | a port's configuration |
| `/root/` | root's files |
| `/home/<user>/` | every other user's files |
| `~/Desktop/` | the desktop's icons and files |
| `~/<app>.conf` | a user's settings for one app (`~/mqtt.conf`) |
| `~/.tdshrc.tdsh` | the user's startup script |
| `~/.<name>` | state a program keeps for the user (`~/.tdsh_history`, `~/.tinydesk_settings`) |
| `/tmp/` | temporary files, may be lost at restart |
| `/sd/` | the SD card, when mounted |

A port must not change what these paths mean. A port that follows the layout
of another ecosystem (TinyTang's `/cores/` for FPGA cores) keeps it and
documents it in its README.

## 7. Versions and releases

### Numbers

- Versions are `MAJOR.MINOR.PATCH`. Tags are `vMAJOR.MINOR.PATCH`.
- The version is in `TD_VERSION` (`td.h`), `PROJECT_VER` of each ESP-IDF
  project, `TDSH_VERSION` (`tdsh.h`) and the `VERSION` file of TinyDesk
  Shell. They are changed only in the release commit.
- TinyDesk and TinyDesk Shell are released together with the same number. A
  TinyDesk release records the TinyDesk Shell tag of the same number as its
  submodule.
- While the major number is 0, any release may change the C API or a
  command. RELEASE_NOTES.md says so under the release when it happens.

### Release files

| Project | Firmware | PC program |
| --- | --- | --- |
| TinyDesk | `tinydesk-<edition>-<version>-<board>-factory.bin`, `...-app.bin` | `tinydesk-<edition>-<os>-<arch>` |
| TinyDesk Shell | `tinydesk-shell-<version>-<board>-factory.bin` | `tinydesk-shell-<os>-<arch>` |
| Port | `<port name>-<version>-<board>.<ext>` | `<port name>-<os>-<arch>` |

`<edition>` is `desktop` or `shell`. `<os>-<arch>` is `linux-x86_64` or
`windows-x64`. A port's files must not start with `tinydesk`.

Every release includes `SHA256SUMS.txt` for its files. Official firmware is
built without a `board.conf` (section 5).

## 8. Commits and pull requests

### Commits

- The subject says what changed, as the user sees it, after the area it is
  in: `Editor: keep the file name when saving`,
  `mqtt status: name the TLS handshake state`. No full stop at the end.
- Release commits are titled `TinyDesk 0.1.4` and `TinyDesk Shell 0.1.4`.
- Shell changes are made in `tinydesk-shell` first. TinyDesk then records the
  new submodule commit with `Update TinyDesk Shell: <what changed>`, or in
  the same commit as the TinyDesk change that needs it.
- Commits that only reformat are added to `.git-blame-ignore-revs`.

### Pull requests

A pull request says:

- what changed for users: commands, options, API, board keys;
- which platforms it was built and run on (board, OS, terminal);
- that the host tests pass and the build has no warnings.

New platforms are welcome as pull requests to `tinydesk-shell` in
`ports/<platform>/`.

### Documentation

README files and documentation are written for someone using TinyDesk, not for
the people developing it. Measurements are given with the board, terminal and
method used; anything not measured is stated as an expectation, not a fact.

## 9. Not standardised yet

These have no upstream rule yet. A port that needs one should open an issue
describing what it did, so the rule can be written from a real case instead of
each port inventing its own:

- a system-wide boot script that runs before any user logs in;
- desktop backgrounds;
- audio and media playback commands;
- displays drawn outside the terminal (on-screen display layers, game
  screens) and how they share the screen with the desktop;
- installing and removing apps or commands at run time;
- targets where `int` is 16 bits (nothing is built or tested on one yet).

## 10. Known deviations

Existing code that does not yet follow this guide. New code follows the guide;
these are changed when the files are next worked on.

- Interface files predate the naming rule:
  - TinyDesk's hardware layers are `ports/posix/hal_posix.c`,
    `ports/windows/hal_win.c` and `ports/esp_idf/app/hal_mux.c`;
  - its shell bridges are `ports/common/tdsh_bridge_host.c` and
    `ports/esp_idf/app/tdsh_bridge_esp.c`, its file layer
    `ports/common/td_fs_stdio.c`;
  - TinyDesk Shell's are `tdsh_platform_espidf.c` and `tdsh_fs_espidf.c`
    (`espidf`, not `esp_idf`), `ports/posix/tdsh_posix.c` (POSIX platform in
    one file) and `tdsh_platform_win.c` (`win`, not `windows`).
- The platform name is `esp-idf/freertos` where the identifier is `esp_idf`,
  and the Windows build reports `windows` without a threading part.
- Most TinyDesk Shell source files do not start with the file-naming comment.
- TinyDesk Shell's platform callbacks name their context argument `context`;
  TinyDesk's use `ctx`.
- `tdsh_espidf.h` uses the guard `TDSH_ESP_IDF_H`.
- `src/core/tdsh_board.c` allocates with the C library's `realloc()` and
  `calloc()` instead of the allocator hooks.
- Most limits in `tdsh.h` (`TDSH_MAX_LINE`, `TDSH_MAX_ARGS`,
  `TDSH_MAX_PATH` and others) are not wrapped in `#ifndef`, so a build
  cannot override them; only `TDSH_MAX_VARS`, `TDSH_VAR_NAME_MAX`,
  `TDSH_VAR_VALUE_MAX` and `TDSH_SCRIPT_TASK_STACK` can be.
- The Editor's limits (`TD_EDITOR_MAX`, `TD_EDITOR_UNDO`,
  `TD_EDITOR_UNDO_OPS`) are in `apps/editor.c`, not `td_config.h`; they
  are wrapped in `#ifndef`.
- `lan` shows its state with no subcommand, not `lan status`.
- Board projects are in `ports/<board>/` in TinyDesk but in `projects/<board>/`
  (and the repository root for the ESP32-C6) in TinyDesk Shell.
- TinyDesk's core sources are `src/<module>.c` without a prefix, while
  TinyDesk Shell's are `src/core/tdsh_<module>.c`. Both are accepted inside
  their own repository.
