#!/usr/bin/env python3

"""Regression tests for nested network plugin ownership and retired identities."""

import tempfile
import unittest
from pathlib import Path

from check_structure import check_net_plugin_families


class NetPluginTests(unittest.TestCase):
   def setUp(self):
      self.directory = tempfile.TemporaryDirectory(prefix="forge-net-layout-")
      self.addCleanup(self.directory.cleanup)
      self.root = Path(self.directory.name)
      for family, children in (("net", ("http", "p2p")), ("net/http", ("server",)),
                               ("net/p2p", ("node", "resolver", "diagnostics", "pubsub"))):
         self.write(f"plugins/{family}/CMakeLists.txt",
                    "".join(f"add_subdirectory({child})\n" for child in children))
      for leaf in ("http/server", "p2p/node", "p2p/resolver", "p2p/diagnostics", "p2p/pubsub"):
         identity = "forge.plugins.net." + leaf.replace("/", ".")
         self.write(f"plugins/net/{leaf}/CMakeLists.txt",
                    f"add_library({identity.replace('.', '_')} STATIC plugin.cpp)\n")
         self.write(f"plugins/net/{leaf}/include/forge/plugins/net/{leaf}/plugin.cppm",
                    f"export module {identity}.plugin;\n")

   def write(self, path, content):
      destination = self.root / path
      destination.parent.mkdir(parents=True, exist_ok=True)
      destination.write_text(content)

   def errors(self):
      errors = []
      check_net_plugin_families(self.root, errors)
      return errors

   def test_nested_families_pass(self):
      self.assertEqual(self.errors(), [])

   def test_retired_directory_fails(self):
      (self.root / "plugins/p2p").mkdir()
      self.assertTrue(any("retired network plugin family" in error for error in self.errors()))

   def test_retired_import_and_target_alias_fail(self):
      self.write("tests/probe.cpp", "import forge.plugins.p2p.node.plugin;\n")
      self.write("CMakeLists.txt", "add_library(forge_plugins_http_server ALIAS forge_plugins_net_http_server)\n")
      self.assertEqual(sum("retired network plugin identity" in error for error in self.errors()), 2)

   def test_group_target_fails(self):
      self.write("plugins/net/CMakeLists.txt", "add_library(forge_plugins_net INTERFACE)\n")
      self.assertTrue(any("grouping may only" in error for error in self.errors()))


if __name__ == "__main__":
   unittest.main()
