#!/usr/bin/env python3

"""Forward declarations reference private types without owning their definitions."""

import tempfile
import unittest
from pathlib import Path

from check_structure import check_plugin_impl_ownership


class PluginImplOwnershipTests(unittest.TestCase):
   def setUp(self):
      directory = tempfile.TemporaryDirectory(prefix="forge-plugin-ownership-")
      self.addCleanup(directory.cleanup)
      self.root = Path(directory.name)
      self.header = self.root / "plugins" / "db" / "cache" / "details" / "plugin_impl.hxx"
      self.header.parent.mkdir(parents=True)

   def errors(self, source):
      self.header.write_text(source)
      errors = []
      check_plugin_impl_ownership(self.root, errors)
      return errors

   def test_forward_declarations_do_not_own_types(self):
      source = ("class version_store;\nstruct resource;\nenum class status : unsigned;\n"
                "struct plugin::impl { resource* current; };\n")
      self.assertEqual(self.errors(source), [])

   def test_multiline_forward_declarations_and_comments_pass(self):
      source = ("class helper\n  /* { not a definition } */\n  ; // class misleading {};\n"
                "struct resource\n;\nenum struct status\n : unsigned\n;\n"
                "struct plugin::impl\n{\n   helper* value;\n};\n")
      self.assertEqual(self.errors(source), [])

   def test_actual_definitions_still_fail(self):
      for definition in ("class helper {};", "struct helper { int value; };", "enum class helper { value };",
                         "class helper\n{\n};", "struct helper\n : base\n{\n};"):
         with self.subTest(definition=definition):
            errors = self.errors(definition + "\nstruct plugin::impl {};\n")
            self.assertEqual(len(errors), 1)
            self.assertIn("move helper to its exact private header", errors[0])

   def test_definition_after_a_forward_declaration_on_the_same_line_fails(self):
      errors = self.errors("class reference; struct helper {};\nstruct plugin::impl {};\n")
      self.assertEqual(len(errors), 1)
      self.assertIn("move helper to its exact private header", errors[0])

   def test_comments_do_not_hide_definition_or_change_line_number(self):
      source = ("// class ignored {};\n/* struct also_ignored\n{}; */\n"
                "class helper /* ; not a forward declaration */\n{\n};\n")
      errors = self.errors(source)
      self.assertEqual(len(errors), 1)
      self.assertIn("plugin_impl.hxx:4:", errors[0])
      self.assertIn("move helper to its exact private header", errors[0])


if __name__ == "__main__":
   unittest.main()
