#!/usr/bin/env python3
"""Portable selftests for the exact approved-profile donor oracle transformation."""

from __future__ import annotations

from collections import Counter
from dataclasses import replace
from contextlib import redirect_stderr
import hashlib
import io
import json
import pathlib
import tempfile
import unittest
from unittest.mock import patch

import port_donor_tests as port


HERE = pathlib.Path(__file__).parent


class ProfileOracleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.historical = {}
        for source, policy in port.PROFILE_SOURCES.items():
            relative = pathlib.Path(source)
            destination = HERE / "donor" / (relative if relative.parts[0] == "spec" else "unit" / relative)
            text = destination.read_text()
            if hashlib.sha256(text.encode()).hexdigest() != policy.historical_port_sha256:
                # The checked-in profile port must reverse to the exact old golden hash.
                for rule in reversed(port.PROFILE_EXPECTATIONS):
                    if rule.source != source:
                        continue
                    start, end = port.profile_case_span(text, rule)
                    block = text[start:end]
                    for old, new in reversed(rule.replacements):
                        if block.count(new) != 1:
                            raise AssertionError(f"{rule.id}: noncanonical checked-in profile port")
                        block = block.replace(new, old, 1)
                    text = text[:start] + block + text[end:]
            if hashlib.sha256(text.encode()).hexdigest() != policy.historical_port_sha256:
                raise AssertionError(f"{source}: historical preimage hash mismatch")
            cls.historical[source] = text

    def apply(self, source: str, text: str | None = None) -> str:
        return port.apply_profile_expectations(
            self.historical[source] if text is None else text,
            pathlib.Path(source), port.PROFILE_SOURCES[source].donor_sha256,
        )

    def apply_with_reviewed_preimage(self, source: str, text: str) -> str:
        policy = replace(port.PROFILE_SOURCES[source], historical_port_sha256=hashlib.sha256(text.encode()).hexdigest())
        with patch.dict(port.PROFILE_SOURCES, {source: policy}):
            return self.apply(source, text)

    def test_exact_seventeen_rule_inventory(self) -> None:
        self.assertEqual(len(port.PROFILE_EXPECTATIONS), 17)
        self.assertEqual(len({rule.id for rule in port.PROFILE_EXPECTATIONS}), 17)
        self.assertEqual({rule.source for rule in port.PROFILE_EXPECTATIONS}, set(port.PROFILE_SOURCES))
        self.assertEqual(Counter(rule.category for rule in port.PROFILE_EXPECTATIONS), {
            "retired_malformed": 6, "capacity": 2, "modern_grammar": 8, "modern_typing": 1,
        })
        self.assertEqual(2 * len(port.PROFILE_EXPECTATIONS), 34)

    def test_all_rules_apply_without_changing_case_ids_or_dropping_assertions(self) -> None:
        added = 0
        for source, before in self.historical.items():
            with self.subTest(source=source):
                after = self.apply(source)
                self.assertEqual(port.case_manifest(before, pathlib.Path(source).name),
                                 port.case_manifest(after, pathlib.Path(source).name))
                self.assertEqual(before.count("\n"), after.count("\n"))
                delta = sum(port.ported_assertions(after).values()) - sum(port.ported_assertions(before).values())
                self.assertEqual(delta, port.PROFILE_SOURCES[source].added_assertions)
                added += delta
                for rule in port.PROFILE_EXPECTATIONS:
                    if rule.source == source:
                        start, end = port.profile_case_span(after, rule)
                        for old, new in rule.replacements:
                            self.assertNotIn(old, after[start:end])
                            self.assertEqual(after[start:end].count(new), 1)
        self.assertEqual(added, 14)

    def test_preserves_every_untargeted_case_block(self) -> None:
        for source, before in self.historical.items():
            after = self.apply(source)
            titles = {rule.title for rule in port.PROFILE_EXPECTATIONS if rule.source == source}
            matches = list(port.CASE_PATTERN.finditer(before))
            after_matches = list(port.CASE_PATTERN.finditer(after))
            for index, match in enumerate(matches):
                if match.group(2) in titles:
                    continue
                end = matches[index + 1].start() if index + 1 < len(matches) else len(before)
                after_end = after_matches[index + 1].start() if index + 1 < len(after_matches) else len(after)
                self.assertEqual(before[match.start():end], after[after_matches[index].start():after_end])

    def test_rejects_changed_donor_source_hash(self) -> None:
        source = next(iter(self.historical))
        with self.assertRaisesRegex(RuntimeError, "pinned donor source hash"):
            port.apply_profile_expectations(self.historical[source], pathlib.Path(source), "0" * 64)

    def test_rejects_changed_historical_port_hash(self) -> None:
        source = next(iter(self.historical))
        with self.assertRaisesRegex(RuntimeError, "historical port hash"):
            self.apply(source, self.historical[source] + "\n")

    def test_rejects_missing_case_anchor(self) -> None:
        rule = port.PROFILE_EXPECTATIONS[0]
        text = self.historical[rule.source].replace(rule.title, "missing title", 1)
        with self.assertRaisesRegex(RuntimeError, "exactly one title/tags anchor"):
            self.apply_with_reviewed_preimage(rule.source, text)

    def test_rejects_changed_case_tags(self) -> None:
        rule = port.PROFILE_EXPECTATIONS[0]
        text = self.historical[rule.source]
        start, end = port.profile_case_span(text, rule)
        text = text[:start] + text[start:end].replace(rule.tags, "[different]", 1) + text[end:]
        with self.assertRaisesRegex(RuntimeError, "exactly one title/tags anchor"):
            self.apply_with_reviewed_preimage(rule.source, text)

    def test_rejects_duplicate_case_anchor(self) -> None:
        rule = port.PROFILE_EXPECTATIONS[0]
        text = self.historical[rule.source]
        start, end = port.profile_case_span(text, rule)
        with self.assertRaisesRegex(RuntimeError, "exactly one title/tags anchor"):
            self.apply_with_reviewed_preimage(rule.source, text + text[start:end])

    def test_rejects_missing_statement_anchor(self) -> None:
        rule = port.PROFILE_EXPECTATIONS[0]
        text = self.historical[rule.source].replace(rule.replacements[0][0], "   unexpected();", 1)
        with self.assertRaisesRegex(RuntimeError, "exactly one statement anchor"):
            self.apply_with_reviewed_preimage(rule.source, text)

    def test_rejects_duplicate_statement_anchor(self) -> None:
        rule = port.PROFILE_EXPECTATIONS[0]
        old = rule.replacements[0][0]
        text = self.historical[rule.source].replace(old, old + "\n" + old, 1)
        with self.assertRaisesRegex(RuntimeError, "exactly one statement anchor"):
            self.apply_with_reviewed_preimage(rule.source, text)

    def test_rejects_line_changing_replacement(self) -> None:
        rule = port.PROFILE_EXPECTATIONS[0]
        old, new = rule.replacements[0]
        changed = replace(rule, replacements=((old, new + "\n"),))
        with patch.object(port, "PROFILE_EXPECTATIONS", (changed,) + port.PROFILE_EXPECTATIONS[1:]):
            with self.assertRaisesRegex(RuntimeError, "compiled case line IDs"):
                self.apply(rule.source)

    def test_rejects_wrong_assertion_delta(self) -> None:
        source = next(iter(self.historical))
        policy = replace(port.PROFILE_SOURCES[source], added_assertions=0)
        with patch.dict(port.PROFILE_SOURCES, {source: policy}):
            with self.assertRaisesRegex(RuntimeError, "reviewed assertion count"):
                self.apply(source)

    def test_rejects_fixture_case_misassociation(self) -> None:
        rules = tuple(replace(rule, fixtures=("binary.37.wasm",)) if rule.id == "modern_binary_38" else rule
                      for rule in port.PROFILE_EXPECTATIONS)
        with patch.object(port, "PROFILE_EXPECTATIONS", rules):
            with self.assertRaisesRegex(RuntimeError, "fixture name is not bound"):
                self.apply("spec/binary_tests.cpp")

    def test_rejects_duplicate_or_unused_rule_inventory(self) -> None:
        for rules in (port.PROFILE_EXPECTATIONS + (port.PROFILE_EXPECTATIONS[0],),
                      port.PROFILE_EXPECTATIONS[:-1],
                      (replace(port.PROFILE_EXPECTATIONS[0], source="unused.cpp"),) + port.PROFILE_EXPECTATIONS[1:]):
            with self.subTest(rules=len(rules)), patch.object(port, "PROFILE_EXPECTATIONS", rules):
                with self.assertRaisesRegex(RuntimeError, "approved 17 cases"):
                    port.build_profile_manifest(pathlib.Path("not-read"), pathlib.Path("not-read"))

    def test_retains_all_flag_and_trailing_code_negative_checks(self) -> None:
        flags = self.apply("allow_u32_limits_flags_tests.cpp")
        self.assertEqual(flags.count("BOOST_CHECK_THROW("), 24)
        trailing = self.apply("allow_code_after_function_end_tests.cpp")
        self.assertEqual(trailing.count("for(auto wasm : fail_wasms)"), 2)
        self.assertEqual(trailing.count("BOOST_CHECK_THROW("), 8)

    def test_depth_refusal_preserves_earlier_calls_and_has_supported_recovery(self) -> None:
        source = "implementation_limits_tests.cpp"
        before = self.historical[source]
        after = self.apply(source)
        marker = "   // Very large call depth"
        self.assertTrue(after.startswith(before[:before.index(marker)]))
        self.assertIn("BOOST_CHECK_THROW(bkend.initialize(nullptr, dynamic_options{1024*1024}), exceptions::profile);",
                      after)
        self.assertTrue(after.endswith(
            '   bkend.initialize(nullptr, dynamic_options{51});\n'
            '   BOOST_TEST(static_cast<bool>(!bkend.call_with_return("env", "call", (uint32_t)50)));\n}\n'))

    def test_modern_fixtures_are_exact_and_do_not_relax_negative_neighbors(self) -> None:
        selected = {name for rule in port.PROFILE_EXPECTATIONS if rule.category.startswith("modern_")
                    for name in rule.fixtures}
        self.assertEqual(selected, {"binary.38.wasm", "binary.39.wasm", "binary.40.wasm", "binary.41.wasm",
                                    "binary.43.wasm", "binary.44.wasm", "binary.45.wasm", "binary.46.wasm",
                                    "unreached-invalid.87.wasm"})
        after = self.apply("spec/binary_tests.cpp")
        for number in (37, 42):
            anchor = f'   auto code = read_wasm( std::string(wasm_directory) + "binary.{number}.wasm");\n'
            self.assertIn(anchor + "   BOOST_CHECK_THROW(backend_t(code, nullptr), std::exception);", after)

    def test_generic_spec_generator_transform_has_no_profile_rules(self) -> None:
        original = ('BACKEND_TEST_CASE("Testing wasm <binary_38_wasm>", "[binary_38_wasm_tests]") {\n'
                    '   CHECK_THROWS_AS(backend_t(code, nullptr), std::exception);\n}\n')
        generated = port.transform_text(original, pathlib.Path("spec/binary_tests.cpp"))
        self.assertIn("BOOST_CHECK_THROW(", generated)
        self.assertNotIn("BOOST_REQUIRE_NO_THROW(", generated)

    def test_unrelated_sources_and_retained_zero_blocktype_are_untouched(self) -> None:
        text = "unchanged\n"
        for source in ("allow_zero_blocktype_tests.cpp", "spec/names_tests.cpp", "other.cpp"):
            self.assertEqual(port.apply_profile_expectations(text, pathlib.Path(source), "not-used"), text)

    def test_historical_manifest_and_corpus_inputs_remain_pinned(self) -> None:
        manifest_path = HERE / "donor_manifest.json"
        self.assertEqual(port.sha256(manifest_path), port.DONOR_MANIFEST_SHA256)
        manifest = json.loads(manifest_path.read_text())
        self.assertEqual(manifest["donor"]["commit"], port.DONOR_COMMIT)
        self.assertEqual(manifest["fixtures"]["commit"], port.FIXTURE_COMMIT)
        self.assertEqual(len(manifest["sources"]), 105)
        self.assertEqual(sum(len(row["cases"]) for row in manifest["sources"]), 2568)
        self.assertEqual(len(manifest["fixtures"]["files"]), 2473)
        fixture_hashes = {row["path"]: row["sha256"] for row in manifest["fixtures"]["files"]}
        names = {name for rule in port.PROFILE_EXPECTATIONS for name in rule.fixtures} | {
            "binary.37.wasm", "binary.42.wasm"}
        for name in names:
            self.assertEqual(port.sha256(HERE.parents[1] / "vendor/eos-vm-test-wasms" / name), fixture_hashes[name])

    def test_write_cannot_refresh_a_changed_historical_manifest(self) -> None:
        historical = json.loads((HERE / "donor_manifest.json").read_text())
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            baseline = root / "donor_manifest.json"
            baseline.write_text("{}\n")
            output = root / "port"
            output.mkdir()
            marker = output / "untouched"
            marker.write_bytes(b"preserve\n")
            profile = root / "profile.json"
            argv = ["port_donor_tests.py", "--donor", "not-read", "--fixtures", "not-read", "--write",
                    "--output", str(output), "--manifest", str(baseline), "--profile-manifest", str(profile)]
            with patch.object(port, "build_manifest", return_value=historical), patch("sys.argv", argv):
                with redirect_stderr(io.StringIO()) as error:
                    self.assertEqual(port.main(), 1)
            self.assertIn("refusing to rebaseline", error.getvalue())
            self.assertEqual(baseline.read_bytes(), b"{}\n")
            self.assertEqual(marker.read_bytes(), b"preserve\n")
            self.assertFalse(profile.exists())


class HostFunctionHarnessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        text = (HERE / "donor/unit/host_functions_tests.cpp").read_text()
        if hashlib.sha256(text.encode()).hexdigest() != port.HOST_FUNCTIONS_PORT_SHA256:
            for old, new, count in reversed(port.HOST_FUNCTIONS_HARNESS):
                if text.count(new) != count:
                    raise AssertionError("noncanonical host function harness")
                text = text.replace(new, old)
        if hashlib.sha256(text.encode()).hexdigest() != port.HOST_FUNCTIONS_PORT_SHA256:
            raise AssertionError("host function historical port hash mismatch")
        cls.historical = text

    def apply(self, text: str | None = None, donor_hash: str = port.HOST_FUNCTIONS_DONOR_SHA256) -> str:
        return port.adapt_host_function_harness(
            self.historical if text is None else text, pathlib.Path("host_functions_tests.cpp"), donor_hash)

    def test_preserves_all_cases_lines_assertions_and_original_fixture(self) -> None:
        prepared = self.apply()
        self.assertEqual(prepared, (HERE / "donor/unit/host_functions_tests.cpp").read_text())
        self.assertEqual(port.case_manifest(self.historical, "host_functions_tests.cpp"),
                         port.case_manifest(prepared, "host_functions_tests.cpp"))
        self.assertEqual(port.ported_assertions(self.historical), port.ported_assertions(prepared))
        self.assertEqual(self.historical.count("\n"), prepared.count("\n"))
        self.assertIn('#include "host_functions_tests_1.wasm.hpp"', prepared)
        for old, new, count in port.HOST_FUNCTIONS_HARNESS:
            self.assertNotIn(old, prepared)
            self.assertEqual(prepared.count(new), count)

    def test_rejects_changed_donor_and_port(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "pinned donor source hash"):
            self.apply(donor_hash="0" * 64)
        with self.assertRaisesRegex(RuntimeError, "historical port hash"):
            self.apply(self.historical + "\n")

    def test_rejects_missing_and_duplicate_anchors(self) -> None:
        for old, _, count in port.HOST_FUNCTIONS_HARNESS:
            for changed in (self.historical.replace(old, "missing", 1), self.historical + old):
                digest = hashlib.sha256(changed.encode()).hexdigest()
                with self.subTest(anchor=old, count=count), patch.object(port, "HOST_FUNCTIONS_PORT_SHA256", digest):
                    with self.assertRaisesRegex(RuntimeError, "statement anchors"):
                        self.apply(changed)

    def test_unrelated_sources_are_untouched(self) -> None:
        self.assertEqual(port.adapt_host_function_harness("unchanged", pathlib.Path("other.cpp"), "unused"),
                         "unchanged")


if __name__ == "__main__":
    unittest.main()
