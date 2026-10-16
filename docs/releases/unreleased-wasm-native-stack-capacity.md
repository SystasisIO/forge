# Optimizing WASM local native-stack capacity

The optimizing compiler profile now requires codegen version 3. Regenerate
older codegen2 native artifacts through the owning compiler/session before use;
the canonical profile rejects them before executable mapping. Host invocation
ABI version 2 and its existing field layout are unchanged. Existing trap values
remain unchanged; `native_stack_exhausted` is appended as value 12.

Canonical `max_call_depth` exhaustion remains guest `stack_overflow`. Local
native frame allowance and an owned physical native-stack guard fault instead
produce `interpret::exceptions::execution_unavailable` after raw recovery,
host cleanup and stop-callback deregistration. Product callers must preserve
their enclosing rollback/non-invalid operational classification. Do not map all
stack_overflow traps to operational failure: the canonical depth limit is not a
local capacity option. Foreign C++ host faults remain foreign/fatal.

The executor's explicit `max_guest_stack_bytes` remains the physical usable
budget, now rounded down to an OS page. It must include a fixed conservative
4 MiB fallback reserve taken from Spring's stack policy. Increase existing
smaller configurations explicitly; the executor never adds that memory behind
the configured cap. Cold binding caches the largest validated native frame
`F` from the existing prepared descriptors. Logical frame allowance is the
admitted physical window minus the ABI return anchor, `F`, and that reserve.
This protects a callee whose machine prologue or entry epoch slow call executes
before its software byte-budget guard. A first/start frame that cannot fit is a
local unavailable error before guest memory reset or host effects.

Each guard is at least the supported 16 KiB fixed-frame bound, rounded to an OS
page, and adds explicit mapping overhead outside the usable budget. A larger
native frame is refused as local execution unavailable, not invalid WASM. Nested
invocations reuse the same bottom reserve and per-thread Mach recovery storage;
they do not consume 4 MiB of ordinary pthread stack or a new reserve per host
callback. The previous ordinary host-stack and paused raw-bridge gap are
separate quantities.

The 4 MiB reserve is a conservative local resource policy, not a mathematical
maximum for arbitrary libc code. Neither this change nor guard width is proof
of complete native execution. Compiler-to-executor finite small/big-stack,
actual canonical-depth, nested and both target runtime gates remain separate
required verification; current source changes do not declare those gates PASS.
