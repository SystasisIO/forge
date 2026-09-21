#!/usr/bin/env python3

from __future__ import annotations

import re
import sys
from collections import defaultdict
from pathlib import Path


SOURCE_SUFFIXES = {".cpp", ".cppm", ".hpp", ".hxx"}
LAYOUT_ROOTS = ("libraries", "plugins", "guest/libraries")
SCAN_ROOTS = ("libraries", "plugins", "guest/libraries", "guest/tests", "tests", "tools")
EXCLUDED_PARTS = {".git", "legacy", "vendor", "__pycache__"}
MODULE_NAME = r"forge(?:\.[A-Za-z_][A-Za-z0-9_]*)+(?::[A-Za-z_][A-Za-z0-9_]*)?"
MODULE_DECLARATION = re.compile(rf"^\s*export\s+module\s+({MODULE_NAME})\s*;")
MODULE_UNIT = re.compile(rf"^\s*(?:export\s+)?module\s+({MODULE_NAME})\s*;")
MODULE_IMPORT = re.compile(rf"^\s*(?:export\s+)?import\s+({MODULE_NAME}|:[A-Za-z_][A-Za-z0-9_]*)\s*;")
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"][^>"]+[>"])')
BROAD_EXPORT = re.compile(r"^\s*export\s*\{")
CONDITIONAL_START = re.compile(r"^\s*#\s*(?:if|ifdef|ifndef)\b")
CONDITIONAL_BRANCH = re.compile(r"^\s*#\s*(?:elif|else)\b")
CONDITIONAL_END = re.compile(r"^\s*#\s*endif\b")
PRIVATE_DECLARATION = re.compile(r"^\s*(?:class|struct|enum(?:\s+class)?)\s+([A-Za-z_][A-Za-z0-9_:]*)")
VM_WASM_INTERPRET_EXPORT = re.compile(r"\bFORGE_VM_WASM_INTERPRET_EXPORT\b")
UNQUALIFIED_C_MEMORY = re.compile(r"(?<![:\w])(?:memcpy|memmove|memset|memcmp)\s*\(")
LEGACY_VM_WASM_SOURCE_IDENTITIES = (
   (re.compile(r"\bforge_vm_wasm(?!_interpret(?:_|\b))"), "legacy VM target identity"),
   (re.compile(r"(?<![/A-Za-z0-9_])vm_wasm(?!_interpret(?:_|\b))"), "legacy VM component identity"),
   (re.compile(r"\bForge::vm_wasm_softfloat_internal\b"), "legacy SoftFloat export identity"),
   (re.compile(r"forge/internal/vm_wasm/softfloat"), "legacy SoftFloat install path"),
   (re.compile(r"forge\.vm\.wasm(?!\.interpret(?:\b|:))"), "legacy VM module identity"),
   (re.compile(r"forge::vm::wasm(?!::interpret(?:\b|::))"), "legacy VM namespace identity"),
   (re.compile(r"forge/vm/wasm/(?!interpret/)"), "legacy VM public include identity"),
   (re.compile(r"\bFORGE_VM_WASM(?!_INTERPRET(?:_|\b))"), "legacy VM macro identity"),
)
LEGACY_VM_WASM_CMAKE_IDENTITIES = (
   (re.compile(r"\bforge_vm_wasm(?!_interpret(?:_|\b))"), "legacy VM target identity"),
   (re.compile(r"(?<![/A-Za-z0-9_])vm_wasm(?!_interpret(?:_|\b))"), "legacy VM component identity"),
   (re.compile(r"forge/internal/vm_wasm/softfloat"), "legacy SoftFloat install path"),
   (re.compile(r"libraries/vm/wasm/include"), "legacy VM include root"),
   (re.compile(r"\bFORGE_VM_WASM(?!_INTERPRET(?:_|\b))"), "legacy VM macro identity"),
)
LEGACY_CONTRACT_TOOLING_SOURCE_IDENTITIES = (
   (re.compile(r"\bforge_contract_(?:abi|attributes|validation|manifest|testing)\b"), "legacy Contract Tooling target identity"),
   (re.compile(r"forge\.contract\.(?:abi|attributes|validation|manifest|testing)(?:\b|:)"), "legacy Contract Tooling module identity"),
   (re.compile(r"forge::contract::(?:abi|attributes|validation|manifest|testing)(?:\b|::)"), "legacy Contract Tooling namespace identity"),
   (re.compile(r"forge/contract/(?:abi|attributes|validation|manifest|testing)(?:\b|/)"), "legacy Contract Tooling include identity"),
)
LEGACY_CONTRACT_TOOLING_CMAKE_IDENTITIES = (
   (re.compile(r"\bforge_contract_(?:abi|attributes|validation|manifest|testing)(?:\b|_)"), "legacy Contract Tooling target identity"),
   (re.compile(r"\bForge::forge_contract_(?:abi|attributes|validation|manifest|testing)\b"), "legacy Contract Tooling export identity"),
   (re.compile(r"libraries/contract/(?:abi|attributes|validation|manifest|testing)(?:\b|/)"), "legacy Contract Tooling library path"),
   (re.compile(r"tests/package_contract_(?:abi|attributes|validation|manifest|testing)_component"), "legacy Contract Tooling package test path"),
   (re.compile(r"\bFORGE_ENABLE_CONTRACT_TOOLING\b"), "legacy Contract Tooling option"),
   (re.compile(r"\bForge_BUILT_WITH_CONTRACT_TOOLING\b"), "legacy Contract Tooling package variable"),
)


def source_files(root: Path, roots: tuple[str, ...]) -> list[Path]:
   files: list[Path] = []
   for name in roots:
      base = root / name
      if not base.exists():
         continue
      for path in base.rglob("*"):
         if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
         relative = path.relative_to(root)
         if any(part in EXCLUDED_PARTS or part.startswith("build-") for part in relative.parts):
            continue
         files.append(path)
   return sorted(files)


def check_layout(root: Path, errors: list[str]) -> None:
   for path in source_files(root, LAYOUT_ROOTS):
      relative = path.relative_to(root)
      parts = relative.parts
      if path.suffix == ".hxx" and "details" not in parts:
         errors.append(f"{relative}: private .hxx must live under details/")
      if path.suffix == ".hpp" and "details" in parts:
         errors.append(f"{relative}: details/ headers must use .hxx")
      if path.suffix == ".cppm" and "include" not in parts:
         errors.append(f"{relative}: public .cppm must live under include/")
      if path.suffix == ".cpp" and ("include" in parts or "details" in parts):
         errors.append(f"{relative}: implementation .cpp must live at the library/plugin root")


def check_aggregates(root: Path, errors: list[str]) -> None:
   plugin_source = root / "plugins" / "plugins.cpp"
   if plugin_source.exists():
      errors.append("plugins/plugins.cpp: code-less plugin aggregate must not own a source")

   cmake = (root / "plugins" / "CMakeLists.txt").read_text()
   if not re.search(r"add_library\s*\(\s*forge_plugins\s+INTERFACE\s*\)", cmake):
      errors.append("plugins/CMakeLists.txt: forge_plugins must be an INTERFACE target")

   anchor = re.compile(r"\b(?:aggregate|dummy)_anchor\b")
   for path in source_files(root, ("libraries", "plugins", "tests")):
      for line_number, line in enumerate(path.read_text(errors="ignore").splitlines(), 1):
         if anchor.search(line):
            errors.append(f"{path.relative_to(root)}:{line_number}: dummy anchor symbol is forbidden")

   comments = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)
   module_line = re.compile(rf"^\s*(?:export\s+)?module(?:\s+{MODULE_NAME})?\s*;\s*$", re.MULTILINE)
   import_line = re.compile(rf"^\s*(?:export\s+)?import\s+(?:{MODULE_NAME}|:[A-Za-z_]\w*)\s*;\s*$", re.MULTILINE)
   include_line = re.compile(r"^\s*#\s*include\s*[<\"][^>\"]+[>\"]\s*$", re.MULTILINE)
   for path in source_files(root, LAYOUT_ROOTS):
      if path.suffix != ".cppm":
         continue
      source = comments.sub("", path.read_text(errors="ignore"))
      if import_line.search(source) is None:
         continue
      remainder = include_line.sub("", import_line.sub("", module_line.sub("", source))).strip()
      if not remainder:
         errors.append(f"{path.relative_to(root)}: aggregate-only module is forbidden")


def check_p2p_scoped_peer_mutations(root: Path, errors: list[str]) -> None:
   path = root / "libraries/net/p2p/relay_discovery.cpp"
   source = path.read_text(errors="ignore")
   for forbidden in ("store.find(", "store.upsert("):
      if forbidden in source:
         errors.append(
            f"{path.relative_to(root)}: relay maintenance must use scoped peer-store mutations, not {forbidden}"
         )
   for required in ("store.mark_discovery_failure(", "store.prune_expired_relay_reservations("):
      if required not in source:
         errors.append(
            f"{path.relative_to(root)}: relay maintenance is missing scoped mutation {required}"
         )


def component_roots(root: Path) -> list[Path]:
   roots: list[Path] = []
   for top in LAYOUT_ROOTS:
      for cmake in (root / top).rglob("CMakeLists.txt"):
         component = cmake.parent
         if any(component.glob("*.cpp")):
            roots.append(component)
   return sorted(roots)


def matching_headers(component: Path, stem: str) -> list[Path]:
   matches: list[Path] = []
   include = component / "include"
   if include.exists():
      matches.extend(include.rglob(f"{stem}.cppm"))
   private = component / "details" / f"{stem}.hxx"
   if private.exists():
      matches.append(private)
   return sorted(matches)


def check_pairing(root: Path, errors: list[str]) -> None:
   for component in component_roots(root):
      sources = {path.stem: path for path in component.glob("*.cpp")}
      headers = {stem: matching_headers(component, stem) for stem in sources}

      for stem, source in sorted(sources.items()):
         relative = source.relative_to(root)
         direct = headers[stem]
         if len(direct) == 1:
            continue
         if len(direct) > 1:
            owners = ", ".join(str(path.relative_to(root)) for path in direct)
            errors.append(f"{relative}: implementation has multiple exact owners: {owners}")
            continue

         aspect_owners = [
            owner
            for owner in sources
            if stem.startswith(f"{owner}_") and len(headers[owner]) == 1
         ]
         if aspect_owners:
            continue
         errors.append(
            f"{relative}: implementation needs an exact {stem}.cppm/{stem}.hxx owner "
            "or a paired X.cpp for an X_<aspect>.cpp source"
         )


def check_tls_context_ownership(root: Path, errors: list[str]) -> None:
   tls_root = root / "libraries" / "net" / "tls"
   required = (
      tls_root / "CMakeLists.txt",
      tls_root / "context.cpp",
      tls_root / "include" / "forge" / "net" / "tls" / "options.cppm",
      tls_root / "include" / "forge" / "net" / "tls" / "context.cppm",
      tls_root / "include" / "forge" / "net" / "tls" / "exceptions.cppm",
   )
   for path in required:
      if not path.exists():
         errors.append(f"{path.relative_to(root)}: forge_net_tls ownership file is required")

   context_source = (tls_root / "context.cpp").read_text(errors="ignore")
   for required_token in (
      "SSL_CTX_set_min_proto_version",
      "SSL_CTX_check_private_key",
      "X509_STORE_add_cert",
      "SSL_CTX_add_client_CA",
      "parse_trust_anchor_bundle",
      "OPENSSL_cleanse",
      "SSL_CTX_set_alpn_select_cb",
      "client_alpn_wire",
      "require_peer_certificate",
      "SSL_set_tlsext_host_name",
      "SSL_get1_peer_certificate",
      "X509_check_host",
      "validate_peer",
      "context_provider::replace",
   ):
      if required_token not in context_source:
         errors.append(f"libraries/net/tls/context.cpp: missing TLS context invariant {required_token}")

   duplicated_context_setup = (
      "asio::ssl::context::tls_client",
      "asio::ssl::context::tls_server",
      ".use_certificate_chain(",
      ".use_private_key(",
      ".add_certificate_authority(",
      ".set_default_verify_paths(",
      "SSL_CTX_set_min_proto_version(",
      "SSL_CTX_set_max_proto_version(",
      "SSL_CTX_set_alpn_select_cb(",
   )
   for relative in ("libraries/net/stcp/connection.cpp", "libraries/net/http/connection.cpp"):
      source = (root / relative).read_text(errors="ignore")
      for token in duplicated_context_setup:
         if token in source:
            errors.append(f"{relative}: TLS context setup belongs to forge_net_tls ({token})")

   expected_dependencies = {
      "libraries/net/stcp/CMakeLists.txt": "forge_net_tls",
      "libraries/net/http/CMakeLists.txt": "forge_net_tls",
      "libraries/net/websocket/CMakeLists.txt": "forge_net_tls",
   }
   for relative, dependency in expected_dependencies.items():
      if dependency not in (root / relative).read_text(errors="ignore"):
         errors.append(f"{relative}: TLS consumer must link {dependency}")

   if "forge_crypto_pki" not in (tls_root / "CMakeLists.txt").read_text(errors="ignore"):
      errors.append("libraries/net/tls/CMakeLists.txt: TLS peer extraction must link forge_crypto_pki")

   http_server_source = (root / "libraries/net/http/server.cpp").read_text(errors="ignore")
   for token in (
      "template <typename Stream>",
      "server_session_base",
      "tls_context_provider->snapshot()",
      "async_handshake(asio::ssl::stream_base::server",
      "max_pending_tls_handshakes",
      "reserve_tls_handshake",
      "release_tls_handshake",
      "cancel_pending_tls_handshakes",
      "wait_until_tls_handshakes_complete",
      "stream_.async_read_some",
      "tls_snapshot_",
      "disarm_stream_expiry",
      "close_after_response",
      "stream.async_shutdown(asio::as_tuple",
      "cancel_tls_shutdown_at_deadline",
      "run_tls_shutdown(stream) || cancel_tls_shutdown_at_deadline(stream, deadline)",
   ):
      if token not in http_server_source:
         errors.append(f"libraries/net/http/server.cpp: TLS server invariant is missing ({token})")
   if "stream_.socket().async_read_some" in http_server_source:
      errors.append("libraries/net/http/server.cpp: disconnect monitor must read the outer HTTP/TLS stream")
   if "async_wait([&stream" in http_server_source:
      errors.append("libraries/net/http/server.cpp: TLS shutdown deadline must not outlive the stream by reference")

   http_server_module = (root / "libraries/net/http/include/forge/net/http/server.cppm").read_text(errors="ignore")
   for token in ("forge.net.tls.context", "tls_context_provider", "handshake_timeout", "max_pending_tls_handshakes"):
      if token not in http_server_module:
         errors.append(f"libraries/net/http/include/forge/net/http/server.cppm: TLS server option is missing ({token})")

   websocket_module = (root / "libraries/net/websocket/include/forge/net/websocket/connection.cppm").read_text(errors="ignore")
   websocket_source = (root / "libraries/net/websocket/connection.cpp").read_text(errors="ignore")
   for path, source, token in (
      ("libraries/net/websocket/include/forge/net/websocket/connection.cppm", websocket_module,
       "context_snapshot_ptr tls_context_snapshot"),
      ("libraries/net/websocket/connection.cpp", websocket_source, "tls_context_snapshot"),
   ):
      if token not in source:
         errors.append(f"{path}: TLS WebSocket handoff must retain the accepted context snapshot")

   plugin_types = (root / "plugins/http/server/include/forge/plugins/http/server/types.cppm").read_text(errors="ignore")
   plugin_config = (root / "plugins/http/server/config.cpp").read_text(errors="ignore")
   plugin_source = (root / "plugins/http/server/plugin.cpp").read_text(errors="ignore")
   plugin_impl = (root / "plugins/http/server/plugin_impl.cpp").read_text(errors="ignore")
   tls_secret_material_header = (
      root / "plugins/http/server/details/tls_secret_material.hxx").read_text(errors="ignore")
   tls_secret_material_source = (root / "plugins/http/server/tls_secret_material.cpp").read_text(errors="ignore")
   plugin_api = (root / "plugins/http/server/include/forge/plugins/http/server/api.cppm").read_text(errors="ignore")
   plugin_impl_header = (root / "plugins/http/server/details/plugin_impl.hxx").read_text(errors="ignore")
   for path, source, tokens in (
      ("plugins/http/server/include/forge/plugins/http/server/types.cppm", plugin_types,
       ("tls.mode", "tls.certificate-chain-secret", "tls.private-key-secret", "tls.client-ca-secret",
        "tls.handshake-timeout-ms", "tls.max-pending-handshakes")),
      ("plugins/http/server/config.cpp", plugin_config,
       ("address.is_loopback()", 'value.bind_address == "localhost"', "validate_tls_config")),
      ("plugins/http/server/plugin.cpp", plugin_source,
       ("settings.tls_mode_value != tls_mode::disabled", "make_tls_context_provider", "lifecycle_generation")),
      ("plugins/http/server/plugin_impl.cpp", plugin_impl,
       ("http.server.tls.certificate-chain", "http.server.tls.private-key", "http.server.tls.client-ca",
        "provider->replace(std::move(replacement))", "lifecycle_generation != reload_generation",
        '"details/tls_secret_material.hxx"', "tls_secret_material", "clear_tls_context_options", "secure_erase")),
      ("plugins/http/server/details/tls_secret_material.hxx", tls_secret_material_header,
       ("struct tls_secret_material", "~tls_secret_material();",
        "operator=(tls_secret_material&& other) noexcept;")),
      ("plugins/http/server/tls_secret_material.cpp", tls_secret_material_source,
       ("tls_secret_material::~tls_secret_material()", "tls_secret_material::operator=(tls_secret_material&& other)",
        "secure_erase")),
      ("plugins/http/server/details/plugin_impl.hxx", plugin_impl_header,
       ("std::shared_ptr<forge::plugins::crypto::secrets::api>", "lifecycle_generation")),
      ("plugins/http/server/include/forge/plugins/http/server/api.cppm", plugin_api,
       ("reload_tls", 'FORGE_API_CONTRACT("forge.plugins.http.server", 2, 0)')),
   ):
      for token in tokens:
         if token not in source:
            errors.append(f"{path}: HTTP Server plugin TLS invariant is missing ({token})")

   plugin_cmake = (root / "plugins/http/server/CMakeLists.txt").read_text(errors="ignore")
   if "forge_plugins_crypto_secrets" not in plugin_cmake:
      errors.append("plugins/http/server/CMakeLists.txt: TLS-enabled HTTP Server plugin must link Crypto Secrets")
   if "tls_secret_material.cpp" not in plugin_cmake:
      errors.append("plugins/http/server/CMakeLists.txt: private TLS secret material implementation is not compiled")
   if "struct tls_secret_material" in plugin_impl:
      errors.append("plugins/http/server/plugin_impl.cpp: private TLS secret material needs its exact details pair")
   if "load_tls_context_options" in plugin_impl or "co_return forge::net::tls::context_options" in plugin_impl:
      errors.append("plugins/http/server/plugin_impl.cpp: TLS secret material must not escape in context_options")

   tls_http_tests = (root / "tests/tls/http_server_tests.cpp").read_text(errors="ignore")
   for token in (
      "http_server_accepts_tls_1_3_before_parsing_http",
      "http_server_mutual_tls_verifies_client_chain",
      "http_server_tls_has_no_plaintext_fallback_and_bounds_pending_handshakes",
      "http_server_tls_rotation_keeps_established_http_sessions_usable",
      "http_server_shutdown_cancels_pending_tls_handshakes_without_waiting_for_close_notify",
      "http_server_tls_normal_close_sends_close_notify",
      "http_server_tls_close_joins_deadline_handlers_before_session_release",
      "http_server_tls_reciprocates_client_close_notify_while_keep_alive_idle",
      "http_server_tls_reciprocates_client_close_notify_before_first_request",
      "http_server_tls_websocket_handoff_retains_the_connection_snapshot",
      "http_server_tls_disconnect_cancels_streaming_body_owner",
      "http_server_mutual_tls_loads_every_certificate_from_trust_anchor_bundle",
      "tls_context_rejects_malformed_trailing_trust_anchor_bundle",
   ):
      if token not in tls_http_tests:
         errors.append(f"tests/tls/http_server_tests.cpp: missing HTTP TLS regression ({token})")

   plugin_tests = (root / "tests/plugins/plugins_tests.cpp").read_text(errors="ignore")
   for token in (
      "http_server_plugin_tls_reload_preserves_live_context_and_cannot_publish_after_shutdown",
      "http_server_plugin_tls_reload_rejects_malformed_material_and_keeps_live_context",
   ):
      if token not in plugin_tests:
         errors.append(f"tests/plugins/plugins_tests.cpp: missing TLS reload regression ({token})")

   http_source = (root / "libraries/net/http/connection.cpp").read_text(errors="ignore")
   for token in (
      "tls::context_provider",
      "tls_context_provider.snapshot()",
      "tls::protocol_policy::system_default",
      "tls::make_beast_stream",
      "tls::configure_client_stream",
      "tls::validate_peer",
   ):
      if token not in http_source:
         errors.append(f"libraries/net/http/connection.cpp: HTTPS TLS integration is incomplete ({token})")

   stcp_interface = (root / "libraries/net/stcp/include/forge/net/stcp/connection.cppm").read_text(errors="ignore")
   if "export import forge.net.tls.context" in stcp_interface:
      errors.append("libraries/net/stcp/include/forge/net/stcp/connection.cppm: TLS is not an STCP public module")

   stcp_source = (root / "libraries/net/stcp/connection.cpp").read_text(errors="ignore")
   for token in (
      "tls::make_asio_stream",
      "tls::configure_client_stream",
      "tls::validate_peer",
      "tls::extract_peer_certificate_chain",
      "tls::selected_alpn",
   ):
      if token not in stcp_source:
         errors.append(f"libraries/net/stcp/connection.cpp: TLS delegation is incomplete ({token})")

   for relative in ("libraries/net/stcp/connection.cpp", "libraries/net/http/connection.cpp"):
      source = (root / relative).read_text(errors="ignore")
      for token in (
         "native_context(",
         "SSL_set_tlsext_host_name",
         "SSL_set_alpn_protos",
         "SSL_get0_alpn_selected",
         "SSL_get1_peer_certificate",
         "SSL_get_peer_cert_chain",
         "SSL_get_verify_result",
         "X509_check_host",
         "X509_check_ip_asc",
         "host_name_verification",
      ):
         if token in source:
            errors.append(f"{relative}: TLS session mechanics belong to forge_net_tls ({token})")

   package_components = {
      "tests/package_net_tls_component/CMakeLists.txt": ("net_tls",),
      "tests/package_net_stcp_component/CMakeLists.txt": ("net_stcp",),
      "tests/package_net_http_component/CMakeLists.txt": ("asio", "net_http"),
   }
   for relative, components in package_components.items():
      source = (root / relative).read_text(errors="ignore")
      package = re.search(r"find_package\s*\(\s*Forge\b(?P<body>.*?)\)", source, re.DOTALL)
      requested = set(re.findall(r"\b[A-Za-z][A-Za-z0-9_]*\b", package.group("body"))) if package else set()
      for component in components:
         if component not in requested:
            errors.append(f"{relative}: relocated package consumer must request {component}")

   if "tls::context_provider" not in http_source or "tls_context_provider.snapshot()" not in http_source:
      errors.append("libraries/net/http/connection.cpp: HTTPS client must acquire forge_net_tls snapshots")

   tls_context_module = (tls_root / "include" / "forge" / "net" / "tls" / "context.cppm").read_text(errors="ignore")
   tls_context_source = (tls_root / "context.cpp").read_text(errors="ignore")
   for token in (
      "make_asio_stream",
      "make_beast_stream",
      "context_for_stream",
      "context_snapshot_ptr snapshot",
   ):
      if token not in tls_context_module:
         errors.append(f"libraries/net/tls/include/forge/net/tls/context.cppm: immutable stream factory is incomplete ({token})")
   if "SSL_new(" in tls_context_source or "new_native_stream_handle" in tls_context_module:
      errors.append("forge_net_tls: TLS stream factory must not transfer raw SSL handle ownership")
   if "make_stream" in tls_context_module:
      errors.append("forge_net_tls: TLS stream factory must not accept arbitrary stream types")
   private_context_snapshot = tls_context_module.partition("private:")[2]
   if "context_for_stream" not in private_context_snapshot:
      errors.append("libraries/net/tls/include/forge/net/tls/context.cppm: TLS context access must remain private")

   tls_tests = (root / "tests" / "tls" / "tls_tests.cpp").read_text(errors="ignore")
   for token in ("stream_factory_creates_distinct_connection_ssl_handles", "make_asio_stream"):
      if token not in tls_tests:
         errors.append(f"tests/tls/tls_tests.cpp: TLS stream immutability regression coverage is incomplete ({token})")

   tls_package_main = (root / "tests" / "package_net_tls_component" / "main.cpp").read_text(errors="ignore")
   if "import forge.net.tls.exceptions;" not in tls_package_main:
      errors.append("tests/package_net_tls_component/main.cpp: net_tls package consumer must import TLS exceptions directly")

   root_cmake = (root / "CMakeLists.txt").read_text(errors="ignore")
   for token in ("libraries/net/tls/include", "forge_net_tls", "net_tls"):
      if token not in root_cmake:
         errors.append(f"CMakeLists.txt: forge_net_tls package registration is incomplete ({token})")

   package_config = (root / "cmake" / "ForgeConfig.cmake.in").read_text(errors="ignore")
   for token in ("net_tls", '_forge_add_component(net_tls)', '"net_tls" IN_LIST _FORGE_COMPONENTS'):
      if token not in package_config:
         errors.append(f"cmake/ForgeConfig.cmake.in: forge_net_tls package registration is incomplete ({token})")
   if 'elseif("${component}" STREQUAL "net_tls")\n         _forge_add_component(exceptions)\n         _forge_add_component(crypto_pki)' not in package_config:
      errors.append("cmake/ForgeConfig.cmake.in: net_tls package component must resolve crypto_pki")
   if 'elseif("${component}" STREQUAL "net_websocket")\n         _forge_add_component(exceptions)\n         _forge_add_component(asio)\n         _forge_add_component(net_tls)' not in package_config:
      errors.append("cmake/ForgeConfig.cmake.in: net_websocket package component must resolve net_tls")
   if 'elseif("${component}" STREQUAL "plugins_http_server")' not in package_config or \
      '_forge_add_component(plugins_crypto_secrets)' not in package_config.partition('elseif("${component}" STREQUAL "plugins_http_server")')[2].partition('elseif("${component}" STREQUAL "plugins_log_otlp")')[0]:
      errors.append("cmake/ForgeConfig.cmake.in: plugins_http_server package component must resolve Crypto Secrets")
   if 'elseif("${component}" STREQUAL "plugins_log_otlp")' not in package_config or \
      '_forge_add_component(plugins_crypto_secrets)' not in package_config.partition('elseif("${component}" STREQUAL "plugins_log_otlp")')[2].partition('elseif("${component}" STREQUAL "plugins_db_store")')[0]:
      errors.append("cmake/ForgeConfig.cmake.in: plugins_log_otlp package component must resolve Crypto Secrets")

   otlp_plugin_cmake = (root / "plugins/log/otlp/CMakeLists.txt").read_text(errors="ignore")
   if "forge_plugins_crypto_secrets" not in otlp_plugin_cmake:
      errors.append("plugins/log/otlp/CMakeLists.txt: OTLP secret headers require Crypto Secrets")


def check_http_cookie_asset_boundaries(root: Path, errors: list[str]) -> None:
   required = (
      "libraries/net/http/cookie.cpp",
      "libraries/net/http/assets.cpp",
      "libraries/net/http/include/forge/net/http/cookie.cppm",
      "libraries/net/http/include/forge/net/http/assets.cppm",
   )
   for relative in required:
      if not (root / relative).exists():
         errors.append(f"{relative}: HTTP cookie/assets ownership file is required")

   http_cmake = (root / "libraries/net/http/CMakeLists.txt").read_text(errors="ignore")
   for token in ("cookie.cpp", "assets.cpp", "forge_target_modules_at(forge_net_http net/http)"):
      if token not in http_cmake:
         errors.append(f"libraries/net/http/CMakeLists.txt: HTTP cookie/assets target registration is incomplete ({token})")

   cookie_module = (root / "libraries/net/http/include/forge/net/http/cookie.cppm").read_text(errors="ignore")
   cookie_source = (root / "libraries/net/http/cookie.cpp").read_text(errors="ignore")
   for token in (
      "export module forge.net.http.cookie",
      "parse_cookie_header",
      "format_cookie_header",
      "parse_set_cookie_header",
      "format_set_cookie",
      "append_set_cookie",
      "same_site",
   ):
      if token not in cookie_module:
         errors.append(f"libraries/net/http/include/forge/net/http/cookie.cppm: cookie API is incomplete ({token})")
   for token in ("reject_controls", "validate_domain", "label_ends_hyphen", "SameSite=None requires Secure",
                 "cookie SameSite value is invalid"):
      if token not in cookie_source:
         errors.append(f"libraries/net/http/cookie.cpp: strict cookie validation is incomplete ({token})")

   types_module = (root / "libraries/net/http/include/forge/net/http/types.cppm").read_text(errors="ignore")
   types_source = (root / "libraries/net/http/types.cpp").read_text(errors="ignore")
   if "set_cookie" in types_module or "response::set_cookie" in types_source:
      errors.append("forge_net_http: raw response::set_cookie must not bypass forge.net.http.cookie")

   asset_module = (root / "libraries/net/http/include/forge/net/http/assets.cppm").read_text(errors="ignore")
   asset_source = (root / "libraries/net/http/assets.cpp").read_text(errors="ignore")
   for token in ("export module forge.net.http.assets", "asset_mount", "asset_bundle",
                 "forge::asio::compute::executor read_executor", "max_file_bytes", "max_path_bytes"):
      if token not in asset_module:
         errors.append(f"libraries/net/http/include/forge/net/http/assets.cppm: asset API is incomplete ({token})")
   for token in (
      "percent_escaped",
      "character == '\\\\'",
      "percent_escaped && character == '/'",
      ".symlinks = symlink_policy::reject",
      "static_file_root",
      "spa_fallback",
      "public, max-age=31536000, immutable",
   ):
      if token not in asset_source:
         errors.append(f"libraries/net/http/assets.cpp: asset safety/cache invariant is missing ({token})")

   file_module = (root / "libraries/net/http/include/forge/net/http/file.cppm").read_text(errors="ignore")
   file_source = (root / "libraries/net/http/file.cpp").read_text(errors="ignore")
   for token in ("symlink_policy", "symlinks = symlink_policy::reject", "forge::asio::compute::executor read_executor"):
      if token not in file_module:
         errors.append(f"libraries/net/http/include/forge/net/http/file.cppm: public symlink policy is missing ({token})")
   for token in ("openat(", "O_NOFOLLOW", "O_NONBLOCK", "fstat(", "pread(", "open_relative_file",
                 "open_followed_relative_file", "resolved.lexically_relative(root)", "if_none_match_matches",
                 "if_range_matches", "execute_file_io", '"http-file-open"', '"http-static-file-open"',
                 '"http-file-pread"'):
      if token not in file_source:
         errors.append(f"libraries/net/http/file.cpp: descriptor-relative static file invariant is missing ({token})")
   if "open_file(resolved" in file_source:
      errors.append("libraries/net/http/file.cpp: followed static files must reopen through the stable root descriptor")
   if "if_range" not in types_module or 'return "If-Range"' not in types_source:
      errors.append("forge_net_http: If-Range field mapping is missing")

   router_module = (root / "libraries/net/http/include/forge/net/http/router.cppm").read_text(errors="ignore")
   router_source = (root / "libraries/net/http/router.cpp").read_text(errors="ignore")
   for token in ("mount_assets(asset_mount value, forge::asio::compute::executor read_executor)",
                 "reserve_path_prefix", "asset_mounts_", "reserved_path_prefixes_"):
      if token not in router_module:
         errors.append(f"libraries/net/http/include/forge/net/http/router.cppm: asset router boundary is incomplete ({token})")
   for token in (
      "asset_mounts_overlap",
      "reserved API path prefix",
      "reserved HTTP path prefix overlaps an asset mount",
      "path_exists(routes_",
   ):
      if token not in router_source:
         errors.append(f"libraries/net/http/router.cpp: asset/API priority invariant is missing ({token})")
   router_preflight = (root / "libraries/net/http/router_server_access.cpp").read_text(errors="ignore")
   for token in ("mount.serves", "mount.contains", "status::method_not_allowed"):
      if token not in router_preflight:
         errors.append(f"libraries/net/http/router_server_access.cpp: asset preflight handling is incomplete ({token})")

   plugin_api = (root / "plugins/http/server/include/forge/plugins/http/server/api.cppm").read_text(errors="ignore")
   plugin_source = (root / "plugins/http/server/plugin.cpp").read_text(errors="ignore")
   plugin_impl = (root / "plugins/http/server/plugin_impl.cpp").read_text(errors="ignore")
   for token in ("mount_assets", "forge.net.http.assets"):
      if token not in plugin_api:
         errors.append(f"plugins/http/server/include/forge/plugins/http/server/api.cppm: typed asset mount is missing ({token})")
   if "result.insert" not in plugin_source or '"Set-Cookie"' not in plugin_source:
      errors.append("plugins/http/server/plugin.cpp: repeated Set-Cookie projection must use insert")
   if "HTTP asset mounts must not overlap" not in plugin_impl:
      errors.append("plugins/http/server/plugin_impl.cpp: plugin asset overlap validation is missing")
   for token in ("context.has_compute()", "context.compute()", "file_read_executor"):
      if token not in plugin_source:
         errors.append(f"plugins/http/server/plugin.cpp: asset compute ownership is incomplete ({token})")
   for token in ("asset_bundle{std::move(value), std::move(executor)}",
                 "HTTP asset mounts require the application compute executor"):
      if token not in plugin_impl:
         errors.append(f"plugins/http/server/plugin_impl.cpp: asset compute injection is incomplete ({token})")

   package_http = (root / "tests/package_net_http_component/main.cpp").read_text(errors="ignore")
   package_plugin = (root / "tests/package_plugins_http_server/main.cpp").read_text(errors="ignore")
   for token in ("import forge.net.http.cookie;", "import forge.net.http.assets;"):
      if token not in package_http:
         errors.append(f"tests/package_net_http_component/main.cpp: direct package import is missing ({token})")
   if "import forge.net.http.assets;" not in package_plugin:
      errors.append("tests/package_plugins_http_server/main.cpp: plugin package consumer must import asset mount directly")
   if "import forge.asio.compute;" not in package_http:
      errors.append("tests/package_net_http_component/main.cpp: low-level file package consumer must import compute")

   http_tests = (root / "tests/http_websocket/network_tests.cpp").read_text(errors="ignore")
   for token in (
      "http_cookie_rejects_malformed_and_injectable_headers",
      "foo-.example",
      "/admin/%2fsecret",
      "/admin/%2Fsecret",
      "/admin/%5csecret",
      "http_asset_mount_serves_streaming_get_head_ranges_and_cache_policy",
      "http_static_file_root_honors_conditional_and_if_range_semantics",
      "http_static_file_options_preserve_legacy_aggregate_and_gate_conditionals",
      "http_static_file_root_and_file_response_follow_contained_symlinks_when_explicitly_enabled",
      "http_static_file_root_keeps_root_descriptor_across_root_name_swap",
      "http_static_file_root_keeps_opened_descriptor_across_name_swap",
      "http_static_file_root_rejects_fifo_without_blocking",
      "http_file_reads_use_bounded_compute_capacity_without_stalling_runtime_workers",
      "http_request_time_file_open_is_bounded_and_does_not_stall_runtime_workers",
   ):
      if token not in http_tests:
         errors.append(f"tests/http_websocket/network_tests.cpp: cookie/assets regression is missing ({token})")
   plugin_tests = (root / "tests/plugins/plugins_tests.cpp").read_text(errors="ignore")
   for token in (
      "http_server_plugin_preserves_repeated_set_cookie_from_middleware",
      "http_server_plugin_mounts_assets_without_shadowing_a_narrow_api_prefix",
      "http_server_plugin_rejects_asset_publication_without_application_compute",
      "http_server_plugin_rejects_root_api_prefix_overlapping_asset_mount_before_listener_start",
   ):
      if token not in plugin_tests:
         errors.append(f"tests/plugins/plugins_tests.cpp: plugin cookie/assets regression is missing ({token})")

   migration = (root / "docs/iterations/forge-native-admin-foundation-v1.md").read_text(errors="ignore")
   for token in ("response.set_cookie(name, value)", "append_set_cookie(", "Stable source surface",
                 "release notes must name `response::set_cookie`"):
      if token not in migration:
         errors.append(f"docs/iterations/forge-native-admin-foundation-v1.md: HTTP cookie migration is incomplete ({token})")


def check_auth_pairing_boundaries(root: Path, errors: list[str]) -> None:
   family = root / "libraries" / "auth"
   leaf = family / "pairing"
   required = (
      family / "CMakeLists.txt",
      leaf / "CMakeLists.txt",
      leaf / "pairing.cpp",
      leaf / "README.md",
      leaf / "include" / "forge" / "auth" / "pairing" / "exceptions.cppm",
      leaf / "include" / "forge" / "auth" / "pairing" / "types.cppm",
      leaf / "include" / "forge" / "auth" / "pairing" / "pairing.cppm",
      root / "tests" / "auth" / "pairing_tests.cpp",
      root / "tests" / "package_auth_pairing_component" / "CMakeLists.txt",
      root / "tests" / "package_auth_pairing_component" / "main.cpp",
   )
   for path in required:
      if not path.exists():
         errors.append(f"{path.relative_to(root)}: forge_auth_pairing ownership file is required")

   family_cmake = (family / "CMakeLists.txt").read_text(errors="ignore")
   if "add_subdirectory(pairing)" not in family_cmake:
      errors.append("libraries/auth/CMakeLists.txt: auth family must register pairing leaf")
   if re.search(r"add_library\s*\(\s*forge_auth\s", family_cmake):
      errors.append("libraries/auth/CMakeLists.txt: empty auth family root must not define forge_auth aggregate")

   leaf_cmake = (leaf / "CMakeLists.txt").read_text(errors="ignore")
   for token in (
      "add_library(forge_auth_pairing STATIC pairing.cpp)",
      "forge_target_modules_at(forge_auth_pairing auth/pairing)",
      "forge_codec_base64",
      "forge_crypto_core",
      "forge_crypto_digest",
      "forge_exceptions",
   ):
      if token not in leaf_cmake:
         errors.append(f"libraries/auth/pairing/CMakeLists.txt: pairing target is missing {token}")

   exceptions_module = (leaf / "include" / "forge" / "auth" / "pairing" / "exceptions.cppm").read_text(errors="ignore")
   for token in (
      "export module forge.auth.pairing.exceptions",
      "token_invalid",
      "replayed",
      "capacity_exceeded",
      "generation_exhausted",
      "credential_id_invalid",
      "token_collision",
   ):
      if token not in exceptions_module:
         errors.append(f"libraries/auth/pairing/include/forge/auth/pairing/exceptions.cppm: pairing exception is missing {token}")

   types_module = (leaf / "include" / "forge" / "auth" / "pairing" / "types.cppm").read_text(errors="ignore")
   for token in (
      "export module forge.auth.pairing.types",
      "token_digest",
      "bootstrap_record",
      "pending_request",
      "pending_issuance",
      "pre_session_digest",
      "pre_session_consumed",
      "credential_binding",
      "approved_credential",
      "credential_id",
      "approval_options",
      "credential",
      "trusted, non-decreasing wall-clock",
   ):
      if token not in types_module:
         errors.append(f"libraries/auth/pairing/include/forge/auth/pairing/types.cppm: pairing record is missing {token}")
   bootstrap_record_match = re.search(r"struct bootstrap_record \{(.*?)\n\};", types_module, re.DOTALL)
   if bootstrap_record_match is not None and "secret_string" in bootstrap_record_match.group(1):
      errors.append("libraries/auth/pairing/include/forge/auth/pairing/types.cppm: persisted bootstrap record must not contain a clear token")
   pending_record_match = re.search(r"struct pending_request \{(.*?)\n\};", types_module, re.DOTALL)
   if pending_record_match is not None and "secret_string" in pending_record_match.group(1):
      errors.append("libraries/auth/pairing/include/forge/auth/pairing/types.cppm: persisted pending record must not contain a clear pre-session token")

   pairing_module = (leaf / "include" / "forge" / "auth" / "pairing" / "pairing.cppm").read_text(errors="ignore")
   for token in (
      "export module forge.auth.pairing.pairing",
      "begin_bootstrap",
      "identify_bootstrap_token",
      "consume_bootstrap",
      "supersede_pending",
      "identify_pre_session",
      "validate_pre_session",
      "consume_approved_pre_session",
      "credential_binding",
      "approve_pending",
      "approval_options",
      "rotate_credential_downscope",
      "revoke_credential",
   ):
      if token not in pairing_module:
         errors.append(f"libraries/auth/pairing/include/forge/auth/pairing/pairing.cppm: pairing transition is missing {token}")

   pairing_source = (leaf / "pairing.cpp").read_text(errors="ignore")
   for token in (
      "random_bytes(bootstrap_token_bytes)",
      "padding::omit",
      "padding_policy::forbid",
      "constant_time_equal",
      "byte_view(token.view())",
      "bootstrap creation",
      "pending request creation",
      "out-of-range resolution time",
      "invalid revocation time",
      "bootstrap.consumed = true",
      "pre_session_digest",
      "pre_session_consumed",
      "issue_distinct_token",
      "identify_bootstrap_token",
      "identify_pre_session",
      "consume_approved_pre_session",
      "approved_credential",
      "require_pre_session_lifecycle",
      "require_credential_id",
      "scope_baseline",
      "credential rotation cannot escalate scopes",
   ):
      if token not in pairing_source:
         errors.append(f"libraries/auth/pairing/pairing.cpp: pairing security invariant is missing {token}")
   for forbidden in ("forge.db", "forge.net", "forge.plugins", "objectdb", "mdbx"):
      if forbidden in pairing_source or forbidden in pairing_module or forbidden in types_module:
         errors.append(f"libraries/auth/pairing: product integration dependency is forbidden ({forbidden})")

   consume_source = pairing_source.partition("pending_issuance consume_bootstrap")[2].partition(
      "pending_issuance supersede_pending")[0]
   supersede_source = pairing_source.partition("pending_issuance supersede_pending")[2].partition(
      "token_digest identify_pre_session")[0]
   if ("auto result = pending_issuance" not in consume_source or
       consume_source.find("auto result = pending_issuance") > consume_source.find("bootstrap.consumed = true") or
       "return result;" not in consume_source):
      errors.append("libraries/auth/pairing/pairing.cpp: consume must construct its pre-session issuance before consuming bootstrap")
   if ("auto result = pending_issuance" not in supersede_source or
       supersede_source.find("auto result = pending_issuance") > supersede_source.find("pending.state = pending_state::superseded") or
       "return result;" not in supersede_source):
      errors.append("libraries/auth/pairing/pairing.cpp: supersede must construct its pre-session issuance before mutating pending")
   approved_consume_source = pairing_source.partition("credential_binding consume_approved_pre_session")[2].partition(
      "credential approve_pending")[0]
   if ("require_pending_record" not in approved_consume_source or
       "verify_token(pending.pre_session_digest, pre_session_token)" not in approved_consume_source or
       "require_pre_session_lifecycle" not in approved_consume_source or
       "pending.state != pending_state::approved" not in approved_consume_source or
       "auto result = *pending.approved_credential;" not in approved_consume_source or
       approved_consume_source.find("auto result = *pending.approved_credential;") >
       approved_consume_source.find("pending.pre_session_consumed = true") or
       approved_consume_source.find("pending.pre_session_consumed = true") <
       approved_consume_source.find("pending.state != pending_state::approved")):
      errors.append("libraries/auth/pairing/pairing.cpp: approved pre-session exchange must validate before consuming")

   pre_session_validation = pairing_source.partition("void validate_pre_session")[2].partition(
      "credential_binding consume_approved_pre_session")[0]
   if ("require_pending_record" not in pre_session_validation or
       "verify_token(pending.pre_session_digest, pre_session_token)" not in pre_session_validation or
       "require_pre_session_lifecycle" not in pre_session_validation or
       pre_session_validation.find("require_pending_record") >
       pre_session_validation.find("verify_token(pending.pre_session_digest, pre_session_token)") or
       pre_session_validation.find("verify_token(pending.pre_session_digest, pre_session_token)") >
       pre_session_validation.find("require_pre_session_lifecycle")):
      errors.append("libraries/auth/pairing/pairing.cpp: pre-session validation must verify the token before lifecycle")

   approval_source = pairing_source.partition("credential approve_pending")[2].partition("void reject_pending")[0]
   if ("pending.approved_credential.emplace" not in approval_source or
       "auto approved_pending = pending;" not in approval_source or
       "pending = std::move(approved_pending);" not in approval_source or
       "pending.state = pending_state::approved" not in approval_source or
       approval_source.find("pending.approved_credential.emplace") >
       approval_source.find("pending.state = pending_state::approved") or
       approval_source.find("auto approved_pending = pending;") >
       approval_source.find("pending = std::move(approved_pending);")):
      errors.append("libraries/auth/pairing/pairing.cpp: approval must bind the credential before publishing approved state")

   record_validation = pairing_source.partition("void require_pending_record")[2].partition(
      "void require_pending_time")[0]
   for token in (
      "pending.approved_credential.has_value()",
      "!pending.approved_credential.has_value()",
      "pending.approved_credential->generation != 1",
   ):
      if token not in record_validation:
         errors.append(f"libraries/auth/pairing/pairing.cpp: pending binding invariant is missing ({token})")

   pre_session_lifecycle = pairing_source.partition("void require_pre_session_lifecycle")[2].partition(
      "void require_credential_record")[0]
   for token in (
      "now < pending.created_at",
      "pending.pre_session_consumed",
      "now >= pending.expires_at",
   ):
      if token not in pre_session_lifecycle:
         errors.append(f"libraries/auth/pairing/pairing.cpp: pre-session lifecycle invariant is missing ({token})")
   if (pre_session_lifecycle.find("now < pending.created_at") >
       pre_session_lifecycle.find("pending.pre_session_consumed") or
       pre_session_lifecycle.find("pending.pre_session_consumed") >
       pre_session_lifecycle.find("now >= pending.expires_at")):
      errors.append("libraries/auth/pairing/pairing.cpp: pre-session lifecycle must check time before replay and expiry")

   root_cmake = (root / "CMakeLists.txt").read_text(errors="ignore")
   for token in (
      "add_subdirectory(libraries/auth)",
      "forge_auth_pairing",
      "auth_pairing",
      "libraries/auth/pairing/include/forge",
   ):
      if token not in root_cmake:
         errors.append(f"CMakeLists.txt: forge_auth_pairing registration is incomplete ({token})")

   root_readme = (root / "README.md").read_text(errors="ignore")
   if "[auth/pairing](libraries/auth/pairing/README.md)" not in root_readme or "`forge_auth_pairing`" not in root_readme:
      errors.append("README.md: forge_auth_pairing library registry entry is missing")

   package_config = (root / "cmake" / "ForgeConfig.cmake.in").read_text(errors="ignore")
   for token in (
      "auth_pairing",
      'elseif("${component}" STREQUAL "auth_pairing")',
      "_forge_add_component(codec_base64)",
      "_forge_add_component(crypto_core)",
      "_forge_add_component(crypto_digest)",
   ):
      if token not in package_config:
         errors.append(f"cmake/ForgeConfig.cmake.in: auth_pairing package registration is incomplete ({token})")

   pairing_tests = (root / "tests" / "auth" / "pairing_tests.cpp").read_text(errors="ignore")
   for token in (
      "bootstrap_uses_random_base64url_secret_and_persists_only_a_digest",
      "bootstrap_rejects_noncanonical_trailing_pad_bits_without_consuming_the_record",
      "bootstrap_consumption_is_one_time_bounded_and_does_not_report_clear_tokens",
      "pre_session_is_canonical_digest_only_and_strictly_identifiable",
      "pre_session_validation_and_supersession_rotate_the_secret",
      "approved_pre_session_exchanges_once_and_rejected_or_expired_requests_cannot_exchange",
      "persisted_transition_clocks_reject_backdating_without_mutation",
      "malformed_persisted_terminal_timestamps_are_rejected_without_mutation",
      "pending_transitions_canonicalize_scopes_and_require_explicit_supersession",
      "approval_rotation_and_revocation_are_terminal_and_cannot_escalate_scopes",
   ):
      if token not in pairing_tests:
         errors.append(f"tests/auth/pairing_tests.cpp: pairing regression is missing ({token})")

   package_consumer = (root / "tests" / "package_auth_pairing_component" / "main.cpp").read_text(errors="ignore")
   for token in ("import forge.auth.pairing.exceptions;", "import forge.auth.pairing.pairing;", "consume_bootstrap",
                 "identify_pre_session", "consume_approved_pre_session", "credential_binding"):
      if token not in package_consumer:
         errors.append(f"tests/package_auth_pairing_component/main.cpp: direct pairing package import is missing ({token})")


def check_auth_session_boundaries(root: Path, errors: list[str]) -> None:
   family = root / "libraries" / "auth"
   leaf = family / "session"
   required = (
      leaf / "CMakeLists.txt",
      leaf / "session.cpp",
      leaf / "README.md",
      leaf / "include" / "forge" / "auth" / "session" / "exceptions.cppm",
      leaf / "include" / "forge" / "auth" / "session" / "types.cppm",
      leaf / "include" / "forge" / "auth" / "session" / "session.cppm",
      root / "tests" / "auth" / "session_tests.cpp",
      root / "tests" / "package_auth_session_component" / "CMakeLists.txt",
      root / "tests" / "package_auth_session_component" / "main.cpp",
   )
   for path in required:
      if not path.exists():
         errors.append(f"{path.relative_to(root)}: forge_auth_session ownership file is required")

   family_cmake = (family / "CMakeLists.txt").read_text(errors="ignore")
   if "add_subdirectory(session)" not in family_cmake:
      errors.append("libraries/auth/CMakeLists.txt: auth family must register session leaf")

   leaf_cmake = (leaf / "CMakeLists.txt").read_text(errors="ignore")
   for token in (
      "add_library(forge_auth_session STATIC session.cpp)",
      "forge_target_modules_at(forge_auth_session auth/session)",
      "forge_auth_pairing",
      "forge_codec_base64",
      "forge_crypto_core",
      "forge_crypto_digest",
      "forge_exceptions",
   ):
      if token not in leaf_cmake:
         errors.append(f"libraries/auth/session/CMakeLists.txt: session target is missing {token}")

   exceptions_module = (leaf / "include" / "forge" / "auth" / "session" / "exceptions.cppm").read_text(errors="ignore")
   for token in (
      "export module forge.auth.session.exceptions",
      "token_invalid",
      "csrf_invalid",
      "idle_expired",
      "credential_mismatch",
      "credential_revoked",
      "replayed",
      "secret_collision",
   ):
      if token not in exceptions_module:
         errors.append(f"libraries/auth/session/include/forge/auth/session/exceptions.cppm: session exception is missing {token}")

   types_module = (leaf / "include" / "forge" / "auth" / "session" / "types.cppm").read_text(errors="ignore")
   for token in (
      "export module forge.auth.session.types",
      "forge.auth.pairing.types",
      "trusted, non-decreasing wall-clock",
      "session_record",
      "session_issuance",
      "principal",
      "credential_generation",
      "idle_timeout",
      "session_state",
      "device_grant_record",
      "device_grant_issuance",
      "device_grant_refresh",
      "device_grant_state",
      "rotation_generation",
   ):
      if token not in types_module:
         errors.append(f"libraries/auth/session/include/forge/auth/session/types.cppm: session record is missing {token}")
   session_record_match = re.search(r"struct session_record \{(.*?)\n\};", types_module, re.DOTALL)
   if session_record_match is not None and "secret_string" in session_record_match.group(1):
      errors.append("libraries/auth/session/include/forge/auth/session/types.cppm: persisted session record must not contain clear secrets")
   device_grant_match = re.search(r"struct device_grant_record \{(.*?)\n\};", types_module, re.DOTALL)
   if device_grant_match is not None and "secret_string" in device_grant_match.group(1):
      errors.append("libraries/auth/session/include/forge/auth/session/types.cppm: persisted device grant must not contain a clear token")

   session_module = (leaf / "include" / "forge" / "auth" / "session" / "session.cppm").read_text(errors="ignore")
   for token in (
      "export module forge.auth.session.session",
      "issue_session",
      "validate_issuance",
      "identify_session_token",
      "validate_session",
      "verify_csrf_secret",
      "renew_idle",
      "rotate_session",
      "logout_session",
      "revoke_session",
      "issue_device_grant",
      "validate_device_grant_issuance",
      "identify_device_grant_token",
      "validate_device_grant",
      "rotate_device_grant",
      "refresh_device_grant",
      "revoke_device_grant",
   ):
      if token not in session_module:
         errors.append(f"libraries/auth/session/include/forge/auth/session/session.cppm: session transition is missing {token}")
   issue_declaration = session_module.partition("issue_session")[2].partition("identify_session_token")[0]
   rotate_declaration = session_module.partition("rotate_session")[2].partition("logout_session")[0]
   if "secret_string" in issue_declaration or "secret_string" in rotate_declaration:
      errors.append("libraries/auth/session/include/forge/auth/session/session.cppm: issue/rotate must not accept caller-selected secrets")

   session_source = (leaf / "session.cpp").read_text(errors="ignore")
   for token in (
      "random_bytes(secret_bytes)",
      "padding::omit",
      "padding_policy::forbid",
      "identify_secret",
      "validate_issuance",
      "identify_session_token",
      "constant_time_equal",
      "require_session_record(issuance.record)",
      "issuance.record.state != session_state::active",
      "verify_secret(issuance.session_token, issuance.record.session_digest, false)",
      "verify_secret(issuance.csrf_secret, issuance.record.csrf_digest, true)",
      "credential_generation",
      "credential_revoked",
      "canonical_idle_expiry",
      "session transition time regressed",
      "record.state = session_state::rotated",
      "record.state = session_state::revoked",
      "record.idle_expires_at != canonical_idle_expiry",
      "*record.terminal_at < record.last_activity_at",
      "*record.terminal_at >= record.idle_expires_at",
      "session and CSRF digests must differ",
      "device grant token was replayed after rotation",
      "record.rotation_generation + 1",
      "record.state = device_grant_state::rotated",
      "record.state = device_grant_state::revoked",
   ):
      if token not in session_source:
         errors.append(f"libraries/auth/session/session.cpp: session security invariant is missing {token}")
   for forbidden in ("forge.db", "forge.net", "forge.plugins", "objectdb", "mdbx"):
      if forbidden in session_source or forbidden in session_module or forbidden in types_module:
         errors.append(f"libraries/auth/session: product integration dependency is forbidden ({forbidden})")

   rotate_source = session_source.partition("session_issuance rotate_session")[2].partition("void logout_session")[0]
   if ("auto result = issue_session" not in rotate_source or
       rotate_source.find("auto result = issue_session") > rotate_source.find("record.state = session_state::rotated") or
       "return result;" not in rotate_source):
      errors.append("libraries/auth/session/session.cpp: rotation must construct its result before mutating the old session")

   issue_source = session_source.partition("session_issuance issue_session")[2].partition("principal validate_session")[0]
   collision_check = "constant_time_equal(session_material.span(), csrf_material.span())"
   if (collision_check not in issue_source or "const auto session_digest" not in issue_source or
       issue_source.find(collision_check) > issue_source.find("const auto session_digest") or
       "exceptions::secret_collision" not in issue_source):
      errors.append("libraries/auth/session/session.cpp: session and CSRF material must differ before record construction")

   renewal_source = session_source.partition("void renew_idle")[2].partition("session_issuance rotate_session")[0]
   next_expiry = "const auto next_idle_expires_at =\n       canonical_idle_expiry"
   if (next_expiry not in renewal_source or "record.last_activity_at = now;" not in renewal_source or
       "record.idle_expires_at = next_idle_expires_at;" not in renewal_source or
       renewal_source.find(next_expiry) > renewal_source.find("record.last_activity_at = now;")):
      errors.append("libraries/auth/session/session.cpp: renewal must compute expiry before mutating activity")

   record_validation = session_source.partition("void require_session_record")[2].partition("void require_active_session")[0]
   digest_check = "session and CSRF digests must differ"
   if (digest_check not in record_validation or
       record_validation.find(digest_check) > record_validation.find("require_credential_id") or
       "record.idle_expires_at != canonical_idle_expiry" not in record_validation or
       "require_supported_time(record.created_at, time_input::record)" not in record_validation):
      errors.append("libraries/auth/session/session.cpp: persisted session validation must reject duplicate digests and non-canonical times")

   root_cmake = (root / "CMakeLists.txt").read_text(errors="ignore")
   for token in ("forge_auth_session", "auth_session", "libraries/auth/session/include/forge"):
      if token not in root_cmake:
         errors.append(f"CMakeLists.txt: forge_auth_session registration is incomplete ({token})")

   root_readme = (root / "README.md").read_text(errors="ignore")
   if "[auth/session](libraries/auth/session/README.md)" not in root_readme or "`forge_auth_session`" not in root_readme:
      errors.append("README.md: forge_auth_session library registry entry is missing")

   package_config = (root / "cmake" / "ForgeConfig.cmake.in").read_text(errors="ignore")
   if ('elseif("${component}" STREQUAL "auth_session")\n         _forge_add_component(auth_pairing)' not in package_config or
       "auth_session" not in package_config):
      errors.append("cmake/ForgeConfig.cmake.in: auth_session package registration is incomplete")

   session_tests = (root / "tests" / "auth" / "session_tests.cpp").read_text(errors="ignore")
   for token in (
      "issue_generates_independent_digest_only_canonical_secrets",
      "issuance_validation_rejects_mixed_and_tampered_secret_pairs",
      "validation_rejects_bounded_malformed_and_noncanonical_secrets",
      "validation_returns_principal_and_enforces_credential_binding",
      "absolute_idle_and_renewal_boundaries_preserve_secret_digests",
      "backdated_session_transitions_fail_without_mutation",
      "malformed_persisted_timestamps_are_rejected_without_mutation",
      "extreme_time_options_are_checked_without_overflow",
      "rotation_prevents_fixation_and_logout_revoke_are_terminal",
      "device_grant_rotation_preserves_absolute_expiry_and_detects_replay",
      "device_grant_is_credential_bound_and_does_not_issue_a_session",
      "device_grant_refresh_rotates_grant_and_issues_bound_session",
      "device_grant_expiry_revoke_and_backdated_transitions_are_terminal",
      "device_grant_raw_round_trip_rejects_malformed_state",
   ):
      if token not in session_tests:
         errors.append(f"tests/auth/session_tests.cpp: session regression is missing ({token})")

   package_consumer = (root / "tests" / "package_auth_session_component" / "main.cpp").read_text(errors="ignore")
   for token in ("import forge.auth.session.exceptions;", "import forge.auth.session.session;", "validate_issuance",
                 "verify_csrf_secret"):
      if token not in package_consumer:
         errors.append(f"tests/package_auth_session_component/main.cpp: direct session package import is missing ({token})")

   tests_cmake = (root / "tests" / "CMakeLists.txt").read_text(errors="ignore")
   for token in ("test_forge_auth_session", "package_auth_session_component", "forge_auth_session"):
      if token not in tests_cmake:
         errors.append(f"tests/CMakeLists.txt: auth_session test registration is incomplete ({token})")


def check_auth_http_boundaries(root: Path, errors: list[str]) -> None:
   family = root / "libraries" / "auth"
   leaf = family / "http"
   required = (
      leaf / "CMakeLists.txt",
      leaf / "policy.cpp",
      leaf / "README.md",
      leaf / "include" / "forge" / "auth" / "http" / "exceptions.cppm",
      leaf / "include" / "forge" / "auth" / "http" / "types.cppm",
      leaf / "include" / "forge" / "auth" / "http" / "policy.cppm",
      root / "tests" / "auth" / "http_auth_tests.cpp",
      root / "tests" / "package_auth_http_component" / "CMakeLists.txt",
      root / "tests" / "package_auth_http_component" / "main.cpp",
   )
   for path in required:
      if not path.exists():
         errors.append(f"{path.relative_to(root)}: forge_auth_http ownership file is required")

   family_cmake = (family / "CMakeLists.txt").read_text(errors="ignore")
   if "add_subdirectory(http)" not in family_cmake:
      errors.append("libraries/auth/CMakeLists.txt: auth family must register http leaf")
   if re.search(r"add_library\s*\(\s*forge_auth\s", family_cmake):
      errors.append("libraries/auth/CMakeLists.txt: empty auth family root must not define forge_auth aggregate")

   leaf_cmake = (leaf / "CMakeLists.txt").read_text(errors="ignore")
   for token in (
      "add_library(forge_auth_http STATIC policy.cpp)",
      "forge_target_modules_at(forge_auth_http auth/http)",
      "forge_exceptions",
      "forge_crypto_core",
      "forge_auth_session",
      "forge_net_http",
   ):
      if token not in leaf_cmake:
         errors.append(f"libraries/auth/http/CMakeLists.txt: HTTP auth target is missing {token}")
   if "forge_auth_pairing" in leaf_cmake or "forge_plugins" in leaf_cmake:
      errors.append("libraries/auth/http/CMakeLists.txt: HTTP auth must not bypass its session and HTTP leaf boundaries")

   exceptions_module = (leaf / "include" / "forge" / "auth" / "http" / "exceptions.cppm").read_text(errors="ignore")
   for token in (
      "export module forge.auth.http.exceptions",
      "malformed_evidence",
      "duplicate_evidence",
      "origin_mismatch",
      "csrf_mismatch",
      "scope_denied",
   ):
      if token not in exceptions_module:
         errors.append(f"libraries/auth/http/include/forge/auth/http/exceptions.cppm: HTTP auth exception is missing {token}")

   types_module = (leaf / "include" / "forge" / "auth" / "http" / "types.cppm").read_text(errors="ignore")
   for token in (
      "export module forge.auth.http.types",
      "cookie_policy",
      "origin_policy",
      "browser_request_evidence",
      "session_evidence",
      "authorization_options",
      "security_header_options",
   ):
      if token not in types_module:
         errors.append(f"libraries/auth/http/include/forge/auth/http/types.cppm: HTTP auth value is missing {token}")

   policy_module = (leaf / "include" / "forge" / "auth" / "http" / "policy.cppm").read_text(errors="ignore")
   for token in (
      "export module forge.auth.http.policy",
      "make_origin_policy",
      "extract_session_evidence",
      "authorize",
      "append_pre_session_cookie",
      "append_approved_session_cookies",
      "append_rotated_session_cookies",
      "append_logout_cookies",
      "apply_security_headers",
   ):
      if token not in policy_module:
         errors.append(f"libraries/auth/http/include/forge/auth/http/policy.cppm: HTTP auth policy is missing {token}")

   policy_source = (leaf / "policy.cpp").read_text(errors="ignore")
   for token in (
      "parse_cookie_header",
      "append_set_cookie",
      "identify_session_token",
      "validate_session",
      "verify_csrf_secret",
      "constant_time_equal",
      "take_cookie_value",
      "std::move(found->value)",
      "secure_erase(found->value)",
      "validate_issuance(issuance)",
      "auto staged = response",
      "response = std::move(staged)",
      "boost::urls::parse_uri",
      "canonical.normalize",
      "remove_port",
      "__Host-",
      "same_site::strict",
      "Content-Security-Policy",
      "frame-ancestors 'none'",
   ):
      if token not in policy_source:
         errors.append(f"libraries/auth/http/policy.cpp: HTTP auth security invariant is missing {token}")
   for forbidden in ("forge.db", "forge.plugins", "objectdb", "mdbx", "router", "rate_limit", "audit"):
      if forbidden in policy_source or forbidden in policy_module or forbidden in types_module:
         errors.append(f"libraries/auth/http: product or transport ownership is forbidden ({forbidden})")

   root_cmake = (root / "CMakeLists.txt").read_text(errors="ignore")
   for token in ("forge_auth_http", "auth_http", "libraries/auth/http/include/forge"):
      if token not in root_cmake:
         errors.append(f"CMakeLists.txt: forge_auth_http registration is incomplete ({token})")

   root_readme = (root / "README.md").read_text(errors="ignore")
   if "[auth/http](libraries/auth/http/README.md)" not in root_readme or "`forge_auth_http`" not in root_readme:
      errors.append("README.md: forge_auth_http library registry entry is missing")

   package_config = (root / "cmake" / "ForgeConfig.cmake.in").read_text(errors="ignore")
   if ('elseif("${component}" STREQUAL "auth_http")\n         _forge_add_component(exceptions)\n         _forge_add_component(crypto_core)\n         _forge_add_component(auth_session)\n         _forge_add_component(net_http)' not in package_config or
       "auth_http" not in package_config):
      errors.append("cmake/ForgeConfig.cmake.in: auth_http package registration is incomplete")

   http_tests = (root / "tests" / "auth" / "http_auth_tests.cpp").read_text(errors="ignore")
   for token in (
      "authorization_enforces_safe_and_mutating_origin_matrix",
      "extraction_rejects_missing_duplicate_and_malformed_browser_evidence",
      "origin_policy_requires_canonical_browser_origins",
      "authorization_propagates_session_binding_and_rejects_csrf_or_scope_escalation",
      "cookie_policy_preserves_repeated_set_cookie_and_clears_browser_state",
      "cookie_issuance_validates_session_integrity_before_atomic_response_update",
      "security_headers_are_strict_without_global_asset_no_store",
   ):
      if token not in http_tests:
         errors.append(f"tests/auth/http_auth_tests.cpp: HTTP auth regression is missing ({token})")

   package_consumer = (root / "tests" / "package_auth_http_component" / "main.cpp").read_text(errors="ignore")
   for token in ("import forge.auth.http.exceptions;", "import forge.auth.http.policy;", "make_session_cookie",
                 "identify_session_token", "validate_issuance"):
      if token not in package_consumer:
         errors.append(f"tests/package_auth_http_component/main.cpp: direct HTTP auth package import is missing ({token})")

   tests_cmake = (root / "tests" / "CMakeLists.txt").read_text(errors="ignore")
   for token in ("test_forge_auth_http", "package_auth_http_component", "forge_auth_http"):
      if token not in tests_cmake:
         errors.append(f"tests/CMakeLists.txt: auth_http test registration is incomplete ({token})")


def check_macro_only_header(root: Path, path: Path, errors: list[str]) -> None:
   text = re.sub(r"/\*.*?\*/", "", path.read_text(errors="ignore"), flags=re.DOTALL)
   in_macro = False

   for line_number, line in enumerate(text.splitlines(), 1):
      stripped = re.sub(r"//.*$", "", line).strip()
      if in_macro:
         in_macro = line.rstrip().endswith("\\")
         continue
      if not stripped:
         continue
      if stripped.startswith("#"):
         if re.match(r"#\s*define\b", stripped):
            in_macro = line.rstrip().endswith("\\")
         continue
      errors.append(
         f"{path.relative_to(root)}:{line_number}: macro-only public header contains a C++ declaration"
      )


def check_vm_wasm_interpret_boundaries(root: Path, errors: list[str]) -> None:
   family = root / "libraries" / "vm" / "wasm"
   if not family.exists():
      return

   root_cmake = root / "CMakeLists.txt"
   stale_include_registration = f"{family.relative_to(root).as_posix()}/include"
   if root_cmake.is_file() and stale_include_registration in root_cmake.read_text(errors="ignore"):
      errors.append(f"CMakeLists.txt: stale {stale_include_registration} registration")

   unexpected_family_entries = {path.name for path in family.iterdir()} - {"interpret"}
   if unexpected_family_entries:
      errors.append(
         f"{family.relative_to(root)}: empty vm::wasm family root contains: "
         f"{', '.join(sorted(unexpected_family_entries))}"
      )

   component = family / "interpret"
   if not component.exists():
      errors.append(f"{component.relative_to(root)}: vm_wasm_interpret leaf is missing")
      return

   details = component / "details"
   if details.exists():
      errors.append(f"{details.relative_to(root)}: vm_wasm_interpret must not install or compile private source headers")

   include = component / "include" / "forge" / "vm" / "wasm" / "interpret"
   allowed_headers = {"host_function.hpp", "opcode_macros.hpp"}
   headers = {path.name for path in include.glob("*.hpp")}
   unexpected = headers - allowed_headers
   if unexpected:
      errors.append(f"{include.relative_to(root)}: unexpected public headers: {', '.join(sorted(unexpected))}")

   for name in sorted(allowed_headers):
      path = include / name
      if not path.exists():
         errors.append(f"{path.relative_to(root)}: required macro-only public header is missing")
         continue
      check_macro_only_header(root, path, errors)

   for path in sorted(include.glob("*.cppm")):
      relative = path.relative_to(root)
      source = path.read_text(errors="ignore")
      for line_number, line in enumerate(source.splitlines(), 1):
         included = INCLUDE.match(line)
         if included and (".hxx" in included.group(1) or "details/" in included.group(1)):
            errors.append(f"{relative}:{line_number}: public VM module includes a private source header")
         if included and "forge/vm/wasm/interpret/" in included.group(1) and included.group(1) not in {
            "<forge/vm/wasm/interpret/host_function.hpp>",
            "<forge/vm/wasm/interpret/opcode_macros.hpp>",
         }:
            errors.append(f"{relative}:{line_number}: VM components must use module imports")
         if VM_WASM_INTERPRET_EXPORT.search(line):
            errors.append(f"{relative}:{line_number}: FORGE_VM_WASM_INTERPRET_EXPORT is forbidden")
         if UNQUALIFIED_C_MEMORY.search(line):
            errors.append(f"{relative}:{line_number}: VM modules must qualify C memory functions through std")


def check_vm_wasm_interpret_identities(root: Path, files: list[Path], errors: list[str]) -> None:
   identity_files = set(files)
   identity_files.update(source_files(root, ("libraries/tooling", "tools")))
   for path in sorted(identity_files):
      relative = path.relative_to(root)
      for line_number, line in enumerate(path.read_text(errors="ignore").splitlines(), 1):
         for expression, description in LEGACY_VM_WASM_SOURCE_IDENTITIES:
            if expression.search(line):
               errors.append(f"{relative}:{line_number}: {description} is forbidden")

   cmake_paths = [
      root / "CMakeLists.txt",
      root / "cmake" / "ForgeConfig.cmake.in",
      root / ".github" / "workflows" / "vm-wasm.yml",
   ]
   for base in (root / "libraries", root / "guest", root / "plugins", root / "tests"):
      if base.exists():
         cmake_paths.extend(base.rglob("CMakeLists.txt"))

   for path in sorted(set(cmake_paths)):
      if not path.is_file():
         continue
      relative = path.relative_to(root)
      source = path.read_text(errors="ignore")
      for line_number, line in enumerate(source.splitlines(), 1):
         if relative == Path("tests/CMakeLists.txt") and line.strip() == "add_subdirectory(vm_wasm)":
            continue
         for expression, description in LEGACY_VM_WASM_CMAKE_IDENTITIES:
            if expression.search(line):
               errors.append(f"{relative}:{line_number}: {description} is forbidden")


def check_contract_tooling_boundaries(root: Path, files: list[Path], errors: list[str]) -> None:
   family = root / "libraries" / "tooling"
   expected_family_entries = {"CMakeLists.txt", "README.md", "abi", "attributes", "validation", "manifest", "testing"}
   if not family.is_dir():
      errors.append("libraries/tooling: Contract Tooling family root is missing")
      return

   unexpected_family_entries = {path.name for path in family.iterdir()} - expected_family_entries
   if unexpected_family_entries:
      errors.append(
         "libraries/tooling: empty tooling family root contains: "
         f"{', '.join(sorted(unexpected_family_entries))}"
      )

   for leaf in ("abi", "attributes", "validation", "manifest", "testing"):
      component = family / leaf
      if not component.is_dir():
         errors.append(f"{component.relative_to(root)}: Contract Tooling leaf is missing")
         continue
      legacy_include = component / "include" / "forge" / "contract" / leaf
      if legacy_include.exists():
         errors.append(f"{legacy_include.relative_to(root)}: legacy Contract Tooling include path is forbidden")
      legacy_component = root / "libraries" / "contract" / leaf
      if legacy_component.exists():
         errors.append(f"{legacy_component.relative_to(root)}: legacy Contract Tooling leaf is forbidden")

   family_cmake = family / "CMakeLists.txt"
   if family_cmake.is_file():
      source = family_cmake.read_text(errors="ignore")
      if re.search(r"\badd_library\s*\(", source):
         errors.append("libraries/tooling/CMakeLists.txt: tooling family root must not define an aggregate target")

   tests_cmake = root / "tests" / "CMakeLists.txt"
   if tests_cmake.is_file():
      source = tests_cmake.read_text(errors="ignore")
      expected_test_paths = (
         "tooling/contract_tests.cpp",
         "package_tooling_abi_component",
         "package_tooling_attributes_component",
         "package_tooling_validation_component",
         "package_tooling_manifest_component",
      )
      for relative in expected_test_paths:
         if relative not in source:
            errors.append(f"tests/CMakeLists.txt: Contract Tooling test path is missing: {relative}")
         if not (root / "tests" / relative).exists():
            errors.append(f"tests/{relative}: Contract Tooling test path does not exist")
      for stale_path in ("contract/contract_tests.cpp", "contract_testing/", "package_contract_"):
         if stale_path in source:
            errors.append(f"tests/CMakeLists.txt: stale Contract Tooling test path is forbidden: {stale_path}")

   for path in files:
      relative = path.relative_to(root)
      source = path.read_text(errors="ignore")
      for line_number, line in enumerate(source.splitlines(), 1):
         for expression, description in LEGACY_CONTRACT_TOOLING_SOURCE_IDENTITIES:
            if expression.search(line):
               errors.append(f"{relative}:{line_number}: {description} is forbidden")
         if re.search(r"\bnamespace\s+forge::tooling\s*\{", line):
            errors.append(f"{relative}:{line_number}: tooling is a grouping namespace; use a leaf namespace")
         if re.search(r"\bexport\s+module\s+forge\.tooling\s*;", line):
            errors.append(f"{relative}:{line_number}: tooling aggregate module is forbidden")
         if re.search(r"\bnamespace\s+\w+\s*=\s*forge::contract::(?:abi|attributes|validation|manifest|testing)", line):
            errors.append(f"{relative}:{line_number}: legacy Contract Tooling namespace alias is forbidden")

   cmake_paths = [root / "CMakeLists.txt", root / "cmake" / "ForgeConfig.cmake.in"]
   workflows = root / ".github" / "workflows"
   if workflows.is_dir():
      cmake_paths.extend(workflows.glob("*.yml"))
   for base in (root / "libraries", root / "guest", root / "plugins", root / "tests", root / "tools"):
      if base.exists():
         cmake_paths.extend(base.rglob("CMakeLists.txt"))

   for path in sorted(set(cmake_paths)):
      if not path.is_file():
         continue
      relative = path.relative_to(root)
      source = path.read_text(errors="ignore")
      for line_number, line in enumerate(source.splitlines(), 1):
         for expression, description in LEGACY_CONTRACT_TOOLING_CMAKE_IDENTITIES:
            if expression.search(line):
               errors.append(f"{relative}:{line_number}: {description} is forbidden")
         if re.search(r"\badd_library\s*\(\s*forge_tooling\b", line):
            errors.append(f"{relative}:{line_number}: tooling aggregate target is forbidden")
         if re.search(r"\badd_library\s*\(\s*forge_(?:contract_(?:abi|attributes|validation|manifest|testing)|tooling)\s+ALIAS\b", line):
            errors.append(f"{relative}:{line_number}: Contract Tooling aliases are forbidden")
         if re.search(r'"\$\{component\}"\s+STREQUAL\s+"tooling"', line):
            errors.append(f"{relative}:{line_number}: tooling aggregate package component is forbidden")
      for component in ("contract_abi", "contract_attributes", "contract_validation", "contract_manifest", "contract_testing"):
         if re.search(
            rf"\bfind_package\s*\(\s*Forge\b(?:(?!\)).)*\b{component}\b",
            source,
            flags=re.IGNORECASE | re.DOTALL,
         ):
            errors.append(f"{relative}: legacy Contract Tooling package component {component} is forbidden")
         if re.search(rf'"\$\{{component\}}"\s+STREQUAL\s+"{component}"', source):
            errors.append(f"{relative}: legacy Contract Tooling package component {component} is forbidden")


def check_plugin_impl_ownership(root: Path, errors: list[str]) -> None:
   for path in sorted((root / "plugins").rglob("details/plugin_impl.hxx")):
      for line_number, line in enumerate(path.read_text(errors="ignore").splitlines(), 1):
         declaration = PRIVATE_DECLARATION.match(line)
         if declaration and declaration.group(1) != "plugin::impl":
            errors.append(
               f"{path.relative_to(root)}:{line_number}: plugin_impl.hxx may only own plugin::impl; "
               f"move {declaration.group(1)} to its exact private header"
            )


def check_chain_savanna_boundaries(root: Path, errors: list[str]) -> None:
   component = root / "libraries" / "chain" / "savanna"
   if not component.exists():
      return

   neutral = {
      "include/forge/chain/savanna/exceptions.cppm",
      "include/forge/chain/savanna/types.cppm",
      "include/forge/chain/savanna/policy.cppm",
      "include/forge/chain/savanna/finality_core.cppm",
      "include/forge/chain/savanna/qc.cppm",
      "include/forge/chain/savanna/vote.cppm",
      "include/forge/chain/savanna/vote_accumulator.cppm",
      "include/forge/chain/savanna/validation.cppm",
      "include/forge/chain/savanna/finalizer_safety.cppm",
      "include/forge/chain/savanna/rank.cppm",
      "types.cpp",
      "policy.cpp",
      "finality_core.cpp",
      "qc.cpp",
      "vote.cpp",
      "vote_accumulator.cpp",
      "vote_accumulator_impl.cpp",
      "details/vote_accumulator_impl.hxx",
      "validation.cpp",
      "finalizer_safety.cpp",
      "rank.cpp",
   }
   protocol_bound = {
      "include/forge/chain/savanna/policy_state.cppm",
      "include/forge/chain/savanna/extensions.cppm",
      "include/forge/chain/savanna/genesis.cppm",
      "include/forge/chain/savanna/header_state.cppm",
      "include/forge/chain/savanna/candidate.cppm",
      "include/forge/chain/savanna/checkpoint.cppm",
      "include/forge/chain/savanna/admission.cppm",
      "include/forge/chain/savanna/finality_witness.cppm",
      "policy_state.cpp",
      "extensions.cpp",
      "genesis.cpp",
      "header_state.cpp",
      "admission.cpp",
      "finality_witness.cpp",
   }
   actual = {
      str(path.relative_to(component))
      for path in source_files(root, ("libraries/chain/savanna",))
      if "values" not in path.relative_to(component).parts
   }
   for relative in sorted((neutral | protocol_bound) - actual):
      errors.append(f"libraries/chain/savanna/{relative}: required Savanna module source is missing")
   for relative in sorted(actual - (neutral | protocol_bound)):
      errors.append(f"libraries/chain/savanna/{relative}: unclassified Savanna production source")

   forbidden_everywhere = {
      "bls12-381": "private BLS backend",
      "bls12_381": "private BLS backend",
      "blockchain::": "product namespace",
      "storlane::": "product namespace",
      "eosio::": "donor namespace",
      "spring::": "donor namespace",
   }
   for path in source_files(root, ("libraries/chain/savanna",)):
      relative = path.relative_to(root)
      source = path.read_text(errors="ignore")
      for token, owner in forbidden_everywhere.items():
         if token in source:
            errors.append(f"{relative}: Savanna must not depend on {owner} ({token})")

   protocol_import = re.compile(
      r"(?:forge\.chain\.protocol(?:\.|\b)|forge::chain::protocol|forge/chain/protocol)"
   )
   for relative in sorted(neutral):
      path = component / relative
      if path.is_file() and protocol_import.search(path.read_text(errors="ignore")):
         errors.append(f"{path.relative_to(root)}: neutral Savanna kernel imports Chain Protocol")

   values = component / "values"
   for path in source_files(root, ("libraries/chain/savanna/values",)):
      if protocol_import.search(path.read_text(errors="ignore")):
         errors.append(f"{path.relative_to(root)}: neutral Savanna values import Chain Protocol")

   cmake = component / "CMakeLists.txt"
   cmake_source = cmake.read_text(errors="ignore")
   target_definitions = re.findall(r"\badd_library\s*\(\s*(forge_chain_savanna\S*)", cmake_source)
   if target_definitions != ["forge_chain_savanna"]:
      errors.append(
         f"{cmake.relative_to(root)}: protocol-bound Savanna integration must remain in the single "
         "forge_chain_savanna target"
      )
   if "forge_chain_protocol" not in cmake_source:
      errors.append(f"{cmake.relative_to(root)}: forge_chain_savanna must link its approved protocol integration")
   for relative in sorted(path for path in protocol_bound if path.endswith(".cpp")):
      if Path(relative).name not in cmake_source:
         errors.append(f"{cmake.relative_to(root)}: forge_chain_savanna omits {relative}")


def check_bls_value_ownership(root: Path, files: list[Path], errors: list[str]) -> None:
   values = root / "libraries/crypto/bls/include/forge/crypto/bls/bls_values.cppm"
   if values.exists():
      source = values.read_text(errors="ignore")
      if "bls12_381" in source or "bls12-381" in source:
         errors.append(f"{values.relative_to(root)}: public BLS values must not expose the vendor backend")

   public_bls = root / "libraries/crypto/bls/include/forge/crypto/bls"
   for path in sorted(public_bls.glob("*.cppm")):
      source = path.read_text(errors="ignore")
      if "affine_non_montgomery_le" in source:
         errors.append(f"{path.relative_to(root)}: public BLS API must use canonical value serialization")

   forbidden_aliases = re.compile(
      r"\busing\s+(?:public_key_value|signature_value)\b|"
      r"\btypedef\b[^;]*\b(?:public_key_value|signature_value)\b"
   )
   for path in files:
      source = path.read_text(errors="ignore")
      if forbidden_aliases.search(source):
         errors.append(f"{path.relative_to(root)}: removed BLS value aliases are forbidden")

   authority = root / "libraries/chain/protocol/include/forge/chain/protocol/finalizer_authority.cppm"
   if authority.exists() and re.search(r"\bstruct\s+finalizer_authority\b", authority.read_text(errors="ignore")):
      errors.append(f"{authority.relative_to(root)}: finalizer_authority must alias the canonical Savanna finalizer")

   for relative in ("libraries/chain/protocol", "libraries/chain/savanna"):
      for path in source_files(root, (relative,)):
         source = path.read_text(errors="ignore")
         if re.search(r"std::vector\s*<\s*char\s*>\s+public_key", source):
            errors.append(f"{path.relative_to(root)}: public BLS protocol fields must use the canonical value type")


def check_chain_api_shape(root: Path, errors: list[str]) -> None:
   component = root / "libraries" / "chain" / "api"
   if not component.exists():
      return

   include = component / "include" / "forge" / "chain" / "api"
   nested_modules = sorted(path for path in include.rglob("*.cppm") if path.parent != include)
   if nested_modules:
      rendered = ", ".join(str(path.relative_to(root)) for path in nested_modules)
      errors.append("chain API modules must use a flat public include layout: " + rendered)

   forbidden_directories = {
      "types",
      "info",
      "block",
      "state",
      "transaction",
      "admin",
      "client",
   }
   nested_components = sorted(
      path.name
      for path in component.iterdir()
      if path.is_dir() and path.name in forbidden_directories
   )
   if nested_components:
      errors.append("chain API must be one flat library, found nested components: " + ", ".join(nested_components))

   cmake = (component / "CMakeLists.txt").read_text()
   if not re.search(r"add_library\s*\(\s*forge_chain_api\s+STATIC\b", cmake):
      errors.append("libraries/chain/api/CMakeLists.txt: expected one compiled forge_chain_api target")
   if re.search(r"\bforge_chain_api_(?:types|info|block|state|transaction|admin|client)\b", cmake):
      errors.append("libraries/chain/api/CMakeLists.txt: split chain API targets are forbidden")

   nested_api_namespace = re.compile(r"\bnamespace\s+forge::chain::api::(?:info|block|state|transaction|admin)\b")
   wire_record = re.compile(
      r"^\s*(?:export\s+)?(?:struct|enum\s+class)\s+\w*(?:request|result|response)\b",
      re.MULTILINE,
   )
   for path in sorted(include.glob("*.cppm")):
      source = path.read_text(errors="ignore")
      if nested_api_namespace.search(source):
         errors.append(f"{path.relative_to(root)}: API names must be classes in forge::chain::api")
      if "BOOST_DESCRIBE" in source or wire_record.search(source):
         errors.append(f"{path.relative_to(root)}: chain API wire records belong to forge.chain.protocol")

   protocol_include = root / "libraries" / "chain" / "protocol" / "include" / "forge" / "chain" / "protocol"
   get_dto = re.compile(r"^\s*(?:export\s+)?(?:struct|class|using)\s+get_\w+", re.MULTILINE)
   query_modules = ("audit.cppm", "info.cppm", "block_query.cppm", "state_query.cppm",
                    "transaction_query.cppm", "admin.cppm")
   for name in query_modules:
      path = protocol_include / name
      if not path.exists():
         errors.append(f"{path.relative_to(root)}: missing chain protocol query module")
         continue
      source = path.read_text(errors="ignore")
      if get_dto.search(source):
         errors.append(f"{path.relative_to(root)}: DTO names must not repeat the get operation")
      if re.search(r"\busing\s+\w+_response\s*=", source):
         errors.append(f"{path.relative_to(root)}: response DTOs must be concrete records, not aliases")
   if sorted(protocol_include.glob("api_*.cppm")):
      errors.append("chain protocol query modules must not use the api_* filename prefix")


def check_contract_sdk_workflow(root: Path, errors: list[str]) -> None:
   path = root / ".github" / "workflows" / "contract-sdk.yml"
   if not path.exists():
      return

   source = path.read_text(errors="ignore")
   if "  pull_request:\n" in source:
      pull_request = source.split("  pull_request:\n", 1)[1].split("  push:\n", 1)[0]
      for required in (
         '      - "CMakeLists.txt"',
         '      - "cmake/**"',
         '      - "libraries/asio/**"',
         '      - "libraries/chain/core/**"',
         '      - "libraries/chain/protocol/**"',
         '      - "libraries/codec/json/**"',
         '      - "libraries/compression/**"',
         '      - "libraries/config/core/**"',
         '      - "libraries/core/**"',
         '      - "libraries/crypto/**"',
         '      - "libraries/db/**"',
         '      - "libraries/exceptions/**"',
         '      - "libraries/db/ids/**"',
         '      - "libraries/raw/**"',
         '      - "libraries/reflect/**"',
         '      - "libraries/schema/**"',
         '      - "libraries/variant/**"',
         '      - "libraries/vm/wasm/**"',
         '      - "libraries/tooling/**"',
         '      - "guest/**"',
         '      - "tools/**"',
         '      - "vendor/**"',
      ):
         if required not in pull_request:
            errors.append(
               f"{path.relative_to(root)}: pull_request paths must include {required.strip()[2:]}"
            )
   elif "  workflow_dispatch:\n" not in source:
      errors.append(f"{path.relative_to(root)}: workflow must define pull_request or workflow_dispatch")

   sysroot_cache_inputs = "hashFiles('guest/sysroot/build.sh', 'guest/sysroot/include/**')"
   if sysroot_cache_inputs not in source:
      errors.append(
         f"{path.relative_to(root)}: contract sysroot cache key must hash its build script and headers"
      )

   macos_sdkroot = 'echo "SDKROOT=$(xcrun --sdk macosx --show-sdk-path)" >> "$GITHUB_ENV"'
   if source.count(macos_sdkroot) < 2:
      errors.append(
         f"{path.relative_to(root)}: macOS developer and release jobs must export the selected SDKROOT"
      )

   recovery_contract = "-DFORGE_CONTRACT_TEST_RECOVERY_WASM="
   if source.count(recovery_contract) != 2:
      errors.append(
         f"{path.relative_to(root)}: developer and release E2E jobs must execute the recovery contract"
      )

   for incompatible_flag in ('CXXFLAGS=-stdlib=libc++', 'LDFLAGS=-stdlib=libc++'):
      if incompatible_flag in source:
         errors.append(
            f"{path.relative_to(root)}: Linux host tooling must not override its packaged C++ ABI; "
            f"remove {incompatible_flag}"
         )

   for required in (
      "ppa:ubuntu-toolchain-r/test",
      "g++-15",
      "FORGE_CONTRACT_LLVM_SOURCE_DIR",
      "--target forge_contract_llvm -j 4",
   ):
      if required not in source:
         errors.append(
            f"{path.relative_to(root)}: Contract SDK workflow is missing {required}"
         )

   release_build = re.search(
      r"cmake --build build/contract-release-consumer\s+\\\n"
      r"\s+--target (?P<targets>(?:[^\n]|\\\n)+?)\s+\\\n"
      r"\s+-j 4",
      source,
   )
   required_release_contracts = {"recordtest", "legacynotify", "recovery"}
   release_targets = set() if release_build is None else set(release_build.group("targets").split())
   missing_release_contracts = sorted(required_release_contracts - release_targets)
   if missing_release_contracts:
      errors.append(
         f"{path.relative_to(root)}: release consumer must build E2E contracts before configuration: "
         f"{', '.join(missing_release_contracts)}"
      )


def check_chain_audited_api_workflow(root: Path, errors: list[str]) -> None:
   path = root / ".github" / "workflows" / "chain-audited-api.yml"
   if not path.exists():
      return

   source = path.read_text(errors="ignore")
   required_developer_dir = (
      "FORGE_MACOS_DEVELOPER_DIR: /Applications/Xcode_26.3.app/Contents/Developer"
   )
   if required_developer_dir not in source:
      errors.append(
         f"{path.relative_to(root)}: macOS acceptance must pin the Xcode 26.3 developer directory"
      )

   sdkroot_export = 'echo "SDKROOT=$(xcrun --sdk macosx --show-sdk-path)" >> "$GITHUB_ENV"'
   if source.count(sdkroot_export) != 2:
      errors.append(
         f"{path.relative_to(root)}: native and performance jobs must export the selected macOS SDKROOT"
      )

   osx_sysroot = 'osx_options+=("-DCMAKE_OSX_SYSROOT=$SDKROOT")'
   if source.count(osx_sysroot) != 2:
      errors.append(
         f"{path.relative_to(root)}: native and performance configure steps must use the selected macOS SDKROOT"
      )

   isolated_glaze_prefix = 'CMAKE_PREFIX_PATH=$RUNNER_TEMP/forge-glaze;'
   if isolated_glaze_prefix in source:
      errors.append(
         f"{path.relative_to(root)}: isolated Glaze prefix must not enter CMAKE_PREFIX_PATH"
      )

   exact_glaze_config = 'glaze_config="$RUNNER_TEMP/forge-glaze/share/glaze/glazeConfig.cmake"'
   resolved_glaze_dir = 'echo "FORGE_GLAZE_DIR=$(cd "$(dirname "$glaze_config")" && pwd -P)"'
   explicit_glaze_dir = '-Dglaze_DIR="$FORGE_GLAZE_DIR"'
   shared_dependency_prefixes = (
      'CMAKE_PREFIX_PATH=$(brew --prefix);$(brew --prefix boost);'
      '$(brew --prefix libngtcp2);$(brew --prefix openssl@3)'
   )
   if (
      source.count(exact_glaze_config) != 3
      or source.count(resolved_glaze_dir) != 3
      or source.count(explicit_glaze_dir) != 4
      or source.count(shared_dependency_prefixes) != 3
   ):
      errors.append(
         f"{path.relative_to(root)}: every configure lane must isolate Glaze and preserve shared dependency prefixes"
      )

   for baseline, upper_bytes in (("1m", "8589934592"), ("10m", "68719476736")):
      invocation = re.compile(
         rf"--baseline {baseline}\s+--mdbx-upper-bytes {upper_bytes}\s+\\\s+--machine-label"
      )
      if invocation.search(source) is None:
         errors.append(
            f"{path.relative_to(root)}: {baseline} performance baseline must use its measured MDBX upper size"
         )

   try:
      native_acceptance = source.split("      - name: Build acceptance targets\n", 1)[1].split(
         "      - name: Run acceptance\n", 1
      )
      build_acceptance = native_acceptance[0]
      run_acceptance = native_acceptance[1].split("\n  sanitizer:\n", 1)[0]
   except IndexError:
      errors.append(f"{path.relative_to(root)}: cannot locate native acceptance steps")
      return

   for required_target in ("test_forge_package_chain_api_component", "test_forge_package_db_mdbx_component"):
      if required_target not in build_acceptance:
         errors.append(f"{path.relative_to(root)}: acceptance build is missing {required_target}")

   for required_test in (
      "test_forge_structure",
      "test_forge_vendor_compile_policy",
      "test_forge_vendor_compile_policy_multi_config",
      "test_forge_package_chain_api_component",
      "test_forge_package_db_mdbx_component",
      "test_forge_package_explicit_glaze_dir",
   ):
      if required_test not in run_acceptance:
         errors.append(f"{path.relative_to(root)}: acceptance test run is missing {required_test}")


def check_mdbx_module_boundary(root: Path, errors: list[str]) -> None:
   component = root / "libraries" / "db" / "mdbx"
   if not component.exists():
      return

   legacy_header = component / "details" / "error.hxx"
   if legacy_header.exists():
      errors.append(
         f"{legacy_header.relative_to(root)}: MDBX error declarations must use a private module partition"
      )

   partition = component / "include" / "forge" / "db" / "mdbx" / "error.cppm"
   if not partition.exists():
      errors.append(f"{partition.relative_to(root)}: MDBX error module partition is missing")
      return

   source = partition.read_text(errors="ignore")
   declaration = "export module forge.db.mdbx.driver:error;"
   if declaration not in source:
      errors.append(f"{partition.relative_to(root)}: expected private partition {declaration}")
   include_position = source.find("#include <string_view>")
   declaration_position = source.find(declaration)
   if include_position < 0 or declaration_position < 0 or include_position > declaration_position:
      errors.append(
         f"{partition.relative_to(root)}: string_view must be included in the global module fragment"
      )

   for implementation in sorted(component.glob("*.cpp")):
      implementation_source = implementation.read_text(errors="ignore")
      if "require_mdbx_success(" in implementation_source and "import :error;" not in implementation_source:
         errors.append(
            f"{implementation.relative_to(root)}: MDBX error helpers must come from the private module partition"
         )


def check_contract_sdk_components(root: Path, errors: list[str]) -> None:
   path = root / "guest" / "CMakeLists.txt"
   if not path.exists():
      return

   contract_include = root / "guest" / "libraries" / "contract" / "include" / "forge" / "contract"
   nested_modules = sorted(
      path for path in contract_include.rglob("*.cppm") if path.parent != contract_include
   )
   if nested_modules:
      rendered = ", ".join(str(path.relative_to(root)) for path in nested_modules)
      errors.append("guest contract modules must use a flat public include layout: " + rendered)

   source_c_headers = sorted(contract_include.glob("*.h"))
   if source_c_headers:
      rendered = ", ".join(str(header.relative_to(root)) for header in source_c_headers)
      errors.append(
         "generated Contract SDK C ABI headers must live outside library source include: " + rendered
      )

   eosio_include = root / "guest" / "libraries" / "eosio" / "include" / "eosio"
   for header in sorted(eosio_include.glob("*.hpp")):
      source = header.read_text(errors="ignore")
      if "boost/pfr" in source or "boost::pfr" in source:
         errors.append(
            f"{header.relative_to(root)}: EOSIO veneer must delegate aggregate serialization to forge.raw"
         )

   types_template = root / "guest" / "cmake" / "types.h.in"
   if not types_template.exists():
      errors.append("guest/cmake/types.h.in: generated Contract SDK C ABI types template is missing")

   source = path.read_text(errors="ignore")
   for required in (
      "-DCMAKE_C_FLAGS=${_forge_contract_llvm_path_map_flags}",
      "-DCMAKE_CXX_FLAGS=${_forge_contract_llvm_path_map_flags}",
      "-DCMAKE_C_FLAGS=${_forge_contract_wasm_path_map_flags}",
      "-DCMAKE_CXX_FLAGS=${_forge_contract_wasm_path_map_flags}",
      "-DCMAKE_ASM_FLAGS=${_forge_contract_wasm_path_map_flags}",
      "-DCMAKE_C_FLAGS=${_forge_contract_path_map_flags}",
      "-DCMAKE_CXX_FLAGS=${_forge_contract_path_map_flags}",
   ):
      if required not in source:
         errors.append(
            f"{path.relative_to(root)}: release SDK sub-builds must preserve path mapping: {required}"
         )

   libraries_cmake = (
      root / "guest" / "cmake" / "ForgeContractLibraries.cmake"
   ).read_text(errors="ignore")
   for required in (
      '"-ffile-prefix-map=${_product_source_root}=./source"',
      '"-fdebug-prefix-map=${_product_source_root}=./source"',
      '"-ffile-prefix-map=${CMAKE_BINARY_DIR}=./build"',
      '"-fdebug-prefix-map=${CMAKE_BINARY_DIR}=./build"',
      "_forge_contract_freeze_guest_target(",
      "FORGE_CONTRACT_SOURCE_ROOT",
   ):
      if required not in libraries_cmake:
         errors.append(
            "guest/cmake/ForgeContractLibraries.cmake: guest targets must share "
            f"project-wide path mapping: {required}"
         )
   for forbidden in (
      '"-ffile-prefix-map=${_source_dir}=./source"',
      '"-fdebug-prefix-map=${_source_dir}=./source"',
      '"-ffile-prefix-map=${_binary_dir}=./build"',
      '"-fdebug-prefix-map=${_binary_dir}=./build"',
   ):
      if forbidden in libraries_cmake:
         errors.append(
            "guest/cmake/ForgeContractLibraries.cmake: target-local path mapping "
            f"creates incompatible CMake 4.4 module variants: {forbidden}"
         )

   root_cmake = (root / "CMakeLists.txt").read_text(errors="ignore")
   for required in (
      '"$<BUILD_INTERFACE:-ffile-prefix-map=${_forge_contract_host_source_root}=.>"',
      '"$<BUILD_INTERFACE:-fdebug-prefix-map=${_forge_contract_host_source_root}=.>"',
      '"$<BUILD_INTERFACE:-ffile-prefix-map=${CMAKE_BINARY_DIR}=./build>"',
      '"$<BUILD_INTERFACE:-fdebug-prefix-map=${CMAKE_BINARY_DIR}=./build>"',
   ):
      if required not in root_cmake:
         errors.append(
            f"CMakeLists.txt: contract-sdk-host must map source and build paths: {required}"
         )

   tools_project = re.search(
      r"ExternalProject_Add\(\s*forge_contract_tools(?P<body>.*?)\n\s*\)", source, re.DOTALL
   )
   if (
      tools_project is None
      or "-DCMAKE_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}" not in tools_project.group("body")
   ):
      errors.append(
         f"{path.relative_to(root)}: release tools must inherit the SDK install libdir"
      )

   try:
      developer_profile = source.split(
         'else()\n   find_package(Clang 22.1 CONFIG REQUIRED)', 1
      )[1].split(
         'endif()\n\nset(_forge_contract_input_sysroot', 1
      )[0]
   except IndexError:
      errors.append(f"{path.relative_to(root)}: cannot locate developer Contract SDK profile")
      return

   for component in (
      "tooling_abi",
      "tooling_attributes",
      "tooling_validation",
      "tooling_manifest",
   ):
      if developer_profile.count(component) != 2:
         errors.append(
            f"{path.relative_to(root)}: developer Contract SDK must request {component} "
            "with and without an explicit Forge_DIR"
         )


def check_eosio_veneer(root: Path, errors: list[str]) -> None:
   path = root / "guest" / "libraries" / "eosio" / "include" / "eosio" / "dispatcher.hpp"
   if not path.exists():
      return

   source = path.read_text(errors="ignore")
   for forbidden in ("switch (action)", "execute_action<"):
      if forbidden in source:
         errors.append(f"{path.relative_to(root)}: EOSIO veneer must not own dispatcher algorithms")
   if "::forge::contract::dispatch(" not in source:
      errors.append(f"{path.relative_to(root)}: EOSIO dispatcher must delegate to forge.contract.dispatcher")

   generator = root / "libraries" / "tooling" / "abi" / "generator.cpp"
   generated_source = generator.read_text(errors="ignore")
   for forbidden in ('output << "   switch (action)',):
      if forbidden in generated_source:
         errors.append(
            f"{generator.relative_to(root)}: generated dispatcher must delegate to forge.contract.dispatcher"
         )
   if 'forge::contract::dispatch(name{receiver}' not in generated_source:
      errors.append(
         f"{generator.relative_to(root)}: generated dispatcher does not delegate to forge.contract.dispatcher"
      )

   asset = root / "guest" / "libraries" / "eosio" / "include" / "eosio" / "asset.hpp"
   if asset.exists():
      asset_source = asset.read_text(errors="ignore")
      for forbidden in ("struct asset", "struct extended_asset", "raw_pack(", "raw_unpack("):
         if forbidden in asset_source:
            errors.append(
               f"{asset.relative_to(root)}: EOSIO asset veneer must not own {forbidden.rstrip('(')}"
            )

   name = root / "guest" / "libraries" / "eosio" / "include" / "eosio" / "name.hpp"
   if name.exists():
      name_source = name.read_text(errors="ignore")
      for forbidden in ("raw_pack(", "raw_unpack("):
         if forbidden in name_source:
            errors.append(
               f"{name.relative_to(root)}: EOSIO name veneer must not own {forbidden.rstrip('(')}"
            )


def check_contract_sdk_architecture(root: Path, errors: list[str]) -> None:
   forbidden_paths = (
      root / "guest" / "cmake" / "ForgeContractGraph.cmake",
      root / "guest" / "build" / "CMakeLists.txt",
      root / "libraries" / "contract" / "graph",
      root / "tests" / "package_contract_graph_component",
   )
   for path in forbidden_paths:
      if path.exists():
         errors.append(
            f"{path.relative_to(root)}: reconstructed contract graph surface is forbidden"
         )

   reverse_graph_tokens = (
      re.compile(r"\$<LINK_ONLY:"),
      re.compile(r"::@\("),
      re.compile(r"_forge_contract_guest_dependency"),
      re.compile(r"_forge_contract_collect_registry"),
      re.compile(r"\bFORGE_CONTRACT_OWNER_IDS\b"),
      re.compile(r"contract-graph\.json"),
      re.compile(r"forge_install_contract_(?:library|package)"),
      re.compile(r"forge_register_contract_library_targets"),
   )
   architecture_sources = (
      root / "guest" / "cmake" / "ForgeContractLibraries.cmake",
      root / "guest" / "cmake" / "ForgeContractBuild.cmake",
      root / "guest" / "cmake" / "ForgeContractFunctions.cmake",
   )
   for path in architecture_sources:
      source = path.read_text(errors="ignore")
      for token in reverse_graph_tokens:
         if token.search(source):
            errors.append(
               f"{path.relative_to(root)}: Contract SDK must not reverse-parse the native CMake graph ({token.pattern})"
            )

   libraries_source = architecture_sources[0].read_text(errors="ignore")
   for property_name in ("LINK_LIBRARIES", "INTERFACE_LINK_LIBRARIES"):
      if property_name not in libraries_source:
         errors.append(
            "guest/cmake/ForgeContractLibraries.cmake: guest target immutability "
            f"must include {property_name}"
         )
      for path in architecture_sources[1:]:
         if re.search(rf"\b{property_name}\b", path.read_text(errors="ignore")):
            errors.append(
               f"{path.relative_to(root)}: Contract SDK must not inspect "
               f"{property_name}"
            )
   if re.search(
      r"get_target_property\s*\([^)]*\b(?:INTERFACE_)?LINK_LIBRARIES\b",
      libraries_source,
      re.DOTALL,
   ):
      errors.append(
         "guest/cmake/ForgeContractLibraries.cmake: dependency properties may "
         "only participate in generic immutable-state comparison"
      )

   root_cmake = (root / "CMakeLists.txt").read_text(errors="ignore")
   if re.search(r"(?m)^(?!function\()\s*forge_target_contract_guest_component\(", root_cmake):
      errors.append("CMakeLists.txt: host targets must declare guest identities beside their own definitions")
   if "forge_register_contract_guest_component" in root_cmake:
      errors.append("CMakeLists.txt: legacy central guest-component mapping is forbidden")

   attribute_plugin = root / "tools" / "attr-plugin" / "plugin.cpp"
   attribute_source = attribute_plugin.read_text(errors="ignore")
   for token in ("forge.contract.graph", "dependency_scope", "source_graph"):
      if token in attribute_source:
         errors.append(
            f"{attribute_plugin.relative_to(root)}: attribute plugin must not own contract graph policy ({token})"
         )

   guest_codec = root / "guest" / "libraries" / "codec"
   if guest_codec.exists():
      errors.append("guest/libraries/codec: guest codec forwarding family is forbidden")

   forbidden_modules = (
      "forge.core.encoding",
      "forge.crypto.base64",
      "forge.crypto.base58",
      "forge.crypto.hex",
      "forge.contract.base64",
   )
   for path in source_files(root, SCAN_ROOTS):
      source = path.read_text(errors="ignore")
      for module in forbidden_modules:
         if module in source:
            errors.append(f"{path.relative_to(root)}: removed codec module {module} is forbidden")
      for shim in ("public_key_shim", "signature_shim", "private_key_shim"):
         if shim in source:
            errors.append(f"{path.relative_to(root)}: removed asymmetric shim {shim} is forbidden")

   asymmetric_value = (
      root
      / "libraries"
      / "crypto"
      / "asymmetric"
      / "include"
      / "forge"
      / "crypto"
      / "asymmetric"
      / "values.cppm"
   )
   asymmetric_source = asymmetric_value.read_text(errors="ignore")
   if "FORGE_CONTRACT_GUEST" in asymmetric_source:
      errors.append(f"{asymmetric_value.relative_to(root)}: asymmetric values must not have host/guest definitions")

   duplicate_value_roots = (root / "libraries" / "chain" / "protocol", root / "guest" / "libraries")
   duplicate_value = re.compile(r"\b(?:class|struct)\s+(?:public_key|signature)\b|\busing\s+(?:public_key|signature)\s*=\s*std::variant")
   for value_root in duplicate_value_roots:
      for path in source_files(root, (str(value_root.relative_to(root)),)):
         if duplicate_value.search(path.read_text(errors="ignore")):
            errors.append(f"{path.relative_to(root)}: asymmetric values belong to forge.crypto.asymmetric.values")

   contract = root / "guest" / "libraries" / "contract"
   include = contract / "include" / "forge" / "contract"
   implementation_units = {
      "action",
      "authorization",
      "bitset",
      "call",
      "compatibility_asset",
      "crypto",
      "crypto_bls_ext",
      "crypto_ext",
      "deferred_transaction",
      "dispatcher",
      "instant_finality",
      "intrinsics",
      "multi_index",
      "print",
      "privileged",
      "producer_schedule",
      "rope",
      "system",
      "transaction",
   }
   for stem in sorted(implementation_units):
      if not (include / f"{stem}.cppm").exists() or not (contract / f"{stem}.cpp").exists():
         errors.append(f"guest contract implementation unit {stem} must have an exact .cppm/.cpp pair")

   header_only = {
      "binary_extension",
      "compatibility_name",
      "contract",
      "datastream",
      "fixed_bytes",
      "ignore",
      "key",
      "powers",
      "singleton",
      "string",
      "varint",
   }
   for stem in sorted(header_only):
      if (contract / f"{stem}.cpp").exists():
         errors.append(f"guest contract header-only module {stem} must not own a .cpp")

   moved_records = (
      "code_hash_result",
      "blockchain_parameters",
      "kv_parameters",
      "finalizer_authority",
      "finalizer_policy",
      "call_data_header",
   )
   for path in sorted(include.glob("*.cppm")):
      source = path.read_text(errors="ignore")
      for record in moved_records:
         if re.search(rf"\bstruct\s+{record}\b", source):
            errors.append(f"{path.relative_to(root)}: {record} belongs to forge.chain.protocol")


def check_crypto_family(root: Path, files: list[Path], errors: list[str]) -> None:
   leaf_namespaces = {
      "asymmetric",
      "bls",
      "bn256",
      "core",
      "digest",
      "keystore",
      "math",
      "pki",
      "signer",
      "symmetric",
   }
   forbidden_modules = (
      "forge.crypto.types",
      "forge.crypto.secret_bytes",
      "forge.crypto.random",
      "forge.crypto.sha1",
      "forge.crypto.sha224",
      "forge.crypto.sha256",
      "forge.crypto.sha3",
      "forge.crypto.sha512",
      "forge.crypto.ripemd160",
      "forge.crypto.blake2",
      "forge.crypto.hmac",
      "forge.crypto.packhash",
      "forge.crypto.aes",
      "forge.crypto.chacha20_poly1305",
      "forge.crypto.kdf",
      "forge.crypto.asymmetric.value",
      "forge.crypto.p256",
      "forge.crypto.secp256k1",
      "forge.crypto.ed25519",
      "forge.crypto.rsa",
      "forge.crypto.webauthn",
      "forge.crypto.x25519",
      "forge.crypto.der",
      "forge.crypto.pem",
      "forge.crypto.x509",
      "forge.crypto.bigint",
      "forge.crypto.modular_arithmetic",
      "forge.crypto.base32",
      "forge.crypto.city",
   )
   root_namespace = re.compile(r"^(?:export\s+)?namespace\s+forge::crypto\s*\{")

   for path in files:
      relative = path.relative_to(root)
      for line_number, line in enumerate(path.read_text(errors="ignore").splitlines(), 1):
         if root_namespace.match(line.strip()):
            errors.append(
               f"{relative}:{line_number}: forge::crypto is a grouping namespace; "
               "public symbols must belong to a Crypto leaf"
            )
         for match in re.finditer(r"\bforge::crypto::([A-Za-z_][A-Za-z0-9_]*)", line):
            owner = match.group(1)
            if owner not in leaf_namespaces:
               errors.append(
                  f"{relative}:{line_number}: forge::crypto::{owner} bypasses the Crypto leaf namespace"
               )
         for module in forbidden_modules:
            if re.search(rf"\b{re.escape(module)}\b", line):
               errors.append(f"{relative}:{line_number}: removed Crypto module {module} is forbidden")

   cmake_files = [root / "CMakeLists.txt", root / "cmake" / "ForgeConfig.cmake.in"]
   cmake_files.extend(root.glob("libraries/**/CMakeLists.txt"))
   cmake_files.extend(root.glob("plugins/**/CMakeLists.txt"))
   cmake_files.extend(root.glob("tests/**/CMakeLists.txt"))
   cmake_files.extend(root.glob("guest/**/CMakeLists.txt"))
   cmake_files.extend(root.glob("guest/cmake/**/*.cmake"))
   cmake_files.extend(root.glob("guest/cmake/**/*.cmake.in"))
   for path in sorted(set(cmake_files)):
      source = path.read_text(errors="ignore")
      if re.search(r"\bforge_crypto\b", source):
         errors.append(f"{path.relative_to(root)}: removed Crypto aggregate target is forbidden")
      for package in re.finditer(
         r"\bfind_package\s*\(\s*Forge\b(?P<arguments>[^)]*)\)",
         source,
         flags=re.IGNORECASE | re.DOTALL,
      ):
         arguments = {
            value.casefold()
            for token in re.findall(r'"[^"]*"|\S+', package.group("arguments"))
            for value in token.strip('"').split(";")
         }
         if "crypto" in arguments:
            errors.append(f"{path.relative_to(root)}: removed Crypto package component is forbidden")

   removed_paths = (
      root / "libraries" / "crypto" / "include",
      root / "libraries" / "crypto" / "base32.cpp",
      root / "libraries" / "crypto" / "city.cpp",
      root / "libraries" / "crypto" / "city_crc.cpp",
   )
   for path in removed_paths:
      if path.exists():
         errors.append(f"{path.relative_to(root)}: removed monolithic Crypto path is forbidden")


def check_auth_chain_signer_boundaries(root: Path, errors: list[str]) -> None:
   removed_plugin = root / "plugins" / "crypto" / "signer"
   if removed_plugin.exists():
      errors.append("plugins/crypto/signer: removed signer plugin must not return")

   auth_root = root / "libraries" / "auth"
   for path in source_files(root, (str(auth_root.relative_to(root)),)):
      source = path.read_text(errors="ignore")
      for line_number, line in enumerate(source.splitlines(), 1):
         if re.search(r"^\s*(?:export\s+)?import\s+forge\.chain\.", line):
            errors.append(f"{path.relative_to(root)}:{line_number}: client auth must not import Chain")
         if re.search(r"^\s*(?:export\s+)?import\s+forge\.plugins\.", line):
            errors.append(f"{path.relative_to(root)}:{line_number}: client auth must not import plugins")

   signer_root = root / "plugins" / "chain" / "signer"
   forbidden_signer_imports = re.compile(
      r"^\s*(?:export\s+)?import\s+forge\.(?:net\.|api\.(?:http|p2p)\.|plugins\.(?:http|p2p)\.)"
   )
   for path in source_files(root, (str(signer_root.relative_to(root)),)):
      source = path.read_text(errors="ignore")
      for line_number, line in enumerate(source.splitlines(), 1):
         if forbidden_signer_imports.search(line):
            errors.append(
               f"{path.relative_to(root)}:{line_number}: Chain signer must not import transport plugins"
            )


def check_modules(root: Path, files: list[Path], errors: list[str]) -> None:
   declarations: dict[str, list[tuple[Path, int]]] = defaultdict(list)
   imports: list[tuple[str, Path, int]] = []

   for path in files:
      relative = path.relative_to(root)
      source_lines = path.read_text(errors="ignore").splitlines()
      unit_name = next((match.group(1) for line in source_lines if (match := MODULE_UNIT.match(line))), None)
      unit_primary = unit_name.split(":", 1)[0] if unit_name else None
      seen_imports: dict[str, int] = {}
      seen_includes: dict[tuple[str, tuple[tuple[int, int], ...]], int] = {}
      conditional_stack: list[list[int]] = []
      next_conditional = 0
      named_module_declared = False

      for line_number, line in enumerate(source_lines, 1):
         if CONDITIONAL_START.match(line):
            next_conditional += 1
            conditional_stack.append([next_conditional, 0])
         elif CONDITIONAL_BRANCH.match(line) and conditional_stack:
            conditional_stack[-1][1] += 1
         elif CONDITIONAL_END.match(line) and conditional_stack:
            conditional_stack.pop()

         declaration = MODULE_DECLARATION.match(line)
         if declaration:
            declarations[declaration.group(1)].append((relative, line_number))

         if MODULE_UNIT.match(line):
            named_module_declared = True

         imported = MODULE_IMPORT.match(line)
         if imported:
            name = imported.group(1)
            if name.startswith(":"):
               if unit_primary is None:
                  errors.append(f"{relative}:{line_number}: relative import has no owning module")
                  continue
               name = f"{unit_primary}{name}"
            imports.append((name, relative, line_number))
            if name in seen_imports:
               errors.append(
                  f"{relative}:{line_number}: duplicate import {name} "
                  f"(first at line {seen_imports[name]})"
               )
            else:
               seen_imports[name] = line_number

         included = INCLUDE.match(line)
         if included:
            if path.suffix == ".cppm" and named_module_declared and included.group(1).startswith("<"):
               errors.append(
                  f"{relative}:{line_number}: system header include must stay in the global module fragment"
               )
            context = tuple((block, branch) for block, branch in conditional_stack)
            key = (included.group(1), context)
            if key in seen_includes:
               errors.append(
                  f"{relative}:{line_number}: duplicate include {included.group(1)} "
                  f"in the same conditional branch (first at line {seen_includes[key]})"
               )
            else:
               seen_includes[key] = line_number

         if path.suffix == ".cppm" and BROAD_EXPORT.match(line):
            errors.append(f"{relative}:{line_number}: manual broad export block is forbidden")

   for name, owners in sorted(declarations.items()):
      if len(owners) > 1:
         locations = ", ".join(f"{path}:{line}" for path, line in owners)
         errors.append(f"module {name} has multiple declarations: {locations}")

   known_modules = set(declarations)
   for name, path, line_number in imports:
      if name not in known_modules:
         errors.append(f"{path}:{line_number}: import references unknown Forge module {name}")


def main() -> int:
   if len(sys.argv) != 2:
      print("usage: check_structure.py <repository-root>", file=sys.stderr)
      return 2

   root = Path(sys.argv[1]).resolve()
   errors: list[str] = []
   files = source_files(root, SCAN_ROOTS)

   check_layout(root, errors)
   check_aggregates(root, errors)
   check_tls_context_ownership(root, errors)
   check_http_cookie_asset_boundaries(root, errors)
   check_auth_pairing_boundaries(root, errors)
   check_auth_session_boundaries(root, errors)
   check_auth_http_boundaries(root, errors)
   check_p2p_scoped_peer_mutations(root, errors)
   check_pairing(root, errors)
   check_vm_wasm_interpret_boundaries(root, errors)
   check_vm_wasm_interpret_identities(root, files, errors)
   check_contract_tooling_boundaries(root, files, errors)
   check_plugin_impl_ownership(root, errors)
   check_chain_savanna_boundaries(root, errors)
   check_bls_value_ownership(root, files, errors)
   check_chain_api_shape(root, errors)
   check_chain_audited_api_workflow(root, errors)
   check_mdbx_module_boundary(root, errors)
   check_contract_sdk_workflow(root, errors)
   check_contract_sdk_components(root, errors)
   check_eosio_veneer(root, errors)
   check_contract_sdk_architecture(root, errors)
   check_crypto_family(root, files, errors)
   check_auth_chain_signer_boundaries(root, errors)
   check_modules(root, files, errors)

   if errors:
      for error in sorted(set(errors)):
         print(error, file=sys.stderr)
      return 1

   print(f"Forge structure check passed ({len(files)} first-party source files scanned)")
   return 0


if __name__ == "__main__":
   raise SystemExit(main())
