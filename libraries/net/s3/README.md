# S3 client

`forge_net_s3` is an optional C++23 client for S3 object operations. Import
`forge.net.s3.client`, `forge.net.s3.types`, or `forge.net.s3.exceptions`.
The public source contract is **Preview**. AWS SDK types and HTTP implementation
remain private. The library owns no application database, authorization,
artifact identity, retention policy, or durable upload journal.

Use this client for bounded asynchronous access to an explicitly configured
S3 endpoint. Use a product service above it for permissions, immutable keys,
content digests, credentials resolution, retention and crash recovery.

## Build and dependencies

Enable `FORGE_ENABLE_S3=ON` and supply AWS SDK **1.11.900**, component `s3`,
through `CMAKE_PREFIX_PATH` or `AWSSDK_DIR`. The verified source revision is
`51167b10e1e8818efb5fd57be6f8ce09e4bcd79d` (tag `1.11.900`), including its pinned
CRT submodules. Installed consumers request `find_package(Forge CONFIG REQUIRED
COMPONENTS net_s3)` and link `Forge::forge_net_s3`.

Public dependencies are `forge_asio`, `forge_exceptions`, and
`forge_crypto_core`. AWS SDK is a private implementation/link dependency.
Build only `BUILD_ONLY=s3`, with `ENABLE_TESTING=OFF`. Core and CRT dependencies
are still required. The entire graph must use the same OpenSSL implementation
as Forge. A prebuilt SDK or CRT may pull a different OpenSSL transitively;
verify that graph before deployment. Building SDK dependencies from source
requires `USE_OPENSSL=ON`, explicit OpenSSL/crypto discovery and local s2n
selection; `OPENSSL_ROOT_DIR` alone does not control CRT `find_package(crypto)`.
On a machine with another installed s2n, use `CMAKE_DISABLE_FIND_PACKAGE_s2n=ON`
for the SDK source build so its own pinned s2n is used. Never modify the global
package-manager installation to repair an application build.

## Ownership and example

The application owns one dedicated, bounded `forge::asio::compute::pool` for
blocking S3 calls. Its executor must not be the application's CPU pool or I/O
executor. Synchronous SDK calls run only on these workers; continuations return
to the caller's Asio executor. The SDK async/callable APIs are not used.
`try_submit` rejects a full executor immediately; the client adds no waiting
submission queue. `config.max_calls` bounds active and queued client calls.

```cpp
import forge.asio.compute;
import forge.net.s3.client;

forge::asio::compute::pool workers{{
   .worker_threads = 2,
   .max_pending_tasks = 8,
   .max_waiting_submissions = 0,
   .thread_name = "object-storage",
}};

forge::net::s3::config config;
config.endpoint = "https://storage.example.test";
config.signing_endpoint = "https://download.example.test";
config.region = "us-east-1";
config.identity = resolved_credentials; // Resolve secret references outside S3.
forge::net::s3::client client{workers.get_executor(), std::move(config)};

// In the application's coroutine, source remains available until completion.
const forge::net::s3::object target{"results", "immutable/content-key", {}};
auto stored = co_await client.put(target, source_path,
   {.content_type = "image/png", .if_absent = true});
auto link = co_await client.presign(target, std::chrono::seconds{300});
// Reveal link.url.view() only to the authorized recipient; never log it.

client.request_stop();
co_await client.shutdown();
co_await workers.shutdown();
```

The SDK process scope initializes once while clients exist and shuts down after
the last client backend is destroyed. The private CRT bootstrap has one event
loop thread and a bounded host resolver. Do not independently call
`Aws::InitAPI`/`Aws::ShutdownAPI` in the same process. If another library owns
the SDK global lifecycle, integrate that ownership before combining them.
Destroying a client requests stop. `shutdown()` closes admission and waits for
all admitted work, including work whose caller stopped waiting, before releasing
SDK clients and the process scope. It must be awaited before the dedicated pool
is destroyed. One shutdown waiter is allowed at a time. No detached task or
forced thread termination is used.
SDK teardown also runs on a dedicated worker. If the shared dedicated executor
is full, shutdown reports `busy` and retains the backend for a later shutdown
attempt; reserve capacity and stop/drain consumers before shutting down this pool.

## Transfers and errors

`put` accepts owned bytes or a file path. Byte sources are limited by
`max_memory_bytes`. A file is opened on a worker, read through a 64 KiB seekable
buffer and kept open through the transfer. The caller owns the source file's
lifetime and must prevent concurrent content changes. Automatic multipart uses
one part at a time, bounded by `part_bytes`; the default is 8 MiB and the minimum
is 5 MiB. At most 10,000 parts are admitted. Larger objects require an explicitly
larger part size within the configured memory budget. Per-call RAM includes the
owned input, one part and stream buffers; account for `max_calls` when sizing
the process. No ordinary file upload materializes the complete file in RAM.

`get` returns bounded bytes or writes a bounded temporary file beside its
destination. Successful file download flushes and syncs the file, then renames
it into place. A failed/canceled download removes its temporary file. This is
file publication, not a durable directory transaction or application journal.
Range responses must match length and `Content-Range`. Explicit limits apply
even when the endpoint lies about its `Content-Length`.
Control/XML responses are separately limited to 1 MiB before SDK parsing.
Optional response checksums supplied by the endpoint are validated by the SDK;
checksum failures reject the read. Request checksums follow the SDK's required
operation rules; product content digests remain independently required.

`begin`, `upload`, `parts`, `complete` and `abort` expose multipart identities,
part ETags and bounded listing cursors for application recovery. Persist the
upload ID before proceeding when crash recovery is required. ETags are opaque
server validators, not content digests. Conditions (`if_absent`, `if_match`)
apply to single PUT and multipart completion. Versions are supported on
head/get/delete; put, multipart and presign reject a version rather than
silently targeting a different object.

SDK retries are disabled. A transport failure, cancellation or server failure
after mutation dispatch raises `exceptions::unknown_outcome`; reconcile the
stable bucket/key or upload before any application retry. Cancellation before
dispatch raises `canceled`; a passed deadline raises `deadline`. Stopped or
full admission raises `stopped`/`busy`. Known 404, denied access and failed write
conditions map to `not_found`, `denied` and `conflict`. AWS raw error messages,
credentials and full signed URLs are never attached to errors.
Unexpected backend failures are sanitized and use the same typed boundary:
`service` before possible mutation and `unknown_outcome` after it. Existing
Forge errors retain their type, source location and redacted context.

Each request has finite connection/request timeouts and an operation deadline.
SDK progress callbacks observe caller stop tokens, pool stop, client stop and
deadline. Canceling an Asio waiter may return before the synchronous call ends;
its buffers/client remain owned until actual completion. Cancellation does not
promise that an S3 mutation had no effect. A successful response confirms the
operation even if cancellation raced with that response. The SDK's finite
timeouts remain the upper bound when no progress callback runs.

Automatic multipart attempts one finite best-effort abort after a failed part.
It never automatically aborts after completion was attempted because completion
may have succeeded. Failed cleanup, a lost begin response or a process crash
can leave uploads behind. Configure the bucket's incomplete-upload lifecycle
and keep an application recovery journal; the client does not claim to clean
every orphan. Multipart errors include the known upload identity for recovery.
Their context also records whether cleanup was attempted and confirmed, and
whether completion had been attempted. Failed cleanup does not replace the
original failure.

## Credentials and URLs

Credentials use `forge::crypto::core::secret_string`. Resolve secret references
outside this library, then rotate with `update_credentials`. There is no ambient
credential chain, metadata-service discovery, external process or unsigned
fallback. Session credential expiry caps signed URL lifetime. Signing uses the
external endpoint from the start; do not rewrite a signed hostname. TLS peer
verification is enabled and HTTP redirects are disabled. HTTP endpoints are
accepted for explicit local deployments/tests; deployment policy belongs to
the application. S3 Express, directory buckets and multi-region/ARN behavior
are not verified contracts of this leaf.

## Tests and limits

Build `test_forge_net_s3` and run `ctest -R '^test_forge_net_s3$'`. The loopback
HTTP fixture exercises the real pinned AWS SDK: binary put/head/get, ranges,
response bounds, write conditions, missing objects, external-host signing and
credential expiry without unsigned fallback, corrupt response checksums, streamed
file multipart/download, explicit recovery parts,
failed-part cleanup, unknown mutation without retry, preflight and active
cancellation, overload rejection, and shutdown with an active request. It runs the fixture and caller on a single Asio
I/O thread while SDK requests use the dedicated worker pool.

These checks do not establish MinIO/AWS live compatibility, TLS deployment,
actual URL authorization/expiry, large-file memory measurements, process-crash
recovery or bucket lifecycle behavior. Those require endpoint acceptance tests.
