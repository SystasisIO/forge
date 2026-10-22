# WAMR native-stack admission compatibility

Executor construction again admits a physical guest-stack budget above the
fixed 4 MiB fallback reserve plus 32-byte return anchor. The WAMR port's
constructor-wide requirement for an additional maximum-sized 64 KiB frame is
removed; an artifact's actual capacity is checked at invocation, not guessed
before binding.

The executor retains its 64 KiB checked-frame/guard cap and page-rounded usable
mapping. Before memory initialization, module start, active post-instantiation
hooks or host effects, it checks room for the actual entry/initialization frame
in addition to the largest prepared function frame, fallback reserve and anchor.
A physical shortfall remains typed execution_unavailable; canonical logical
depth limits and guest traps are unchanged. No configured usable budget grows.

No consumer configuration increase or artifact relabeling is required by this
compatibility fix. Existing prepared artifacts still need the normal native
profile/provenance checks. Rebuild/reinstall the native package and qualify it
under its new source/inventory identity; prior-package receipts are not proof
for the changed runtime.
