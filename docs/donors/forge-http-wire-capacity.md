# Buffered HTTP wire allocation

## Problem and ownership

The buffered HTTP serializer formerly appended each native Beast buffer to a
growing byte vector. Logical wire size remained bounded, while geometric vector
growth could retain more allocation than the message length. A consumer with an
intentional retained-capacity bound could reject the otherwise valid request.
The observed failure was 378 logical bytes in a 440-byte retained vector.

The allocation owner is the existing
[HTTP wire serializer](../../libraries/net/http/wire.cpp), paired with its
[public module](../../libraries/net/http/include/forge/net/http/wire.cppm).
No transport, transcript verifier, application policy or second HTTP encoder is
introduced. The consumer's retained-memory checks remain unchanged.

## Primary donor inspected

The inspected dependency is Boost.Beast from Boost 1.90.0:

- `boost/beast/http/serializer.hpp`, `next`/`consume` contract: the synchronous
  visitor receives a native ConstBufferSequence; consuming advances that
  serializer's cursor. At completion the visitor may not be called.
- `boost/beast/http/impl/serializer.hpp`, `do_init`, `do_header` and `consume`:
  the serializer combines fields-writer buffers with body-writer buffers without
  constructing a contiguous body copy.
- `boost/beast/http/string_body.hpp`, `writer`: it retains a const body reference;
  `init`/`get` do not mutate or copy body bytes.

Forge's concrete conversion produces one owned Beast `string_body` message.
Two fresh serializers over that same unchanged message are therefore safe.
This reasoning is not a general permission to replay a stateful custom
BodyWriter or a streaming body producer.

## Accepted mechanism and bounds

Keep the existing pre-copy validation. The first serializer sums only delivered
buffer lengths, checking each addition against `max_wire_bytes - total` before
arithmetic. It consumes each visited nonempty sequence and is destroyed before
output reservation. No body bytes are copied during this pass.

Reserve the measured wire size once in the fresh output byte vector. A second
fresh serializer performs the existing bounded buffer copies. Its copied size
must also remain within the measured size and equal it at completion; otherwise
the operation fails with a typed error. Generated framing inspection and
sensitive-field offsets still operate on the final exact bytes.

The existing conversion to a Beast message still copies its body. The change
does not promise allocation-free serialization or eliminate header-writer
storage. It removes incremental output growth and its transient overlap.

On the inspected libc++ implementation, fresh vector reservation allocates the
requested element count through the default allocator. ISO C++ only guarantees
capacity at least that count; the exact-capacity regression must remain a gate
on every supported standard-library/toolchain combination. A different platform
does not gain permission to weaken a consumer's retained-memory bound.

## Required checks

The existing [wire tests](../../tests/http/wire_tests.cpp) retain their golden
bytes, strict framing/injection rejection, case-insensitive/repeated sensitive
offsets, and header/body/combined/wire bounds. Additional segmented-header cases
cover empty and binary bodies through 32 KiB, exact `capacity == size`, unchanged
source, parse roundtrip, sensitive offsets, exact wire limit and one byte below
that limit. Native execution results are separate qualification evidence; this
note does not claim a successful build, test run or consumer session.
