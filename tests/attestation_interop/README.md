# F1 Independent JWT Interop (J2 / J3)

Test-only Python Authlib **joserfc 1.6.0**, Pydantic v2 and Java **Nimbus
JOSE+JWT 10.5** consumers. They independently verify Forge signatures and
independently sign synthetic fixtures for the native Forge verifier. No Forge
Raw decoder, production OAuth/token service, key provider or C++ library is
implemented here. `driver.cpp` and build/CI wiring are coordinator-owned.

Normative inputs: approved architecture plan sections 10.3 and 10.10.1, plus
the coordinator's exact fixture/CLI contract below. This corpus covers J2/J3
and selected J1/J5/J6/J7 checks; it is **not** acceptance of all J1-J10 or F1.
Real interop and Java compilation are **NOT_RUN locally**. Only the lightweight
syntax command is authorized locally; execute the rest in CI.

## Public Command

From the repository root, in each CI matrix job:

```sh
python3.12 -m venv "$RUNNER_TEMP/f1-interop-venv"
"$RUNNER_TEMP/f1-interop-venv/bin/python" -m pip install \
  --require-hashes --only-binary=:all: \
  -r tests/attestation_interop/requirements.txt
"$RUNNER_TEMP/f1-interop-venv/bin/python" -m pip check
"$RUNNER_TEMP/f1-interop-venv/bin/python" -B tests/attestation_interop/run.py \
  --driver "$FORGE_INTEROP_DRIVER" \
  --work "$RUNNER_TEMP/f1-interop-work" \
  --evidence "$RUNNER_TEMP/f1-interop-evidence.json"
```

`--work` and `--evidence` must not exist. Use separate paths per job/retry;
the harness does not delete or overwrite previous evidence. `emit` receives
an already-created work directory. Run outside the source tree so generated
credentials cannot accidentally enter a commit. The runner returns **0 PASS,
1 interoperability FAILED, 2 HARNESS_ERROR**. Native verifier crashes, timeout,
exit 2 or unexpected output never count as a valid negative test.

Requirements: CPython **3.12**, JDK **17+**, and the already-built native driver.
The Python runtime closure is locked for **ubuntu-26.04 x86_64** and
**macos-15 arm64**. Wheel hashes cover both platforms without weakening
`--require-hashes`; other interpreters/architectures fail closed. No source
dependency builds or extras are used. Pip/venv/JDK are CI toolchain inputs,
not recursively resolved runtime dependencies.

The runner downloads only the official Nimbus JAR from Maven Central and
checks `maven.lock.json` before launching Java in source-file mode. This is
compilation/execution **in CI**, not a local syntax check. For an offline CI
lane, set `NIMBUS_JAR` or pass `--nimbus-jar PATH`; the same hash is required.
`--java PATH` selects the CI JDK launcher. No Maven plugins or transitive
resolution are run: the official JAR contains shaded Gson 2.13.1 and JCIP
annotations 1.0-1. Optional Tink/Bouncy Castle are not used for JDK ES256.

Dependency-free local check (no pip, Java, native driver, keys or tokens):

```sh
python3 -B tests/attestation_interop/run.py --syntax
```

This checks Python AST, lock JSON, and unsigned corpus structure/IDs. It does
not compile Java, import third-party Python libraries or claim interop PASS.

## Coordinator Driver Contract

```text
driver emit <directory>
    writes forge.jwt and public.jwk; exit 0 on success
driver schema <directory>
    writes document.schema.json: actual full Forge-exported document schema,
    with the native product schema bound at att.claims; exit 0 on success
driver verify <token-file> <public-JWK-file>
    exit 0 + stdout ACCEPT: accepted
    exit 1 + stdout REJECT: invalid credential
    exit 2: harness/infrastructure error
```

Tokens are exact ASCII Compact bytes, **without a trailing newline**. Public
JWKs contain `kty=EC`, `crv=P-256`, canonical 32-byte `x`/`y`, and optionally
`kid=interop-key`, `alg=ES256`, `use=sig`, `key_ops=[verify]`. No private `d`,
unknown members or remote key lookup are admitted. The JWK supplied as a CLI
argument is the explicit trusted key; token-controlled key URLs/material are
never consulted. The coordinator's Forge fixture uses its known test-only
P-256 scalar 1 / standard generator public key. External signing keys must
**not** reuse that key: Python and Java each generate fresh independent P-256
keys in memory and export **public JWKs only**.

The common test profile is fixed, not inferred from unverified claims:

| Field / Rule | Value |
|---|---|
| Protected header, exactly | `alg=ES256`, `typ=forge-attestation+jwt`, `kid=interop-key` |
| `iss` | `https://issuer.example.test` |
| `aud`, string only | `https://resource.example.test` |
| `att`, exactly | `v=1`, `purpose=artifact.read`, `schema=https://schemas.example.test/grant/v1`, `claims` |
| `jti` | `g_` plus 64 lowercase hex digits; native fixture may choose any conforming ID |
| Fixed verifier time | `1800000100` |
| Native fixture `iat / nbf / exp` | `1800000000 / 1800000000 / 1800000300` |
| Lifetime / skew | at most 300 seconds / exactly 0 seconds |
| NumericDate | integer, `0..253402300799`; `iat <= nbf < exp`, `iat/nbf <= now`, `now < exp` |

Native fixture claims, with exact names and types:

```json
{
  "object": "o_17",
  "generation": "18446744073709551615",
  "ranges": [{"offset": "9007199254740993", "length": "1"}],
  "note": "\u043c\u0435\u0434\u0438\u0430"
}
```

Claims have exactly `object`, `generation`, `ranges`, `note`; every range has
exactly `offset`, `length`. All are required. `object` is a nonempty string;
`note` is also nonempty. Both strings have the native fixture's **1..128
Unicode scalar values** product bound, matching JSON Schema `minLength` /
`maxLength`; this is neither a UTF-8 byte count nor a UTF-16 code-unit count.
The separate global 2,048 UTF-8 byte cap still applies to every string/key.
`ranges` is an array of **1..8** items. These are narrower
product limits than the global F1 envelope bounds below. Counters are canonical decimal **strings**, `0` or a
nonzero leading digit followed by digits, at most `18446744073709551615`.
No float conversion, coercion, unknown fields or NUL is accepted. No business
range-sum/overlap rule is invented: this fixture tests wire/schema constraints,
not storage authorization. The native verifier must enforce these same rules.

Both external verifiers enforce 16,384 token bytes, 512 decoded header bytes,
8,192 decoded payload bytes, container depth 8 (root object depth 1), 512 value
nodes including root, 128 members/elements per container, and 2,048 UTF-8 bytes
per string/key. They reject duplicates after unescape, invalid UTF-8, lone
surrogates, NUL, trailing JSON, NaN/Infinity, noncanonical/padded Base64url,
non-64-byte signatures, forbidden headers, algorithms and profiles. URI
identifiers use exact constants, not normalization or network dereference.
Ordinary JSON whitespace/order/escapes remain legal. Signatures are always
checked against the **original segments**, never a reserialized document.

## Execution And Evidence

`run.py` performs the following, failing closed at each step:

1. Creates unsigned `vectors.json`, verifies the pinned Nimbus JAR, invokes
   native `emit` and `schema`, and checks that native `verify` accepts its own baseline.
2. Runs Python and Java independently. Each verifies `forge.jwt` with Nimbus
   or joserfc as appropriate and compares all expected claims (native `jti`
   must satisfy the profile but is not hard-coded).
3. Each language independently generates/signs the same case IDs, writes its
   own public JWKs, self-checks every expected result and writes a manifest.
4. Python verifies Java's entire corpus; Java verifies Python's entire corpus.
   The runner checks manifest completeness, file hashes and semantic equality
   of every positive fixture with the unsigned reference.
5. Native `verify` checks **every** Python and Java case against its paired
   public JWK. Only exact exit/output agreement produces gate PASS.

Individual commands, normally invoked by the runner:

```text
python_harness.py emit <work>
python_harness.py verify-set <work> <other-manifest.json>
java --class-path <pinned-jar> AttestationInterop.java emit <work>
java --class-path <pinned-jar> AttestationInterop.java verify-set <work> <other-manifest.json>
```

`corpus.py` supplies unsigned bytes and expected results, **not a shared
verifier**. Python uses explicit profile/time/bounds assertions and validates
against the actual Forge export plus untouched generated Pydantic models.
Java has separate assertions and a bounded strict
Gson streaming walk using the copy already shaded in Nimbus, not a custom
JSON tokenizer. Signature algorithms and encoding are owned by the real JOSE
libraries. Normal `valid` tokens also exercise their high-level JWS writers.
Malformed signed fixtures deliberately use the libraries' ES256 signing
primitive over exact synthetic input; no handwritten ECDSA/crypto is present.
An invalid `alg` header can still carry a mathematically valid ES256 signature;
this tests algorithm/profile rejection, not acceptance under the false alg.
Tamper/encoding/wrong-key cases are identified separately by manifest `mode`.

The corpus includes positive reordered/escaped JSON, exact byte limits,
UTF-8 wire/product scalar string limits, product array boundaries and decimal boundaries; negatives cover
missing/unknown fields at all modeled levels, header substitution, OAuth-style
types/claims, malformed JSON, bounds, wrong numeric kinds, overflow, time
ordering/lifetime/expiry, tampering and same-`kid` wrong-key verification.
It is not an exhaustive J6 fuzz/sanitizer suite, RNG proof, product delegation,
revocation, trust-snapshot/cache test or standards-vector qualification.

Per-language `manifest.json` includes case IDs, expectations, modes, public-JWK
and token SHA-256 values, plus native fixture/corpus hashes. The evidence JSON
contains only statuses, public fixed profile values, source/driver/dependency
hashes and per-case driver exits. It never contains token bytes, payloads or
key bytes. Child stdout/stderr are suppressed even on failure; `last_step`
identifies the failing phase or native case without unsafe exception diagnostics.
Keep evidence on failure. Raw `.jwt` files are synthetic but still credentials;
do not upload the entire work directory to public CI logs/artifacts. No private
key is written, logged, serialized or committed. Work paths are restrictive,
created once, and intentionally not auto-cleaned by this harness.

## Generated Schema Example

`schema_example.py` consumes **`document.schema.json` emitted by the native
driver**. It invokes pinned `datamodel-code-generator 0.83.0` with the strict
Pydantic v2 flags already used in
[the existing qualification harness](../api/openapi_python/qualify.py).
It reuses that donor's version checks, exact JSON loading and equality helper.
There is no second Python schema generator, reconstructed native schema,
generated-model rewrite, subclass or monkeypatch. The JSON Schema dialect
input and root class name `Attestation` are the only generator adaptations.
Remote references are rejected before code generation; schema validation also
has an explicitly offline reference registry.

Lookaround patterns, including the exporter's strict-end `(?![\s\S])`, do not
require a CLI override or model patch. The pinned generator automatically emits
`ConfigDict(regex_engine="python-re")` for models whose fields use lookaround;
this upstream support dates to
[0.25.7 / PR 1945](https://datamodel-code-generator.koxudaxi.dev/changelog/#0257-2024-06-11)
and is present in the audited official 0.83.0 wheel's Pydantic v2 backend.
The generated Python `re` configuration is intentional; the default Rust regex
engine does not support these assertions. The native/exported patterns remain
unchanged. CI imports the untouched generated module before qualification,
checks its hash immediately after import and during validation, and records
`generated_import`, per-model `regex_engines`, and `regex_policy` in both
`schema_evidence.json` and the runner's safe evidence. `pydantic-default` means
that model declares no regex engine override, not that every model must switch.
An import/configuration failure is a harness error, never a qualified result.

Generation writes untouched `python/models.py`. Structural qualification
checks the actual exported schema before the generated strict model, including
required/extra common and nested product fields, range bounds, decimal max+1,
numeric coercion, leading zeros and trailing newline at every counter location.
Zero, `2^53+1` and `UINT64_MAX` must remain exact strings. The exported schema
**itself**, not just manual profile checks, must reject these negative controls;
a lost numeric upper bound fails qualification. `python/schema_evidence.json`
records schema/model/donor hashes, versions and safe per-case verdicts, never
input JSON. Raw-model acceptance is an observation; the qualified consumer is
schema-first. The runner includes these safe rows in evidence on a schema
regression. No fallback to a manual schema is allowed if the export is missing.

Unicode length cases cover both `object` and `note`: BMP U+00E9 and
supplementary U+1D11E at 1, 65, 128 and 129 scalars, plus escaped surrogate
pairs at the supplementary boundary and boundary+1. In particular, 65 BMP
scalars exceeding 128 UTF-8 bytes must pass, as must 128 supplementary scalars
occupying 256 UTF-16 units / 512 UTF-8 bytes. At 129 scalars both must fail.
For these cases, the actual exported schema, untouched raw Pydantic model and
schema-first consumer must all agree, independently of manual profile checks.
The common wire byte caps, NUL rejection and malformed-surrogate rejection
are unchanged; no Unicode normalization is performed.

Global JSON byte/depth/node/UTF-8 limits and cross-field time policy remain
explicit checks; JSON Schema cannot describe all of them. Pydantic validates
shape, **not authenticity**: the JWT consumer validates authenticated claims only
after successful signature verification. Unsigned preflight parsing is solely
for strict/bounded resource handling; unsigned schema qualification is separate.
The schema/model subset is now tied to a real Forge export, but it still does
not prove the complete J4 Raw/ABI/native/guest boundary or J9 installed-artifact
parity. Those remain separate coordinator/integration gates. Generic exported
URI/purpose rules do not choose the expected profile: `profile_checks.py`
keeps exact issuer/audience/purpose/schema/header/time policy explicit.

## Pins And Official Sources

`requirements.txt` pins all 31 distributions in the generator/validator/JOSE closure and accepted wheel
hashes. Hashes were checked against versioned official PyPI metadata and the
downloaded wheel bytes, without installation/build. For example,
[joserfc 1.6.0 metadata](https://pypi.org/pypi/joserfc/1.6.0/json),
[Pydantic 2.14.1 metadata](https://pypi.org/pypi/pydantic/2.14.1/json).
The binary wheel allowlist covers cp312 manylinux x86_64 and macOS arm64;
cryptography additionally uses compatible cp311-abi3 manylinux/universal2 wheels.

`maven.lock.json` records the exact JAR and published POM URL/SHA-256. The POM
and embedded artifact metadata establish the JAR-only runtime closure for this
ES256 lane. The JAR bytes, not an unpinned Maven resolver or mirror, are used.

Library references:
[joserfc JWS](https://jose.authlib.org/en/guide/jws/),
[Nimbus EC signature example](https://connect2id.com/products/nimbus-jose-jwt/examples/jws-with-ec-signature),
[Pydantic models](https://docs.pydantic.dev/2.11/concepts/models/),
[official Nimbus 10.5 POM](https://repo.maven.apache.org/maven2/com/nimbusds/nimbus-jose-jwt/10.5/nimbus-jose-jwt-10.5.pom).

The generator/validator top-level pins match the existing OpenAPI qualification:
`datamodel-code-generator==0.83.0`, `pydantic==2.14.1`, `jsonschema==4.26.0`.
Their full closure requires `typing_extensions==4.16.0`; the old F0 4.15.0
pin cannot be reused with this generator. Cryptography/CFFI/pycparser pins stay
at the F0 versions. All binary additions have both Linux x86_64 and macOS arm64
wheel hashes; pure Python wheels are platform independent.

Integration still needed: coordinator-owned `schema` command, driver/build target and CI wiring,
CPython 3.12 + JDK 17+ in both matrix jobs, dependency installation, execution
of the public command, retained safe evidence, and exact-head review. This
directory changes no root CMake, CI, libraries, or native implementation.
