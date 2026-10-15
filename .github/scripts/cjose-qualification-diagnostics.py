"""Export bounded static CMake/compiler diagnostics, never raw tool/runtime logs."""

import argparse
import json
import pathlib
import re
import shlex

LIMIT = 2 * 1024 * 1024
VARIABLES = ("FORGE_CJOSE_HAS_QUALIFIED_DEPENDENCIES", "FORGE_CJOSE_HAS_EXTERNAL_SIGNING",
             "CMAKE_C_COMPILER_WORKS", "CMAKE_C_ABI_COMPILED", "CMAKE_C_COMPILER_ABI")


def compiler_invocations(text, replacements):
    invocations = []
    for line in text.splitlines():
        try:
            words = shlex.split(line)
        except ValueError:
            continue
        compiler = next((i for i, word in enumerate(words) if re.fullmatch(r"(?:.*/)?(?:clang|cc)(?:-\d+)?", word)), None)
        if compiler is None or not any(word.startswith("-") for word in words[compiler + 1:]):
            continue
        flags = [word for word in words[compiler + 1:] if re.fullmatch(
            r"(?:-O[0-3gs]|-g(?:[0-3])?|-f(?:no-)?(?:omit-frame-pointer|PIC|PIE)|-fsanitize=[a-z0-9,_-]+|"
            r"-std=[a-z0-9+]+|-mmacosx-version-min=[0-9.]+|-Wl,-(?:search_paths_first|headerpad_max_install_names)|-c)", word)]
        for word in words[compiler + 1:]:
            if word.startswith("-fuse-ld=") and pathlib.Path(word.split("=", 1)[1]).name == "ld64.lld":
                flags.append(safe_line(word, replacements))
        invocations.append({"compiler": safe_line(words[compiler], replacements), "flags": flags[:24]})
    return invocations[:4]


def bounded_text(path):
    with path.open("rb") as stream:
        stream.seek(0, 2)
        stream.seek(max(0, stream.tell() - LIMIT))
        return stream.read(LIMIT).decode("utf-8", errors="replace")


def safe_line(line, replacements):
    for source, replacement in replacements:
        line = line.replace(str(source), replacement)
    # Static errors can still quote user-provided configuration. Drop sensitive
    # lines rather than trying to preserve their values or source snippets.
    if re.search(r"private[ _-]?key|password|secret|credential|bearer|authorization|\btoken\b|\bpayload\b", line, re.I):
        return "[sensitive diagnostic line omitted]"
    line = re.sub(r"https?://\S+", "<url>", line)
    line = re.sub(r"[A-Za-z0-9+/=_-]{64,}", "<blob>", line)
    return re.sub(r"[^\x20-\x7e]", "?", line.strip())[:400]


def static_errors(text, replacements):
    selected = []
    context = 0
    for line in text.splitlines():
        stripped = line.lstrip()
        if re.match(r"^(?:\d+\s*\||\|\s*\^|#(?:include|error)|Run Build Command|Change Dir:|Working Directory:)", stripped):
            continue
        if re.search(r"(?:^|\s)(?:/\S*/)?(?:clang|cc)(?:-\d+)?\s+-", stripped):
            # Invocation flags are exported separately, never the full command.
            continue
        starts_cmake = stripped.startswith("CMake Error") or stripped.startswith("Could NOT find")
        compiler = re.match(r"^(?:\S+\.(?:c|h|cpp|hpp):\d+(?::\d+)?:\s*(?:fatal )?error:|(?:clang|cc|ld|collect2)(?:-[0-9]+)?:.*(?:error:|undefined|not found)|\S*ld:\s)", stripped)
        undefined = re.match(r'''^["'`]([A-Za-z_][A-Za-z0-9_@.]*)["'`],? referenced from:''', stripped)
        if undefined:
            selected.append("undefined symbol: " + undefined[1])
            continue
        if stripped.startswith("Undefined symbols for architecture "):
            selected.append(safe_line(line, replacements))
            continue
        if starts_cmake:
            context = 24
        elif context and line and not line[0].isspace():
            context = 0
        if starts_cmake or compiler or (context and line[:1].isspace()):
            if stripped:
                selected.append(safe_line(line, replacements))
        if context:
            context -= 1
        if len(selected) == 80:
            break
    return selected


def try_compile_events(text, replacements):
    results = []
    # CMake's configure-log event delimiter; do not parse or export arbitrary
    # YAML values, compiler environment, compile commands or source buffers.
    for block in re.split(r"(?m)^  -[ \t]*$", text):
        if not re.search(r'kind:\s*["\']?try_compile-v1', block):
            continue
        variable = re.search(r'variable:[ \t]*["\']?(' + "|".join(VARIABLES) + r')(?:["\']|\s|$)', block)
        if not variable:
            continue
        marker = re.search(r"(?m)^([ \t]*)stdout: \|[^\n]*\n", block)
        output = []
        if marker:
            indent = len(marker[1]) + 2
            for line in block[marker.end():].splitlines():
                if line.strip() and len(line) - len(line.lstrip()) < indent:
                    break
                output.append(line[indent:])
        exit_match = re.search(r"(?m)^\s*exitCode:\s*(-?\d+)\s*$", block)
        results.append({"variable": variable[1],
                        "exit": int(exit_match[1]) if exit_match else None,
                        "diagnostics": static_errors("\n".join(output), replacements),
                        "compiler_invocations": compiler_invocations("\n".join(output), replacements)})
    return results[-8:]


def collect(work, root):
    evidence = work / "evidence"
    replacements = ((work, "$WORK"), (root, "$ROOT"))
    logs = []
    for path in sorted((work / "raw").glob("*.log")):
        if not re.search(r"(?:configure|build|feature-detection(?:-fallback)?)\.log$", path.name):
            continue
        diagnostics = static_errors(bounded_text(path), replacements)
        if diagnostics:
            logs.append({"phase": path.stem, "diagnostics": diagnostics})
    (evidence / "safe-build-diagnostics.json").write_text(json.dumps(logs, indent=2) + "\n")
    configure = []
    builds = {
        "candidate-feature-detection": "candidate-feature-detection-build",
        "candidate-feature-detection-fallback": "candidate-feature-detection-fallback-build",
        "upstream-feature-detection": "upstream-feature-detection-build",
        "sanitized-configure": "sanitized-build",
        "sanitized-shared-configure": "sanitized-shared-consumer",
        "sanitized-static-configure": "sanitized-static-consumer",
        "legacy-abi-configure": "legacy-abi-build",
    }
    for name, build in builds.items():
        path = work / build / "CMakeFiles/CMakeConfigureLog.yaml"
        events = try_compile_events(bounded_text(path), replacements) if path.is_file() else []
        configure.append({"phase": name, "log_present": path.is_file(),
                          "status": "CAPTURED" if events else "NO_MATCHED_TRY_COMPILE", "events": events})
    (evidence / "feature-trycompile-diagnostics.json").write_text(json.dumps(configure, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--root", required=True, type=pathlib.Path)
    args = parser.parse_args()
    collect(args.work, args.root)
