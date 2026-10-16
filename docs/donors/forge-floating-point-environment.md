# Invocation-owned deterministic floating-point environment

2026-10-13. Existing numeric algorithms and the accepted SoftFloat TLS build
owner remain unchanged. This source revision replaces the rejected
`49690d97` lifetime wiring, not its preserved evidence or review history.

## Existing owners and approved host-fault policy

`forge.vm.wasm.interpret.floating_point_environment` is a real paired
implementation. Construction saves four SoftFloat TLS values, then selects
nearest-even / tininess-after-rounding / flags zero / extended precision 80.
Its `noexcept` destructor restores the exact saved values. Copy/move are
deleted; there is no pimpl, mutex, public globals API or new math algorithm.

The coordinated existing signal-owner amendment v3 is
`signal-boundary-amendment-v3.md`, SHA-256
`00cf69050ac9c6df1b360aa600f8d7928f401015b086857bb91a881d3036752e`.
Its exact declarations handoff is `signal-owner-v3-interface-handoff.patch`,
SHA-256 `6fd199fe340fd6a7a0ee1c96deec80c001f7af65b674477aaae3a636a69a47ce`.
The runtime worktree owns its paired signal implementation; this scope does
not introduce another process handler, transport or exception store.

The human-approved policy distinguishes software-validated WASM argument
failures from genuine C++ host SIGSEGV/SIGBUS. The former remain typed VM
failures; the latter forward to the displaced OS disposition (ordinarily
fatal). There is no raw host-fault recovery through live C++ RAII, and no
promise that destructors run after a fatal hardware fault.

## Exact gateway routing

Both JIT and interpreter/debug numeric execute roots use the existing
two-mode `longjmp_on_exception` adapter around their **entire** C++ body:
saved host/operand/call state, type and argument conversion, FP guard,
guest-boundary typed tail, result conversion and cleanup. Only a statically
checked trivially copyable `optional<operand_stack_elem>` result carrier
remains outside. The caller and the typed tail of the private guest
checkpoint must both have NULL recovery.

Each numeric root constructs an entry environment guard first, before type
checks or custom argument conversion, so normal and throwing converters
cannot replace the saved original caller state. The existing post-conversion
guest guard is retained: converters may mutate all four values, and guest
arithmetic must still begin canonical. Both guards and all cleanup live inside
the same NULL-recovery callback; the entry guard restores the actual caller
last. This adds no environment API or duplicate reset implementation.

All three registered-host paths delegate to `invoke_host`. Its existing
adapter suspends recovery before constructing the FP guard, obtaining the
execution interface, invoking the registry/proxies or entering user C++.
It contains no inner legacy raw-fault checkpoint. The two imported-only
numeric paths likewise call directly under NULL recovery. Defined guest
code alone uses the existing private checkpoint inside the encompassing
NULL C++ lifetime scope.

When already NULL, the adapter calls ordinary C++ directly: nested user
catches see original typed exceptions and complete their local unwind. When
entering from active guest code, the signal owner suspends recovery, catches
into invocation-owned storage and restores the ancestor only after all
callback and exception frames have completed. Its final nonlocal transport
has only trivial state. Mask/range/destination publication and restoration,
POD status handling and fail-stop restoration failures belong to the one
signal owner and require its separate source/test acceptance.

A callback may mutate all four values and invoke nested WASM. Nested guest
arithmetic is canonical and then restores the callback's mutated state.
Callback completion restores the guest's saved environment, including
accumulated flags; whole invocation completion restores the external state.
The guard is not reset on internal WASM-to-WASM calls.

## Regression and current evidence scope

The real backend-matrix fixtures cover four-value nesting; halfway arithmetic
after host mutation; export/numeric/table/re-exported imports and start;
exact host exception identity/destructor completion; a nested typed host
failure caught locally; ordinary guest traps and a guest load at the first
inaccessible byte. Fatal C++ protected-memory and foreign SIGBUS probes run
only in bounded child processes. Both hardware-fault dispositions are reset
to default; the actual PROT_NONE load may terminate with SIGSEGV or SIGBUS
(including Darwin's SIGBUS behavior). The separate explicit `raise(SIGBUS)`
probe must terminate with SIGBUS. These assert OS signal termination, not
host cleanup. Real custom root-converter cases mutate all four values and
either return or throw the original typed exception after local RAII cleanup;
they check exact external restoration, canonical halfway guest results and
a later successful invocation. Legacy watchdog tests cover guest-loop timeout, a locally
caught nested timeout and timeout while user C++ cleanup/FP scopes are live,
followed by safe guest reentry and a later successful invocation.

The existing TLS regression expects canonical guest arithmetic while each
thread retains its distinct ambient four-value state. Raw C mode-dependent
rounding/flags checks are unchanged.

**This narrow P2 correction has not yet been compiled or executed.** The paired
signal-owner implementation and deterministic transition-window tests are
separate required prerequisites. Previous four focused interpreter cases,
unit 192 / internal 12 / spec 2428 and selected five-target CTest logs are
preserved under `artifacts/oc-phase0/build/fp-environment-*`; they apply only
to the rejected narrower revision, not to this source.

Local macOS arm64 can establish interpreter evidence only. Native JIT and
OC, guarded-stack/callback-bounce, concurrent signal-owner, installed-package
and product gates remain NOT_RUN until their actual lanes run.
