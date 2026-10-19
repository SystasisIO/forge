#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import forge.chain.api.abi;
import forge.chain.protocol.values;
import forge.codec.json;
import forge.crypto.digest.sha256;
import forge.execution.wasm.compiled_module;
import forge.raw.raw;
import forge.schema.constraint;
import forge.variant.schema;

namespace {
void require(bool value, std::string_view message) {
   if (!value) {
      throw std::runtime_error{std::string{message}};
   }
}

std::vector<std::uint8_t> load(const std::filesystem::path& path) {
   require(std::filesystem::file_size(path) <= 1024 * 1024, "oversized fixture artifact");
   std::ifstream file{path, std::ios::binary};
   require(file.good(), "missing fixture artifact");
   return {std::istreambuf_iterator<char>{file}, {}};
}
} // namespace

// Intentionally no fixture/product module or C++ claims declaration here.
int run(int argc, char** argv) {
   if (argc != 6) {
      return 2;
   }
   namespace api = forge::chain::api;
   namespace chain = forge::chain::protocol;
   namespace json = forge::codec::json;
   namespace execution = forge::execution::wasm;
   const auto options = json::read_options{.unknown_fields = json::unknown_field_policy::error,
                                          .described_records = json::described_record_policy::exact,
                                          .max_bytes = 1024 * 1024};
   const auto binding = json::load_value(argv[5], options);
   require(binding.ok() && binding.value.is_object(), "invalid local artifact binding");
   const auto& expected = binding.value.get_object();
   require(expected["version"].as_uint64() == 1 && expected["root"].get_string() == "fixture::grant" &&
           expected["schema"].get_string() == "https://schemas.example.test/grant/v1",
           "unsupported artifact binding");
   for (const auto& item : {std::pair{2, "abi"}, std::pair{3, "metadata"}, std::pair{4, "constraints"}}) {
      const auto bytes = load(argv[item.first]);
      require(forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{bytes}).str() ==
                  expected[item.second].get_string(),
              "artifact content does not match installed binding");
   }
   const auto abi = json::load<chain::abi_def>(argv[2], options);
   const auto metadata = json::load<chain::abi_metadata>(argv[3], options);
   const auto schema_json = json::load_value(argv[4], options);
   require(abi.ok() && metadata.ok() && schema_json.ok(), "invalid fixture artifacts");
   require(metadata.value.roots.size() == 1 && metadata.value.roots.front().cpp_type == "fixture::grant",
           "wrong named ABI root");
   const auto schema = forge::schema::load_artifact(forge::variant_schema::to_input(schema_json.value));
   const auto& root = metadata.value.roots.front().type;
   const auto code = load(argv[1]);
   const auto account = chain::account_name{chain::make_name("granttest").value};
   const auto invoke = [&](std::string_view action, std::span<const std::uint8_t> arguments) {
      execution::compiled_module module{code};
      return module.invoke({account, chain::action_name{chain::make_name(action).value}, arguments}).data;
   };
   const auto wrapped = invoke("fresh", {});
   const auto raw = forge::raw::unpack_exact<forge::raw::bytes>(wrapped);
   const auto decoded = api::abi_bin_to_json(abi.value, metadata.value, root, raw);
   require(forge::variant_schema::validate(schema, decoded).empty(), "generated guest result violates schema");
   require(api::abi_json_to_bin(abi.value, metadata.value, root, decoded, {},
                                api::abi_json_scalar_policy::exact) == raw,
           "offline ABI did not preserve exact bytes");
   require(invoke("echo", raw) == raw, "guest/native Raw parity failed");
   const auto& claims = decoded.get_object()["att"].get_object()["claims"].get_object();
   require(claims["generation"].get_string() == "18446744073709551615" &&
           claims["ranges"].get_array().front().get_object()["offset"].get_string() == "9007199254740993",
           "wide decimal lost precision");
   auto corrupted = raw;
   corrupted.push_back(0);
   bool rejected = false;
   try {
      (void)api::abi_bin_to_json(abi.value, metadata.value, root, corrupted);
   } catch (const api::abi_serialization_error&) {
      rejected = true;
   }
   require(rejected, "trailing Raw bytes accepted");
   auto wrong_version = schema_json.value;
   auto wrong_object = forge::mutable_variant_object{wrong_version.get_object()};
   wrong_object.set("version", std::uint64_t{2});
   rejected = false;
   try {
      (void)forge::schema::load_artifact(forge::variant_schema::to_input(forge::variant{wrong_object}));
   } catch (const std::exception&) {
      rejected = true;
   }
   require(rejected, "unknown artifact version accepted");
   return 0;
}

int main(int argc, char** argv) {
   try {
      return run(argc, argv);
   } catch (const std::exception&) {
      std::cerr << "artifact boundary rejected\n";
      return 1;
   }
}
