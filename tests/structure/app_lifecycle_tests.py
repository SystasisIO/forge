#!/usr/bin/env python3

import tempfile
import unittest
from pathlib import Path

from check_structure import check_app_lifecycle_ownership


class AppLifecycleTests(unittest.TestCase):
   def setUp(self):
      directory = tempfile.TemporaryDirectory(prefix="forge-app-layout-")
      self.addCleanup(directory.cleanup)
      self.root = Path(directory.name)

   def write(self, path, source):
      destination = self.root / path
      destination.parent.mkdir(parents=True, exist_ok=True)
      destination.write_text(source)

   def errors(self):
      errors = []
      check_app_lifecycle_ownership(self.root, errors)
      return errors

   def test_common_hooks_pass(self):
      self.write("plugins/db/store/plugin.cppm",
                 "class plugin { protected: void on_request_stop() noexcept override; };")
      self.assertEqual(self.errors(), [])

   def test_indented_public_override_fails(self):
      self.write("plugins/db/store/plugin.cppm",
                 "class plugin {\n   void request_stop() noexcept override;\n};")
      self.assertTrue(any("guarded request_stop" in error for error in self.errors()))

   def test_retired_signal_module_fails(self):
      self.write("libraries/app/include/forge/app/signals.cppm", "export module forge.app.signals;")
      self.assertTrue(any("signal module" in error for error in self.errors()))

   def test_duplicate_state_and_legacy_lookup_fail(self):
      self.write("libraries/app/plugin.cpp",
                 "enum class plugin_state {};\nvoid config_value();")
      self.assertEqual(len(self.errors()), 2)


if __name__ == "__main__":
   unittest.main()
