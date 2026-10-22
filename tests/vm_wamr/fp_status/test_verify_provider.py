"""Bounded positive/negative controls for the link-provider gate, no checkout needed."""

import unittest

from verify_provider import SYMBOLS, verify


COMMON = "CMakeFiles/forge_wamr_fp_status_common.dir/tree/wasm_runtime_common.c.o"
ARCHIVE = "libraries/libforge_vm_wasm_interpret.a(wasm_exec_env.c.o)"


class ProviderTest(unittest.TestCase):
    def symbols(self, prefix=""):
        return "\n".join(f"00001234 T {prefix}{name}" for name in SYMBOLS)

    def test_darwin(self):
        verify(f"[  1] {COMMON}\n[  2] {ARCHIVE}", self.symbols("_"))

    def test_darwin_indexed_archive(self):
        verify(COMMON + "\n" + ARCHIVE.replace(".a(", ".a[7]("), self.symbols("_"))

    def test_linux(self):
        verify(f" .text 0x0 0x123 {COMMON}\n{ARCHIVE}", self.symbols())

    def test_missing_injected_common(self):
        with self.assertRaisesRegex(ValueError, "missing explicit"):
            verify(ARCHIVE, self.symbols())

    def test_missing_real_allocator(self):
        with self.assertRaisesRegex(ValueError, "missing exec-env"):
            verify(COMMON, self.symbols())

    def test_uninjected_archive_common(self):
        for suffix in (".a(", ".a[9]("):
            with self.subTest(suffix=suffix), self.assertRaisesRegex(ValueError, "uninjected"):
                verify(COMMON + "\n" + ARCHIVE + "\nlibforge_vm_wasm_interpret"
                       + suffix + "wasm_runtime_common.c.o)", self.symbols())

    def test_missing_definition(self):
        with self.assertRaisesRegex(ValueError, "one definition"):
            verify(COMMON + "\n" + ARCHIVE, self.symbols().split("\n", 1)[1])

    def test_undefined_reference_is_not_definition(self):
        with self.assertRaisesRegex(ValueError, "one definition"):
            verify(COMMON + "\n" + ARCHIVE, self.symbols().replace("T ", "U ", 1))

    def test_duplicate_definition(self):
        with self.assertRaisesRegex(ValueError, "one definition"):
            verify(COMMON + "\n" + ARCHIVE, self.symbols() + "\n00005678 T " + SYMBOLS[0])


if __name__ == "__main__":
    unittest.main()
