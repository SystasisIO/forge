#!/usr/bin/env python3

"""Approved Crypto leaves do not permit symbols in the grouping namespace."""

import tempfile
import unittest
from pathlib import Path

from check_structure import check_crypto_family


class CryptoFamilyTests(unittest.TestCase):
   def setUp(self):
      directory = tempfile.TemporaryDirectory(prefix="forge-crypto-family-")
      self.addCleanup(directory.cleanup)
      self.root = Path(directory.name)
      self.write("CMakeLists.txt", "")
      self.write("cmake/ForgeConfig.cmake.in", "")

   def write(self, name, source):
      path = self.root / name
      path.parent.mkdir(parents=True, exist_ok=True)
      path.write_text(source)
      return path

   def errors(self, source):
      probe = self.write("tests/probe.cpp", source)
      errors = []
      check_crypto_family(self.root, [probe], errors)
      return errors

   def test_approved_exchange_leaf_passes(self):
      self.write("libraries/crypto/exchange/CMakeLists.txt",
                 "add_library(forge_crypto_exchange STATIC exchange.cpp)\n")
      source = ("export namespace forge::crypto::exchange { struct manifest {}; }\n"
                "forge::crypto::exchange::manifest value;\n"
                "import forge.crypto.exchange;\n")
      self.assertEqual(self.errors(source), [])

   def test_grouping_namespace_and_root_symbol_still_fail(self):
      errors = self.errors("export namespace forge::crypto { struct manifest {}; }\n"
                           "forge::crypto::manifest value;\n")
      self.assertEqual(len(errors), 2)
      self.assertTrue(any("forge::crypto is a grouping namespace" in error for error in errors))
      self.assertTrue(any("forge::crypto::manifest bypasses" in error for error in errors))

   def test_exchange_lookalikes_are_not_approved(self):
      for name in ("exchange_extra", "exchanges"):
         with self.subTest(name=name):
            errors = self.errors(f"forge::crypto::{name}::manifest value;\n")
            self.assertEqual(len(errors), 1)
            self.assertIn(f"forge::crypto::{name} bypasses", errors[0])

   def test_removed_root_module_still_fails(self):
      errors = self.errors("import forge.crypto.sha256;\n")
      self.assertEqual(len(errors), 1)
      self.assertIn("removed Crypto module forge.crypto.sha256 is forbidden", errors[0])

   def test_removed_aggregate_target_and_component_still_fail(self):
      self.write("CMakeLists.txt", "add_library(forge_crypto INTERFACE)\n"
                                 "find_package(Forge CONFIG REQUIRED COMPONENTS crypto)\n")
      errors = self.errors("")
      self.assertEqual(len(errors), 2)
      self.assertTrue(any("removed Crypto aggregate target" in error for error in errors))
      self.assertTrue(any("removed Crypto package component" in error for error in errors))


if __name__ == "__main__":
   unittest.main()
