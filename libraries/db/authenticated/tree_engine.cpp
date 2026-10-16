module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <boost/asio/awaitable.hpp>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

module forge.db.authenticated.tree_engine;

import forge.db.authenticated.exceptions;
import forge.db.authenticated.codec;
import forge.db.authenticated.hash;
import forge.db.authenticated.types;
import forge.db.core.record;

#include "details/backend_call.hxx"

namespace forge::db::authenticated::detail {

namespace {

constexpr auto node_record = std::byte{1};
constexpr auto value_record = std::byte{2};
constexpr auto version_record = std::byte{3};
constexpr auto latest_record = std::byte{4};
constexpr auto node_reference_record = std::byte{5};
constexpr auto value_reference_record = std::byte{6};
constexpr auto node_gc_record = std::byte{7};
constexpr auto value_gc_record = std::byte{8};
constexpr auto retention_guard_record = std::byte{9};
constexpr auto format_version = std::byte{4};

boost::asio::awaitable<std::optional<bytes>> read_record(const get_record_fn& get, forge::db::core::record_key key) {
   try {
      co_return co_await get(std::move(key));
   } catch (const forge::exceptions::base&) {
      throw;
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::backend_failure, "authenticated record reader failed",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::backend_failure, "authenticated record reader failed");
   }
}

boost::asio::awaitable<void> write_record(const put_record_fn& put, forge::db::core::record_key key, bytes value) {
   try {
      co_await put(std::move(key), std::move(value));
   } catch (const forge::exceptions::base&) {
      throw;
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::backend_failure, "authenticated record writer failed",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::backend_failure, "authenticated record writer failed");
   }
}

boost::asio::awaitable<void> erase_record(const erase_record_fn& erase, forge::db::core::record_key key) {
   try {
      co_await erase(std::move(key));
   } catch (const forge::exceptions::base&) {
      throw;
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::backend_failure, "authenticated record eraser failed",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::backend_failure, "authenticated record eraser failed");
   }
}

void append_u16(bytes& output, std::uint16_t value) {
   output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
   output.push_back(static_cast<std::byte>(value & 0xffU));
}

void append_u32(bytes& output, std::uint32_t value) {
   for (auto shift = 24; shift >= 0; shift -= 8) {
      output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
   }
}

void append_u64(bytes& output, std::uint64_t value) {
   for (auto shift = 56; shift >= 0; shift -= 8) {
      output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
   }
}

void append_digest(bytes& output, const digest& value) {
   const auto source = value.to_uint8_span();
   for (const auto byte : source) {
      output.push_back(static_cast<std::byte>(byte));
   }
}

void append_sized(bytes& output, std::span<const std::byte> value) {
   if (value.size() > max_framed_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated node byte array exceeds uint32 framing");
   }
   append_u32(output, static_cast<std::uint32_t>(value.size()));
   output.insert(output.end(), value.begin(), value.end());
}

void append_sized(bytes& output, std::string_view value) {
   append_sized(output, std::span<const std::byte>{reinterpret_cast<const std::byte*>(value.data()), value.size()});
}

std::size_t varuint32_wire_size(std::size_t value) {
   if (value > std::numeric_limits<std::uint32_t>::max()) {
      FORGE_THROW_EXCEPTION(exceptions::proof_limit_exceeded, "authenticated proof count exceeds uint32 framing");
   }
   auto result = std::size_t{1};
   while (value >= 0x80U) {
      value >>= 7U;
      ++result;
   }
   return result;
}

std::size_t value_wire_size(std::size_t size) {
   if (size > max_framed_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::proof_limit_exceeded, "authenticated proof value exceeds uint32 framing");
   }
   return varuint32_wire_size(size) + size;
}

void append_range_node(std::vector<range_proof_node>& output, range_proof_node node, std::size_t& wire_bytes,
                       const limits& settings) {
   if (output.size() >= settings.max_proof_nodes) {
      FORGE_THROW_EXCEPTION(exceptions::proof_limit_exceeded, "authenticated range proof exceeds node limit");
   }
   const auto old_count_bytes = varuint32_wire_size(output.size());
   const auto new_count_bytes = varuint32_wire_size(output.size() + 1U);
   const auto node_bytes = wire_size(node);
   const auto added = node_bytes + (new_count_bytes - old_count_bytes);
   if (wire_bytes > settings.max_proof_bytes || added > settings.max_proof_bytes - wire_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::proof_limit_exceeded,
                            "authenticated range proof exceeds configured wire byte limit");
   }
   output.emplace_back(std::move(node));
   wire_bytes += added;
}

class reader {
 public:
   explicit reader(const bytes& value) : value_{value} {}

   [[nodiscard]] std::byte byte() {
      require(1);
      return value_[offset_++];
   }

   [[nodiscard]] std::uint16_t u16() {
      require(2);
      auto result = std::uint16_t{};
      for (auto count = 0; count < 2; ++count) {
         result = static_cast<std::uint16_t>((result << 8U) | std::to_integer<std::uint8_t>(value_[offset_++]));
      }
      return result;
   }

   [[nodiscard]] std::uint32_t u32() {
      require(4);
      auto result = std::uint32_t{};
      for (auto count = 0; count < 4; ++count) {
         result = (result << 8U) | std::to_integer<std::uint8_t>(value_[offset_++]);
      }
      return result;
   }

   [[nodiscard]] std::uint64_t u64() {
      require(8);
      auto result = std::uint64_t{};
      for (auto count = 0; count < 8; ++count) {
         result = (result << 8U) | std::to_integer<std::uint8_t>(value_[offset_++]);
      }
      return result;
   }

   [[nodiscard]] bytes sized_bytes() {
      const auto size = static_cast<std::size_t>(u32());
      require(size);
      auto result = bytes{value_.begin() + static_cast<std::ptrdiff_t>(offset_),
                          value_.begin() + static_cast<std::ptrdiff_t>(offset_ + size)};
      offset_ += size;
      return result;
   }

   [[nodiscard]] digest digest_value() {
      require(digest::data_size());
      auto raw = std::vector<std::uint8_t>{};
      raw.reserve(digest::data_size());
      for (auto count = std::size_t{}; count < digest::data_size(); ++count) {
         raw.push_back(std::to_integer<std::uint8_t>(value_[offset_++]));
      }
      return digest{std::span<const std::uint8_t>{raw}};
   }

   void finish() const {
      if (offset_ != value_.size()) {
         FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated record has trailing bytes");
      }
   }

 private:
   void require(std::size_t size) const {
      if (size > value_.size() - std::min(offset_, value_.size())) {
         FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated record is truncated");
      }
   }

   const bytes& value_;
   std::size_t offset_ = 0;
};

bool key_less(std::span<const std::byte> left, std::span<const std::byte> right) {
   return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
}

bytes encode_node(const node& value) {
   auto output = bytes{format_version, static_cast<std::byte>(value.type)};
   append_sized(output, std::string_view{value.domain});
   if (value.type == node::kind::leaf) {
      append_sized(output, value.key);
      append_digest(output, value.value_hash);
      return output;
   }

   append_u16(output, value.height);
   append_u64(output, value.size);
   append_sized(output, value.min_key);
   append_sized(output, value.max_key);
   append_sized(output, value.key);
   append_digest(output, value.left);
   append_digest(output, value.right);
   return output;
}

node decode_node(const bytes& value) {
   auto input = reader{value};
   if (input.byte() != format_version) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated node format is unsupported");
   }

   const auto kind = input.byte();
   const auto domain_bytes = input.sized_bytes();
   auto domain = std::string{};
   domain.reserve(domain_bytes.size());
   for (const auto byte : domain_bytes) {
      domain.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
   }
   if (kind == static_cast<std::byte>(node::kind::leaf)) {
      auto result = node{
          .type = node::kind::leaf,
          .domain = std::move(domain),
          .height = 0,
          .size = 1,
          .key = input.sized_bytes(),
          .value_hash = input.digest_value(),
      };
      input.finish();
      return result;
   }
   if (kind != static_cast<std::byte>(node::kind::inner)) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated node kind is invalid");
   }

   const auto height = input.u16();
   const auto size = input.u64();
   auto min_key = input.sized_bytes();
   auto max_key = input.sized_bytes();
   auto separator = input.sized_bytes();
   const auto left = input.digest_value();
   const auto right = input.digest_value();
   auto result = node{
       .type = node::kind::inner,
       .domain = std::move(domain),
       .height = height,
       .size = size,
       .key = std::move(separator),
       .min_key = std::move(min_key),
       .max_key = std::move(max_key),
       .left = left,
       .right = right,
   };
   input.finish();
   if (result.height == 0 || result.size < 2 || !key_less(result.min_key, result.max_key) ||
       !key_less(result.min_key, result.key) || key_less(result.max_key, result.key)) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated inner node metadata is invalid");
   }
   return result;
}

digest node_hash(std::string_view domain, const node& value) {
   if (value.type == node::kind::leaf) {
      return hash_leaf(domain, value.key, value.value_hash);
   }
   return hash_inner(domain, value.height, value.size, value.min_key, value.max_key, value.key, value.left,
                     value.right);
}

const bytes& node_min_key(const node& value) {
   return value.type == node::kind::leaf ? value.key : value.min_key;
}

const bytes& node_max_key(const node& value) {
   return value.type == node::kind::leaf ? value.key : value.max_key;
}

bytes encode_reference_count(std::uint64_t value) {
   auto result = bytes{};
   append_u64(result, value);
   return result;
}

std::uint64_t decode_reference_count(const bytes& value) {
   auto input = reader{value};
   const auto result = input.u64();
   input.finish();
   return result;
}

digest decode_hash_key(const forge::db::core::record_key& key, std::byte expected_prefix) {
   const auto& value = key.bytes();
   if (value.size() != 1U + digest::data_size() || value.front() != expected_prefix) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated garbage key is malformed");
   }
   auto raw = std::vector<std::uint8_t>{};
   raw.reserve(digest::data_size());
   for (auto offset = std::size_t{1}; offset < value.size(); ++offset) {
      raw.push_back(std::to_integer<std::uint8_t>(value[offset]));
   }
   return digest{std::span<const std::uint8_t>{raw}};
}

boost::asio::awaitable<void> increment_reference(get_record_fn get, put_record_fn put, erase_record_fn erase,
                                                 const forge::db::core::record_key& reference_key,
                                                 const forge::db::core::record_key& garbage_key) {
   const auto encoded = co_await read_record(get, reference_key);
   if (!encoded) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated reference record is missing");
   }
   const auto count = decode_reference_count(*encoded);
   if (count == std::numeric_limits<std::uint64_t>::max()) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated reference count overflows");
   }
   if (count == 0) {
      co_await erase_record(erase, garbage_key);
   }
   co_await write_record(put, reference_key, encode_reference_count(count + 1U));
}

boost::asio::awaitable<void> decrement_reference(get_record_fn get, put_record_fn put,
                                                 const forge::db::core::record_key& reference_key,
                                                 const forge::db::core::record_key& garbage_key) {
   const auto encoded = co_await read_record(get, reference_key);
   if (!encoded) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated reference record is missing");
   }
   const auto count = decode_reference_count(*encoded);
   if (count == 0) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated reference count underflows");
   }
   co_await write_record(put, reference_key, encode_reference_count(count - 1U));
   if (count == 1U) {
      co_await write_record(put, garbage_key, {});
   }
}

// Shared by ordinary persistence and fresh checkpoint restoration. Reference
// metadata is always owner-built, never supplied by the checkpoint.
boost::asio::awaitable<void> persist_value_record(const get_record_fn& get, const put_record_fn& put,
                                                  const digest& hash, const bytes& value) {
   const auto content_key = value_key(hash);
   const auto reference_key = value_reference_key(hash);
   const auto existing = co_await read_record(get, content_key);
   if (existing) {
      if (*existing != value || !(co_await read_record(get, reference_key))) {
         FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated value record is inconsistent");
      }
      co_return;
   }
   if (co_await read_record(get, reference_key)) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated value reference exists without content");
   }
   co_await write_record(put, content_key, value);
   co_await write_record(put, reference_key, encode_reference_count(0));
}

boost::asio::awaitable<bool> persist_node_record(const get_record_fn& get, const put_record_fn& put, const digest& hash,
                                                 const node& value) {
   const auto content_key = node_key(hash);
   const auto reference_key = node_reference_key(hash);
   const auto encoded = encode_node(value);
   const auto existing = co_await read_record(get, content_key);
   if (existing) {
      if (*existing != encoded || !(co_await read_record(get, reference_key))) {
         FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated node record is inconsistent");
      }
      co_return false;
   }
   if (co_await read_record(get, reference_key)) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated node reference exists without content");
   }
   co_await write_record(put, content_key, encoded);
   co_await write_record(put, reference_key, encode_reference_count(0));
   co_return true;
}

boost::asio::awaitable<void> retain_node_children(const get_record_fn& get, const put_record_fn& put,
                                                  const erase_record_fn& erase, const node& value) {
   if (value.type == node::kind::leaf) {
      co_await increment_reference(get, put, erase, value_reference_key(value.value_hash),
                                   value_gc_key(value.value_hash));
   } else {
      co_await increment_reference(get, put, erase, node_reference_key(value.left), node_gc_key(value.left));
      co_await increment_reference(get, put, erase, node_reference_key(value.right), node_gc_key(value.right));
   }
}

struct checkpoint_node_summary {
   digest hash;
   std::uint16_t height = 0;
   std::uint64_t size = 0;
   bytes min_key;
   bytes max_key;
};

struct checkpoint_export_frame {
   digest hash;
   node value;
   checkpoint_node_summary left;
   std::uint8_t phase = 0;
};

[[noreturn]] void checkpoint_limit() {
   FORGE_THROW_EXCEPTION(exceptions::proof_limit_exceeded, "authenticated checkpoint exceeds configured bounds");
}

std::uint64_t checkpoint_add(std::uint64_t left, std::uint64_t right) {
   if (right > std::numeric_limits<std::uint64_t>::max() - left) {
      checkpoint_limit();
   }
   return left + right;
}

std::uint64_t checkpoint_multiply(std::uint64_t left, std::uint64_t right) {
   if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
      checkpoint_limit();
   }
   return left * right;
}

std::uint64_t checkpoint_tree_records(std::uint64_t leaves) {
   return leaves == 0U ? 0U : checkpoint_add(leaves, leaves - 1U);
}

void checkpoint_preflight(std::string_view domain, const root& expected, const limits& node_bounds,
                          const checkpoint_limits& bounds) {
   if (domain.empty() || domain.size() > max_base_domain_bytes || !limits_are_valid(node_bounds) ||
       !checkpoint_limits_are_valid(bounds)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store, "authenticated checkpoint policy is invalid");
   }
   const auto state_records = checkpoint_tree_records(expected.state_size);
   const auto change_records = checkpoint_tree_records(expected.change_count);
   if (checkpoint_add(state_records, change_records) > bounds.max_records) {
      checkpoint_limit();
   }
   const auto depth = std::min(bounds.max_depth, node_bounds.max_proof_depth);
   // Bound ALL retained record data before calling any source/sink or allocating
   // traversal vectors: one role's digest history, bounded iterative frames and
   // conservative eight-frame transients (DTO/input/result/raw/record copies).
   // Standard allocator bookkeeping/Asio execution metadata is not a process RSS cap.
   const auto keys = checkpoint_multiply(6U, node_bounds.max_key_bytes);
   const auto frame = checkpoint_add(sizeof(checkpoint_export_frame),
                                     checkpoint_add(keys, checkpoint_multiply(2U, domain.size() + 1U)));
   auto working = checkpoint_multiply(checkpoint_add(depth, 1U), frame);
   working = checkpoint_add(working, checkpoint_multiply(std::max(state_records, change_records), sizeof(digest)));
   working = checkpoint_add(working, checkpoint_multiply(8U, bounds.max_frame_bytes));
   // A valid stored value may exceed the selected checkpoint frame budget. Core
   // returns that whole backend record before export can reject/copy it.
   working = checkpoint_add(working, checkpoint_multiply(2U, node_bounds.max_value_bytes));
   working = checkpoint_add(working, checkpoint_multiply(8U, domain.size() + 1U));
   working = checkpoint_add(working,
                            sizeof(checkpoint_summary) + sizeof(checkpoint_record) + sizeof(checkpoint_node_summary));
   if (working > bounds.max_working_bytes || state_records > std::numeric_limits<std::size_t>::max() ||
       change_records > std::numeric_limits<std::size_t>::max()) {
      checkpoint_limit();
   }
   for (const auto tree : {proof_tree::state, proof_tree::changes}) {
      const auto size = tree == proof_tree::state ? expected.state_size : expected.change_count;
      const auto hash = tree == proof_tree::state ? expected.state_root : expected.change_root;
      if (size == 0U && hash != hash_empty(canonical_tree_domain(domain, tree))) {
         FORGE_THROW_EXCEPTION(exceptions::root_mismatch, "authenticated empty checkpoint root is invalid");
      }
   }
}

void account_checkpoint(const checkpoint_record& record, const limits& node_bounds, const checkpoint_limits& bounds,
                        checkpoint_summary& summary) {
   const auto size = wire_size(record);
   if (size > bounds.max_frame_bytes || summary.records >= bounds.max_records ||
       size > bounds.max_encoded_bytes - summary.encoded_bytes) {
      checkpoint_limit();
   }
   if (const auto* leaf = std::get_if<proof_leaf>(&record.node)) {
      if (!leaf->value || leaf->key.size() > node_bounds.max_key_bytes ||
          leaf->value->size() > node_bounds.max_value_bytes ||
          leaf->value->size() > bounds.max_value_bytes - summary.value_bytes) {
         checkpoint_limit();
      }
      summary.value_bytes += leaf->value->size();
      ++summary.leaves;
   } else {
      const auto& branch = std::get<proof_branch>(record.node);
      if (branch.min_key.size() > node_bounds.max_key_bytes || branch.max_key.size() > node_bounds.max_key_bytes ||
          branch.separator.size() > node_bounds.max_key_bytes ||
          branch.height > std::min(bounds.max_depth, node_bounds.max_proof_depth)) {
         checkpoint_limit();
      }
   }
   ++summary.records;
   summary.encoded_bytes += size;
}

checkpoint_node_summary summarize_checkpoint(const digest& hash, const node& value) {
   return {.hash = hash,
           .height = value.height,
           .size = value.size,
           .min_key = node_min_key(value),
           .max_key = node_max_key(value)};
}

void validate_checkpoint_branch(const node& value, const checkpoint_node_summary& left,
                                const checkpoint_node_summary& right) {
   if (value.type != node::kind::inner || value.left != left.hash || value.right != right.hash ||
       !key_less(left.max_key, right.min_key) || value.key != right.min_key || value.min_key != left.min_key ||
       value.max_key != right.max_key || value.size != checkpoint_add(left.size, right.size) ||
       value.height != static_cast<unsigned>(std::max(left.height, right.height)) + 1U ||
       std::abs(static_cast<int>(left.height) - static_cast<int>(right.height)) > 1) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_proof, "authenticated checkpoint branch topology is inconsistent");
   }
}

void validate_checkpoint_hash(const node& value, const digest& hash, std::string_view domain) {
   if (value.domain != domain || node_hash(domain, value) != hash) {
      FORGE_THROW_EXCEPTION(exceptions::root_mismatch, "authenticated checkpoint node hash or domain is invalid");
   }
}

void validate_checkpoint_value(const proof_leaf& leaf, proof_tree tree) {
   if (!leaf.value || hash_value(*leaf.value) != leaf.value_hash) {
      FORGE_THROW_EXCEPTION(exceptions::root_mismatch, "authenticated checkpoint value is missing or invalid");
   }
   if (tree == proof_tree::changes) {
      const auto decoded = decode_change_value(*leaf.value);
      if (encode_change_value(mutation{.value = decoded}) != *leaf.value) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_proof, "authenticated checkpoint change value is not canonical");
      }
   }
}

void validate_checkpoint_history(std::vector<digest>& hashes) {
   std::sort(hashes.begin(), hashes.end());
   if (std::adjacent_find(hashes.begin(), hashes.end()) != hashes.end()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_proof, "authenticated checkpoint repeats a node address");
   }
}

} // namespace

tree_engine::tree_engine(std::string domain, std::optional<digest> root_hash, get_record_fn get, limits settings)
    : domain_{std::move(domain)}, root_hash_{std::move(root_hash)}, get_{std::move(get)}, limits_{settings} {
   if (domain_.empty() || domain_.size() > max_tree_domain_bytes || !get_) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store, "authenticated tree requires a domain and record reader");
   }
   if (!limits_are_valid(limits_)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store, "authenticated tree limits are invalid");
   }
}

void tree_engine::check_depth(std::uint32_t depth) const {
   if (depth > limits_.max_proof_depth) {
      FORGE_THROW_EXCEPTION(exceptions::proof_limit_exceeded, "authenticated tree depth exceeds configured limit",
                            forge::exceptions::ctx("depth", depth),
                            forge::exceptions::ctx("limit", limits_.max_proof_depth));
   }
}

boost::asio::awaitable<node> tree_engine::load(const digest& hash) {
   if (const auto pending = pending_nodes_.find(hash); pending != pending_nodes_.end()) {
      co_return pending->second;
   }
   if (const auto cached = nodes_.find(hash); cached != nodes_.end()) {
      co_return cached->second;
   }

   const auto encoded = co_await read_record(get_, node_key(hash));
   if (!encoded) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated node is missing",
                            forge::exceptions::ctx("hash", hash.str()));
   }
   auto decoded = decode_node(*encoded);
   if (decoded.domain != domain_ || node_hash(decoded.domain, decoded) != hash) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated node hash does not match its address",
                            forge::exceptions::ctx("hash", hash.str()));
   }
   nodes_.emplace(hash, decoded);
   co_return decoded;
}

boost::asio::awaitable<bytes> tree_engine::load_value(const digest& hash) {
   if (const auto pending = pending_values_.find(hash); pending != pending_values_.end()) {
      co_return pending->second;
   }
   if (const auto cached = values_.find(hash); cached != values_.end()) {
      co_return cached->second;
   }
   const auto encoded = co_await read_record(get_, value_key(hash));
   if (!encoded || encoded->size() > limits_.max_value_bytes || hash_value(*encoded) != hash) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated value is missing or corrupt",
                            forge::exceptions::ctx("hash", hash.str()));
   }
   values_.emplace(hash, *encoded);
   co_return *encoded;
}

boost::asio::awaitable<bytes> tree_engine::load_value_for_proof(const digest& hash, std::size_t remaining_wire_bytes) {
   const auto require_fits = [&](const bytes& value) {
      if (value.size() > limits_.max_value_bytes) {
         FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated value exceeds configured limit",
                               forge::exceptions::ctx("hash", hash.str()));
      }
      if (value_wire_size(value.size()) > remaining_wire_bytes) {
         FORGE_THROW_EXCEPTION(exceptions::proof_limit_exceeded,
                               "authenticated range proof exceeds configured wire byte limit");
      }
   };

   if (remaining_wire_bytes < value_wire_size(0)) {
      FORGE_THROW_EXCEPTION(exceptions::proof_limit_exceeded,
                            "authenticated range proof exceeds configured wire byte limit");
   }
   if (const auto pending = pending_values_.find(hash); pending != pending_values_.end()) {
      require_fits(pending->second);
      co_return pending->second;
   }
   if (const auto cached = values_.find(hash); cached != values_.end()) {
      require_fits(cached->second);
      co_return cached->second;
   }

   auto encoded = co_await read_record(get_, value_key(hash));
   if (!encoded) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated value is missing or corrupt",
                            forge::exceptions::ctx("hash", hash.str()));
   }
   require_fits(*encoded);
   if (hash_value(*encoded) != hash) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated value is missing or corrupt",
                            forge::exceptions::ctx("hash", hash.str()));
   }
   const auto [cached, inserted] = values_.emplace(hash, std::move(*encoded));
   static_cast<void>(inserted);
   co_return cached->second;
}

boost::asio::awaitable<digest> tree_engine::save(node value) {
   value.domain = domain_;
   const auto hash = node_hash(value.domain, value);
   pending_nodes_.insert_or_assign(hash, value);
   nodes_.insert_or_assign(hash, std::move(value));
   co_return hash;
}

boost::asio::awaitable<digest> tree_engine::make_inner(const digest& left, const digest& right, std::uint32_t depth) {
   check_depth(depth);
   const auto left_node = co_await load(left);
   const auto right_node = co_await load(right);
   const auto& left_min = node_min_key(left_node);
   const auto& left_max = node_max_key(left_node);
   const auto& right_min = node_min_key(right_node);
   const auto& right_max = node_max_key(right_node);
   if (!key_less(left_max, right_min)) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated inner node children are not strictly ordered");
   }
   const auto height_value = static_cast<std::uint32_t>(std::max(left_node.height, right_node.height)) + 1U;
   if (height_value > std::numeric_limits<std::uint16_t>::max() ||
       left_node.size > std::numeric_limits<std::uint64_t>::max() - right_node.size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_mutation, "authenticated tree metadata overflow");
   }
   co_return co_await save(node{
       .type = node::kind::inner,
       .height = static_cast<std::uint16_t>(height_value),
       .size = left_node.size + right_node.size,
       .key = right_min,
       .min_key = left_min,
       .max_key = right_max,
       .left = left,
       .right = right,
   });
}

boost::asio::awaitable<digest> tree_engine::balance(const digest& left, const digest& right, std::uint32_t depth) {
   check_depth(depth);
   const auto left_node = co_await load(left);
   const auto right_node = co_await load(right);
   const auto factor = static_cast<int>(left_node.height) - static_cast<int>(right_node.height);

   if (factor > 1) {
      if (left_node.type != node::kind::inner) {
         FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated AVL left branch is malformed");
      }
      const auto left_left = co_await load(left_node.left);
      const auto left_right = co_await load(left_node.right);
      if (left_left.height >= left_right.height) {
         const auto new_right = co_await make_inner(left_node.right, right, depth + 1U);
         co_return co_await make_inner(left_node.left, new_right, depth + 1U);
      }
      if (left_right.type != node::kind::inner) {
         FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated AVL left-right branch is malformed");
      }
      const auto new_left = co_await make_inner(left_node.left, left_right.left, depth + 1U);
      const auto new_right = co_await make_inner(left_right.right, right, depth + 1U);
      co_return co_await make_inner(new_left, new_right, depth + 1U);
   }

   if (factor < -1) {
      if (right_node.type != node::kind::inner) {
         FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated AVL right branch is malformed");
      }
      const auto right_left = co_await load(right_node.left);
      const auto right_right = co_await load(right_node.right);
      if (right_right.height >= right_left.height) {
         const auto new_left = co_await make_inner(left, right_node.left, depth + 1U);
         co_return co_await make_inner(new_left, right_node.right, depth + 1U);
      }
      if (right_left.type != node::kind::inner) {
         FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated AVL right-left branch is malformed");
      }
      const auto new_left = co_await make_inner(left, right_left.left, depth + 1U);
      const auto new_right = co_await make_inner(right_left.right, right_node.right, depth + 1U);
      co_return co_await make_inner(new_left, new_right, depth + 1U);
   }

   co_return co_await make_inner(left, right, depth + 1U);
}

boost::asio::awaitable<std::optional<digest>> tree_engine::insert(std::optional<digest> current, const bytes& key,
                                                                  const digest& value_hash, std::uint32_t depth) {
   check_depth(depth);
   if (!current) {
      co_return co_await save(node{
          .type = node::kind::leaf,
          .height = 0,
          .size = 1,
          .key = key,
          .value_hash = value_hash,
      });
   }

   const auto existing = co_await load(*current);
   if (existing.type == node::kind::leaf) {
      if (existing.key == key) {
         co_return co_await save(node{
             .type = node::kind::leaf,
             .height = 0,
             .size = 1,
             .key = key,
             .value_hash = value_hash,
         });
      }
      const auto inserted = co_await save(node{
          .type = node::kind::leaf,
          .height = 0,
          .size = 1,
          .key = key,
          .value_hash = value_hash,
      });
      co_return key_less(key, existing.key) ? co_await make_inner(inserted, *current, depth + 1U)
                                            : co_await make_inner(*current, inserted, depth + 1U);
   }

   if (key_less(key, existing.key)) {
      const auto updated = co_await insert(existing.left, key, value_hash, depth + 1U);
      co_return co_await balance(*updated, existing.right, depth + 1U);
   }
   const auto updated = co_await insert(existing.right, key, value_hash, depth + 1U);
   co_return co_await balance(existing.left, *updated, depth + 1U);
}

boost::asio::awaitable<std::optional<digest>> tree_engine::erase(std::optional<digest> current, const bytes& key,
                                                                 std::uint32_t depth) {
   check_depth(depth);
   if (!current) {
      co_return std::nullopt;
   }
   const auto existing = co_await load(*current);
   if (existing.type == node::kind::leaf) {
      co_return existing.key == key ? std::nullopt : current;
   }

   if (key_less(key, existing.key)) {
      const auto updated = co_await erase(existing.left, key, depth + 1U);
      if (!updated) {
         co_return existing.right;
      }
      if (*updated == existing.left) {
         co_return current;
      }
      co_return co_await balance(*updated, existing.right, depth + 1U);
   }

   const auto updated = co_await erase(existing.right, key, depth + 1U);
   if (!updated) {
      co_return existing.left;
   }
   if (*updated == existing.right) {
      co_return current;
   }
   co_return co_await balance(existing.left, *updated, depth + 1U);
}

boost::asio::awaitable<tree_result> tree_engine::apply(std::span<const mutation> mutations) {
   const auto normalized = normalize_mutations(mutations, limits_);
   for (const auto& item : normalized) {
      if (item.value) {
         const auto value_hash = hash_value(*item.value);
         pending_values_.insert_or_assign(value_hash, *item.value);
         values_.insert_or_assign(value_hash, *item.value);
         root_hash_ = co_await insert(root_hash_, item.key, value_hash, 0);
      } else {
         root_hash_ = co_await erase(root_hash_, item.key, 0);
      }
   }

   if (!root_hash_) {
      co_return tree_result{.hash = hash_empty(domain_), .size = 0};
   }
   const auto root_node = co_await load(*root_hash_);
   co_return tree_result{.hash = *root_hash_, .size = root_node.size};
}

boost::asio::awaitable<std::optional<bytes>> tree_engine::get(std::span<const std::byte> key) {
   auto current = root_hash_;
   auto depth = std::uint32_t{};
   while (current) {
      check_depth(depth++);
      const auto value = co_await load(*current);
      if (value.type == node::kind::leaf) {
         if (std::ranges::equal(value.key, key)) {
            co_return co_await load_value(value.value_hash);
         }
         co_return std::nullopt;
      }
      current = key_less(key, value.key) ? value.left : value.right;
   }
   co_return std::nullopt;
}

boost::asio::awaitable<point_proof> tree_engine::prove(const root& anchor, std::span<const std::byte> key,
                                                       bool include_value) {
   if (key.size() > limits_.max_key_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::proof_limit_exceeded, "authenticated proof key exceeds configured limit",
                            forge::exceptions::ctx("size", key.size()),
                            forge::exceptions::ctx("limit", limits_.max_key_bytes));
   }
   auto result = point_proof{
       .anchor = anchor,
       .key = bytes{key.begin(), key.end()},
   };
   if (!root_hash_) {
      require_wire_budget(wire_size(result), limits_);
      co_return result;
   }

   auto current = *root_hash_;
   auto forward = std::vector<proof_step>{};
   auto depth = std::uint32_t{};
   while (true) {
      check_depth(depth++);
      const auto value = co_await load(current);
      if (value.type == node::kind::leaf) {
         auto leaf = proof_leaf{
             .key = value.key,
             .value_hash = value.value_hash,
         };
         if (include_value && value.key == result.key) {
            leaf.value = co_await load_value(value.value_hash);
         }
         result.terminal = std::move(leaf);
         break;
      }

      const auto go_left = key_less(key, value.key);
      const auto sibling = co_await load(go_left ? value.right : value.left);
      auto sibling_proof = proof_sibling{};
      if (sibling.type == node::kind::leaf) {
         sibling_proof = proof_leaf{
             .key = sibling.key,
             .value_hash = sibling.value_hash,
         };
      } else {
         sibling_proof = proof_branch{
             .height = sibling.height,
             .size = sibling.size,
             .min_key = sibling.min_key,
             .max_key = sibling.max_key,
             .separator = sibling.key,
             .left_hash = sibling.left,
             .right_hash = sibling.right,
         };
      }
      forward.push_back(proof_step{
          .child = go_left ? branch_side::left : branch_side::right,
          .height = value.height,
          .subtree_size = value.size,
          .min_key = value.min_key,
          .max_key = value.max_key,
          .separator = value.key,
          .sibling = std::move(sibling_proof),
      });
      current = go_left ? value.left : value.right;
   }
   result.path.assign(forward.rbegin(), forward.rend());
   require_wire_budget(wire_size(result), limits_);
   co_return result;
}

boost::asio::awaitable<std::uint64_t> tree_engine::lower_bound_rank(const bytes& key) {
   auto current = root_hash_;
   auto rank = std::uint64_t{};
   auto depth = std::uint32_t{};
   while (current) {
      check_depth(depth++);
      const auto value = co_await load(*current);
      if (value.type == node::kind::leaf) {
         co_return rank + (key_less(value.key, key) ? 1U : 0U);
      }
      if (key_less(key, value.key)) {
         current = value.left;
      } else {
         const auto left = co_await load(value.left);
         if (rank > std::numeric_limits<std::uint64_t>::max() - left.size) {
            FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated range rank overflows");
         }
         rank += left.size;
         current = value.right;
      }
   }
   co_return rank;
}

boost::asio::awaitable<void> tree_engine::emit_items(const digest& current, std::uint64_t offset, std::uint64_t begin,
                                                     std::uint64_t end, bool include_values,
                                                     std::vector<verified_range_item>& output, std::uint32_t depth) {
   check_depth(depth);
   const auto value = co_await load(current);
   if (offset > std::numeric_limits<std::uint64_t>::max() - value.size) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated range offset overflows");
   }
   const auto subtree_end = offset + value.size;
   if (subtree_end <= begin || offset >= end) {
      co_return;
   }
   if (value.type == node::kind::leaf) {
      auto item = verified_range_item{
          .key = value.key,
          .value_hash = value.value_hash,
          .rank = offset,
      };
      if (include_values) {
         item.value = co_await load_value(value.value_hash);
      }
      output.push_back(std::move(item));
      co_return;
   }

   const auto left = co_await load(value.left);
   co_await emit_items(value.left, offset, begin, end, include_values, output, depth + 1U);
   co_await emit_items(value.right, offset + left.size, begin, end, include_values, output, depth + 1U);
}

boost::asio::awaitable<verified_range> tree_engine::scan_range(const root& anchor, range_request request,
                                                               proof_tree tree) {
   if (tree != proof_tree::state && tree != proof_tree::changes) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_range, "authenticated range tree is invalid");
   }
   if (request.limit == 0 || request.limit > limits_.max_range_items) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_range, "authenticated range limit is invalid",
                            forge::exceptions::ctx("limit", request.limit),
                            forge::exceptions::ctx("maximum", limits_.max_range_items));
   }
   if ((request.lower && request.lower->size() > limits_.max_key_bytes) ||
       (request.upper && request.upper->size() > limits_.max_key_bytes)) {
      FORGE_THROW_EXCEPTION(exceptions::proof_limit_exceeded, "authenticated range boundary exceeds key limit");
   }
   if (request.lower && request.upper && !key_less(*request.lower, *request.upper)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_range, "authenticated range must be non-empty");
   }

   const auto total = tree == proof_tree::state ? anchor.state_size : anchor.change_count;
   const auto expected = tree == proof_tree::state ? anchor.state_root : anchor.change_root;
   if ((total == 0 && root_hash_) || (total != 0 && (!root_hash_ || *root_hash_ != expected))) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated range tree does not match its anchor");
   }

   auto result = verified_range{.total_size = total};
   if (total == 0) {
      co_return result;
   }
   const auto first = request.lower ? co_await lower_bound_rank(*request.lower) : std::uint64_t{0};
   const auto upper = request.upper ? co_await lower_bound_rank(*request.upper) : total;
   if (first > upper || upper > total) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated range ranks are inconsistent");
   }
   const auto count = std::min<std::uint64_t>(upper - first, request.limit);
   const auto result_begin = request.reverse ? upper - count : first;
   const auto result_end = request.reverse ? upper : first + count;
   co_await emit_items(*root_hash_, 0U, result_begin, result_end, request.include_values, result.items, 0U);
   if (result.items.size() != count) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated range scan has a gap");
   }
   if (request.reverse) {
      std::ranges::reverse(result.items);
      if (result_begin > first) {
         result.more = true;
         result.next_key = result.items.back().key;
      }
   } else if (result_end < upper) {
      auto successor = std::vector<verified_range_item>{};
      co_await emit_items(*root_hash_, 0U, result_end, result_end + 1U, false, successor, 0U);
      if (successor.size() != 1U) {
         FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated range scan omits its continuation");
      }
      result.more = true;
      result.next_key = std::move(successor.front().key);
   }
   co_return result;
}

boost::asio::awaitable<void> tree_engine::emit_range(const digest& current, std::uint64_t offset,
                                                     std::uint64_t witness_begin, std::uint64_t witness_end,
                                                     std::uint64_t result_begin, std::uint64_t result_end,
                                                     bool include_values, std::vector<range_proof_node>& output,
                                                     std::size_t& wire_bytes, std::uint32_t depth) {
   check_depth(depth);

   const auto value = co_await load(current);
   if (offset > std::numeric_limits<std::uint64_t>::max() - value.size) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated range offset overflows");
   }
   const auto end = offset + value.size;
   if (end <= witness_begin || offset >= witness_end) {
      if (value.type == node::kind::leaf) {
         append_range_node(output,
                           proof_leaf{
                               .key = value.key,
                               .value_hash = value.value_hash,
                           },
                           wire_bytes, limits_);
      } else {
         append_range_node(output,
                           proof_branch{
                               .height = value.height,
                               .size = value.size,
                               .min_key = value.min_key,
                               .max_key = value.max_key,
                               .separator = value.key,
                               .left_hash = value.left,
                               .right_hash = value.right,
                           },
                           wire_bytes, limits_);
      }
      co_return;
   }

   if (value.type == node::kind::leaf) {
      auto leaf = proof_leaf{
          .key = value.key,
          .value_hash = value.value_hash,
      };
      append_range_node(output, std::move(leaf), wire_bytes, limits_);
      if (include_values && offset >= result_begin && offset < result_end) {
         auto loaded = co_await load_value_for_proof(value.value_hash, limits_.max_proof_bytes - wire_bytes);
         const auto added = value_wire_size(loaded.size());
         auto* appended = std::get_if<proof_leaf>(&output.back());
         if (!appended) {
            FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated range proof builder lost its appended leaf");
         }
         appended->value = std::move(loaded);
         wire_bytes += added;
      }
      co_return;
   }

   append_range_node(output,
                     range_inner{
                         .height = value.height,
                         .size = value.size,
                         .min_key = value.min_key,
                         .max_key = value.max_key,
                         .separator = value.key,
                     },
                     wire_bytes, limits_);
   const auto left = co_await load(value.left);
   co_await emit_range(value.left, offset, witness_begin, witness_end, result_begin, result_end, include_values, output,
                       wire_bytes, depth + 1U);
   co_await emit_range(value.right, offset + left.size, witness_begin, witness_end, result_begin, result_end,
                       include_values, output, wire_bytes, depth + 1U);
}

boost::asio::awaitable<range_proof> tree_engine::prove_range(const root& anchor, range_request request,
                                                             proof_tree tree) {
   if (tree != proof_tree::state && tree != proof_tree::changes) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_range, "authenticated range tree is invalid");
   }
   if (request.limit == 0 || request.limit > limits_.max_range_items) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_range, "authenticated range limit is invalid",
                            forge::exceptions::ctx("limit", request.limit),
                            forge::exceptions::ctx("maximum", limits_.max_range_items));
   }
   if ((request.lower && request.lower->size() > limits_.max_key_bytes) ||
       (request.upper && request.upper->size() > limits_.max_key_bytes)) {
      FORGE_THROW_EXCEPTION(exceptions::proof_limit_exceeded, "authenticated range boundary exceeds key limit");
   }
   if (request.lower && request.upper && !key_less(*request.lower, *request.upper)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_range, "authenticated range must be non-empty");
   }

   const auto total = tree == proof_tree::state ? anchor.state_size : anchor.change_count;
   const auto expected = tree == proof_tree::state ? anchor.state_root : anchor.change_root;
   if ((total == 0 && root_hash_) || (total != 0 && (!root_hash_ || *root_hash_ != expected))) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated range tree does not match its anchor");
   }

   auto result = range_proof{
       .anchor = anchor,
       .tree = tree,
       .request = std::move(request),
   };
   auto wire_bytes = wire_size(result);
   require_wire_budget(wire_bytes, limits_);
   if (total == 0) {
      co_return result;
   }

   const auto first = result.request.lower ? co_await lower_bound_rank(*result.request.lower) : std::uint64_t{0};
   const auto upper = result.request.upper ? co_await lower_bound_rank(*result.request.upper) : total;
   if (first > upper || upper > total) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated range ranks are inconsistent");
   }
   const auto available = upper - first;
   const auto count = std::min<std::uint64_t>(available, result.request.limit);
   const auto result_begin = result.request.reverse ? upper - count : first;
   const auto result_end = result.request.reverse ? upper : first + count;
   const auto witness_begin = result_begin == 0 ? 0 : result_begin - 1U;
   const auto witness_end = result_end < total ? result_end + 1U : total;
   co_await emit_range(*root_hash_, 0, witness_begin, witness_end, result_begin, result_end,
                       result.request.include_values, result.nodes, wire_bytes, 0);
   if (wire_size(result) != wire_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated range proof wire accounting is inconsistent");
   }
   require_wire_budget(wire_bytes, limits_);
   co_return result;
}

boost::asio::awaitable<void> tree_engine::persist(get_record_fn get, put_record_fn put, erase_record_fn erase) {
   if (!get || !put || !erase) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store,
                            "authenticated tree persistence requires record reader, writer, and eraser callbacks");
   }
   auto reachable_nodes = std::set<digest>{};
   auto reachable_values = std::set<digest>{};
   auto visit = std::function<void(const digest&)>{};
   visit = [&](const digest& hash) {
      const auto found = pending_nodes_.find(hash);
      if (found == pending_nodes_.end() || !reachable_nodes.insert(hash).second) {
         return;
      }
      if (found->second.type == node::kind::leaf) {
         reachable_values.insert(found->second.value_hash);
      } else {
         visit(found->second.left);
         visit(found->second.right);
      }
   };
   if (root_hash_) {
      visit(*root_hash_);
   }

   auto inserted_nodes = std::vector<std::pair<digest, node>>{};
   for (const auto& [hash, value] : pending_values_) {
      if (!reachable_values.contains(hash)) {
         continue;
      }
      co_await persist_value_record(get, put, hash, value);
   }
   for (const auto& [hash, value] : pending_nodes_) {
      if (!reachable_nodes.contains(hash)) {
         continue;
      }
      if (co_await persist_node_record(get, put, hash, value)) {
         inserted_nodes.emplace_back(hash, value);
      }
   }

   for (const auto& [hash, value] : inserted_nodes) {
      static_cast<void>(hash);
      co_await retain_node_children(get, put, erase, value);
   }
}

std::optional<digest> tree_engine::root_hash() const noexcept {
   return root_hash_;
}

void validate_checkpoint_policy(std::string_view domain, const root& expected, const limits& node_bounds,
                                const checkpoint_limits& checkpoint_bounds) {
   checkpoint_preflight(domain, expected, node_bounds, checkpoint_bounds);
}

boost::asio::awaitable<checkpoint_summary> export_checkpoint_graphs(std::string_view domain, root expected,
                                                                    get_record_fn get, checkpoint_sink sink,
                                                                    limits node_bounds,
                                                                    checkpoint_limits checkpoint_bounds) {
   checkpoint_preflight(domain, expected, node_bounds, checkpoint_bounds);
   if (!get || !sink) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store, "checkpoint export callbacks are missing");
   }
   auto result = checkpoint_summary{.anchor = expected};
   const auto depth = std::min(checkpoint_bounds.max_depth, node_bounds.max_proof_depth);
   for (const auto tree : {proof_tree::state, proof_tree::changes}) {
      const auto leaves = tree == proof_tree::state ? expected.state_size : expected.change_count;
      if (leaves == 0U) {
         continue;
      }
      const auto hash = tree == proof_tree::state ? expected.state_root : expected.change_root;
      const auto role_domain = canonical_tree_domain(domain, tree);
      const auto record_count = checkpoint_tree_records(leaves);
      auto hashes = std::vector<digest>{};
      hashes.reserve(static_cast<std::size_t>(record_count));
      auto stack = std::vector<checkpoint_export_frame>{};
      stack.reserve(static_cast<std::size_t>(depth) + 1U);
      stack.push_back({.hash = hash});
      auto completed = std::optional<checkpoint_node_summary>{};
      while (!stack.empty()) {
         const auto top = stack.size() - 1U;
         if (completed) {
            if (stack[top].phase == 1U) {
               stack[top].left = std::move(*completed);
               completed.reset();
               stack[top].phase = 2U;
               continue;
            }
            if (stack[top].phase != 3U) {
               FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "checkpoint export traversal state is invalid");
            }
            validate_checkpoint_branch(stack[top].value, stack[top].left, *completed);
            const auto& value = stack[top].value;
            auto record = checkpoint_record{.tree = tree,
                                            .hash = stack[top].hash,
                                            .node = proof_branch{.height = value.height,
                                                                 .size = value.size,
                                                                 .min_key = value.min_key,
                                                                 .max_key = value.max_key,
                                                                 .separator = value.key,
                                                                 .left_hash = value.left,
                                                                 .right_hash = value.right}};
            account_checkpoint(record, node_bounds, checkpoint_bounds, result);
            if (hashes.size() >= record_count) {
               FORGE_THROW_EXCEPTION(exceptions::root_mismatch, "checkpoint export exceeds its root node count");
            }
            hashes.push_back(record.hash);
            completed = summarize_checkpoint(record.hash, value);
            co_await sink(std::move(record));
            stack.pop_back();
            continue;
         }
         if (stack[top].phase == 2U) {
            if (stack.size() > depth) {
               checkpoint_limit();
            }
            stack[top].phase = 3U;
            stack.push_back({.hash = stack[top].value.right});
            continue;
         }
         if (stack[top].phase != 0U) {
            FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "checkpoint export child is missing");
         }
         const auto encoded = co_await read_record(get, node_key(stack[top].hash));
         if (!encoded) {
            FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "checkpoint export node is missing");
         }
         // The backend returns a whole record; reject oversized local records
         // before decoding/copying them. This is not a bounded Core::get API.
         const auto node_bytes = checkpoint_add(checkpoint_add(role_domain.size(), 128U),
                                                checkpoint_multiply(3U, node_bounds.max_key_bytes));
         if (encoded->size() > node_bytes) {
            checkpoint_limit();
         }
         stack[top].value = decode_node(*encoded);
         validate_checkpoint_hash(stack[top].value, stack[top].hash, role_domain);
         if (stack[top].value.type == node::kind::inner) {
            if (stack.size() > depth || stack[top].value.height > depth) {
               checkpoint_limit();
            }
            stack[top].phase = 1U;
            stack.push_back({.hash = stack[top].value.left});
            continue;
         }
         const auto value = co_await read_record(get, value_key(stack[top].value.value_hash));
         if (!value) {
            FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "checkpoint export value is missing");
         }
         if (value->size() > node_bounds.max_value_bytes || value->size() > checkpoint_bounds.max_frame_bytes ||
             value->size() > checkpoint_bounds.max_value_bytes - result.value_bytes ||
             stack[top].value.key.size() > node_bounds.max_key_bytes) {
            checkpoint_limit();
         }
         auto record = checkpoint_record{
             .tree = tree,
             .hash = stack[top].hash,
             .node =
                 proof_leaf{.key = stack[top].value.key, .value_hash = stack[top].value.value_hash, .value = *value}};
         account_checkpoint(record, node_bounds, checkpoint_bounds, result);
         validate_checkpoint_value(std::get<proof_leaf>(record.node), tree);
         if (hashes.size() >= record_count) {
            FORGE_THROW_EXCEPTION(exceptions::root_mismatch, "checkpoint export exceeds its root node count");
         }
         hashes.push_back(record.hash);
         completed = summarize_checkpoint(record.hash, stack[top].value);
         co_await sink(std::move(record));
         stack.pop_back();
      }
      if (!completed || completed->hash != hash || completed->size != leaves || hashes.size() != record_count) {
         FORGE_THROW_EXCEPTION(exceptions::root_mismatch, "checkpoint export root metadata is inconsistent");
      }
      validate_checkpoint_history(hashes);
   }
   co_return result;
}

boost::asio::awaitable<checkpoint_summary> restore_checkpoint_graphs(std::string_view domain, root expected,
                                                                     get_record_fn get, put_record_fn put,
                                                                     erase_record_fn erase, checkpoint_source source,
                                                                     limits node_bounds,
                                                                     checkpoint_limits checkpoint_bounds) {
   checkpoint_preflight(domain, expected, node_bounds, checkpoint_bounds);
   if (!get || !put || !erase || !source) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store, "checkpoint restore callbacks are missing");
   }
   auto result = checkpoint_summary{.anchor = expected};
   const auto depth = std::min(checkpoint_bounds.max_depth, node_bounds.max_proof_depth);
   for (const auto tree : {proof_tree::state, proof_tree::changes}) {
      const auto leaves = tree == proof_tree::state ? expected.state_size : expected.change_count;
      const auto record_count = checkpoint_tree_records(leaves);
      const auto hash = tree == proof_tree::state ? expected.state_root : expected.change_root;
      const auto role_domain = canonical_tree_domain(domain, tree);
      auto hashes = std::vector<digest>{};
      hashes.reserve(static_cast<std::size_t>(record_count));
      auto stack = std::vector<checkpoint_node_summary>{};
      stack.reserve(static_cast<std::size_t>(depth) + 1U);
      for (auto index = std::uint64_t{}; index < record_count; ++index) {
         auto record = co_await source();
         if (!record || record->tree != tree) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_proof, "checkpoint is truncated or has the wrong tree role");
         }
         account_checkpoint(*record, node_bounds, checkpoint_bounds, result);
         auto value = node{.domain = role_domain};
         if (const auto* leaf = std::get_if<proof_leaf>(&record->node)) {
            validate_checkpoint_value(*leaf, tree);
            value.key = leaf->key;
            value.value_hash = leaf->value_hash;
            if (stack.size() > depth) {
               checkpoint_limit();
            }
            validate_checkpoint_hash(value, record->hash, role_domain);
            co_await persist_value_record(get, put, value.value_hash, *leaf->value);
         } else {
            if (stack.size() < 2U) {
               FORGE_THROW_EXCEPTION(exceptions::invalid_proof, "checkpoint branch precedes its children");
            }
            const auto& branch = std::get<proof_branch>(record->node);
            value.type = node::kind::inner;
            value.height = branch.height;
            value.size = branch.size;
            value.key = branch.separator;
            value.min_key = branch.min_key;
            value.max_key = branch.max_key;
            value.left = branch.left_hash;
            value.right = branch.right_hash;
            validate_checkpoint_branch(value, stack[stack.size() - 2U], stack.back());
            validate_checkpoint_hash(value, record->hash, role_domain);
            stack.pop_back();
            stack.pop_back();
         }
         if (co_await persist_node_record(get, put, record->hash, value)) {
            co_await retain_node_children(get, put, erase, value);
         }
         stack.push_back(summarize_checkpoint(record->hash, value));
         hashes.push_back(record->hash);
      }
      if (leaves != 0U && (stack.size() != 1U || stack.back().hash != hash || stack.back().size != leaves)) {
         FORGE_THROW_EXCEPTION(exceptions::root_mismatch, "checkpoint graph does not match its root tuple");
      }
      validate_checkpoint_history(hashes);
   }
   if (co_await source()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_proof, "checkpoint contains trailing records");
   }
   co_return result;
}

std::uint64_t tree_engine::size() const noexcept {
   if (!root_hash_) {
      return 0;
   }
   const auto found = nodes_.find(*root_hash_);
   return found == nodes_.end() ? 0 : found->second.size;
}

std::vector<mutation> normalize_mutations(std::span<const mutation> mutations, const limits& settings) {
   if (!limits_are_valid(settings)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store, "authenticated mutation limits are invalid");
   }
   auto normalized = std::map<bytes, std::optional<bytes>>{};
   for (const auto& item : mutations) {
      if (item.key.size() > settings.max_key_bytes) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_mutation, "authenticated key exceeds configured limit",
                               forge::exceptions::ctx("size", item.key.size()),
                               forge::exceptions::ctx("limit", settings.max_key_bytes));
      }
      if (item.value && item.value->size() > settings.max_value_bytes) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_mutation, "authenticated value exceeds configured limit",
                               forge::exceptions::ctx("size", item.value->size()),
                               forge::exceptions::ctx("limit", settings.max_value_bytes));
      }
      normalized.insert_or_assign(item.key, item.value);
   }

   auto result = std::vector<mutation>{};
   result.reserve(normalized.size());
   for (auto& [key, value] : normalized) {
      result.push_back(mutation{.key = std::move(key), .value = std::move(value)});
   }
   return result;
}

forge::db::core::record_key node_key(const digest& hash) {
   auto value = bytes{node_record};
   append_digest(value, hash);
   return forge::db::core::record_key{std::move(value)};
}

forge::db::core::record_key value_key(const digest& hash) {
   auto value = bytes{value_record};
   append_digest(value, hash);
   return forge::db::core::record_key{std::move(value)};
}

forge::db::core::record_key version_key(const digest& id, version_id_t version) {
   auto value = bytes{version_record};
   append_digest(value, id);
   append_u64(value, version);
   return forge::db::core::record_key{std::move(value)};
}

forge::db::core::record_key latest_key(const digest& id) {
   auto value = bytes{latest_record};
   append_digest(value, id);
   return forge::db::core::record_key{std::move(value)};
}

forge::db::core::record_key node_reference_key(const digest& hash) {
   auto value = bytes{node_reference_record};
   append_digest(value, hash);
   return forge::db::core::record_key{std::move(value)};
}

forge::db::core::record_key value_reference_key(const digest& hash) {
   auto value = bytes{value_reference_record};
   append_digest(value, hash);
   return forge::db::core::record_key{std::move(value)};
}

forge::db::core::record_key node_gc_key(const digest& hash) {
   auto value = bytes{node_gc_record};
   append_digest(value, hash);
   return forge::db::core::record_key{std::move(value)};
}

forge::db::core::record_key value_gc_key(const digest& hash) {
   auto value = bytes{value_gc_record};
   append_digest(value, hash);
   return forge::db::core::record_key{std::move(value)};
}

forge::db::core::record_key retention_guard_prefix(const digest& namespace_hash, version_id_t version) {
   auto value = std::vector<std::byte>{retention_guard_record};
   append_digest(value, namespace_hash);
   append_u64(value, version);
   return forge::db::core::record_key{std::move(value)};
}

forge::db::core::record_key retention_guard_key(const digest& namespace_hash, version_id_t version,
                                                std::span<const std::byte> token) {
   auto value = retention_guard_prefix(namespace_hash, version).bytes();
   value.insert(value.end(), token.begin(), token.end());
   return forge::db::core::record_key{std::move(value)};
}

forge::db::core::record_key version_prefix(const digest& id) {
   auto value = bytes{version_record};
   append_digest(value, id);
   return forge::db::core::record_key{std::move(value)};
}

forge::db::core::record_range record_prefix_range(std::byte prefix) {
   auto value = forge::db::core::record_key{bytes{prefix}};
   return {
       .begin = value,
       .prefix = std::move(value),
       .has_end = false,
   };
}

version_id_t decode_version_key(const forge::db::core::record_key& key, const digest& id) {
   const auto& value = key.bytes();
   if (value.size() != 1U + digest::data_size() + sizeof(version_id_t) || value.front() != version_record) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated version key is malformed");
   }
   const auto expected = version_prefix(id).bytes();
   if (!std::equal(expected.begin(), expected.end(), value.begin())) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated version key belongs to another namespace");
   }
   auto version = version_id_t{};
   for (auto offset = expected.size(); offset < value.size(); ++offset) {
      version = (version << 8U) | std::to_integer<std::uint8_t>(value[offset]);
   }
   return version;
}

digest namespace_id(std::string_view domain) {
   return hash_value(std::span<const std::byte>{reinterpret_cast<const std::byte*>(domain.data()), domain.size()});
}

bytes encode_root(const root& value) {
   auto result = bytes{format_version};
   append_u64(result, value.version);
   append_digest(result, value.state_root);
   append_u64(result, value.state_size);
   append_digest(result, value.change_root);
   append_u64(result, value.change_count);
   return result;
}

root decode_root(const bytes& value) {
   auto input = reader{value};
   if (input.byte() != format_version) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated root format is unsupported");
   }
   auto result = root{
       .version = input.u64(),
       .state_root = input.digest_value(),
       .state_size = input.u64(),
       .change_root = input.digest_value(),
       .change_count = input.u64(),
   };
   input.finish();
   return result;
}

boost::asio::awaitable<void> retain_root(get_record_fn get, put_record_fn put, erase_record_fn erase,
                                         const digest& hash) {
   co_await increment_reference(std::move(get), std::move(put), std::move(erase), node_reference_key(hash),
                                node_gc_key(hash));
}

boost::asio::awaitable<void> release_root(get_record_fn get, put_record_fn put, const digest& hash) {
   co_await decrement_reference(std::move(get), std::move(put), node_reference_key(hash), node_gc_key(hash));
}

boost::asio::awaitable<garbage_result> collect_garbage(forge::db::core::transaction& active,
                                                       forge::db::core::family family, std::uint32_t limit) {
   if (limit == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store, "authenticated garbage collection limit must be positive");
   }

   auto get = [&active, family](forge::db::core::record_key key) -> boost::asio::awaitable<std::optional<bytes>> {
      co_return co_await invoke_backend([&] { return active.get_for_update(family, std::move(key)); });
   };
   auto put = [&active, family](forge::db::core::record_key key, bytes value) -> boost::asio::awaitable<void> {
      co_await invoke_backend([&] { return active.put(family, std::move(key), std::move(value)); });
   };
   auto erase = [&active, family](forge::db::core::record_key key) -> boost::asio::awaitable<void> {
      co_await invoke_backend([&] { return active.erase(family, std::move(key)); });
   };

   auto result = garbage_result{};
   auto processed = std::uint32_t{};
   while (processed < limit) {
      auto page = co_await invoke_backend([&] {
         return active.scan_page(
             family, record_prefix_range(node_gc_record),
             {.limit = std::min<std::uint32_t>(limit - processed, forge::db::core::max_page_limit)});
      });
      if (page.entries.empty()) {
         break;
      }
      for (const auto& entry : page.entries) {
         const auto hash = decode_hash_key(entry.key, node_gc_record);
         const auto reference_key = node_reference_key(hash);
         const auto reference = co_await read_record(get, reference_key);
         if (!reference) {
            FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated garbage node reference is missing");
         }
         if (decode_reference_count(*reference) != 0) {
            co_await erase_record(erase, entry.key);
            if (++processed == limit) {
               break;
            }
            continue;
         }
         const auto content_key = node_key(hash);
         const auto encoded = co_await read_record(get, content_key);
         if (!encoded) {
            FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated garbage node is missing");
         }
         const auto value = decode_node(*encoded);
         if (node_hash(value.domain, value) != hash) {
            FORGE_THROW_EXCEPTION(exceptions::corrupt_node,
                                  "authenticated garbage node hash does not match its address");
         }
         if (value.type == node::kind::leaf) {
            co_await decrement_reference(get, put, value_reference_key(value.value_hash),
                                         value_gc_key(value.value_hash));
         } else {
            co_await decrement_reference(get, put, node_reference_key(value.left), node_gc_key(value.left));
            co_await decrement_reference(get, put, node_reference_key(value.right), node_gc_key(value.right));
         }
         co_await erase_record(erase, content_key);
         co_await erase_record(erase, reference_key);
         co_await erase_record(erase, entry.key);
         ++result.nodes;
         if (++processed == limit) {
            break;
         }
      }
   }

   while (processed < limit) {
      auto page = co_await invoke_backend([&] {
         return active.scan_page(
             family, record_prefix_range(value_gc_record),
             {.limit = std::min<std::uint32_t>(limit - processed, forge::db::core::max_page_limit)});
      });
      if (page.entries.empty()) {
         break;
      }
      for (const auto& entry : page.entries) {
         const auto hash = decode_hash_key(entry.key, value_gc_record);
         const auto reference_key = value_reference_key(hash);
         const auto reference = co_await read_record(get, reference_key);
         if (!reference) {
            FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated garbage value reference is missing");
         }
         if (decode_reference_count(*reference) != 0) {
            co_await erase_record(erase, entry.key);
            if (++processed == limit) {
               break;
            }
            continue;
         }
         const auto content_key = value_key(hash);
         const auto encoded = co_await read_record(get, content_key);
         if (!encoded || hash_value(*encoded) != hash) {
            FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated garbage value is missing or corrupt");
         }
         co_await erase_record(erase, content_key);
         co_await erase_record(erase, reference_key);
         co_await erase_record(erase, entry.key);
         ++result.values;
         if (++processed == limit) {
            break;
         }
      }
   }

   const auto node_pending = co_await invoke_backend(
       [&] { return active.scan_page(family, record_prefix_range(node_gc_record), {.limit = 1}); });
   const auto value_pending = co_await invoke_backend(
       [&] { return active.scan_page(family, record_prefix_range(value_gc_record), {.limit = 1}); });
   result.pending = !node_pending.entries.empty() || !value_pending.entries.empty();
   co_return result;
}

} // namespace forge::db::authenticated::detail
