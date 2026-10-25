#!/usr/bin/env python3
"""Independent fixed-input oracle; never invoked by the ordinary CTest suite."""

import hashlib
import json

import Crypto
from Crypto.Hash import keccak


def keccak256(data):
    digest = keccak.new(digest_bits=256)
    digest.update(data)
    return digest.digest()


assert Crypto.__version__ == "3.23.0"
assert keccak256(b"").hex() == "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470"
key = bytes(range(32))
opening = bytes(index % 251 for index in range(2048)) + bytes(range(0xA0, 0xB0))
padded = opening + bytes(16)
words = []
for counter in (0, 64, 65, 1039):
    preimage = counter.to_bytes(32, "big") + key
    words.append({
        "counter": counter,
        "preimage_hex": preimage.hex(),
        "keccak256_hex": keccak256(preimage).hex(),
        "nist_sha3_256_hex": hashlib.sha3_256(preimage).hexdigest(),
    })
blocks = []
for index in (0, 1, 15):
    pad = b"".join(keccak256((65 * index + word).to_bytes(32, "big") + key)
                   for word in range(65))
    ciphertext = bytes(left ^ right for left, right in zip(padded, pad))
    blocks.append({
        "index": index,
        "ciphertext_bytes": len(ciphertext),
        "ciphertext_sha256": hashlib.sha256(ciphertext).hexdigest(),
        "first_word_hex": ciphertext[:32].hex(),
        "last_word_hex": ciphertext[-32:].hex(),
    })
print(json.dumps({
    "schema": "forge.tests.crypto.exchange.fixed.v1",
    "oracle": "PyCryptodome 3.23.0 Crypto.Hash.keccak / hashlib.sha256",
    "key_hex": key.hex(),
    "opening_bytes": len(opening),
    "opening_sha256": hashlib.sha256(opening).hexdigest(),
    "plaintext": "byte[i] = i % 251 for 0 <= i < 2048",
    "blinder_hex": bytes(range(0xA0, 0xB0)).hex(),
    "word_preimage": "BE256(65 * block_index + word_index) || key32",
    "words": words,
    "blocks": blocks,
}, indent=2))
