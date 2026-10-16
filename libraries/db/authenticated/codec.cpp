module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

module forge.db.authenticated.codec;

import forge.db.authenticated.exceptions;
import forge.raw.raw;

namespace forge::db::authenticated {
namespace {

constexpr auto minimum_point_step_wire_bytes = std::size_t{49};
constexpr auto minimum_range_node_wire_bytes = std::size_t{14};
constexpr auto bounded_initial_reserve = std::size_t{1'024};
constexpr auto root_wire_bytes = std::size_t{88};

[[noreturn]] void reject(std::string_view message) {
   FORGE_THROW_EXCEPTION(exceptions::invalid_proof, message);
}

[[noreturn]] void reject_limit(std::string_view message) {
   FORGE_THROW_EXCEPTION(exceptions::proof_limit_exceeded, message);
}

std::size_t checked_add(std::size_t left, std::size_t right) {
   if (left > std::numeric_limits<std::size_t>::max() - right) {
      reject_limit("authenticated proof wire size overflows");
   }
   return left + right;
}

std::size_t varuint32_wire_size(std::size_t value) {
   if (value > std::numeric_limits<std::uint32_t>::max()) {
      reject_limit("authenticated proof count exceeds uint32 framing");
   }
   auto result = std::size_t{1};
   while (value >= 0x80U) {
      value >>= 7U;
      ++result;
   }
   return result;
}

std::size_t byte_array_wire_size(std::size_t size) {
   if (size > max_framed_bytes) {
      reject_limit("authenticated proof byte array exceeds uint32 framing");
   }
   return checked_add(varuint32_wire_size(size), size);
}

std::size_t optional_byte_array_wire_size(const std::optional<bytes>& value) {
   return value ? checked_add(1U, byte_array_wire_size(value->size())) : 1U;
}

bytes as_bytes(const std::vector<std::uint8_t>& value) {
   auto result = bytes{};
   result.reserve(value.size());
   for (const auto byte : value) {
      result.push_back(static_cast<std::byte>(byte));
   }
   return result;
}

class bounded_stream {
 public:
   explicit bounded_stream(std::span<const std::byte> value) : value_{value} {}

   std::size_t read(char* destination, std::size_t size) {
      if (size > remaining()) {
         reject("authenticated proof payload is truncated");
      }
      std::memcpy(destination, value_.data() + position_, size);
      position_ += size;
      return size;
   }

   bool get(char& value) {
      read(&value, 1);
      return true;
   }

   [[nodiscard]] std::size_t remaining() const noexcept {
      return value_.size() - position_;
   }

 private:
   std::span<const std::byte> value_;
   std::size_t position_ = 0;
};

template <typename T> T read_scalar(bounded_stream& stream) {
   auto value = T{};
   forge::raw::unpack(stream, value);
   return value;
}

std::uint32_t read_count(bounded_stream& stream) {
   auto value = std::uint32_t{};
   auto shift = std::uint32_t{};
   auto octets = std::uint32_t{};
   auto byte = std::uint8_t{};
   do {
      if (octets == 5U) {
         reject("authenticated proof contains an oversized varuint");
      }
      byte = read_scalar<std::uint8_t>(stream);
      if (shift == 28U && (byte & 0xf0U) != 0U) {
         reject("authenticated proof varuint overflows uint32");
      }
      value |= static_cast<std::uint32_t>(byte & 0x7fU) << shift;
      shift += 7U;
      ++octets;
   } while ((byte & 0x80U) != 0U);

   const auto minimum_octets = value < (1U << 7U)    ? 1U
                               : value < (1U << 14U) ? 2U
                               : value < (1U << 21U) ? 3U
                               : value < (1U << 28U) ? 4U
                                                     : 5U;
   if (octets != minimum_octets) {
      reject("authenticated proof varuint is not canonical");
   }
   return value;
}

bool read_bool(bounded_stream& stream) {
   const auto value = read_scalar<std::uint8_t>(stream);
   if (value > 1U) {
      reject("authenticated proof bool is not canonical");
   }
   return value != 0U;
}

bytes read_bytes(bounded_stream& stream, std::size_t maximum, std::string_view message) {
   const auto size = static_cast<std::size_t>(read_count(stream));
   if (size > maximum) {
      reject_limit(message);
   }
   if (size > stream.remaining()) {
      reject("authenticated proof byte array is truncated");
   }
   auto result = bytes(size);
   if (size != 0U) {
      stream.read(reinterpret_cast<char*>(result.data()), size);
   }
   return result;
}

std::optional<bytes> read_optional_bytes(bounded_stream& stream, std::size_t maximum, std::string_view message) {
   if (!read_bool(stream)) {
      return std::nullopt;
   }
   return read_bytes(stream, maximum, message);
}

root read_root(bounded_stream& stream) {
   return {
       .version = read_scalar<version_id_t>(stream),
       .state_root = read_scalar<digest>(stream),
       .state_size = read_scalar<std::uint64_t>(stream),
       .change_root = read_scalar<digest>(stream),
       .change_count = read_scalar<std::uint64_t>(stream),
   };
}

proof_leaf read_leaf(bounded_stream& stream, const limits& settings) {
   return {
       .key = read_bytes(stream, settings.max_key_bytes, "authenticated proof leaf key exceeds configured limit"),
       .value_hash = read_scalar<digest>(stream),
       .value = read_optional_bytes(stream, settings.max_value_bytes,
                                    "authenticated proof leaf value exceeds configured limit"),
   };
}

proof_branch read_branch(bounded_stream& stream, const limits& settings) {
   return {
       .height = read_scalar<std::uint16_t>(stream),
       .size = read_scalar<std::uint64_t>(stream),
       .min_key = read_bytes(stream, settings.max_key_bytes,
                             "authenticated proof branch minimum key exceeds configured limit"),
       .max_key = read_bytes(stream, settings.max_key_bytes,
                             "authenticated proof branch maximum key exceeds configured limit"),
       .separator =
           read_bytes(stream, settings.max_key_bytes, "authenticated proof branch key exceeds configured limit"),
       .left_hash = read_scalar<digest>(stream),
       .right_hash = read_scalar<digest>(stream),
   };
}

proof_sibling read_sibling(bounded_stream& stream, const limits& settings) {
   switch (read_count(stream)) {
   case 0:
      return read_leaf(stream, settings);
   case 1:
      return read_branch(stream, settings);
   default:
      reject("authenticated point proof sibling kind is invalid");
   }
}

proof_step read_step(bounded_stream& stream, const limits& settings) {
   const auto child = read_scalar<branch_side>(stream);
   if (child != branch_side::left && child != branch_side::right) {
      reject("authenticated point proof branch side is invalid");
   }
   return {
       .child = child,
       .height = read_scalar<std::uint16_t>(stream),
       .subtree_size = read_scalar<std::uint64_t>(stream),
       .min_key =
           read_bytes(stream, settings.max_key_bytes, "authenticated point proof minimum key exceeds configured limit"),
       .max_key =
           read_bytes(stream, settings.max_key_bytes, "authenticated point proof maximum key exceeds configured limit"),
       .separator =
           read_bytes(stream, settings.max_key_bytes, "authenticated point proof separator exceeds configured limit"),
       .sibling = read_sibling(stream, settings),
   };
}

void require_finished(const bounded_stream& stream) {
   if (stream.remaining() != 0U) {
      reject("authenticated proof contains trailing bytes");
   }
}

std::size_t node_wire_size(const proof_leaf& node) {
   auto result = checked_add(std::size_t{1}, byte_array_wire_size(node.key.size()));
   result = checked_add(result, digest::data_size());
   return checked_add(result, optional_byte_array_wire_size(node.value));
}

std::size_t node_wire_size(const proof_branch& node) {
   auto result = std::size_t{1U + sizeof(node.height) + sizeof(node.size)};
   result = checked_add(result, byte_array_wire_size(node.min_key.size()));
   result = checked_add(result, byte_array_wire_size(node.max_key.size()));
   result = checked_add(result, byte_array_wire_size(node.separator.size()));
   return checked_add(result, 2U * digest::data_size());
}

std::size_t node_wire_size(const range_inner& node) {
   auto result = std::size_t{1U + sizeof(node.height) + sizeof(node.size)};
   result = checked_add(result, byte_array_wire_size(node.min_key.size()));
   result = checked_add(result, byte_array_wire_size(node.max_key.size()));
   return checked_add(result, byte_array_wire_size(node.separator.size()));
}

} // namespace

bytes encode(const point_proof& value) {
   for (const auto& step : value.path) {
      if (step.sibling.valueless_by_exception()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_proof, "authenticated point proof sibling has no value");
      }
   }
   return as_bytes(forge::raw::pack(value));
}

bytes encode(const range_proof& value) {
   static_cast<void>(wire_size(value));
   return as_bytes(forge::raw::pack(value));
}

std::size_t wire_size(const checkpoint_record& value) {
   if (value.tree != proof_tree::state && value.tree != proof_tree::changes) {
      reject("authenticated checkpoint tree is invalid");
   }
   if (value.node.valueless_by_exception()) {
      reject("authenticated checkpoint node has no value");
   }
   return std::visit(
       [](const auto& node) {
          using node_type = std::remove_cvref_t<decltype(node)>;
          if constexpr (std::same_as<node_type, proof_leaf>) {
             if (!node.value) {
                reject("authenticated checkpoint leaf requires its value");
             }
          }
          // The existing proof node codec owns the SAME bounded byte/count framing.
          return checked_add(2U + digest::data_size(), node_wire_size(node));
       },
       value.node);
}

bytes encode(const checkpoint_record& value) {
   const auto expected = wire_size(value);
   auto result = bytes{};
   result.reserve(expected);
   result.push_back(static_cast<std::byte>(checkpoint_format_version));
   const auto payload = forge::raw::pack(value);
   for (const auto byte : payload) {
      result.push_back(static_cast<std::byte>(byte));
   }
   return result;
}

checkpoint_record decode_checkpoint_record(std::span<const std::byte> value, const limits& node_bounds,
                                            const checkpoint_limits& checkpoint_bounds) {
   if (!limits_are_valid(node_bounds) || !checkpoint_limits_are_valid(checkpoint_bounds) ||
       value.size() > checkpoint_bounds.max_frame_bytes || value.size() > checkpoint_bounds.max_encoded_bytes) {
      reject_limit("authenticated checkpoint frame exceeds configured bounds");
   }
   auto stream = bounded_stream{value};
   if (read_scalar<std::uint8_t>(stream) != checkpoint_format_version) {
      reject("authenticated checkpoint format is unsupported");
   }
   auto result = checkpoint_record{};
   result.tree = read_scalar<proof_tree>(stream);
   if (result.tree != proof_tree::state && result.tree != proof_tree::changes) {
      reject("authenticated checkpoint tree is invalid");
   }
   result.hash = read_scalar<digest>(stream);
   // Bound a value before read_leaf allocates, not after constructing the DTO.
   auto leaf_bounds = node_bounds;
   leaf_bounds.max_value_bytes = static_cast<std::size_t>(
       std::min<std::uint64_t>(node_bounds.max_value_bytes, checkpoint_bounds.max_value_bytes));
   result.node = read_sibling(stream, leaf_bounds);
   require_finished(stream);
   static_cast<void>(wire_size(result));
   return result;
}

std::size_t wire_size(const point_proof& value) {
   for (const auto& step : value.path) {
      if (step.sibling.valueless_by_exception()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_proof, "authenticated point proof sibling has no value");
      }
   }
   return forge::raw::pack_size(value);
}

std::size_t wire_size(const range_proof& value) {
   auto result = root_wire_bytes + 1U;
   result = checked_add(result, optional_byte_array_wire_size(value.request.lower));
   result = checked_add(result, optional_byte_array_wire_size(value.request.upper));
   result = checked_add(result, sizeof(value.request.limit) + 2U);
   result = checked_add(result, varuint32_wire_size(value.nodes.size()));
   for (const auto& node : value.nodes) {
      result = checked_add(result, wire_size(node));
   }
   return result;
}

std::size_t wire_size(const range_proof_node& value) {
   if (value.valueless_by_exception()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_proof, "authenticated range proof node has no value");
   }
   return std::visit(
       [](const auto& node) { return node_wire_size(node); },
       value);
}

void require_wire_budget(std::size_t size, const limits& settings) {
   if (!limits_are_valid(settings)) {
      reject_limit("authenticated proof limits are invalid");
   }
   if (size > settings.max_proof_bytes) {
      reject_limit("authenticated proof exceeds configured wire byte limit");
   }
}

point_proof decode_point(std::span<const std::byte> value, const limits& settings) {
   require_wire_budget(value.size(), settings);
   auto stream = bounded_stream{value};
   auto result = point_proof{};
   result.anchor = read_root(stream);
   result.key =
       read_bytes(stream, settings.max_key_bytes, "authenticated point proof query key exceeds configured limit");
   if (read_bool(stream)) {
      result.terminal = read_leaf(stream, settings);
   }
   const auto path_size = read_count(stream);
   if (path_size > settings.max_proof_depth || path_size > settings.max_proof_nodes ||
       (path_size != 0U && path_size > (static_cast<std::size_t>(settings.max_proof_nodes) - 1U) / 2U)) {
      reject_limit("authenticated point proof exceeds configured node limits");
   }
   if (path_size > stream.remaining() / minimum_point_step_wire_bytes) {
      reject("authenticated point proof path count exceeds remaining payload");
   }
   result.path.reserve(std::min<std::size_t>(path_size, bounded_initial_reserve));
   for (auto index = std::uint32_t{}; index < path_size; ++index) {
      result.path.push_back(read_step(stream, settings));
   }
   require_finished(stream);
   return result;
}

range_proof decode_range(std::span<const std::byte> value, const limits& settings) {
   require_wire_budget(value.size(), settings);
   auto stream = bounded_stream{value};
   auto result = range_proof{};
   result.anchor = read_root(stream);
   result.tree = read_scalar<proof_tree>(stream);
   if (result.tree != proof_tree::state && result.tree != proof_tree::changes) {
      reject("authenticated range proof tree is invalid");
   }
   result.request = {
       .lower = read_optional_bytes(stream, settings.max_key_bytes,
                                    "authenticated range lower key exceeds configured limit"),
       .upper = read_optional_bytes(stream, settings.max_key_bytes,
                                    "authenticated range upper key exceeds configured limit"),
       .limit = read_scalar<std::uint32_t>(stream),
       .include_values = read_bool(stream),
       .reverse = read_bool(stream),
   };
   const auto node_count = read_count(stream);
   if (node_count > settings.max_proof_nodes) {
      reject_limit("authenticated range proof exceeds configured node limit");
   }
   if (node_count > stream.remaining() / minimum_range_node_wire_bytes) {
      reject("authenticated range proof node count exceeds remaining payload");
   }
   result.nodes.reserve(std::min<std::size_t>(node_count, bounded_initial_reserve));
   for (auto index = std::uint32_t{}; index < node_count; ++index) {
      switch (read_count(stream)) {
      case 0:
         result.nodes.emplace_back(read_branch(stream, settings));
         break;
      case 1:
         result.nodes.emplace_back(read_leaf(stream, settings));
         break;
      case 2:
         result.nodes.emplace_back(range_inner{
             .height = read_scalar<std::uint16_t>(stream),
             .size = read_scalar<std::uint64_t>(stream),
             .min_key = read_bytes(stream, settings.max_key_bytes,
                                   "authenticated range proof minimum key exceeds configured limit"),
             .max_key = read_bytes(stream, settings.max_key_bytes,
                                   "authenticated range proof maximum key exceeds configured limit"),
             .separator = read_bytes(stream, settings.max_key_bytes,
                                     "authenticated range proof separator exceeds configured limit"),
         });
         break;
      default:
         reject("authenticated range proof node kind is invalid");
      }
   }
   require_finished(stream);
   return result;
}

} // namespace forge::db::authenticated
