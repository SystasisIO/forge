"""Unsigned synthetic vectors only. No Forge code, signing, or acceptance oracle."""

import base64
import copy
import json

NOW = 1800000100
MAX_LIFETIME = 300
SKEW = 0
KID = "interop-key"
TYP = "forge-attestation+jwt"
ISSUER = "https://issuer.example.test"
AUDIENCE = "https://resource.example.test"
PURPOSE = "artifact.read"
SCHEMA = "https://schemas.example.test/grant/v1"
MAX_DATE = 253402300799
UINT64_MAX = "18446744073709551615"
HEADER = {"alg": "ES256", "typ": TYP, "kid": KID}
EXPECTED = {
    "iss": ISSUER, "aud": AUDIENCE, "jti": "g_" + "0123456789abcdef" * 4,
    "iat": 1800000000, "nbf": 1800000000, "exp": 1800000300,
    "att": {"v": 1, "purpose": PURPOSE, "schema": SCHEMA, "claims": {
        "object": "o_17", "generation": UINT64_MAX,
        "ranges": [{"offset": "9007199254740993", "length": "1"}],
        "note": "\u043c\u0435\u0434\u0438\u0430",
    }},
}


def encoded(value):
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")


def json_bytes(value, **options):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), **options).encode("utf-8")


def vectors():
    cases = []
    header, payload = json_bytes(HEADER), json_bytes(EXPECTED)

    def add(name, accept=False, h=header, p=payload, mode="signed"):
        cases.append({"id": name, "accept": accept, "header": encoded(h),
                      "payload": encoded(p), "mode": mode})

    def change(name, path, value=None, remove=False, accept=False, escaped=False):
        document = copy.deepcopy(EXPECTED)
        target = document
        for part in path[:-1]:
            target = target[part]
        if remove:
            del target[path[-1]]
        else:
            target[path[-1]] = value
        raw = (json.dumps(document, ensure_ascii=True, separators=(",", ":")).encode("ascii")
               if escaped else json_bytes(document))
        add(name, accept, p=raw)

    add("valid", True)
    # Legal order, whitespace and escapes must not be mistaken for canonical JSON.
    add("valid-reordered-escaped", True,
        h=json.dumps(dict(reversed(list(HEADER.items()))), indent=1).encode(),
        p=json.dumps(dict(reversed(list(EXPECTED.items()))), ensure_ascii=True, indent=1).encode())
    add("valid-header-512", True, h=header + b" " * (512 - len(header)))
    add("valid-payload-8192", True, p=payload + b" " * (8192 - len(payload)))
    change("valid-note-128-bytes", ["att", "claims", "note"], "a" * 128, accept=True)
    change("valid-note-128-utf8-bytes", ["att", "claims", "note"], "\u00e9" * 64, accept=True)
    # Product maxLength counts scalars, neither UTF-8 bytes nor UTF-16 units.
    for field in ("object", "note"):
        for label, scalar in (("bmp", "\u00e9"), ("supplementary", "\U0001d11e")):
            for count in (1, 65, 128, 129):
                name = f"product-unicode-{field}-{label}-{count}"
                change(name, ["att", "claims", field], scalar * count, accept=count <= 128)
                if label == "supplementary" and count in (128, 129):
                    change(name + "-escaped", ["att", "claims", field], scalar * count,
                           accept=count <= 128, escaped=True)
    change("valid-ranges-8", ["att", "claims", "ranges"],
           [{"offset": str(i), "length": "1"} for i in range(8)], accept=True)
    change("valid-counter-zero", ["att", "claims", "generation"], "0", accept=True)
    change("valid-counter-2pow53plus1", ["att", "claims", "generation"], "9007199254740993", accept=True)
    change("valid-offset-max", ["att", "claims", "ranges", 0, "offset"], UINT64_MAX, accept=True)
    change("valid-length-max", ["att", "claims", "ranges", 0, "length"], UINT64_MAX, accept=True)
    change("valid-length-zero", ["att", "claims", "ranges", 0, "length"], "0", accept=True)
    change("valid-exp-now-plus-one", ["exp"], NOW + 1, accept=True)
    for field in HEADER:
        h = dict(HEADER)
        del h[field]
        add("missing-header-" + field, h=json_bytes(h))
    for field, values in {
        "alg": ["none", "HS256", "ES384", None, True],
        "typ": ["JWT", "at+jwt", "forge-attestation+jwt ", None, 1],
        "kid": ["unknown-key", "", "x" * 129, "bad/kid", "\u043a", None, 1],
    }.items():
        for index, value in enumerate(values):
            add(f"wrong-header-{field}-{index}", h=json_bytes({**HEADER, field: value}))
    for field, value in {"jwk": {}, "jku": ISSUER, "x5u": ISSUER, "x5c": [],
                         "crit": [], "cty": "JWT", "b64": False, "unknown": 1}.items():
        add("forbidden-header-" + field, h=json_bytes({**HEADER, field: value}))
    for field in EXPECTED:
        change("missing-" + field, [field], remove=True)
    for field in ("v", "purpose", "schema", "claims"):
        change("missing-att-" + field, ["att", field], remove=True)
    for field in ("sub", "client_id", "scope", "sid", "cnf", "unknown"):
        change("forbidden-claim-" + field, [field], "not-allowed")
    for field, value in {"iss": ISSUER + "/", "aud": [AUDIENCE], "jti": "g_" + "A" * 64,
                         "att": None}.items():
        change("wrong-" + field, [field], value)
    change("wrong-aud-string", ["aud"], AUDIENCE + "/")
    change("wrong-purpose", ["att", "purpose"], "artifact.write")
    change("wrong-schema", ["att", "schema"], SCHEMA + "/")
    change("wrong-version", ["att", "v"], 2)
    change("bool-version", ["att", "v"], True)
    change("unknown-att-field", ["att", "extra"], 1)
    change("issuer-257", ["iss"], "https://" + "a" * 248 + "/")
    change("audience-257", ["aud"], "https://" + "a" * 248 + "/")
    change("purpose-65", ["att", "purpose"], "a" * 65)
    change("schema-257", ["att", "schema"], "https://" + "a" * 248 + "/")
    change("jti-short", ["jti"], "g_" + "a" * 63)
    change("jti-long", ["jti"], "g_" + "a" * 65)
    change("claims-string", ["att", "claims"], "opaque-raw-is-not-json")
    for field in ("iat", "nbf", "exp"):
        for index, value in enumerate([None, True, "1800000000", 1800000000.0, -1, MAX_DATE + 1]):
            change(f"date-{field}-{index}", [field], value)
    change("date-exponent", ["iat"], 1800000000.0)
    add("date-exponent-lexical", p=payload.replace(b'"iat":1800000000', b'"iat":18e8'))
    change("expired-at-now", ["exp"], NOW)
    change("expired-before-now", ["exp"], NOW - 1)
    change("future-iat", ["iat"], NOW + 1)
    change("future-nbf", ["nbf"], NOW + 1)
    change("iat-after-nbf", ["iat"], 1800000001)
    change("nbf-equals-exp", ["nbf"], 1800000300)
    change("lifetime-301", ["exp"], 1800000301)
    for field in ("object", "generation", "ranges", "note"):
        change("missing-product-" + field, ["att", "claims", field], remove=True)
    change("unknown-product-field", ["att", "claims", "extra"], 1)
    change("object-null", ["att", "claims", "object"], None)
    change("object-empty", ["att", "claims", "object"], "")
    change("note-number", ["att", "claims", "note"], 1)
    for field, path in {"generation": ["att", "claims", "generation"],
                        "offset": ["att", "claims", "ranges", 0, "offset"],
                        "length": ["att", "claims", "ranges", 0, "length"]}.items():
        for index, value in enumerate(["18446744073709551616", "01", "-1", "+1", "1e2", "1\n", 1, True, None]):
            change(f"decimal-{field}-{index}", path, value)
    change("unknown-range-field", ["att", "claims", "ranges", 0, "extra"], 1)
    change("missing-range-offset", ["att", "claims", "ranges", 0, "offset"], remove=True)
    change("missing-range-length", ["att", "claims", "ranges", 0, "length"], remove=True)
    change("ranges-object", ["att", "claims", "ranges"], {})
    change("range-null", ["att", "claims", "ranges", 0], None)
    change("ranges-empty", ["att", "claims", "ranges"], [])
    change("ranges-9", ["att", "claims", "ranges"], [{"offset": "0", "length": "1"}] * 9)
    change("product-object-129", ["att", "claims", "object"], "a" * 129)
    change("product-note-129", ["att", "claims", "note"], "a" * 129)
    change("product-note-empty", ["att", "claims", "note"], "")
    change("ranges-129", ["att", "claims", "ranges"], [{"offset": "0", "length": "1"}] * 129)
    change("string-2049", ["att", "claims", "note"], "a" * 2049)
    change("string-utf8-2050", ["att", "claims", "note"], "\u00e9" * 1025)
    change("escaped-nul", ["att", "claims", "note"], "\0")
    add("header-513", h=header + b" " * (513 - len(header)))
    add("payload-8193", p=payload + b" " * (8193 - len(payload)))
    add("compact-over-16384", p=payload + b" " * 13000)
    add("duplicate-header", h=header[:-1] + b',"kid":"interop-key"}')
    add("escaped-duplicate-header", h=header[:-1] + b',"\\u006bid":"interop-key"}')
    add("duplicate-payload", p=payload[:-1] + b',"iss":"https://issuer.example.test"}')
    add("escaped-duplicate-nested", p=payload.replace(b'"offset":', b'"\\u006fffset":"1","offset":'))
    add("invalid-utf8", p=payload.replace(b'"o_17"', b'"\xff"'))
    add("literal-nul", p=payload.replace(b'"o_17"', b'"\x00"'))
    add("lone-surrogate", p=payload.replace(b'"o_17"', b'"\\ud800"'))
    add("trailing-json", p=payload + b"{}")
    add("payload-array", p=b"[]")
    add("nan", p=payload.replace(b'"iat":1800000000', b'"iat":NaN'))
    add("infinity", p=payload.replace(b'"iat":1800000000', b'"iat":Infinity'))
    add("depth-9", p=b'{"x":' + b"[" * 8 + b"0" + b"]" * 8 + b"}")
    add("nodes-513", p=json_bytes({"x": [[0] * 126] + [[0] * 127 for _ in range(3)]}))
    add("object-members-129", p=json_bytes({str(i): 0 for i in range(129)}))
    add("field-name-2049", p=json_bytes({"x" * 2049: 0}))
    for mode in ("tampered", "wrong-key", "padding", "noncanonical", "extra-segment", "empty-payload", "short-signature"):
        add(mode, mode=mode)
    return cases
