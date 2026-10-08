#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

import forge.chain.api.abi;
import forge.codec.json;

// Consumes Abigen output without importing any of the fixture's C++ payload types.
int main(int argc, char** argv) {
   try {
      if (argc != 3) {
         throw std::runtime_error{"usage: test_forge_abi_metadata_artifact <ABI> <metadata>"};
      }
      namespace protocol = forge::chain::protocol;
      namespace json = forge::codec::json;
      const auto abi = json::load<protocol::abi_def>(argv[1]);
      const auto metadata = json::load<protocol::abi_metadata>(argv[2], {
          .unknown_fields = json::unknown_field_policy::error,
          .described_records = json::described_record_policy::exact});
      const auto input = json::read_value(
          R"({"child":{"value":"tall","movement":"backward"},"maybe":"circle","list":["circle","tall"],"fixed":["tall","circle"],"selection":["shape_alias","tall"]})");
      if (!abi.ok() || !metadata.ok() || !input.ok() || metadata.value.roots.size() != 1U ||
          metadata.value.roots.front().cpp_type != "example::request") {
         throw std::runtime_error{"invalid generated ABI/metadata fixture"};
      }
      const auto& type = metadata.value.roots.front().type;
      const auto bytes = forge::chain::api::abi_json_to_bin(abi.value, metadata.value, type, input.value);
      const auto golden = std::vector<std::uint8_t>{2, 253, 255, 1, 0, 2, 0, 2, 2, 0, 0, 2};
      if (bytes != golden || forge::chain::api::abi_bin_to_json(abi.value, metadata.value, type, bytes) != input.value) {
         throw std::runtime_error{"generated root metadata did not preserve the raw/JSON golden"};
      }
      std::cout << "generated ABI roots and enum metadata preserve dynamic raw/JSON bytes\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
}
