from pathlib import Path
import subprocess
import tempfile
import unittest


class PackageDependencyBoundaryTests(unittest.TestCase):
   def check_graph(self, edges, forbidden=False):
      helper = Path(__file__).resolve().parents[1] / "package_dependency_boundary.cmake"
      with tempfile.TemporaryDirectory() as directory:
         root = Path(directory)
         (root / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.31)\n'
            'project(dependency_boundary LANGUAGES NONE)\n'
            f'include("{helper.as_posix()}")\n'
            'add_library(Forge::forge_safe INTERFACE IMPORTED)\n'
            'add_library(plain_middle INTERFACE IMPORTED)\n'
            'add_library(Forge::forge_db_object INTERFACE IMPORTED)\n'
            + edges + '\n'
            'forge_package_assert_dependency_boundary(Forge::forge_safe "^Forge::forge_db_")\n'
         )
         result = subprocess.run(
            ["cmake", "-S", str(root), "-B", str(root / "build")],
            capture_output=True, text=True, timeout=30,
         )
         if forbidden:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Forbidden dependency", result.stderr)
         else:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

   def test_unrelated_imported_target_is_not_a_dependency(self):
      self.check_graph("")

   def test_direct_link_dependency_is_rejected(self):
      self.check_graph('set_property(TARGET Forge::forge_safe PROPERTY INTERFACE_LINK_LIBRARIES Forge::forge_db_object)', True)

   def test_plain_middle_and_link_only_cannot_hide_dependency(self):
      self.check_graph(
         'set_property(TARGET Forge::forge_safe PROPERTY INTERFACE_LINK_LIBRARIES "$<LINK_ONLY:plain_middle>")\n'
         'set_property(TARGET plain_middle PROPERTY INTERFACE_LINK_LIBRARIES Forge::forge_db_object)', True)

   def test_module_only_dependency_is_rejected(self):
      self.check_graph(
         'set_property(TARGET Forge::forge_safe PROPERTY IMPORTED_CXX_MODULES_LINK_LIBRARIES Forge::forge_db_object)', True)

   def test_config_expression_cannot_hide_dependency(self):
      self.check_graph(
         'set_property(TARGET Forge::forge_safe PROPERTY INTERFACE_LINK_LIBRARIES "$<$<CONFIG:Release>:Forge::forge_db_object>")', True)

   def test_cycle_is_bounded(self):
      self.check_graph(
         'set_property(TARGET Forge::forge_safe PROPERTY INTERFACE_LINK_LIBRARIES plain_middle)\n'
         'set_property(TARGET plain_middle PROPERTY INTERFACE_LINK_LIBRARIES Forge::forge_safe)')


if __name__ == "__main__":
   unittest.main()
