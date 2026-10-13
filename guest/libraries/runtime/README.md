# Guest Runtime

`forge_guest_runtime` is the freestanding wasm32 runtime used by Forge
contracts. It supplies the CDT-derived allocator, C memory/string primitives,
global new/delete, guest-local `errno`, libc++ abort glue and `memory.grow`
integration. It also supplies a pinned CDT musl C-library subset for ctype,
numeric parsing, formatting, classic locale, time formatting and real exit
callback registration. libc++ itself owns regex, locale facets and C++ ABI
pure-virtual termination; the runtime does not replace these algorithms.

The allocator supports aligned allocation, reuse, coalescing, `calloc` and
`realloc`. Allocation failures terminate through the canonical contract check
intrinsic. It owns no files, sockets, threads, clocks or host heap. The
implementation is compiled once while assembling the SDK and installed as
`sysroot/lib/libforge_guest_runtime.a`. Contract sub-builds link that archive;
they do not compile allocator or C runtime sources again. Private
`details/*.hxx` contain declarations only and are not installed. There is no
fake public module because the external C ABI comes from generated sysroot
headers.

Only deterministic `C`/`POSIX` locales are supported. Unsupported names fail
closed; the empty name selects `C` without consulting a host environment.
The locale adapter owns selection and guest-local errno only. Pinned musl owns
the inspected C-locale conversion algorithms and LIFO exit-handler registry.
`exit` explicitly drains that registry; `_Exit` and the host `eosio_exit` do not.
Normal `apply` return does not drain global destructors. VM initialization
discards globals and guest memory before each invocation and reruns startup,
including after normal return, a trap or a host exit. See `PROVENANCE.md` for
the exact donor files and deviations.

Allocator fragmentation, alignment, reuse, growth and exhaustion are exercised
by guest contracts and then executed through `forge.vm.wasm.interpret`. Oversized
requests and arithmetic overflow fail without changing existing allocations.
Aligned allocations are identified by allocator-owned block metadata; bytes in
ordinary allocation headers cannot redirect `free` or `realloc`.

## Dependencies And Boundary

The target uses only the wasm32 compiler builtins, generated intrinsic C API
and the configured guest sysroot. It has no C++ module because its public ABI is
the standard C/runtime surface consumed by libc++ and contract code.

The SDK first copies the configured libc++/libc++abi/compiler-rt sysroot into a
build-owned staging directory. One internal foundation build compiles the
runtime together with the shared raw, codec, chain protocol and contract
implementations. Their archives are installed into the staged sysroot and
covered by both `foundation.json` and the final sysroot hash. Developer mode
therefore never writes to the caller's input sysroot.

## Stability And Tests

Allocation and C memory semantics are production compatibility contracts;
private block layout is not API. Tests cover fragmentation, reuse, aligned
allocation, split/coalesced blocks, growable memory, `calloc`, `realloc`,
new/delete, deterministic exhaustion and the complete declared C string surface
through the real VM memory export. Error strings are stable symbolic errno names
rather than host-localized messages.
The E2E suite also links and executes `<cerrno>` reads and writes, proving that
`errno` does not escape as an undeclared host import.
Relocation tests verify that an installed consumer links the verified archive
through `forge_add_contract` but receives no public archive-path variables,
runtime `.cpp` or private `.hxx` files.
