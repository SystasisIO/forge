module;

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

module forge.tooling.validation.validator;

import forge.codec.json;
import forge.chain.protocol.abi;
import forge.variant.value;
import forge.vm.wasm.interpret.backend;

namespace {

namespace wasm = forge::vm::wasm::interpret;

std::string read_text(const std::filesystem::path& path) {
   auto input = std::ifstream{path, std::ios::binary};
   if (!input) {
      throw std::runtime_error{"cannot open text artifact: " + path.string()};
   }
   return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

wasm::wasm_code read_wasm(const std::filesystem::path& path) {
   auto input = std::ifstream{path, std::ios::binary | std::ios::ate};
   if (!input) {
      throw std::runtime_error{"cannot open WASM: " + path.string()};
   }
   const auto size = input.tellg();
   if (size < 0) {
      throw std::runtime_error{"cannot determine WASM size"};
   }
   auto result = wasm::wasm_code(static_cast<std::size_t>(size));
   input.seekg(0);
   if (!result.empty() && !input.read(reinterpret_cast<char*>(result.data()), size)) {
      throw std::runtime_error{"cannot read complete WASM"};
   }
   return result;
}

std::string text(const auto& bytes) {
   return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

struct intrinsic_signature {
   std::vector<wasm::value_type> parameters;
   std::optional<wasm::value_type> result;
};

wasm::value_type parse_value_type(const forge::variant& value) {
   if (!value.is_string()) {
      throw std::runtime_error{"intrinsic WASM type must be a string"};
   }
   if (value.get_string() == "i32") {
      return wasm::i32;
   }
   if (value.get_string() == "i64") {
      return wasm::i64;
   }
   if (value.get_string() == "f32") {
      return wasm::f32;
   }
   if (value.get_string() == "f64") {
      return wasm::f64;
   }
   throw std::runtime_error{"unsupported intrinsic WASM type: " + value.get_string()};
}

using intrinsic_key = std::pair<std::string, std::string>;
using intrinsic_set = std::map<intrinsic_key, intrinsic_signature>;

intrinsic_set approved_imports(const std::filesystem::path& path) {
   const auto parsed = forge::codec::json::read_value(read_text(path), {.source_name = path.string()});
   if (!parsed.ok() || !parsed.value.is_object()) {
      throw std::runtime_error{"intrinsic manifest is not valid JSON object"};
   }
   const auto& root = parsed.value.get_object();
   if (!root.contains("schema_version") || root["schema_version"].as_uint64() != 1U ||
       !root.contains("interface_version") || root["interface_version"].as_uint64() != 1U ||
       !root.contains("imports") || !root["imports"].is_array()) {
      throw std::runtime_error{"intrinsic manifest has an unsupported schema or interface version"};
   }

   auto result = intrinsic_set{};
   for (const auto& item : root["imports"].get_array()) {
      if (!item.is_object()) {
         throw std::runtime_error{"intrinsic manifest entry must be an object"};
      }
      const auto& entry = item.get_object();
      if (!entry.contains("module") || !entry["module"].is_string() || !entry.contains("import") ||
          !entry["import"].is_string() || !entry.contains("wasm_parameters") || !entry["wasm_parameters"].is_array() ||
          !entry.contains("wasm_result")) {
         throw std::runtime_error{"intrinsic manifest entry is incomplete"};
      }

      auto signature = intrinsic_signature{};
      for (const auto& parameter : entry["wasm_parameters"].get_array()) {
         signature.parameters.push_back(parse_value_type(parameter));
      }
      if (!entry["wasm_result"].is_null()) {
         signature.result = parse_value_type(entry["wasm_result"]);
      }

      const auto key = intrinsic_key{entry["module"].get_string(), entry["import"].get_string()};
      if (!result.emplace(key, std::move(signature)).second) {
         throw std::runtime_error{"intrinsic manifest contains duplicate import: " + key.first + '.' + key.second};
      }
   }
   if (result.empty()) {
      throw std::runtime_error{"intrinsic manifest contains no imports"};
   }
   return result;
}

void validate_abi(const std::filesystem::path& path) {
   const auto parsed = forge::codec::json::read_value(read_text(path), {.source_name = path.string()});
   if (!parsed.ok()) {
      throw std::runtime_error{"contract ABI is not valid JSON"};
   }
   auto abi = forge::chain::protocol::abi_def{};
   try {
      forge::chain::protocol::from_variant(parsed.value, abi);
   } catch (const std::exception& error) {
      throw std::runtime_error{"contract ABI does not match the chain ABI schema: " + std::string{error.what()}};
   }
   if (abi.version != "eosio::abi/1.2" && abi.version != "eosio::abi/1.3") {
      throw std::runtime_error{"contract ABI uses an unsupported version: " + abi.version};
   }
}

void validate_signature(wasm::prepared_module::signature_view actual, const intrinsic_signature& expected,
                        const intrinsic_key& key) {
   if (actual.params.size() != expected.parameters.size()) {
      throw std::runtime_error{"contract import has the wrong parameter count: " + key.first + '.' + key.second};
   }
   for (auto index = std::size_t{0}; index < expected.parameters.size(); ++index) {
      if (actual.params[index] != expected.parameters[index]) {
         throw std::runtime_error{"contract import has a wrong parameter type: " + key.first + '.' + key.second};
      }
   }
   if (actual.results.size() != (expected.result.has_value() ? 1U : 0U) ||
       (expected.result.has_value() && actual.results.front() != *expected.result)) {
      throw std::runtime_error{"contract import has the wrong return type: " + key.first + '.' + key.second};
   }
}

void validate_required_export_signature(std::string_view name, wasm::prepared_module::signature_view actual) {
   if (name != "apply") {
      return;
   }

   if (actual.params.size() != 3U || actual.params[0] != wasm::i64 || actual.params[1] != wasm::i64 ||
       actual.params[2] != wasm::i64 || !actual.results.empty()) {
      throw std::runtime_error{"contract apply export has the wrong signature: expected (i64, i64, i64) -> void"};
   }
}

} // namespace

namespace forge::tooling::validation {

void validate(const request& options) {
   if (!std::filesystem::is_regular_file(options.abi) || std::filesystem::file_size(options.abi) == 0U) {
      throw std::runtime_error{"ABI is missing or empty"};
   }
   validate_abi(options.abi);

   auto code = read_wasm(options.wasm);
   using validator = wasm::backend<std::nullptr_t, wasm::null_backend, wasm::compatibility_options>;
   auto parsed = validator{code, static_cast<wasm::wasm_allocator*>(nullptr)};
   const auto& module = parsed.get_module();
   const auto approved = approved_imports(options.imports);

   for (std::size_t index = 0; index < module.imports.size(); ++index) {
      const auto& entry = module.imports[index];
      if (entry.kind != wasm::external_kind::Function) {
         throw std::runtime_error{"contract imports a non-function host object"};
      }
      const auto key = std::pair{text(entry.module_str), text(entry.field_str)};
      const auto found = approved.find(key);
      if (found == approved.end()) {
         throw std::runtime_error{"unsupported contract import: " + key.first + '.' + key.second};
      }
      validate_signature(module.function_signature(static_cast<std::uint32_t>(index)), found->second, key);
   }

   const auto required = module.get_exported_function(options.required_export);
   if (required == std::numeric_limits<std::uint32_t>::max())
      throw std::runtime_error{"required contract export is missing: " + options.required_export};
   validate_required_export_signature(options.required_export, module.function_signature(required));
}

} // namespace forge::tooling::validation
