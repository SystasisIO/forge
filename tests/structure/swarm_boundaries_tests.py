from pathlib import Path
import tempfile
import unittest

from swarm_boundaries import check_swarm_boundaries


class SwarmBoundariesTests(unittest.TestCase):
   def setUp(self):
      self.directory = tempfile.TemporaryDirectory()
      self.addCleanup(self.directory.cleanup)
      self.root = Path(self.directory.name)
      self.write("libraries/swarm/CMakeLists.txt", "add_subdirectory(core)\n")
      self.write("libraries/swarm/core/CMakeLists.txt", "add_library(forge_swarm_core STATIC descriptor.cpp)\n")
      self.write("libraries/swarm/core/descriptor.cpp", "namespace forge::swarm::core {}\n")
      self.write("libraries/core/include/forge/core/conversion.cppm",
                 "export module forge.core.conversion;\n"
                 "export namespace forge::core { template <class Result> class conversion; }\n")

   def write(self, name, text):
      target = self.root / name
      target.parent.mkdir(parents=True, exist_ok=True)
      target.write_text(text)

   def errors(self):
      errors = []
      check_swarm_boundaries(self.root, errors)
      return errors

   def test_neutral_leaf_passes(self):
      self.assertEqual(self.errors(), [])

   def test_grouping_target_is_rejected(self):
      self.write("libraries/swarm/CMakeLists.txt", "add_library(forge_swarm INTERFACE)\n")
      self.assertTrue(any("aggregate target" in e for e in self.errors()))

   def test_root_namespace_is_rejected(self):
      self.write("libraries/swarm/core/descriptor.cpp", "namespace forge::swarm { struct descriptor {}; }\n")
      self.assertTrue(any("leaf namespace" in e for e in self.errors()))

   def test_upper_layer_import_is_rejected(self):
      for layer in ("chain.core.types", "db.object.store", "api.core", "asio.task", "app.plugin", "net.p2p.node",
                    "swarm.db.content", "swarm.storage.catalog", "swarm.runtime.session"):
         with self.subTest(layer=layer):
            self.write("libraries/swarm/core/descriptor.cpp", f"import forge.{layer};\n")
            self.assertTrue(any("imports an upper layer" in e for e in self.errors()))

   def test_upper_layer_link_is_rejected(self):
      for target in ("forge_db_object", "forge_swarm_db", "forge_swarm_storage"):
         with self.subTest(target=target):
            self.write("libraries/swarm/core/CMakeLists.txt", f"target_link_libraries(forge_swarm_core PUBLIC {target})\n")
            self.assertTrue(any("target depends" in e for e in self.errors()))

   def test_core_receives_time_and_randomness_explicitly(self):
      name = "libraries/swarm/core/transfer.cpp"
      self.write(name, "using time = std::chrono::steady_clock::time_point;\n")
      self.assertEqual(self.errors(), [])
      for source in ("std::chrono::steady_clock::now();", "system_clock::now();",
                     "using clock = std::chrono::steady_clock; clock::now();"):
         with self.subTest(source=source):
            self.write(name, source)
            self.assertTrue(any("time explicitly" in e for e in self.errors()))
      for source in ("std::random_device random;", "std::jthread worker;",
                     "boost::asio::steady_timer timer;", "boost::asio::thread_pool pool;",
                     "boost::asio::io_context context;", "boost::asio::io_service service;"):
         with self.subTest(source=source):
            self.write(name, source)
            self.assertTrue(any("runtime execution" in e for e in self.errors()))

   def test_core_customization_cannot_import_domain(self):
      self.write("libraries/core/include/forge/core/conversion.cppm",
                 "import forge.swarm.core.descriptor;\ntemplate <class T> class conversion;\n")
      self.assertTrue(any("acquire dependencies" in e for e in self.errors()))

   def test_storage_reuses_asio_but_not_payload_backends(self):
      name = "libraries/swarm/storage/file_store.cpp"
      self.write(name, "import forge.asio.compute;\nimport forge.swarm.core.store;\n")
      self.assertEqual(self.errors(), [])
      for module in ("net.p2p.node", "db.blob.store", "db.rocksdb.driver", "rocksdb.database", "api.core"):
         with self.subTest(module=module):
            self.write(name, f"import forge.{module};\n")
            self.assertTrue(any("payload backend" in e for e in self.errors()))

   def test_storage_cannot_create_its_own_thread_pool(self):
      for source in ("std::jthread worker;", "std::async(work);", "boost::asio::thread_pool pool;"):
         with self.subTest(source=source):
            self.write("libraries/swarm/storage/file_store.cpp", source)
            self.assertTrue(any("private worker runtime" in e for e in self.errors()))

   def test_metadata_models_do_not_import_concrete_runtime(self):
      name = "libraries/swarm/db/content.cppm"
      self.write(name, "import forge.swarm.core.checkpoint;\nimport forge.db.object.index;\n")
      self.assertEqual(self.errors(), [])
      for module in ("swarm.storage.catalog", "swarm.protocol.types", "db.rocksdb.driver", "db.blob.store", "net.p2p.node"):
         with self.subTest(module=module):
            self.write(name, f"import forge.{module};\n")
            self.assertTrue(any("indexed metadata imports" in e for e in self.errors()))

   def test_metadata_target_does_not_link_concrete_runtime(self):
      self.write("libraries/swarm/db/CMakeLists.txt", "target_link_libraries(forge_swarm_db PUBLIC forge_swarm_storage)\n")
      self.assertTrue(any("models depend" in e for e in self.errors()))

   def test_protocol_api_conversion_ownership(self):
      allowed = {
         "protocol": ("swarm.core.descriptor", "raw.codec"),
         "api": ("swarm.protocol.peer_message", "api.auth.authenticated_caller"),
         "conversion": ("core.conversion", "swarm.db.content", "swarm.protocol.metadata"),
         "runtime": ("swarm.api.peer", "swarm.core.store", "asio.task", "asio.notification"),
      }
      rejected = {
         "protocol": ("swarm.db.content", "api.core.connection", "net.p2p.node"),
         "api": ("swarm.db.content", "swarm.storage.catalog", "net.p2p.node"),
         "conversion": ("swarm.storage.catalog", "swarm.api.peer", "asio.task"),
         "runtime": ("swarm.db.content", "swarm.storage.file_store", "net.p2p.node", "app.plugin",
                     "api.transport.connection", "api.stream.session"),
      }
      for leaf, modules in allowed.items():
         self.write(f"libraries/swarm/{leaf}/entity.cpp", "".join(f"import forge.{m};\n" for m in modules))
      self.assertEqual(self.errors(), [])
      for leaf, modules in rejected.items():
         for module in modules:
            name = f"libraries/swarm/{leaf}/entity.cpp"
            with self.subTest(leaf=leaf, module=module):
               original = "".join(f"import forge.{m};\n" for m in allowed[leaf])
               self.write(name, f"import forge.{module};\n")
               self.assertTrue(any("reverse dependency" in e for e in self.errors()))
               self.write(name, original)
      for leaf, target in (("protocol", "forge_swarm_db"), ("api", "forge_net_p2p"),
                           ("conversion", "forge_swarm_storage"), ("runtime", "forge_swarm_storage"),
                           ("runtime", "forge_api_p2p")):
         with self.subTest(leaf=leaf, target=target):
            self.write(f"libraries/swarm/{leaf}/CMakeLists.txt", f"target_link_libraries(forge_swarm_{leaf} PUBLIC {target})\n")
            self.assertTrue(any("reverse dependency" in e for e in self.errors()))

   def test_runtime_cannot_create_private_workers(self):
      for source in ("std::thread worker;", "std::async(work);", "boost::asio::thread_pool pool;",
                     "boost::asio::io_context context;"):
         with self.subTest(source=source):
            self.write("libraries/swarm/runtime/engine.cpp", source)
            self.assertTrue(any("private worker runtime" in e for e in self.errors()))

   def test_domain_cannot_redeclare_conversion_primary(self):
      self.write("libraries/swarm/conversion/conversion.cppm", "template <class Result> class conversion;\n")
      self.assertTrue(any("Core primary" in e for e in self.errors()))


if __name__ == "__main__":
   unittest.main()
