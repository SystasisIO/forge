# Unreleased: SoftFloat state linkage changes to TLS

The existing internal SoftFloat target now consistently exports
`THREAD_LOCAL=__thread` to its C definitions and C/C++ consumers. Four state
variables change from ordinary globals to thread-local storage:
`softfloat_roundingMode`, `softfloat_detectTininess`,
`softfloat_exceptionFlags`, and `extF80_roundingPrecision`.

This is a native binary-linkage change. Perform a clean rebuild of the full
SoftFloat archive and every dependent library, executable and module BMI;
do not mix ordinary-state objects/declarations with TLS objects/declarations.
Reinstall the package and rebuild external consumers against the same owning
target and its exported usage requirements. Persisted WASM and Raw protocol
layouts do not change.

The pinned donor checkout and arithmetic algorithms are unchanged. TLS removes
cross-thread sharing of numerical modes/flags; same-thread invocation and
host-callback environment isolation is a separate reviewed execution change.
See the [source/ABI trace and local evidence](../donors/forge-softfloat-tls-boundary.md).
