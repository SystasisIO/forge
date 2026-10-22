#!/usr/bin/env python3
"""Run native candidates and report exact correctness separately from latency."""

import argparse
import csv
import hashlib
import io
import json
import math
import os
import platform
import re
import signal
import statistics
import subprocess
import time
from decimal import Decimal, InvalidOperation
from pathlib import Path


PINS = {"cel": "9c562a306569e90c8dea6d3af71f4433aa386162",
        "exprtk": "1e4a80b5ec9b4832ed59c6faa65f625a01b18ef0"}


def exact_match(expected, actual):
    if expected["status"] == "error":
        if actual["status"] in expected["allowed_status"]:
            return True, "rejected"
        return False, "expected_error_was_not_reported"
    if actual["status"] != "ok":
        return False, "expected_value_was_not_returned"
    if actual["type"] == "bool":
        return False, "numeric_type_mismatch"
    try:
        number = Decimal(actual["value"])
    except InvalidOperation:
        return False, "invalid_numeric_result"
    if not number.is_finite():
        return False, "nonfinite_result_is_not_engine_error"
    if number != Decimal(expected["value"]):
        return False, "exact_numeric_mismatch"
    return True, "exact_value"


def parse_output(text, case, backend, samples, iterations, corpus_hash):
    result = None
    meta = None
    compile_ns = []
    evaluation_ns = []
    for row in csv.reader(io.StringIO(text), delimiter="\t"):
        if not row:
            continue
        if row[0] == "meta" and len(row) == 6 and meta is None:
            meta = {"case": row[1], "backend": row[2], "pin": row[3], "numeric_type": row[4],
                    "corpus_sha256": row[5]}
        elif row[0] == "result" and len(row) == 5 and result is None:
            result = dict(zip(("status", "kind", "type", "value"), row[1:]))
        elif row[0] == "compile" and len(row) == 3:
            if int(row[1]) != len(compile_ns) or int(row[2]) < 0:
                raise ValueError("invalid compile sample")
            compile_ns.append(int(row[2]))
        elif row[0] == "evaluate" and len(row) == 4:
            if int(row[1]) != len(evaluation_ns) or int(row[2]) < 0 or int(row[3]) != iterations:
                raise ValueError("invalid evaluation sample")
            evaluation_ns.append(int(row[2]) / iterations)
        elif row[0] == "checksum" and len(row) == 2:
            int(row[1])
        else:
            raise ValueError("unexpected native result row")
    if not meta or not result or meta["case"] != case["id"] or meta["backend"] != backend:
        raise ValueError("missing or mismatched native metadata")
    if meta["pin"] != PINS[backend]:
        raise ValueError("unexpected backend source pin")
    if meta["corpus_sha256"] != corpus_hash:
        raise ValueError("stale or mismatched compiled corpus")
    if result["status"] not in {"ok", "compile_error", "evaluation_error", "unsupported", "guard_rejected"}:
        raise ValueError("unknown native result status")
    if result["status"] == "ok" and (len(compile_ns) != samples or
            (case["benchmark"] and len(evaluation_ns) != samples)):
        raise ValueError("missing successful benchmark samples")
    matched, classification = exact_match(case["expected"], result)
    return {"case": case["id"], "group": case["group"], "scope": case["scope"],
            "backend": backend, "numeric_type": meta["numeric_type"], "actual": result,
            "expected": case["expected"], "exact_match": matched, "classification": classification,
            "compile_ns": compile_ns, "evaluate_ns_per_op": evaluation_ns}


def execute(binary, case, backend, arguments, raw_directory, corpus_hash):
    command = ["/usr/bin/time", "-l", str(binary.resolve()), case["id"],
               str(arguments.iterations), str(arguments.warmup), str(arguments.samples)]
    started = time.monotonic()
    environment = dict(os.environ, LC_ALL="C")
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, start_new_session=True, env=environment)
    timed_out = False
    try:
        stdout, stderr = process.communicate(timeout=arguments.timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        stdout, stderr = process.communicate()
    elapsed = time.monotonic() - started
    stem = raw_directory / f"{backend}-{case['id']}"
    stem.with_suffix(".tsv").write_text(stdout)
    stem.with_suffix(".stderr").write_text(stderr)
    if timed_out or process.returncode != 0:
        return {"case": case["id"], "backend": backend, "harness_error":
                "timeout" if timed_out else f"exit_{process.returncode}"}
    try:
        result = parse_output(stdout, case, backend, arguments.samples, arguments.iterations, corpus_hash)
    except (ValueError, KeyError) as error:
        return {"case": case["id"], "backend": backend, "harness_error": str(error)}
    peak = re.search(r"^\s*(\d+)\s+maximum resident set size\s*$", stderr, re.MULTILINE)
    if peak is None:
        return {"case": case["id"], "backend": backend, "harness_error": "missing process RSS measurement"}
    result["process_peak_rss_bytes"] = int(peak.group(1))
    result["process_wall_seconds"] = elapsed
    return result


def report_markdown(report):
    lines = ["# Native expression comparison", "", "Research results; not production qualification.", "",
             "Integer expectations are compared exactly, without floating-point tolerance.",
             "Native candidates use different number models; latency alone does not establish suitability.", "",
             "| Backend | Exact matches | Mismatches | Harness errors |",
             "| --- | ---: | ---: | ---: |"]
    for backend, summary in report["summary"].items():
        lines.append(f"| {backend} | {summary['matches']} | {summary['mismatches']} | {summary['harness_errors']} |")
    lines += ["", "## Timed successful cases", "",
              "Compile includes candidate preparation; evaluation reuses the prepared program and bound variables.",
              "RSS is the whole child process peak, including libraries; it is not per-expression allocation.", "",
              "| Case | Backend | Compile median, ns | Evaluate median, ns/op | Process RSS, bytes |",
              "| --- | --- | ---: | ---: | ---: |"]
    for value in report["results"]:
        if value.get("exact_match") and value.get("evaluate_ns_per_op"):
            lines.append(f"| {value['case']} | {value['backend']} | "
                         f"{statistics.median(value['compile_ns']):.0f} | "
                         f"{statistics.median(value['evaluate_ns_per_op']):.1f} | "
                         f"{value['process_peak_rss_bytes']} |")
    lines += ["", "## Mismatches", "", "| Case | Backend | Actual | Classification |",
              "| --- | --- | --- | --- |"]
    for value in report["results"]:
        if "harness_error" in value:
            lines.append(f"| {value['case']} | {value['backend']} | process failure | HARNESS_ERROR |")
        elif not value["exact_match"]:
            actual = value["actual"]
            lines.append(f"| {value['case']} | {value['backend']} | {actual['status']}: "
                         f"{actual['value']} | {value['classification']} |")
    lines += ["", "Cache hits, repeated inputs and the current host do not establish cross-platform determinism.",
              "Linux, contract intrinsics, Spine/WAMR and financial settlement are outside this run.", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cel", required=True, type=Path)
    parser.add_argument("--exprtk", required=True, type=Path)
    parser.add_argument("--corpus", type=Path, default=Path(__file__).with_name("corpus.json"))
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--iterations", type=int, default=10000)
    parser.add_argument("--warmup", type=int, default=1000)
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--timeout", type=float, default=30)
    arguments = parser.parse_args()
    if platform.system() != "Darwin":
        parser.error("this acceptance run is explicitly limited to macOS")
    if not 0 < arguments.iterations <= 1000000 or not 0 <= arguments.warmup <= 1000000 or not 0 < arguments.samples <= 20:
        parser.error("invalid benchmark limits")
    if not math.isfinite(arguments.timeout) or not 0 < arguments.timeout <= 120:
        parser.error("timeout must be finite and between zero and 120 seconds")
    if arguments.output.exists():
        parser.error("output already exists; preserve previous evidence")
    for binary in (arguments.cel, arguments.exprtk):
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error("candidate executable is missing")
    corpus_bytes = arguments.corpus.read_bytes()
    corpus = json.loads(corpus_bytes)
    arguments.output.mkdir(parents=True)
    raw = arguments.output / "raw"
    raw.mkdir()
    report = {"schema": "forge.expression.native.report.v1", "pins": PINS,
              "corpus_sha256": hashlib.sha256(corpus_bytes).hexdigest(),
              "host": {"system": platform.system(), "machine": platform.machine(),
                       "release": platform.release(), "macos": platform.mac_ver()[0]},
              "settings": {key: getattr(arguments, key) for key in ("iterations", "warmup", "samples", "timeout")},
              "binary_sha256": {key: hashlib.sha256(getattr(arguments, key).read_bytes()).hexdigest() for key in PINS},
              "results": []}
    for case in corpus["cases"]:
        for backend in PINS:
            report["results"].append(execute(getattr(arguments, backend), case, backend, arguments, raw,
                                             report["corpus_sha256"]))
    report["summary"] = {}
    for backend in PINS:
        values = [value for value in report["results"] if value["backend"] == backend]
        report["summary"][backend] = {
            "matches": sum(value.get("exact_match", False) for value in values),
            "mismatches": sum("harness_error" not in value and not value["exact_match"] for value in values),
            "harness_errors": sum("harness_error" in value for value in values),
        }
    (arguments.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    (arguments.output / "report.md").write_text(report_markdown(report))
    print(json.dumps(report["summary"], sort_keys=True))
    return 2 if any(value["harness_errors"] for value in report["summary"].values()) else 0


if __name__ == "__main__":
    raise SystemExit(main())
