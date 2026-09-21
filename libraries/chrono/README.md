# forge_chrono

`forge_chrono` owns pure formatting and parsing helpers for `std::chrono`
values and a wide nanosecond-precision `forge::chrono::timestamp`. Use the wide
value for dates outside the signed 64-bit nanosecond epoch range; keep ordinary
clocks and deadlines in `std::chrono`. It owns no clock, scheduler, thread,
FC wire conversion or P2P lifecycle.

## Public Modules

- `forge.chrono.iso8601` provides the legacy Forge ISO forms and strict RFC3339
  nanosecond parsing and formatting.
- `forge.chrono.relative` provides human-readable relative-time formatting.
- `forge.chrono.timestamp` provides a `sys_seconds` value plus a nanosecond
  remainder, comparisons and conversion from `sys_time<nanoseconds>`.

Target: `forge_chrono`. Package component: `chrono`.

## Boundaries

- Callers acquire wall-clock values themselves and pass explicit timestamps.
- `forge_raw` owns FC-compatible binary time serialization.
- `forge_variant` owns conversion to and from dynamic values.
- The leaf has no async runtime, scheduler, network or P2P dependency.
- Wide values and generic RFC3339Nano parsing belong here. Consumers own
  protocol validation, expiry and any original signed text.

## ISO And RFC3339

The `format` and `parse_*` functions retain existing millisecond text while
round-tripping full microsecond values. Generic text parsing does not inherit
the FC `uint32` wire range; that validation belongs to `forge_raw`.
Legacy ISO formatting accepts only the Boost Gregorian range from
`1400-01-01T00:00:00` through `9999-12-31T23:59:59` and throws
`std::out_of_range` before invoking Boost outside that range.
`format_rfc3339` emits canonical UTC text with `Z` and trims only insignificant
fractional zeroes. `parse_rfc3339` accepts `Z` or numeric timezone offsets and
rejects invalid dates, trailing data and values outside the exact `int64`
nanosecond range from `1677-09-21T00:12:43.145224192Z` through
`2262-04-11T23:47:16.854775807Z`.

`parse_rfc3339_timestamp` uses the same parser without narrowing to an
`int64` nanosecond count. It accepts four-digit years, including year 0000
and year 9999, and truncates fractional digits beyond the ninth without
rounding, matching RFC3339Nano consumers. The narrow `parse_rfc3339` still
rejects more than nine fractional digits and checks its exact epoch range.
Both parsers retain the existing uppercase `T`/`Z`, calendar and numeric
offset rules; leap seconds are rejected. Offset normalization may carry the
wide value outside years 0000..9999; formatting such a value throws
`std::out_of_range`. Malformed text throws `std::invalid_argument`.

`timestamp{seconds, remainder}` requires a remainder in [0, 1 second) and
throws `std::invalid_argument` otherwise; it does not silently normalize.
Conversion from signed nanoseconds uses quotient/remainder, including at
`INT64_MIN`. There is no `now()`, arithmetic API or network compatibility alias.

```cpp
#include <chrono>
import forge.chrono.timestamp;
import forge.chrono.iso8601;

const auto expiry = forge::chrono::iso8601::parse_rfc3339_timestamp(
    "9999-12-31T23:59:59.999999999Z");
const auto text = forge::chrono::iso8601::format_rfc3339(expiry);
const auto now = forge::chrono::timestamp{
    std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now())};
const bool expired = expiry < now;
```

Do not canonicalize text before verifying a signature over the original bytes,
or convert wide seconds to nanoseconds without checking the destination range.
Validation targets: `test_forge_chrono` and
`test_forge_package_chrono_component`; IPNS protocol coverage belongs to P2P.
