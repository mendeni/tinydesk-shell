# Memory and lifecycle safety

This refactor treats ownership as part of the architecture.

## Script runtime stays off small task stacks

`uscript_runtime_t`, its line table and explicit script session clones are
heap-backed. ESP-IDF script execution uses the dedicated 32 KiB worker stack.
This is designed to prevent the earlier failure mode where stack overflow
corrupts adjacent FreeRTOS/TLSF heap metadata and the crash appears later in an
unrelated allocation.

## Tracked core allocator

Parser/uScript-owned dynamic memory uses `tdsh_malloc`, `tdsh_calloc`,
`tdsh_realloc`, `tdsh_free` and `tdsh_strdup`. An aligned allocation
header tracks byte size/magic; atomic counters expose live, peak, total and
failed allocations. The ESP `heap` command prints both ESP internal heap and
TinyDesk Shell-tracked state.

The tracker complements rather than replaces ASan, ESP heap poisoning and stack
protection.

## Worker ownership contract

After `worker_run()` successfully starts, the platform owns its argument and
must invoke cleanup exactly once after the worker returns. If startup fails,
ownership remains with the caller. POSIX and FreeRTOS ports follow the same
contract for foreground and detached background execution.

## Fixed-capacity state

- commands: 128
- variables/session: 64
- arguments: 48
- line: 512 bytes
- variable value: 256 bytes
- uScript lines: 1024
- functions: 24
- function recursion: 8
- pipeline stages: 8
- pipeline/substitution capture: 8192 bytes

## Included stress test

`tests/test_memory_stress.c` performs:
- 2,000 command-substitution/pipeline iterations;
- 1,000 in-session uScript runtime cycles;
- 500 isolated foreground script worker cycles;
- 100 detached background script cleanup cycles.

After every iteration it checks tracked live blocks/bytes return to zero. CI
also runs ASan + UBSan + leak detection on POSIX.

## Board validation

After ESP-IDF integration, repeat uScript regression and board hwtests, then
measure internal heap minimum/largest block under repeated scripts and network
traffic. Add concurrent SD+W6100 and long-duration soak tests; host sanitizers
cannot reveal shared-SPI/driver lifecycle faults.
