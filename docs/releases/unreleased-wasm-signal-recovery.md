# WASM host signal-recovery migration

Interpreter/JIT and optimizing host callbacks now execute their complete C++
lifetime with VM signal recovery suspended. This includes argument/result
conversion, local catches, nested VM invocation and cleanup. Typed exceptions
reach guest execution only after those C++ frames have unwound. Nested host code
can catch a typed nested-VM error locally and continue normally.

The existing exception adapter also uses the owning raw suspension for
standalone and imported-only entry with no active checkpoint. SIGPROF is masked
throughout that callback's C++ lifetime, then the original mask is restored.
Typed propagation in this NULL-parent mode is an ordinary C++ rethrow, not an
escape to an ancestor checkpoint.

Genuine C++ host `SIGSEGV`/`SIGBUS` faults are foreign faults, forwarded to the
previous process handler or the default fatal disposition. They are not ordinary
contract errors and do not unwind C++ objects. Products must validate every
guest-controlled pointer, span, string and fixed-width access in software before
entering the host body. This policy does not authorize converting guest argument
errors into process faults. The Spine `prints`/`eosio_assert` conversion uses the
existing full NUL-string validator before the host body, including successful
assert conditions; the message truncation limit still applies after validation.

Only guest execution retains the existing managed signal checkpoint.
Private guest callbacks remain restricted: this change does not promise generic
C++ exception-unwind safety under arbitrary asynchronously delivered signals.
Built-in interpreter typed traps can begin ordinary C++ unwind under the guest
checkpoint; its catch publishes NULL before capture and the typed wrapper tail,
not before every earlier guest EH instruction. Host conversion, user callbacks
and their guards never use that guest checkpoint.

The native
boundary additionally requires an owned instruction pointer and explicit memory,
code or stack-guard ranges. Empty ranges cannot become a foreign-fault catch-all.
Linux `SIGBUS` ownership is unchanged: this owner does not intercept it.

The raw boundary/suspension callbacks are `noexcept`, return POD status, and do
not own exception storage. After restoring the previous recovery destination,
only a destructor-free return or existing guest escape may follow. Failed OS
restoration is fail-stop because a borrowed alternate stack or process-handler
lease cannot safely be reported as released. Ordinary guest traps, typed host
errors, cancellation and local capacity failures are not fail-stop conditions.

Darwin ARM64 adds same-owner cold calling-thread preparation/release. Native
workers must explicitly prepare a borrowed host recovery reserve before entry,
outside any product writer; the reserve remains alive through release/join.
Prepared workers hold stable Mach routes and POSIX handler ownership, with no
per-invocation service or port churn. Existing standalone interpreter entry points
keep bounded same-owner lazy TLS preparation and their old POSIX per-call lifetime.
Linux preparation/release are no-op success; native entry still uses/restores the
actual supplied POSIX alternate stack. Darwin hardware recovery instead lands on
the admitted host reserve before C/C++, preserving caller alternate-stack state.

OS readiness/capacity/unsafe-fork failures now use the existing coded exception
`interpret::exceptions::execution_unavailable` (4000011), not guest `interpreter`
errors. Products must catch it before broad WASM exception mapping and preserve
their enclosing rollback/non-invalid classification. This Forge alias alone is
not end-to-end Spine recovery. NULL callback/invalid caller-span errors remain
separate caller-contract errors.

The FIRST Mach source slice rejects and exactly restores every non-null prior
THREAD exception route before guest entry. Full asynchronous forwarding and
debugger/crash-handler coexistence remain OPEN; no synchronous RPC timeout is
used as a substitute. Mach-initialized fork children cannot reuse the service:
use fresh spawn/exec, and release/keep borrowed resources alive until worker join.

This is not full optimizing runtime acceptance on either required target.
The first full unit/internal/native CTest failed; after causal owner and
fresh-process filter corrections, all three complete groups passed (197/24/18
cases) and each passed three further bounded repetitions. The private legacy
donor raise(SIGSEGV) exposed an invalid Darwin si_code provenance heuristic.
Its correction preserves only the original
private both-empty active checkpoint catch-all; public native and actual nonempty
production interpreter POSIX delivery always forward on Darwin ARM64, with
hardware guest recovery exclusively Mach-owned. Whole-NULL host delivery remains
foreign/fatal. This private trivial-only compatibility is not a general C++
recovery promise or expansion of the public native fault whitelist.

Displaced POSIX simple/detailed handlers likewise run with canonical VM recovery
NULL until their whole callback/cleanup finishes. Legacy typed exceptions use the
existing external outcome; unexpected C++ throw from an active native POSIX
handler is a host-contract defect and terminates while NULL, not a guest trap.
The corrected complete groups passed 197/27/18 cases plus three bounded repeats,
including real foreign-handler owned-memory fatal controls.

Linux/x86-64 OC execution, full foreign-route forwarding and Spine integration
are separately required, not implied by these focused owning-group passes.
