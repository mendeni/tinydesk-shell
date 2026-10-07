# Architecture

```text
Application / Product (product, gateway, test fixture, ...)
             |
             | register commands / call SDK APIs
             v
+-----------------------------------------------+
| Portable TinyDesk Shell Core                          |
| command registry, sessions, parser, uScript,  |
| path jail, portable builtins, memory tracking |
+-----------------------------------------------+
             |
             | tdsh_platform_api_t
             v
+----------------------+   +---------------------+   +---------------------+
| ESP-IDF port         |   | POSIX host port     |   | Windows host port   |
| FreeRTOS, USB, NVS,  |   | pthread, host fs    |   | Win32 console,      |
| LittleFS, network... |   | CI/tests            |   | host fs, tdsh.exe   |
+----------------------+   +---------------------+   +---------------------+
```

## Portable-core rule

Files under `include/` and `src/core/` must not include ESP-IDF or FreeRTOS
headers. Board/network/server drivers live in the platform port or application.

## Command registration

The registry is fixed at `TDSH_MAX_COMMANDS`. The descriptor is copied but
its strings/functions remain caller-owned; use `static const` descriptors.
Registration is normally completed before remote/interactive sessions start.

## Sessions

Each terminal/transport should own a `tdsh_session_t`. Explicit scripts get a
private clone. Future SSH/WebSocket/USB sessions should not share cwd/variables.

## Platform API

The platform API is intentionally small: time, sleep/yield, random bytes,
allocator hooks, a worker primitive and, optionally, the memory regions
`peek` and `poke` may access. Standard C filesystem APIs remain in
core where the platform VFS supports them.

## ESP-IDF ownership

The full C6 port preserves current ESP modules, but an embedding application can
disable SDK network-manager startup and network/hardware command groups. This
prevents two subsystems from owning the same ESP-NETIF, GPIO or board resource.
