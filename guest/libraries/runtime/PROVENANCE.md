# Runtime Provenance

`allocator.cpp` is derived from the active allocator implementation in
AntelopeIO CDT commit `69599db279b7b93d0688502720c15c6962a1401b`, file
`libraries/eosiolib/malloc.cpp`. The allocator algorithms, block metadata,
reuse, coalescing, in-place reallocation and WebAssembly memory growth behavior
are retained. Includes, namespace, check policy, formatting and overflow
handling were adapted to Forge Contract SDK.

The donor is licensed under the EOSIO/Antelope license carried by the donor
repository. Forge's distribution notices must include that license.

## Pinned libc and localization

The C-library subset is compiled directly, without changing donor file bytes,
from `AntelopeIO/cdt-musl` commit `9e6b206efcf203e875f2a659067a295f22cc83a9`
(the submodule pin of CDT `69599db279b7b93d0688502720c15c6962a1401b`). The
list below is exactly its unchanged donor C source inventory, excluding the
already existing math archive and the Forge lock/console adapters. Its
upstream copyright notice is installed as
`share/doc/forge-contract/cdt-musl-COPYRIGHT`; the separate tiny-printf MIT
notice for the rejected tiny-printf implementation is reproduced below. No
musl regex implementation is used.

Locale facets and regular expressions come from unmodified LLVM libc++ / libc++abi
`llvmorg-22.1.8`, commit `ca7933e47d3a3451d81e72ac174dcb5aa28b59d1`, with
localization enabled, wide-character facets and threads disabled, and the
supported musl classic ASCII rune table selected. The existing LLVM license
installation covers these sources.

The private forced-include adapter `details/c_locale.hxx` replaces only
`CURRENT_LOCALE` with the existing musl `C_LOCALE` descriptor and
`CURRENT_UTF8` with zero. This avoids the donor's per-translation-unit
zero-initialized pseudo-thread locale. It does not change parsing, formatting,
regex or collation algorithms. The public sysroot declares matching musl C
locale structures, masks, `tm` and two-word `mbstate_t` layout; LLVM's musl
mbstate forwarding header resolves to that one declaration.

`locale.cpp` is a single-thread, C/POSIX-only selection adapter over the pinned
C descriptor: supported names are C, POSIX and the empty deterministic C name.
Unsupported names return null / EINVAL; no locale files, environment variables,
host locale, filesystem or new host intrinsic is used. Numeric parsing, ctype,
collation, string scanning, formatting and the exit registry remain donor-owned.
The libc subset does not promise filesystem-backed stdio or arbitrary named
locales. Declaration of a C function is not evidence that its file-I/O runtime
is supported; finished-module import validation remains mandatory.

### Exit registry and invocation lifetime

Pinned `src/exit/atexit.c` supplies real LIFO registration, 32 built-in slots,
calloc-backed spill, explicit drain via `__funcs_on_exit`, and its original
empty `__cxa_finalize`. Pinned locking remains single-thread because the
freestanding guest has no threads. `exit` explicitly drains then calls the
host exit intrinsic; `_Exit` and direct `eosio_exit` bypass draining. No
`__wasm_call_dtors` or per-apply shutdown is invented. Normal apply return
does not execute global destructors. Forge VM initialization resets globals
and guest heap before each invocation, then guest startup constructs globals
again. Trap/exit state is discarded under the same reset policy. The fixture
`guest/tests/donor-compatibility` reuses one backend to exercise that boundary.
LLVM owns pure/deleted-virtual handlers; the previous duplicate runtime handler
is removed so localization does not create two strong definitions.

Pinned CDT musl `src/stdio/__lockfile.c` has a commented locking body and a
missing non-void return; importing that undefined behavior is rejected.
`stdio_lock.cpp` instead retains actual single-thread ownership in the pinned
private FILE layout: 0-to-1 first acquisition requires unlock, a nested
acquisition returns false without releasing its owner, and the string scanner's
lock=-1 bypass remains unchanged. The private FILE layout is never installed
as a public host FILE capability. The native `stdio_lock_tests.cpp` checks
acquisition, nesting, bypass and reacquisition; the WASM fixture checks real
string scanning. `__assert_fail` delegates the existing canonical contract
abort check with the expression, instead of pulling stderr/fflush/file-I/O
into the guest runtime. No general stdio or thread support is claimed.

### Compiled Cstdlib source inventory

The following 48 files retain the current CDT musl pin above.

| Pinned donor file | SHA-256 |
| --- | --- |
| `src/ctype/isalnum.c` | `bffd7140ed290ddb10a191c64227138ee5a763170fb4e036b9b2275744f05875` |
| `src/ctype/isalpha.c` | `aa9d81a7829233942c5dbd70981c374b7d9651d6fde829260d8738cdb4874bbc` |
| `src/ctype/isblank.c` | `bbf6fde91cc89854927f2c793afdb6c449a5080cf2e7c912fb2143dbad66ef4c` |
| `src/ctype/iscntrl.c` | `8d529022df72128372af27daa76a86909faddba73dcb9d70d97ec192c3ad953c` |
| `src/ctype/isdigit.c` | `f63f28aa798ae88db58bb27af9e4caf74e0ad7187498a12b1bd48e84f1e17c82` |
| `src/ctype/isgraph.c` | `5cfb7c9481e7308a7c2de42ba4e9b92a6ed1c247f2c7df3b6d3b3181d9ce26c9` |
| `src/ctype/islower.c` | `aa796e9657139e739590f56694633705ae8ab41a05724fc3ccad44afb7b2eccd` |
| `src/ctype/isprint.c` | `4cdafd09e21825ad64eba0272dc0aa3d21cbe202248ae1e0d738c26ce1caea5a` |
| `src/ctype/ispunct.c` | `5b04ee3c5b9bbb5795e83134c01622f784f4407bfcf267fdc823f4f71e8a7767` |
| `src/ctype/isspace.c` | `724a3ac8d6f16e798509d194c1e2c28ffceb684cefac93f937779ff2b347341f` |
| `src/ctype/isupper.c` | `2a987b0c9d8c2dc46cab22209029aee15e25bfe21e2d4e7aeb9d8f8a88098938` |
| `src/ctype/isxdigit.c` | `66807a582b9a5d0cc7decc03c1a8f07759e6a6c7d0a978cd3bfb574f507814dd` |
| `src/ctype/tolower.c` | `d22d29af4ba93461a93c51011d46ea7ad70a3f1e6edce2aada222e8928e5f5b4` |
| `src/ctype/toupper.c` | `ce799632017ac7f6c258ad70125643437654c099dafc457106e55b994649ba6c` |
| `src/exit/atexit.c` | `b86148f2bf2695269e783c5748f9c5ae8cad93b396d112534e4e2622f52061ee` |
| `src/thread/__lock.c` | `08963a18ee777a6abe22655552963bd99f9c1c87e5d40a443f16eca785b5d35f` |
| `src/internal/intscan.c` | `ed96c758c94666f2557059bd8fca26db8340b9e6b417852f1599e7c30f67deca` |
| `src/internal/floatscan.c` | `09f99770670925f3d3f7793cf583dd2117f7dac6261c6bd5817d65ee2a122722` |
| `src/internal/shgetc.c` | `fee4abb10f0ee05560ef7fd26daa281c6aac34c66f4b256a9fc8d4d7a2d5423d` |
| `src/locale/c_locale.c` | `9c01611db90c818a79012dac13d3334c122ad42f8c42064b94064e4f6b3d3276` |
| `src/locale/localeconv.c` | `7dae69907954209d40d09d43267de9960fadc1ccab4bd64a9a3706f09c5992c5` |
| `src/locale/langinfo.c` | `5de0e0583a51cddbf43707dc4aa20c290730209bb730821ad066ea9d7f58ceea` |
| `src/locale/__lctrans.c` | `03a5982fda916d50c8ef1a2bbb13393b50b00eec1a300f3059b5b3633747401f` |
| `src/locale/strcoll.c` | `163aa0dacb8a0a959e600b0d3cd2d780a41ab94dbc5d644d72c5a7ae0ed616fb` |
| `src/locale/strxfrm.c` | `8e8f14f26752276c0864a956b7d0250bde3cf61f50f18f679d4d8c3ed36f3333` |
| `src/stdio/vasprintf.c` | `2fbc06e6cd620a8826c4ade37b3ec5aa79bb5ba15dfab016496283707f49eecc` |
| `src/stdio/__uflow.c` | `08938f226a4dd5246eaa7fd0bd93495763de8fc0d2f7f715a2907e3f4de5fc4f` |
| `src/stdio/__toread.c` | `6fe2e38ce7a99e0ed0f4156d43a91f5d35a5c58b2ffdd377b0e32a1aaadba863` |
| `src/stdio/fwrite.c` | `ce3909181733483fc588fc73d6847a906f9058533a5b1a1ff5028f39eea74d18` |
| `src/stdio/__towrite.c` | `fb6da7183430ad23465b80076ec93cd757d03cfe0bec829f0e395896a02bfd49` |
| `src/stdio/sscanf.c` | `5646b6d93e28e016511027d0cc784717011ee9952c65516773a91272130e3100` |
| `src/stdio/vsscanf.c` | `3d25b43c8a1854dbc3cc1d0b3479c5f4ac4623a644da102fe7843f7707f7763f` |
| `src/stdio/vfscanf.c` | `372b08cdf8d07c121f5937b31ace77e8a1621155ecc1e4d10758527e843216fe` |
| `src/stdio/__string_read.c` | `3139d23272de345048504040d817269795d22f3d77994dc5e108056dded18d8f` |
| `src/multibyte/mbrtowc.c` | `626dee2f0d0ff154a129567714a9df8890330bb3d5535154d29407c02afcc82c` |
| `src/multibyte/mbsinit.c` | `efa33b1b2d0ce4b2eaaa8dfbcde901e745f6e047d744a401dfc4be01be1f09ec` |
| `src/multibyte/internal.c` | `925f6ac1d116631148076f868700689913ab6c2040b21be16d3abefa598514bd` |
| `src/multibyte/wcrtomb.c` | `a8f5bfa624c88f1e3214bcd46d909fb9c5b172ac9b634fb32a101161ab680693` |
| `src/multibyte/wctomb.c` | `046e38f206c7203d0b1295dcdcd3080e3b1c335ff2e1433f0626066a73e8befc` |
| `src/string/wcslen.c` | `4baada75103e9906fa85dd6650c07752c1803b36cbdfc1b5e0678c5e3f3223a6` |
| `src/string/strnlen.c` | `3a72ef5250b314ca0d8ff0fed2f6aa10c8b2b26605806c759faf0d1a9b835a9e` |
| `src/stdlib/abs.c` | `c0aaa1954b223e0b22f9ab677c234a8892bc20bc5a9543909115272ae61fa4cc` |
| `src/stdlib/strtol.c` | `4bbb5a865929abc269b00881f38462acc90e8f8d38ab774fcddfc2b1a5a7134f` |
| `src/stdlib/strtod.c` | `675c5145ce183312174d7bd599c4fc5739f32d59cb7eb1553ea4a4d04602c882` |
| `src/time/strftime.c` | `6c98bfbccc6180f998a526e5e23afd18f7fc1a3f5b615f12a43f64924becc9ab` |
| `src/time/__tm_to_secs.c` | `dbba28fd7ccb3a774b50732b77d675a3b5b6e4a48a7c713bd31d625018be88c1` |
| `src/time/__year_to_secs.c` | `7091835a29665512d66e8a9374eed322708ddf06b0a19b0a09fa6512d748decf` |
| `src/time/__month_to_secs.c` | `ec626ffe4ac4521d7376b148b74cf6eecfe17142e4a1daefd18f9f68c4b01eae` |

### Canonical formatting source restoration

Current CDT `src/stdio/printf.c` is a tiny printf replacement; its missing
g/e/a/L formatting silently emits conversion letters and cannot implement
libc++ numeric facets. Its `vfprintf.c` is disabled with `#if 0`. Both are
rejected as the formatting owner. Seven files are copied byte-for-byte from
existing musl ancestor `628cf979b249fa76a80962e2eefe05073216a4db` (VERSION
1.1.18), before EOS-specific file removal, into `donors/musl-628cf979/`.
The three canonical algorithm/string-sink files are vfprintf/vsnprintf/snprintf;
four original thin wrappers retain printf/vprintf/sprintf/vsprintf entrypoints.
No format-parser or float-conversion algorithm is written in Forge. Their
COPYRIGHT is identical to the installed current donor COPYRIGHT (SHA-256
`70ca142d257e2690a1f8eda8a296e64a6d1b16d8aee6784f8ddcf67f3163635d`).

| Ancestor donor file | SHA-256 |
| --- | --- |
| `src/stdio/vfprintf.c` | `32346d1505684238af3e395ee6892fd011d259e67c39727c8e36acb089fe0654` |
| `src/stdio/vsnprintf.c` | `cd8eabbda4c22837169960b4868e3fc14c4fd7670f00f85a372cfe2c365015a3` |
| `src/stdio/snprintf.c` | `be661cb258170863a1372b8e5998ef345bb8ef8e3f06d38a603dfda77eed4d4d` |
| `src/stdio/vsprintf.c` | `7290ad13793a6668b68d075e2aa57678f08639aa4ce7345eeff16d9f9cb4bf7c` |
| `src/stdio/sprintf.c` | `f758fc864ff028c4d2e3430a1ff3f7d40c6c5f04a6e97e80f54784957bcb6d81` |
| `src/stdio/printf.c` | `593392914543ab5bfccb967db68ca3529674e316870ecc5b6fc427ebf3edf777` |
| `src/stdio/vprintf.c` | `543306a39ffc2e626edaddac93e5c1ddff821b0172f6ddadb134a569b81ed440` |

`stdio_console.cpp` supplies only private stdout/stderr string sinks forwarded
to the existing `prints_l` intrinsic. Canonical vfprintf restores and flushes
its temporary buffer before return; console state is invocation-local and reset
with guest memory. There is no descriptor, fopen, filesystem, host FILE or thread
service. `strtod.c`'s incorrect two-argument _l aliases are mechanically renamed
to private symbols during compilation; `locale.cpp` supplies typed three-argument
POSIX wrappers delegating the unchanged strtof/strtod/strtold algorithms in C
locale. This avoids wasm signature-mismatch thunks without changing scalar
numeric conversion algorithms.

### Stack resource compatibility

The standard SDK linker reserve changes explicitly from 8192 to 32768 bytes.
On the pinned LLVM22.1.8 Release build, `printf_core` reserves 8160 bytes after
inlining musl `fmt_fp`; `vfprintf` adds 288 bytes, before any caller frames.
The real default-profile fixture trapped with WASM memory out-of-bounds in
`cstdlib`; those two frames alone exceed 8192. The 32-KiB reserve is a bounded
resource setting, not a new runtime/profile mode or an algorithm, contract ABI,
long-double ABI or donor-source adaptation. Numeric, regex, exit/reset and
console fixtures must execute under that same default profile, and artifacts'
actual memory limits remain part of product-chain compatibility validation.
The compiled default-profile `donorcompat` and multi-source `dispatchshdw`
fixtures both declare initial=2 and maximum=256 64-KiB pages; their mutable
stack-pointer initializer is 32768. The executable backend uses the existing
528-page compatibility ceiling and asserts the fixture's memory limits. Initial
memory of the production donor artifacts is measured separately; the reserve
change must not be described as memory-footprint neutral without that evidence.
All sixteen donor acceptance artifacts were measured with initial=2 and
maximum=256 pages and stack initializer=32768; the eight preserved baseline
artifacts declare the same initial/max limits with stack initializer=8192.
Their ABI hashes remain unchanged, while their WASM hashes change as expected.
No unlimited-recursion or filesystem support is inferred.

### Tiny printf notice (as carried by pinned donor source)

```text
///////////////////////////////////////////////////////////////////////////////
// \author (c) Marco Paland (info@paland.com)
//             2014-2018, PALANDesign Hannover, Germany
//
// \license The MIT License (MIT)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
```
