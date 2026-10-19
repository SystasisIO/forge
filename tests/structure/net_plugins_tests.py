#!/usr/bin/env python3

"""Regression tests for nested network plugin ownership and retired identities."""

import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

from check_structure import check_net_plugin_families, check_plugin_descriptor_ownership, check_tls_context_ownership


class NetPluginTests(unittest.TestCase):
   def setUp(self):
      self.directory = tempfile.TemporaryDirectory(prefix="forge-net-layout-")
      self.addCleanup(self.directory.cleanup)
      self.root = Path(self.directory.name)
      for family, children in (("net", ("http", "p2p", "tlsnotary")), ("net/http", ("server",)),
                               ("net/p2p", ("node", "resolver")), ("net/tlsnotary", ("session",))):
         self.write(f"plugins/{family}/CMakeLists.txt",
                    "".join(f"add_subdirectory({child})\n" for child in children))
      for leaf in ("http/server", "p2p/node", "p2p/resolver", "tlsnotary/session"):
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
      check_plugin_descriptor_ownership(self.root, errors)
      return errors

   def test_nested_families_pass(self):
      self.assertEqual(self.errors(), [])

   def test_tlsnotary_leaf_identity_must_match_path(self):
      self.write("plugins/net/tlsnotary/session/include/forge/plugins/net/tlsnotary/session/plugin.cppm",
                 "export module forge.plugins.tlsnotary.session.plugin;\n")
      self.assertTrue(any("network plugin module does not match its path" in error for error in self.errors()))

   def test_tlsnotary_group_cannot_own_target(self):
      self.write("plugins/net/tlsnotary/CMakeLists.txt", "add_library(forge_plugins_net_tlsnotary INTERFACE)\n")
      self.assertTrue(any("grouping may only" in error for error in self.errors()))

   def test_descriptor_only_units_fail(self):
      for path in ("plugins/db/store/descriptor.cpp",
                   "plugins/net/p2p/node/include/forge/plugins/net/p2p/node/descriptor.cppm"):
         self.write(path, "// Removed wrapper must not return.\n")
      self.assertEqual(sum("descriptor-only units" in error for error in self.errors()), 2)

   def test_descriptor_is_static_plugin_member(self):
      path = "plugins/net/p2p/node/include/forge/plugins/net/p2p/node/plugin.cppm"
      prefix = "export module forge.plugins.net.p2p.node.plugin;\n"
      self.write(path, prefix + "class plugin {\n public:\n"
                 "   [[nodiscard]] static forge::app::plugin_descriptor descriptor();\n};\n")
      self.assertEqual(self.errors(), [])
      self.write(path, prefix + "[[nodiscard]] forge::app::plugin_descriptor descriptor();\n")
      self.assertTrue(any("static plugin member" in error for error in self.errors()))

   def test_configurable_static_descriptor_passes(self):
      self.write("plugins/chain/signer/include/forge/plugins/chain/signer/plugin.cppm",
                 "class plugin {\n public:\n"
                 "   [[nodiscard]] static forge::app::plugin_descriptor\n"
                 "   descriptor(plugin_options options = {});\n};\n")
      self.assertEqual(self.errors(), [])

   def test_exported_free_descriptor_fails(self):
      self.write("plugins/db/store/include/forge/plugins/db/store/plugin.cppm",
                 "export [[nodiscard]] forge::app::plugin_descriptor descriptor();\n")
      self.assertTrue(any("static plugin member" in error for error in self.errors()))

   def test_default_descriptor_wrapper_fails(self):
      self.write("plugins/db/store/include/forge/plugins/db/store/plugin.cppm",
                 "class plugin {\n public:\n"
                 "   [[nodiscard]] static forge::app::plugin_descriptor default_descriptor();\n};\n")
      self.assertTrue(any("default_descriptor is forbidden" in error for error in self.errors()))

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

   def test_metadata_follows_all_type_declarations(self):
      self.write("plugins/net/p2p/node/include/forge/plugins/net/p2p/node/config.cppm",
                 "export module forge.plugins.net.p2p.node.config;\n"
                 "struct first {};\nclass second {};\nenum class mode { one };\n"
                 "BOOST_DESCRIBE_STRUCT(first, (), ())\nBOOST_DESCRIBE_ENUM(mode, one)\n"
                 "export template <> struct forge::schema::rules<first> {};\n")
      self.assertEqual(self.errors(), [])

   def test_interleaved_metadata_fails(self):
      for metadata in ("BOOST_DESCRIBE_STRUCT(first, (), ())",
                       "BOOST_DESCRIBE_ENUM(mode, one)",
                       "FORGE_EXPORT_API(first, contract)",
                       "export template <> struct forge::schema::rules<first> {};"):
         for declaration in ("struct later {};", "class later;", "enum class later { one };",
                             "template <typename T> struct later {};"):
            with self.subTest(metadata=metadata, declaration=declaration):
               self.write("plugins/net/p2p/node/include/forge/plugins/net/p2p/node/config.cppm",
                          "export module forge.plugins.net.p2p.node.config;\nstruct first {};\n" +
                          metadata + "\n   " + declaration + "\n")
               self.assertTrue(any("final metadata block" in error for error in self.errors()))

   def test_schema_metadata_in_owning_namespace(self):
      for begin, end in (("namespace forge::schema {", "}"),
                         ("namespace forge { namespace schema {", "}}")):
         for described in ("", "BOOST_DESCRIBE_STRUCT(first, (), ())\n"):
            with self.subTest(begin=begin, described=described):
               path = "plugins/net/p2p/node/include/forge/plugins/net/p2p/node/config.cppm"
               source = ("export module forge.plugins.net.p2p.node.config;\nstruct first {};\n" +
                         described + begin + "\ntemplate <> struct rules<first> {};\n" + end + "\n")
               self.write(path, source)
               self.assertEqual(self.errors(), [])
               self.write(path, source + "struct later {};\n")
               self.assertTrue(any("final metadata block" in error for error in self.errors()))

   def test_unrelated_rules_are_not_schema_metadata(self):
      self.write("plugins/net/p2p/node/include/forge/plugins/net/p2p/node/config.cppm",
                 "export module forge.plugins.net.p2p.node.config;\nnamespace product {\n"
                 "template <> struct rules<int> {};\nstruct ordinary {};\n}\n")
      self.assertEqual(self.errors(), [])

   def test_metadata_examples_in_comments_or_strings_are_not_declarations(self):
      self.write("plugins/net/p2p/node/include/forge/plugins/net/p2p/node/config.cppm",
                 "export module forge.plugins.net.p2p.node.config;\n"
                 "// BOOST_DESCRIBE_STRUCT(example, (), ())\n"
                 'constexpr auto example = "BOOST_DESCRIBE_STRUCT(example, (), ())";\n'
                 "struct first {};\nBOOST_DESCRIBE_STRUCT(first, (), ())\n"
                 "/* struct later {}; */\n")
      self.assertEqual(self.errors(), [])


class HttpTlsOwnershipTests(unittest.TestCase):
   def test_extracted_http_owners_keep_negative_tls_checks(self):
      root = Path(__file__).resolve().parents[2]
      original_read = Path.read_text
      for relative in (
         "server_impl.cpp", "server_session_model.cpp", "beast_body_reader_source.cpp",
         "request_read_ownership.cpp", "details/server_session_model.hxx",
         "details/beast_body_reader_source.hxx", "details/request_read_ownership.hxx",
      ):
         path = root / "libraries/net/http" / relative
         for token, message in (
            ("stream_.socket().async_read_some", "disconnect monitor must read the outer HTTP/TLS stream"),
            ("async_wait([&stream", "TLS shutdown deadline must not outlive the stream by reference"),
         ):
            with self.subTest(path=relative, token=token):
               def mutated_read(candidate, *args, **kwargs):
                  text = original_read(candidate, *args, **kwargs)
                  return text + "\n" + token if candidate == path else text

               errors = []
               with patch.object(Path, "read_text", mutated_read):
                  check_tls_context_ownership(root, errors)
               expected = f"libraries/net/http/{relative}: {message}"
               self.assertIn(expected, errors)


if __name__ == "__main__":
   unittest.main()
