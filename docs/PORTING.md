# Porting TinyDesk Shell

Implement `tdsh_platform_api_t` and provide a filesystem/VFS supporting the
standard C calls used by core (`fopen`, `stat`, `opendir`, `rename`, etc.).

Minimum useful hooks:
1. monotonic milliseconds;
2. sleep/yield;
3. random bytes;
4. optional allocator family;
5. worker/thread primitive when dedicated-stack/background scripts are needed.

A single-threaded target can leave `worker_run` null: foreground scripts run
synchronously and background scripts report unsupported.

Do not spread target `#ifdef`s through `src/core`. Add a new port directory and
run the same host conformance tests against the unchanged core.
