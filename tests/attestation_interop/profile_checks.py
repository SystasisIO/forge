"""Explicit test-only F1 profile assertions, independent of exported schemas."""

import re

from corpus import AUDIENCE, HEADER, ISSUER, MAX_DATE, MAX_LIFETIME, NOW, PURPOSE, SCHEMA, SKEW, UINT64_MAX


def require(condition):
    if not condition:
        raise ValueError("profile rejection")


def text(value, minimum=0, maximum=2048):
    require(type(value) is str)
    require("\0" not in value and minimum <= len(value.encode("utf-8", errors="strict")) <= maximum)
    return value


def product_text(value, minimum, maximum):
    # Keep the wire byte cap and Unicode validation separate from schema length.
    text(value)
    require(minimum <= len(value) <= maximum)
    return value


def bounded_tree(value):
    nodes = 0

    def visit(item, depth):
        nonlocal nodes
        nodes += 1
        require(nodes <= 512)
        if isinstance(item, (dict, list)):
            require(depth <= 8 and len(item) <= 128)
            children = item.values() if isinstance(item, dict) else item
            if isinstance(item, dict):
                for key in item:
                    text(key)
            for child in children:
                visit(child, depth + 1 if isinstance(child, (dict, list)) else depth)
        elif isinstance(item, str):
            text(item)

    visit(value, 1)
    return value


def fields(value, names):
    require(type(value) is dict and set(value) == set(names.split()))


def check_header(value):
    fields(value, "alg typ kid")
    require(value == HEADER and all(type(item) is str for item in value.values()))


def counter(value):
    require(re.fullmatch(r"0|[1-9][0-9]*", text(value, 1, 20)) is not None)
    require(int(value) <= int(UINT64_MAX))


def check_document(value):
    fields(value, "iss aud jti iat nbf exp att")
    require(text(value["iss"], 1, 256) == ISSUER and text(value["aud"], 1, 256) == AUDIENCE)
    require(re.fullmatch(r"g_[0-9a-f]{64}", text(value["jti"], 66, 66)) is not None)
    for name in ("iat", "nbf", "exp"):
        require(type(value[name]) is int and 0 <= value[name] <= MAX_DATE)
    iat, nbf, exp = (value[name] for name in ("iat", "nbf", "exp"))
    require(iat <= nbf < exp and exp - iat <= MAX_LIFETIME)
    require(iat <= NOW + SKEW and nbf <= NOW + SKEW and NOW < exp)
    att = value["att"]
    fields(att, "v purpose schema claims")
    require(type(att["v"]) is int and att["v"] == 1)
    require(text(att["purpose"], 1, 64) == PURPOSE and text(att["schema"], 1, 256) == SCHEMA)
    claims = att["claims"]
    fields(claims, "object generation ranges note")
    # Product fixture bounds come from the coordinator's native product_schema.
    product_text(claims["object"], 1, 128)
    product_text(claims["note"], 1, 128)
    counter(claims["generation"])
    require(type(claims["ranges"]) is list and 1 <= len(claims["ranges"]) <= 8)
    for item in claims["ranges"]:
        fields(item, "offset length")
        counter(item["offset"])
        counter(item["length"])
