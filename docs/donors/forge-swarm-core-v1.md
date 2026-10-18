# Swarm Core Donor Mapping

PR1 adds immutable-file descriptors, content identity and Merkle mechanisms to
`forge_swarm_core`. The implementation plan is
[Forge Swarm v1](../iterations/forge-content-swarm-v1.md); the public contract is
documented in the [Core guide](../../libraries/swarm/core/README.md).

## Reviewed Sources

- [BEP 52](https://www.bittorrent.org/beps/bep_0052.html), revision
  `7392d83f374b071ca9601728c27be2d05e199e0c`: file alignment, 16 KiB base
  blocks, SHA-256 roots and piece layers.
- libtorrent commit `b4e9e2471e7abce2a58585a7d7fbc4e8d1edc47e`:
  `include/libtorrent/file_storage.hpp`, `include/libtorrent/piece_picker.hpp`,
  `include/libtorrent/aux_/merkle.hpp`, `src/merkle.cpp`,
  `src/merkle_tree.cpp`, `test/test_merkle.cpp`,
  `test/test_merkle_tree.cpp` and `test/test_file_storage.cpp`.
- Existing Forge Crypto SHA-256, Raw bounded decoding and enum wire metadata,
  Core UTF-8 helpers, CMake component exports and installed package consumers.

## Accepted Mechanisms

- Files have independent roots and do not share a piece.
- Leaves hash actual block bytes, including a short last block. Missing leaves
  use zero digests, with empty subtree hashes derived at the relevant height.
- A file no larger than one piece retains its natural tree height. A short
  final piece in a larger file is padded to the piece height.
- Piece proof orientation follows the file-local piece index. Bounds and
  expected lengths are checked before hashing or indexing.
- File paths have deterministic component-wise byte ordering; identity includes
  their exact canonical bytes and file sizes as well as hashes.

## Forge Decisions

- Identity is SHA-256 over canonical Forge Raw descriptor bytes. It is not a
  torrent infohash, CID or claim of BitTorrent/IPFS wire compatibility.
- An empty file has a zero-root sentinel and no pieces. BEP 52 omits its root;
  the required Forge field has one explicit representation.
- Version, geometry, metadata and path limits are part of the documented v1
  contract. Strict decode rejects trailing data and nonminimal counts and
  preflights allocation against the actual input and domain limits.
- Descriptor validation preserves UTF-8 bytes. Filesystem-specific collision
  and representability checks belong to the later file-store adapter.
- No donor networking, session, disk-thread, bencoding or tracker code enters
  the library. No Chain Merkle helper is reused with different padding rules.
- `forge.core.conversion` is only a primary-template declaration in existing
  Core; domain conversion specializations follow in their owning leaves.

## Evidence And Reproduction

`tests/swarm_core` contains frozen Raw/id fixtures and independently calculated
SHA-256/Merkle vectors, boundary geometries and malformed inputs. The native
`file_hasher` accepts bounded consecutive blocks rather than copying a whole
file into another buffer.

The optional `test_forge_swarm_libtorrent` target compiles unchanged sources
from the pinned local checkout into a test executable only. It compares file
roots, piece layers and proof siblings over 55 file/piece geometries. Configure
with `FORGE_SWARM_LIBTORRENT_DONOR_ROOT` pointing to that checkout; a different
commit is rejected. The product target and installed package acquire no
libtorrent dependency. This proves the tested algorithms, not donor network
interoperability or later Swarm runtime behavior.

```sh
cmake --build build/pr1 -j 4 --target \
  test_forge_swarm_core test_forge_swarm_libtorrent test_forge_structure
ctest --test-dir build/pr1 --output-on-failure \
  -R '^test_forge_(swarm_core|swarm_libtorrent|swarm_structure|structure)$'
cmake --build build/pr1 -j 4 --target \
  test_forge_package_core_lightweight test_forge_package_swarm_core_component
git diff --check
```

Installed Core-only acceptance uses a separate consumer module defining its own
explicit specialization, with a second translation unit using it. Swarm Core
acceptance imports only its leaf modules and links its own package component.
Full CI, network transport acceptance, storage crash tests and production proof
belong to other work; they are not inferred from PR1's algorithm tests.
