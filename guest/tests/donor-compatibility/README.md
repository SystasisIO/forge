# Donor compatibility executable checks

Build the installed SDK consumer targets `donorcompat_artifacts` and
`dispatchshdw_artifacts` first. Configure this directory with the normal
native Forge package and `FORGE_CONTRACT_TEST_DONOR_COMPAT_WASM` pointing to
the finished `donorcompat.wasm`, then build and run CTest. The interpreter
uses the canonical 528-page compatibility policy. The JIT case is compiled
only when the native package supports it; arm64 interpreter success is not
JIT evidence.

The same backend is initialized repeatedly across normal return, trap,
direct `eosio_exit`, explicit C `exit`, global `std::set` construction, 40
LIFO registrations and a 64-invocation unfreed heap allocation probe. Normal
apply return does not drain global destructors. Explicit exit drains them;
initialization discards previous invocation memory/state.

Numeric checks execute the actual LLVM C-locale facets and pinned musl
formatter under the SDK's default 32-KiB stack reserve, including double
extremes and long double beyond double range. They cover bounded formatting,
console flushing beyond 80 bytes, errno, ASCII classification, scanning,
unsupported named-locale failure and `%Z` assertion failure/reuse. Regex
checks use the original donor IPFS expression, lengths, zero/punctuation
rejection and repeated invocation. No full filesystem stdio or arbitrary
locale support is inferred.

Raw checks compare ignored action/variant authority arguments, extended
symbol layout and asset text at precision 0/4/18/255 with the native protocol
formatter. `guest/tests/donor-protocol` separately checks the amended public
native formatter against explicit text/wire witnesses and can run against
the actual canonical build-tree target or its installed package. The ABI
tooling fixtures check the explicit `extended_symbol` struct; a product's
canonical host ABI parse/JSON encode/decode checks remain separate.
