"""Swarm's deterministic core must remain independent of its future runtime."""

from __future__ import annotations

import re
from pathlib import Path


def check_swarm_boundaries(root: Path, errors: list[str]) -> None:
   family = root / "libraries/swarm"
   dispatcher = family / "CMakeLists.txt"
   if not dispatcher.is_file():
      errors.append("libraries/swarm: family dispatcher is missing")
      return
   if re.search(r"add_library\s*\(\s*(?:Forge::)?forge_swarm(?:\s|\))", dispatcher.read_text()):
      errors.append("libraries/swarm: family root must not own an aggregate target")

   for source in family.rglob("*"):
      if not source.is_file() or source.suffix not in {".cpp", ".cppm", ".hxx", ".hpp"}:
         continue
      text = source.read_text()
      name = source.relative_to(root)
      if re.search(r"\bnamespace\s+forge::swarm\s*\{", text):
         errors.append(f"{name}: symbols must belong to a Swarm leaf namespace")
      if "core" not in source.relative_to(family).parts:
         continue
      if re.search(r"\bimport\s+forge\.(?:chain|db|api|asio|app|net|plugins|execution|vm)(?:[.;])", text):
         errors.append(f"{name}: deterministic Swarm core imports an upper layer")

   cmake = family / "core/CMakeLists.txt"
   if not cmake.is_file():
      errors.append("libraries/swarm/core: target definition is missing")
   elif re.search(r"\bforge_(?:chain|db|api|asio|app|net|plugins|execution|vm)(?:_|\b)", cmake.read_text()):
      errors.append("libraries/swarm/core: target depends on an upper layer")

   conversion = root / "libraries/core/include/forge/core/conversion.cppm"
   if not conversion.is_file():
      errors.append("Core conversion primary-template module is missing")
   else:
      text = conversion.read_text()
      if re.search(r"^\s*(?:export\s+)?import\b|^\s*#\s*include\b", text, re.MULTILINE):
         errors.append("Core conversion declaration must not acquire dependencies")
      if not re.search(r"template\s*<[^>]+>\s*class\s+conversion\s*;", text):
         errors.append("Core conversion must declare an incomplete primary template")
