"""Fail closed unless the isolated worker owns its allocation and C++ runtime closure."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


ALLOCATIONS = {
    "_malloc", "_calloc", "_realloc", "_free", "_aligned_alloc", "_posix_memalign",
    "_valloc", "_malloc_size", "_mmap", "_munmap",
}
FORBIDDEN_SYSTEM_CALLS = {
    "malloc", "calloc", "realloc", "free", "aligned_alloc", "posix_memalign", "valloc", "malloc_size",
    "mmap", "munmap", "mremap", "mprotect", "vm_allocate", "vm_map", "mach_vm_allocate", "mach_vm_map",
    "sbrk", "brk", "reallocf", "reallocarray", "strdup", "strndup", "asprintf", "vasprintf",
    "getline", "getdelim", "syscall", "tlv_bootstrap", "cxa_thread_atexit", "cxa_thread_atexit_impl",
    "cxa_atexit", "pthread_create",
}
# This exact system boundary is reviewed with the pinned scalar-only CEL profile.
# Additions require source inspection; allocator/VM/TLS/thread escapes above are
# rejected independently, even if somebody accidentally adds one to this list.
SYSTEM_IMPORTS = set("""
__DefaultRuneLocale __dyld_find_unwind_sections __dyld_register_func_for_remove_image
___assert_rtn ___error ___stack_chk_fail ___stack_chk_guard ___stderrp ___stdoutp __exit
_abort _arc4random_buf _bzero _clock_gettime _close _dladdr _dlopen _dlsym _fcntl
_fflush _fprintf _fputc _fputs _fwrite _getenv _getpid _gettimeofday _isatty
_mach_absolute_time _mach_continuous_time _mach_timebase_info _memcmp _memchr
_memcpy _memmove _memset _nanosleep _poll _pthread_cond_broadcast _pthread_cond_signal
_pthread_cond_wait _pthread_getspecific _pthread_key_create _pthread_key_delete
_pthread_mach_thread_np _pthread_mutex_destroy _pthread_mutex_init _pthread_mutex_lock
_pthread_mutex_trylock _pthread_mutex_unlock _pthread_once _pthread_rwlock_rdlock
_pthread_rwlock_unlock _pthread_rwlock_wrlock _pthread_self _pthread_setspecific
_pthread_threadid_np _read _recv _sched_yield _send _sigaction _sigaddset _sigemptyset
_sigfillset _signal _setrlimit _snprintf _strcmp _strerror _strlen _strncmp _strnlen _sysconf
_vsnprintf _vfprintf _write
""".split())
SYSTEM_LIBRARIES = {
    "/usr/lib/libSystem.B.dylib",
    "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation",
}
DIAGNOSTICS_INITIALIZER = "forge_expression_initialize_diagnostics"
# Darwin Abseil keeps the symbolizer in the linked stack-diagnostic objects.
# It is unreachable after the image's first initializer disables both fatal-log
# symbolization and mutex deadlock reports. Ordinary backtrace() is OS unwind
# bookkeeping, explicitly outside the private allocation-pool contract.
DIAGNOSTICS_IMPORTS = {"_backtrace", "_backtrace_symbols"}
QUALIFIED_PROFILE = {"cel_commit": "9c562a306569e90c8dea6d3af71f4433aa386162",
                     "llvm_version": "22.1.8", "profile_version": 1}
# These groups qualify only this pinned scalar profile and its constructor-first
# diagnostics policy. They are not a general claim that the system functions
# cannot allocate. In particular, named locale/timezone/file operations remain
# linked through upstream virtual tables but are not exposed by this worker.
PROFILE_IMPORTS = {
    # Compiler integer helpers, pure libm operations and caller-owned buffers.
    "arithmetic_and_buffers": set("""
        ___divti3 ___udivti3 ___umodti3 _frexp _ldexp _modf _nan
        _memset_pattern16 _strchr _strncpy _wcslen _wmemchr
        _strtol _strtoll _strtoul _strtoull _strerror_r _snprintf_l
        ____mb_cur_max_l ___maskrune ___tolower ___toupper
    """.split()),
    # Darwin mutex/condvar state is in caller storage; TSD keys use fixed native
    # key tables. No new thread can enter this single-thread helper.
    "fixed_native_thread_state": set("""
        _pthread_cond_init _pthread_cond_timedwait _pthread_cond_timedwait_relative_np
        _pthread_getschedparam _pthread_sigmask
    """.split()),
    # cctz's UTC path does not load a zone. The adapter never initializes Abseil
    # logging or calls LocalTimeZone, and admission excludes time overloads before
    # planning. TZ=UTC alone would NOT suffice: local_time_zone calls CF first.
    "unreachable_local_timezone": set("""
        _CFRelease _CFStringGetCString _CFStringGetLength _CFStringGetMaximumSizeForEncoding
        _CFTimeZoneCopyDefault _CFTimeZoneGetName _gmtime_r _localtime_r _mktime _strptime
    """.split()),
    # The helper uses Raw and unistd for IPC. FILE input/file opening and named
    # message catalogs survive in libc++ facet/vtable bodies, not worker paths.
    # Darwin stderr is a fixed unbuffered FILE; fatal diagnostics use only it.
    "unreachable_file_and_catalog_facets": set("""
        ___stdinp _catclose _catgets _catopen _fclose _fopen _fputwc _fread _fseek
        _fseeko _ftello _getc _getwc _setbuf _ungetc _ungetwc
    """.split()),
    # Darwin classic locale uses LC_C_LOCALE (null), not a created locale.
    # Float parsing/formatting, time/wide conversion and named locale facets are
    # not reachable through typed scalar inputs or integer/string diagnostics.
    # Pratt's rejected float literals use Abseil from_chars, not system strtod.
    "unreachable_locale_and_float_facets": set("""
        _asprintf_l _freelocale _mbrlen_l _mbrtowc_l _mbsnrtowcs_l _mbsrtowcs_l
        _mbtowc_l _sscanf _strftime _strftime_l _strtod_l _strtof _strtof_l _strtold_l
        _wcrtomb_l _wcsnrtombs_l
    """.split()),
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def output(command):
    return subprocess.check_output([str(part) for part in command], text=True)


def allocation_symbol(symbol):
    plain = symbol.lstrip("_").split("$", 1)[0]
    return (plain in FORBIDDEN_SYSTEM_CALLS or plain.startswith(("malloc_zone_", "malloc_type_"))
            or bool(re.match(r"^__Z(?:n[aw]|d[al])", symbol)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--backend-manifest", type=Path, required=True)
    parser.add_argument("--runtime-manifest", type=Path, required=True)
    parser.add_argument("--link-map", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    root = args.source_root.resolve(strict=True)
    llvm = args.compiler.resolve(strict=True).parent.parent
    nm = llvm / "bin/llvm-nm"
    pins = json.loads((root / "cmake/cel/sources.json").read_text())
    if (pins["cel"]["commit"] != QUALIFIED_PROFILE["cel_commit"]
            or pins["llvm"]["version"] != QUALIFIED_PROFILE["llvm_version"]):
        raise RuntimeError("Worker system-import policy requires review for changed CEL/LLVM pins")
    if output(["/usr/bin/lipo", "-archs", binary]).strip() != "arm64":
        raise RuntimeError("Expression worker must contain exactly the qualified ARM64 architecture")
    initializers = re.findall(r"^\s+(0x[0-9a-fA-F]+)\s+(\S+)\s*$",
                              output(["/usr/bin/xcrun", "dyld_info", "-inits", binary]), re.MULTILINE)
    defined = {}
    addresses = {}
    for line in output([nm, "--defined-only", "--format=posix", binary]).splitlines():
        words = line.split()
        if len(words) >= 3:
            defined[words[0]] = words[1]
            addresses[words[0]] = int(words[2], 16)
    # dyld_info may print <anon-address> for a local symbol. Its official image
    # offsets and llvm-nm's official symbol addresses still identify it exactly.
    image_base = addresses.get("__mh_execute_header")
    hook = addresses.get("_" + DIAGNOSTICS_INITIALIZER)
    if (not initializers or image_base is None or hook is None
            or image_base + int(initializers[0][0], 16) != hook):
        raise RuntimeError("Worker diagnostics must be disabled by its first image initializer: "
                           + repr(initializers))
    undefined = {line.strip() for line in output([nm, "-u", binary]).splitlines() if line.strip()}
    escapes = sorted(symbol for symbol in undefined if allocation_symbol(symbol))
    if escapes:
        raise RuntimeError("Worker allocation/runtime escapes remain undefined: " + ", ".join(escapes))
    qualified_imports = set().union(*PROFILE_IMPORTS.values())
    unknown = sorted(undefined - SYSTEM_IMPORTS - DIAGNOSTICS_IMPORTS - qualified_imports)
    if unknown:
        raise RuntimeError("Worker has unqualified system imports: " + ", ".join(unknown))
    required = {"_malloc", "_free", "___cxa_get_globals", "___cxa_get_globals_fast", "___emutls_get_address", "___cxa_atexit",
                "_" + DIAGNOSTICS_INITIALIZER}
    if required - defined.keys():
        raise RuntimeError("Worker is missing its local allocation/runtime definitions: " + ", ".join(sorted(required - defined.keys())))
    retained = {symbol: kind for symbol, kind in defined.items()
                if symbol in ALLOCATIONS or re.match(r"^__Z(?:n[aw]|d[al])", symbol) or symbol in required
                or symbol in {"___cxa_finalize", "_atexit"}}
    if any(not kind.islower() for kind in retained.values()):
        raise RuntimeError("Worker allocation/runtime symbols must be local, not interposable exports")
    libraries = []
    for line in output(["/usr/bin/otool", "-L", binary]).splitlines()[1:]:
        if line.strip():
            libraries.append(line.strip().split(" (", 1)[0])
    if set(libraries) - SYSTEM_LIBRARIES:
        raise RuntimeError("Worker links unqualified shared runtimes/libraries: " + ", ".join(sorted(set(libraries) - SYSTEM_LIBRARIES)))
    link_map = args.link_map.read_text()
    if "libforge_asio.a" in link_map or re.search(r"libforge_expression\.a(?:\[[^]]+\])?\(runtime(?:_impl)?\.cpp\.o\)", link_map):
        raise RuntimeError("The single-thread worker extracted the parent Asio/process runtime")
    objects = [match.group(1) for match in re.finditer(r"^\[\s*\d+\]\s+(.+)$", link_map, re.MULTILINE)]
    if not objects or not any("libforge_expression_backend.a" in item for item in objects):
        raise RuntimeError("Worker link map does not establish the genuine private CEL closure")
    runtime = json.loads(args.runtime_manifest.read_text())
    backend = json.loads(args.backend_manifest.read_text())
    backend_archive = args.backend_manifest.parent / "libforge_expression_backend.a"
    if digest(backend_archive) != backend["archive_sha256"]:
        raise RuntimeError("Worker backend differs from its source/toolchain manifest")
    for group in ("archives", "objects"):
        for item in runtime[group].values():
            if digest(Path(item["path"])) != item["sha256"]:
                raise RuntimeError("Worker static runtime changed since qualification: " + item["path"])
    sources = set((root / "libraries/expression").rglob("*.cpp"))
    sources.update((root / "libraries/expression").rglob("*.cppm"))
    sources.update((root / "libraries/expression/details").glob("*.hxx"))
    sources.update((root / "tools/expression-worker").glob("*"))
    sources.update(root / name for name in (
        "libraries/expression/CMakeLists.txt", "cmake/ForgeCEL.cmake", "cmake/cel/BUILD.bazel",
        "cmake/cel/MODULE.bazel.lock", "cmake/cel/sources.json", "cmake/cel/build.py", "cmake/cel/verify_worker.py"))
    sources = {path for path in sources if path.is_file()}
    manifest = {
        "schema": "forge.expression.worker.native.v1",
        "binary": {"name": binary.name, "sha256": digest(binary), "architecture": "arm64"},
        "scope": "isolated single-thread native worker; fixed private heap; OS runtime and native stack are separate",
        "backend_manifest_sha256": digest(args.backend_manifest),
        "backend_archive_sha256": backend["archive_sha256"],
        "runtime_manifest_sha256": digest(args.runtime_manifest),
        "llvm_version": runtime["llvm_version"], "runtime_archives": runtime["archives"],
        "qualified_profile": QUALIFIED_PROFILE,
        "qualified_profile_imports": {group: sorted(undefined & symbols)
                                      for group, symbols in PROFILE_IMPORTS.items()},
        "source_hashes": {str(path.relative_to(root)): digest(path) for path in sorted(sources)},
        "undefined_system_symbols": sorted(undefined), "local_allocation_runtime_symbols": retained,
        "image_initializers": [{"image_offset": offset, "dyld_symbol": symbol}
                               for offset, symbol in initializers],
        "diagnostics_policy": {
            "first_initializer": DIAGNOSTICS_INITIALIZER,
            "fatal_log_symbolization": False, "fatal_log_stack_frames": 0,
            "mutex_deadlock_detection": "ignore",
            "native_stack_bytes": 8 * 1024 * 1024, "core_dump_bytes": 0,
            "sigpipe": "ignored before dependency C++ initializers",
            "linked_unreachable_symbolizer_imports": sorted(undefined & {"_backtrace_symbols"}),
            "os_unwinder_bookkeeping_outside_pool": "_backtrace" in undefined,
        },
        "shared_libraries": libraries, "link_map_sha256": digest(args.link_map),
        "linked_objects": [item.replace(str(root), "${SOURCE_ROOT}").replace(str(llvm), "${LLVM_ROOT}") for item in objects],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2) + "\n")
    print("Expression worker: verified local allocator/static runtime and genuine CEL closure; " + str(args.output))


if __name__ == "__main__":
    main()
