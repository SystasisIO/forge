module;

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module forge.db.authenticated.tree_engine;

import forge.db.authenticated.types;
import forge.db.core.driver;
import forge.db.core.record;

export namespace forge::db::authenticated::detail {

using get_record_fn = std::function<boost::asio::awaitable<std::optional<bytes>>(forge::db::core::record_key)>;
using put_record_fn = std::function<boost::asio::awaitable<void>(forge::db::core::record_key, bytes)>;
using erase_record_fn = std::function<boost::asio::awaitable<void>(forge::db::core::record_key)>;

struct node {
   enum class kind : std::uint8_t {
      leaf = 1,
      inner = 2,
   };

   kind type = kind::leaf;
   std::string domain;
   std::uint16_t height = 0;
   std::uint64_t size = 1;
   bytes key;
   bytes min_key;
   bytes max_key;
   digest value_hash;
   digest left;
   digest right;
};

struct tree_result {
   digest hash;
   std::uint64_t size = 0;
};

class tree_engine {
 public:
   tree_engine(std::string domain, std::optional<digest> root_hash, get_record_fn get, limits settings);

   boost::asio::awaitable<tree_result> apply(std::span<const mutation> mutations);
   boost::asio::awaitable<std::optional<bytes>> get(std::span<const std::byte> key);
   boost::asio::awaitable<verified_range> scan_range(const root& anchor, range_request request, proof_tree tree);
   boost::asio::awaitable<point_proof> prove(const root& anchor, std::span<const std::byte> key, bool include_value);
   boost::asio::awaitable<range_proof> prove_range(const root& anchor, range_request request, proof_tree tree);
   boost::asio::awaitable<void> persist(get_record_fn get, put_record_fn put, erase_record_fn erase);

   [[nodiscard]] std::optional<digest> root_hash() const noexcept;
   [[nodiscard]] std::uint64_t size() const noexcept;

 private:
   boost::asio::awaitable<node> load(const digest& hash);
   boost::asio::awaitable<bytes> load_value(const digest& hash);
   boost::asio::awaitable<bytes> load_value_for_proof(const digest& hash, std::size_t remaining_wire_bytes);
   boost::asio::awaitable<digest> save(node value);
   boost::asio::awaitable<digest> make_inner(const digest& left, const digest& right, std::uint32_t depth);
   boost::asio::awaitable<digest> balance(const digest& left, const digest& right, std::uint32_t depth);
   boost::asio::awaitable<std::optional<digest>> insert(std::optional<digest> current, const bytes& key,
                                                        const digest& value_hash, std::uint32_t depth);
   boost::asio::awaitable<std::optional<digest>> erase(std::optional<digest> current, const bytes& key,
                                                       std::uint32_t depth);
   boost::asio::awaitable<std::uint64_t> lower_bound_rank(const bytes& key);
   boost::asio::awaitable<void> emit_items(const digest& current, std::uint64_t offset, std::uint64_t begin,
                                           std::uint64_t end, bool include_values,
                                           std::vector<verified_range_item>& output, std::uint32_t depth);
   boost::asio::awaitable<void> emit_range(const digest& current, std::uint64_t offset, std::uint64_t witness_begin,
                                           std::uint64_t witness_end, std::uint64_t result_begin,
                                           std::uint64_t result_end, bool include_values,
                                           std::vector<range_proof_node>& output, std::size_t& wire_bytes,
                                           std::uint32_t depth);
   void check_depth(std::uint32_t depth) const;

   std::string domain_;
   std::optional<digest> root_hash_;
   get_record_fn get_;
   limits limits_;
   std::map<digest, node> nodes_;
   std::map<digest, bytes> values_;
   std::map<digest, node> pending_nodes_;
   std::map<digest, bytes> pending_values_;
};

[[nodiscard]] std::vector<mutation> normalize_mutations(std::span<const mutation> mutations, const limits& settings);
[[nodiscard]] forge::db::core::record_key node_key(const digest& hash);
[[nodiscard]] forge::db::core::record_key value_key(const digest& hash);
[[nodiscard]] forge::db::core::record_key version_key(const digest& namespace_id, version_id_t version);
[[nodiscard]] forge::db::core::record_key latest_key(const digest& namespace_id);
[[nodiscard]] forge::db::core::record_key node_reference_key(const digest& hash);
[[nodiscard]] forge::db::core::record_key value_reference_key(const digest& hash);
[[nodiscard]] forge::db::core::record_key node_gc_key(const digest& hash);
[[nodiscard]] forge::db::core::record_key value_gc_key(const digest& hash);
[[nodiscard]] forge::db::core::record_key retention_guard_key(const digest& namespace_id, version_id_t version,
                                                              std::span<const std::byte> token);
[[nodiscard]] forge::db::core::record_key retention_guard_prefix(const digest& namespace_id, version_id_t version);
[[nodiscard]] forge::db::core::record_key version_prefix(const digest& namespace_id);
[[nodiscard]] forge::db::core::record_range record_prefix_range(std::byte prefix);
[[nodiscard]] version_id_t decode_version_key(const forge::db::core::record_key& key, const digest& namespace_id);
[[nodiscard]] digest namespace_id(std::string_view domain);

[[nodiscard]] bytes encode_root(const root& value);
[[nodiscard]] root decode_root(const bytes& value);
boost::asio::awaitable<void> retain_root(get_record_fn get, put_record_fn put, erase_record_fn erase,
                                         const digest& hash);
boost::asio::awaitable<void> release_root(get_record_fn get, put_record_fn put, const digest& hash);

void validate_checkpoint_policy(std::string_view domain, const root& expected, const limits& node_bounds,
                                const checkpoint_limits& checkpoint_bounds);
boost::asio::awaitable<checkpoint_summary> export_checkpoint_graphs(std::string_view domain, root expected,
                                                                    get_record_fn get, checkpoint_sink sink,
                                                                    limits node_bounds,
                                                                    checkpoint_limits checkpoint_bounds);
boost::asio::awaitable<checkpoint_summary> restore_checkpoint_graphs(std::string_view domain, root expected,
                                                                     get_record_fn get, put_record_fn put,
                                                                     erase_record_fn erase, checkpoint_source source,
                                                                     limits node_bounds,
                                                                     checkpoint_limits checkpoint_bounds);

struct garbage_result {
   std::uint64_t nodes = 0;
   std::uint64_t values = 0;
   bool pending = false;
};

boost::asio::awaitable<garbage_result> collect_garbage(forge::db::core::transaction& active,
                                                       forge::db::core::family family, std::uint32_t limit);

} // namespace forge::db::authenticated::detail
