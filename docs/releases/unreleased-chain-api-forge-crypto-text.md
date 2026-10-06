# UNRELEASED — Chain API asymmetric crypto text

This note records the maintainer-approved, narrowly scoped pre-stabilization
`MINOR` text-contract break. It does not assign a release version, create a tag
or release, or modify already published release notes.

## Affected public surfaces

The local `forge.chain.api.abi` operations `abi_json_to_bin` and
`abi_bin_to_json` now use the existing canonical Forge asymmetric text profile
for ABI `public_key` and `signature` scalars, including nested fields, aliases,
containers and action/table values. `action_to_variant` and
`transaction_to_variant` use the same decoder when an ABI is available; their
decoded crypto fields therefore use Forge text too. Consumers of local ABI
packing, action JSON construction and table-row JSON decoding must update
together. No alternate ABI profile, fallback or output mode is supplied.

| Existing typed family | Canonical public key | Canonical signature |
| --- | --- | --- |
| Secp256k1 | `PUB_SECP256K1_` | `SIG_SECP256K1_` |
| P256 | `PUB_P256_` | `SIG_P256_` |
| WebAuthn | `PUB_WEBAUTHN_` | `SIG_WEBAUTHN_` |
| Ed25519 | `PUB_ED25519_` | `SIG_ED25519_` |
| RSA | `PUB_RSA_` | `SIG_RSA_` |

The former Antelope ABI inputs `EOS`, `PUB_K1_`, `PUB_R1_`, `PUB_WA_`,
`SIG_K1_`, `SIG_R1_` and `SIG_WA_` are rejected as `invalid_json` with the
existing ABI category, logical path and binary offset. The explicit
`encoding::antelope()` API remains available; donor fixtures and binary
compatibility oracles are not removed. Ed25519 and RSA were already members of
the protocol variants and the Forge text profile; using that profile at the
ABI boundary introduces no new algorithm or binary variant.

## Mechanical migration

For old Antelope public-key/signature fields in client-owned JSON, configuration,
stored presentation data or fixtures, use an explicit one-time conversion:

```cpp
import forge.crypto.asymmetric;

namespace asymmetric = forge::crypto::asymmetric;

const auto key = asymmetric::encoding::antelope().parse_public(old_public_text);
const auto new_public_text = asymmetric::encoding::forge().format(key);

const auto signature = asymmetric::encoding::antelope().parse_signature(old_signature_text);
const auto new_signature_text = asymmetric::encoding::forge().format(signature);
```

Use the typed values to regenerate each affected public field and expected JSON
result, then rebuild and test the consumer against the changed boundary. Do not
replace text prefixes manually: the existing codecs also own profile-specific
checksums. The changed ABI boundary does not silently convert old input.

Regenerate manifests, receipts or other artifacts that commit exact JSON text
through their owning generators after changing public fields. Do not reuse an
old exact-text digest or edit a derived receipt marker by hand. Upgrade all
writers and readers expecting the former textual shapes together; retaining
old JSON expectations is not a supported compatibility mode.

## Unchanged contracts and gates

Raw packing/unpacking, protocol variant indices and field order, ABI schema,
limits and error categories, transaction/block IDs, signing digests, signature
bytes and persisted binary records are unchanged. No binary-data reset or
cryptographic re-signing is required solely to re-encode presentation text.
TLS PKI, PEM/DER and libp2p peer-ID/address formats are unrelated protocols and
are not changed by this migration.

The existing Chain API test target checks all five typed crypto families,
fixed pre-change K1 public-key/signature bytes, Raw identity, nested records and
typed rejection diagnostics. Product consumers must separately run their
action/bootstrap and table/projection acceptance gates; library tests alone
do not establish downstream readiness.
