# HTTP Observability And Executable App Examples

Status: implementation and focused acceptance in progress.

## Scope

- Repair App README/runtime examples against the current guarded lifecycle.
  Prefer builder/daemon composition, protected hooks, metadata after types,
  immutable environment and awaited configuration/cleanup.
- Compile and run the canonical examples; mechanically check their documented
  source instead of treating illustrative Markdown as tested production code.
- Add HTTP server operational logs and explicitly opt-in redacted completion
  logs, using Forge macros and existing sink routing.
- Keep App as plugin lifecycle owner. The HTTP plugin logs TLS context reload
  outcomes, not a second server lifecycle.
- Preserve HTTP/TLS/auth behavior, streaming, public defaults and wire formats.
  Do not bump the project version or migrate downstream products.

## Acceptance

Focused native builds use one build tree and `-j4`. Run App/example, HTTP server,
TLS/auth, streaming, lifecycle, logger and package consumers plus structure
checks. Cover secrets/exception redaction, request opt-in, one streaming terminal
record, canceled/failed versus completed sends, bounded noisy failures, failed
TLS reload preservation and throwing sinks. No full CI is requested.

Two independent exact-head reviews cover observability/privacy/semantics and
ownership/layout/examples. Fix valid findings before a reviewed PR is merged
into `dev`. Preserve the acceptance logs, then remove only this task's inactive
build tree. No neighboring worktree or downstream source is part of cleanup.

## Boundaries

The application supplies log routing/exporters. HTTP owns neither OTLP lifecycle
nor delivery guarantees. Access records are diagnostic, not billing, security
audit authority or proof that a remote application consumed bytes. Library
readiness and product production-readiness are separate claims.

See the [donor traceability](../donors/http-server-observability.md).
