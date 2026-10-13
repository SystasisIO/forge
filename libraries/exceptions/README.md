# forge_exceptions

`forge_exceptions` is the central FORGE exception layer: std-compatible typed
exceptions, numeric category codes, redacted diagnostic context and FC-like
capture semantics without bringing back the old FC exception hierarchy.

## When To Use

- Throw typed domain errors that callers can catch by concrete type.
- Wrap a lower-level exception with phase/resource context.
- Throw assertion and deadline failures with source location and typed context.
- Format nested exception chains for diagnostics without depending on `forge_log`.

## When Not To Use

- Do not model schema/config validation errors here; those live in `forge_schema`.
- Do not serialize diagnostic capture context through `variant` or JSON.
- Do not put application-specific error enums in FORGE core. Application code
  owns its own typed errors and declares categories next to those enums.

## Public API

Modules:

- `forge.exceptions`

Macro-only header:

- `forge/exceptions/macros.hpp`

Target: `forge_exceptions`.

Dependencies: `forge_core` only. It must not import `log`, `variant`, `json`,
`raw` or `crypto`.

## Examples

### Throw With Context

```cpp
#include <forge/exceptions/macros.hpp>

import forge.exceptions;

FORGE_THROW(
   "cannot open vault",
   forge::exceptions::ctx("path", "forge.vault"),
   forge::exceptions::secret("passphrase", "not logged"));
```

`secret(...)` values render as `<redacted>` in `what()`,
`format_exception_chain()` and log helpers.

### Throw A Typed Exception

```cpp
#include <forge/exceptions/macros.hpp>

import forge.exceptions;

namespace cache_errors {
enum class code : std::uint8_t {
   chunk_not_found = 1,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "cache")

using chunk_not_found =
   forge::exceptions::coded_exception<code, code::chunk_not_found>;
} // namespace cache_errors

FORGE_THROW_EXCEPTION(
   cache_errors::chunk_not_found,
   "chunk not found",
   forge::exceptions::ctx("ref", ref));
```

Callers can catch the concrete type:

```cpp
try {
   co_await cache.read(request);
} catch (const cache_errors::chunk_not_found& error) {
   handle_missing_chunk(error.code());
}
```

### Throw With Typed Operational Data

An exception may derive from `coded_exception` and own additional typed data,
such as a retry delay. Its constructor accepts `(data, message, fields,
source_location)`. Use `FORGE_THROW_EXCEPTION_WITH_DATA` to retain the concrete
type and the throw site's location:

```cpp
FORGE_THROW_EXCEPTION_WITH_DATA(
   service_errors::busy, retry_details{.after = std::chrono::milliseconds{250}},
   "service is at capacity", forge::exceptions::ctx("phase", "admission"));
```

The macro forwards the data expression exactly once and supports move-only
values. Data is not automatically logged or serialized into diagnostic context.
Classify failures by their type/category/code, then read the concrete exception's
typed data. Do not parse a retry decision from `what()` or diagnostic fields.

### Assert With Debug Context

```cpp
#include <forge/exceptions/macros.hpp>

import forge.exceptions;

void open_slot(std::uint32_t index, std::uint32_t capacity) {
   FORGE_ASSERT(
      index < capacity,
      "slot index is out of range",
      forge::exceptions::ctx("index", index),
      forge::exceptions::ctx("capacity", capacity));
}
```

`FORGE_ASSERT` throws a std-compatible `context_error`; callers should still catch
`std::exception` at process boundaries because other FORGE layers intentionally use
standard exceptions such as `std::invalid_argument` and `std::out_of_range`.

### Preserve Nested Cause

```cpp
#include <forge/exceptions/macros.hpp>

import forge.exceptions;

try {
   parse_config();
} FORGE_CAPTURE_AND_RETHROW(
   "config bootstrap failed",
   forge::exceptions::ctx("component", "http"))
```

The rethrow uses `std::throw_with_nested`, so callers can inspect the outer
`forge::exceptions::context_error` and the original inner exception.

### Format A Nested Exception Chain

```cpp
#include <forge/exceptions/macros.hpp>

import forge.exceptions;

try {
   try {
      parse_config();
   } FORGE_CAPTURE_AND_RETHROW(
      "config load failed",
      forge::exceptions::ctx("source", "service.yaml"),
      forge::exceptions::secret("passphrase", passphrase))
} catch (const std::exception& error) {
   auto chain = forge::exceptions::format_exception_chain(error);
   // chain contains outer context, inner std::exception::what(), and redacted secrets.
}
```

### Deadline Check

```cpp
#include <forge/exceptions/macros.hpp>

FORGE_CHECK_DEADLINE(deadline, forge::exceptions::ctx("phase", "handshake"));
```

This throws a std-compatible `context_error` with `std::errc::timed_out`.

### Process Boundary With Graceful Shutdown

At a daemon boundary, catch `std::exception`, format the chain, request
shutdown, and return an error. Do not turn recoverable startup failures into
`abort()` or detached cleanup.

```cpp
#include <forge/exceptions/macros.hpp>

import forge.exceptions;

int run_service() {
   try {
      return run_foreground();
   } catch (const std::exception& error) {
      auto chain = forge::exceptions::format_exception_chain(error);
      report_startup_failure(chain);
      request_stop_noexcept();
      shutdown_best_effort();
      return 1;
   }
}
```

The chain is diagnostic text, not a control-flow taxonomy. Application code should
use typed domain errors for decisions such as retry, backoff or user messaging.

### Route Capture Logs To `forge_log`

`forge_exceptions` exposes a neutral callback. The consuming program may route that
callback to `forge_log`, syslog, a test capture vector or any other sink.

```cpp
#include <forge/exceptions/macros.hpp>

import forge.exceptions;
import forge.log.logger;
import forge.log.record;

auto log = forge::logger{"worker"};

forge::exceptions::set_log_sink([&](std::string_view chain) {
   log.error(
      "cleanup failed",
      {
         forge::log_ctx("exception-chain", chain),
         forge::log_secret("session-token", token),
      });
});

try {
   cleanup_best_effort();
} FORGE_CAPTURE_AND_LOG(
   "cleanup best-effort path failed",
   forge::exceptions::ctx("phase", "shutdown"),
   forge::exceptions::secret("session-token", token))
```

`FORGE_CAPTURE_AND_LOG` deliberately swallows the current exception after routing
it to the callback. Use it only for cleanup paths where continuing is correct.
For correctness paths, use `FORGE_CAPTURE_AND_RETHROW` or
`FORGE_CAPTURE_LOG_AND_RETHROW`. FORGE exceptions keep their dynamic type and receive
extra context; non-FORGE exceptions are wrapped into a sanitized `context_error`.

## Risks And Anti-Patterns

- Do not convert every error into `context_error`. Use standard exception types
  when no structured context is needed.
- Do not throw `std::runtime_error` for public FORGE/app/network/API boundary
  failures that need stable handling. Add a typed exception family instead.
- Do not log-and-continue from correctness paths. Capture helpers must preserve
  failure semantics, not create silent recovery.
- Do not expose secret values through messages, `what()` strings or field names.
  Use `secret(key, value)` for data that may be sensitive.

## Typical Mistakes

- Do not catch only `forge::exceptions::context_error` at process boundaries. Also catch
  `std::exception` because many FORGE layers intentionally throw standard errors.
- Do not put secrets into the plain message string. Use `secret(key, value)`.
- Do not use `FORGE_CAPTURE_AND_LOG` on correctness paths if the error must
  propagate; logging must not become silent recovery.
- Do not call `std::terminate()`/`abort()` just to get a stack trace. Capture
  context, log the chain, and let the application lifecycle shut down.
- Do not branch on substrings from `what()`. Context fields are for diagnostics;
  application control flow should use typed errors or explicit result codes.

## Tests

`test_forge_exceptions` covers redaction, nested exception chains, assertion macro
behavior and deadline errors.
