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
                               ("net/p2p", ("node", "resolver"))):
         self.write(f"plugins/{family}/CMakeLists.txt",
                    "".join(f"add_subdirectory({child})\n" for child in children))
      for leaf in ("http/server", "p2p/node", "p2p/resolver"):
         identity = "forge.plugins.net." + leaf.replace("/", ".")
         self.write(f"plugins/net/{leaf}/CMakeLists.txt",
                    f"add_library({identity.replace('.', '_')} STATIC plugin.cpp)\n")
         self.write(f"plugins/net/{leaf}/include/forge/plugins/net/{leaf}/plugin.cppm",
                    f"export module {identity}.plugin;\n")
      for role in ("pubsub_api", "diagnostics_api", "events_api"):
         self.write(f"plugins/net/p2p/node/include/forge/plugins/net/p2p/node/{role}.cppm",
                    f"export module forge.plugins.net.p2p.node.{role};\nclass {role};\n")
      for name in ("config", "types", "exceptions"):
         self.write(f"plugins/net/p2p/node/include/forge/plugins/net/p2p/node/{name}.cppm",
                    f"export module forge.plugins.net.p2p.node.{name};\n" +
                    ('FORGE_DECLARE_EXCEPTION_CATEGORY(exceptions::code, "forge.plugins.net.p2p.node")\n'
                     if name == "exceptions" else ""))

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

   def test_retired_pubsub_and_diagnostics_packages_fail(self):
      for leaf in ("pubsub", "diagnostics"):
         (self.root / "plugins/net/p2p" / leaf).mkdir()
      self.assertEqual(sum("retired network plugin family" in error for error in self.errors()), 2)

   def test_retired_mediator_and_component_fail(self):
      self.write("tests/probe.cpp", "import forge.plugins.net.p2p.node.host_event_source;\n")
      self.write("CMakeLists.txt", "find_package(Forge COMPONENTS plugins_net_p2p_pubsub)\n")
      self.assertEqual(sum("retired network plugin identity" in error for error in self.errors()), 2)

   def test_missing_focused_contract_and_monolithic_api_fail(self):
      (self.root / "plugins/net/p2p/node/include/forge/plugins/net/p2p/node/events_api.cppm").unlink()
      self.write("plugins/net/p2p/node/include/forge/plugins/net/p2p/node/api.cppm",
                 "class api {};\nclass events_api {};\n")
      self.assertTrue(any("missing focused node consumer contract" in error for error in self.errors()))
      self.assertTrue(any("may only own the main api contract" in error for error in self.errors()))

   def test_missing_canonical_config_fails(self):
      (self.root / "plugins/net/p2p/node/include/forge/plugins/net/p2p/node/config.cppm").unlink()
      self.assertTrue(any("missing canonical node config" in error for error in self.errors()))

   def test_secondary_exception_set_fails(self):
      self.write("plugins/net/p2p/node/include/forge/plugins/net/p2p/node/extra.cppm",
                 'class transport_exceptions {};\n'
                 'FORGE_DECLARE_EXCEPTION_CATEGORY(transport_exceptions::code, "node.transport")\n')
      errors = self.errors()
      self.assertTrue(any("one exceptions set" in error for error in errors))
      self.assertTrue(any("exactly one exception category" in error for error in errors))
      self.assertTrue(any("belongs to exceptions.cppm" in error for error in errors))

   def test_reintroduced_per_role_values_fails(self):
      for name in ("pubsub_types", "diagnostics_types", "resource_config"):
         self.write(f"plugins/net/p2p/node/include/forge/plugins/net/p2p/node/{name}.cppm",
                    f"export module forge.plugins.net.p2p.node.{name};\n")
      self.assertEqual(sum("canonical types/config/exceptions" in error for error in self.errors()), 3)

   def test_role_specific_error_category_fails(self):
      self.write("plugins/net/p2p/node/include/forge/plugins/net/p2p/node/exceptions.cppm",
                 "export module forge.plugins.net.p2p.node.exceptions;\n"
                 'FORGE_DECLARE_EXCEPTION_CATEGORY(exceptions::code, "forge.plugins.net.p2p.node.pubsub")\n')
      self.assertTrue(any("canonical plugin category" in error for error in self.errors()))

   def test_types_cannot_depend_on_config_or_schema(self):
      for dependency in ("forge.plugins.net.p2p.node.config", "forge.schema.object"):
         with self.subTest(dependency=dependency):
            self.write("plugins/net/p2p/node/include/forge/plugins/net/p2p/node/types.cppm",
                       "export module forge.plugins.net.p2p.node.types;\n" + f"import {dependency};\n")
            self.assertTrue(any("must not import configuration/schema" in error for error in self.errors()))

   def test_types_cannot_own_declarative_settings(self):
      self.write("plugins/net/p2p/node/include/forge/plugins/net/p2p/node/types.cppm",
                 "export module forge.plugins.net.p2p.node.types;\nstruct pubsub_config {};\n")
      self.assertTrue(any("settings belong to config.cppm" in error for error in self.errors()))


if __name__ == "__main__":
   unittest.main()
