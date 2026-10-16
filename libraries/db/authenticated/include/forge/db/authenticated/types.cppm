module;

#include <boost/describe.hpp>
#include <forge/raw/serialization.hpp>
#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

export module forge.db.authenticated.types;

export import forge.crypto.digest.sha256;

import forge.raw.datastream;
import forge.raw.raw;

export namespace forge::db::authenticated {

using bytes = std::vector<std::byte>;
using version_id_t = std::uint64_t;
using digest = forge::crypto::digest::sha256;
inline constexpr auto max_wire_proof_bytes = std::size_t{20U << 20U};
inline constexpr auto max_wire_proof_nodes = std::uint32_t{1U << 20U};
inline constexpr auto max_framed_bytes = static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
inline constexpr auto max_tree_domain_bytes = max_framed_bytes;
inline constexpr auto max_base_domain_bytes = max_tree_domain_bytes - 1U;
inline constexpr auto hard_max_proof_depth = std::uint32_t{256};

struct limits {
   std::size_t max_key_bytes = 1U << 20U;
   std::size_t max_value_bytes = 64U << 20U;
   std::size_t max_proof_bytes = 8U << 20U;
   std::uint32_t max_proof_depth = hard_max_proof_depth;
   std::uint32_t max_proof_nodes = 100'000;
   std::uint32_t max_range_items = 4'096;
};

[[nodiscard]] constexpr bool limits_are_valid(const limits& value) noexcept {
   return value.max_key_bytes <= max_framed_bytes && value.max_value_bytes <= max_framed_bytes &&
          value.max_proof_bytes != 0U && value.max_proof_bytes <= max_wire_proof_bytes && value.max_proof_depth != 0U &&
          value.max_proof_depth <= hard_max_proof_depth && value.max_proof_nodes != 0U &&
          value.max_proof_nodes <= max_wire_proof_nodes && value.max_range_items != 0U &&
          value.max_range_items <= value.max_proof_nodes;
}

struct mutation {
   bytes key;
   std::optional<bytes> value;

   bool operator==(const mutation&) const = default;
};

struct root {
   version_id_t version = 0;
   digest state_root;
   std::uint64_t state_size = 0;
   digest change_root;
   std::uint64_t change_count = 0;

   bool operator==(const root&) const = default;
};

enum class branch_side : std::uint8_t {
   left,
   right,
};

struct proof_leaf {
   bytes key;
   digest value_hash;
   std::optional<bytes> value;

   bool operator==(const proof_leaf&) const = default;
};

struct proof_branch {
   std::uint16_t height = 0;
   std::uint64_t size = 0;
   bytes min_key;
   bytes max_key;
   bytes separator;
   digest left_hash;
   digest right_hash;

   bool operator==(const proof_branch&) const = default;
};

using proof_sibling = std::variant<proof_leaf, proof_branch>;

struct proof_step {
   branch_side child = branch_side::left;
   std::uint16_t height = 0;
   std::uint64_t subtree_size = 0;
   bytes min_key;
   bytes max_key;
   bytes separator;
   proof_sibling sibling;

   bool operator==(const proof_step&) const = default;
};

struct point_proof {
   root anchor;
   bytes key;
   std::optional<proof_leaf> terminal;
   std::vector<proof_step> path;

   bool operator==(const point_proof&) const = default;
};

struct verified_point {
   bool exists = false;
   std::optional<digest> value_hash;
   std::optional<bytes> value;
   std::uint64_t rank = 0;

   bool operator==(const verified_point&) const = default;
};

enum class proof_tree : std::uint8_t {
   state,
   changes,
};

// Checkpoint framing is independent of the authenticated hash schema.
inline constexpr std::uint8_t checkpoint_format_version = 1;

struct checkpoint_record {
   proof_tree tree = proof_tree::state;
   digest hash;
   proof_sibling node;

   bool operator==(const checkpoint_record&) const = default;
};

// Explicit policy is required: zero-initialized limits admit no checkpoint.
struct checkpoint_limits {
   std::uint64_t max_records = 0;
   std::uint64_t max_encoded_bytes = 0;
   std::uint64_t max_value_bytes = 0;
   std::size_t max_frame_bytes = 0;
   std::size_t max_working_bytes = 0;
   std::uint32_t max_depth = 0;
};

[[nodiscard]] constexpr bool checkpoint_limits_are_valid(const checkpoint_limits& value) noexcept {
   return value.max_records != 0U && value.max_encoded_bytes != 0U && value.max_value_bytes != 0U &&
          value.max_value_bytes <= value.max_encoded_bytes && value.max_frame_bytes != 0U &&
          value.max_frame_bytes <= max_framed_bytes && value.max_frame_bytes <= value.max_encoded_bytes &&
          value.max_working_bytes >= value.max_frame_bytes && value.max_depth != 0U &&
          value.max_depth <= hard_max_proof_depth;
}

struct checkpoint_summary {
   root anchor;
   std::uint64_t records = 0;
   std::uint64_t leaves = 0;
   std::uint64_t value_bytes = 0;
   std::uint64_t encoded_bytes = 0;

   bool operator==(const checkpoint_summary&) const = default;
};

using checkpoint_sink = std::function<boost::asio::awaitable<void>(checkpoint_record)>;
using checkpoint_source = std::function<boost::asio::awaitable<std::optional<checkpoint_record>>()>;
using checkpoint_verify = std::function<boost::asio::awaitable<void>(checkpoint_summary)>;

struct range_request {
   std::optional<bytes> lower;
   std::optional<bytes> upper;
   std::uint32_t limit = 256;
   bool include_values = true;
   bool reverse = false;

   bool operator==(const range_request&) const = default;
};

struct range_inner {
   std::uint16_t height = 0;
   std::uint64_t size = 0;
   bytes min_key;
   bytes max_key;
   bytes separator;

   bool operator==(const range_inner&) const = default;
};

using range_proof_node = std::variant<proof_branch, proof_leaf, range_inner>;

struct range_proof {
   root anchor;
   proof_tree tree = proof_tree::state;
   range_request request;
   std::vector<range_proof_node> nodes;

   bool operator==(const range_proof&) const = default;
};

struct verified_range_item {
   bytes key;
   digest value_hash;
   std::optional<bytes> value;
   std::uint64_t rank = 0;

   bool operator==(const verified_range_item&) const = default;
};

struct verified_range {
   std::vector<verified_range_item> items;
   std::optional<bytes> next_key;
   std::uint64_t total_size = 0;
   bool more = false;

   bool operator==(const verified_range&) const = default;
};

struct staged_version {
   root commitment;
   digest mutation_digest;

   bool operator==(const staged_version&) const = default;
};

struct prune_options {
   std::uint32_t max_versions = 128;
   std::uint32_t max_garbage_records = 4'096;
};

struct prune_result {
   std::uint64_t versions_pruned = 0;
   std::uint64_t nodes_collected = 0;
   std::uint64_t values_collected = 0;
   bool complete = false;

   bool operator==(const prune_result&) const = default;
};

BOOST_DESCRIBE_STRUCT(mutation, (), (key, value))
BOOST_DESCRIBE_STRUCT(root, (), (version, state_root, state_size, change_root, change_count))
BOOST_DESCRIBE_STRUCT(proof_leaf, (), (key, value_hash, value))
BOOST_DESCRIBE_STRUCT(proof_branch, (), (height, size, min_key, max_key, separator, left_hash, right_hash))
BOOST_DESCRIBE_STRUCT(checkpoint_record, (), (tree, hash, node))
BOOST_DESCRIBE_STRUCT(checkpoint_limits, (),
                      (max_records, max_encoded_bytes, max_value_bytes, max_frame_bytes, max_working_bytes, max_depth))
BOOST_DESCRIBE_STRUCT(proof_step, (), (child, height, subtree_size, min_key, max_key, separator, sibling))
BOOST_DESCRIBE_STRUCT(point_proof, (), (anchor, key, terminal, path))
BOOST_DESCRIBE_STRUCT(range_request, (), (lower, upper, limit, include_values, reverse))
BOOST_DESCRIBE_STRUCT(range_inner, (), (height, size, min_key, max_key, separator))
BOOST_DESCRIBE_STRUCT(range_proof, (), (anchor, tree, request, nodes))

} // namespace forge::db::authenticated

FORGE_DECLARE_SERIALIZATION_PACK(forge::db::authenticated::point_proof)
FORGE_DECLARE_SERIALIZATION_PACK(forge::db::authenticated::range_proof)
FORGE_DECLARE_SERIALIZATION_PACK(forge::db::authenticated::checkpoint_record)
