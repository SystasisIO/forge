# Isolated Compiler Transport Ownership

Date: 2026-10-22. Status: repair candidate, runtime qualification pending.

## Inspected Pattern

Boost.Process 1.90 `boost/process/v2/stdio.hpp` separates
`process_input_binding` and `process_output_binding`. Its POSIX readable and
writable pipe bindings retain different descriptor owners and map them to the
appropriate standard descriptor with `dup2`. Adopt the independent lifetime of
stdin and stdout, not a new launcher or a second process owner.

The existing compiler-session child now uses two private Unix socket pairs in
one direction each. Socket-backed writes retain the existing per-socket
SIGPIPE isolation; replacing them with ordinary pipes would require a separate
signal policy and is not part of this repair. Close the parent request writer
after ACK, while retaining the response reader through strict output EOF.
The existing unmanaged Boost.Process handle remains the sole physical reaper.

## Boundary And Checks

The target is the existing private `compiler_session::impl::child` pair in
`libraries/vm/wasm/optimizing`, not a reusable transport facade. Spawn must
publish both parent descriptors before its PID without a throwing step; failed
setup/adoption, cancellation and shutdown must account for both descriptors.
Response EOF alone is not successful process completion.

Retain the registered bounded-session, late-spawn, workspace-refusal and
deadline-context checks, strict protocol-negative cases and real consumer
preflight tests. New transport tests and actual execution must qualify the
repair independently of source review and earlier CI. The triggering evidence
localizes a Darwin half-close/EOF liveness failure; it does not establish a
universal kernel defect. No timeout, protocol or public API change is approved.
