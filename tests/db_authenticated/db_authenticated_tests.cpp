#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <exception>
#include <limits>
#include <memory>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

import forge.asio.affine;
import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.db.authenticated.codec;
import forge.db.authenticated.exceptions;
import forge.db.authenticated.hash;
import forge.db.authenticated.proof;
import forge.db.authenticated.standards;
import forge.db.authenticated.store;
import forge.db.authenticated.transaction;
import forge.db.authenticated.types;
import forge.db.core.driver;
import forge.db.core.exceptions;
import forge.db.core.record;
import forge.db.mdbx.driver;
import forge.db.object.store;
import forge.db.revision.store;
import forge.raw.raw;

namespace {

std::filesystem::path make_test_root(std::string name) {
   static auto sequence = std::atomic_uint64_t{0};
   auto root = std::filesystem::temp_directory_path() /
               (std::move(name) + "_" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
   std::filesystem::remove_all(root);
   return root;
}

forge::db::authenticated::bytes bytes(std::string value) {
   return {
       reinterpret_cast<const std::byte*>(value.data()),
       reinterpret_cast<const std::byte*>(value.data() + value.size()),
   };
}

std::string text(const forge::db::authenticated::bytes& value) {
   return {
       reinterpret_cast<const char*>(value.data()),
       reinterpret_cast<const char*>(value.data() + value.size()),
   };
}

forge::db::authenticated::digest hash_text(std::string value) {
   const auto encoded = bytes(std::move(value));
   return forge::db::authenticated::hash_value(encoded);
}

forge::db::authenticated::mutation put(std::string key, std::string value) {
   return {
       .key = bytes(std::move(key)),
       .value = bytes(std::move(value)),
   };
}

forge::db::authenticated::mutation erase(std::string key) {
   return {.key = bytes(std::move(key))};
}

boost::asio::awaitable<std::shared_ptr<forge::db::mdbx::driver>> open_driver(const std::filesystem::path& path,
                                                                             const forge::asio::affine::lane& lane) {
   co_return co_await forge::db::mdbx::driver::open(
       forge::db::mdbx::config{
           .path = path.string(),
           .families = {"authenticated", "objectdb"},
       },
       lane.get_executor());
}

forge::db::authenticated::store make_store(const std::shared_ptr<forge::db::mdbx::driver>& driver,
                                           std::string domain = "forge.test.authenticated.state.v1") {
   return forge::db::authenticated::store{
       driver,
       {
           .family = forge::db::core::family{"authenticated"},
           .domain = std::move(domain),
       },
   };
}

class throwing_driver final : public forge::db::core::driver {
 public:
   boost::asio::awaitable<void> async_flush(bool) override {
      co_return;
   }

 private:
   boost::asio::awaitable<std::unique_ptr<forge::db::core::session>> open_transaction() override {
      throw std::runtime_error{"test authenticated transaction backend failure"};
      co_return nullptr;
   }

   boost::asio::awaitable<std::unique_ptr<forge::db::core::session>> open_snapshot() override {
      throw std::runtime_error{"test authenticated snapshot backend failure"};
      co_return nullptr;
   }
};

bool key_less(const forge::db::authenticated::bytes& left, const forge::db::authenticated::bytes& right) {
   return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
}

struct inspected_node {
   std::uint16_t height = 0;
   std::uint64_t size = 0;
   forge::db::authenticated::bytes min_key;
   forge::db::authenticated::bytes max_key;
};

inspected_node inspect_expanded_tree(const std::vector<forge::db::authenticated::range_proof_node>& nodes,
                                     std::size_t& next) {
   if (next >= nodes.size()) {
      throw std::runtime_error{"truncated authenticated proof tree"};
   }
   const auto& encoded = nodes[next++];
   if (const auto* leaf = std::get_if<forge::db::authenticated::proof_leaf>(&encoded)) {
      return {
          .height = 0,
          .size = 1,
          .min_key = leaf->key,
          .max_key = leaf->key,
      };
   }
   const auto* inner = std::get_if<forge::db::authenticated::range_inner>(&encoded);
   if (!inner) {
      throw std::runtime_error{"fully expanded proof contains an opaque branch"};
   }
   const auto left = inspect_expanded_tree(nodes, next);
   const auto right = inspect_expanded_tree(nodes, next);
   const auto height = static_cast<std::uint16_t>(std::max(left.height, right.height) + 1U);
   if (inner->height != height || inner->size != left.size + right.size || inner->min_key != left.min_key ||
       inner->max_key != right.max_key || inner->separator != right.min_key || !key_less(left.max_key, right.min_key) ||
       std::abs(static_cast<int>(left.height) - static_cast<int>(right.height)) > 1) {
      throw std::runtime_error{"authenticated proof violates AVL+ ordering"};
   }
   return {
       .height = inner->height,
       .size = inner->size,
       .min_key = inner->min_key,
       .max_key = inner->max_key,
   };
}

boost::asio::awaitable<void> check_ranked_version(forge::db::authenticated::store& authenticated,
                                                  std::string_view domain,
                                                  forge::db::authenticated::version_id_t version,
                                                  const std::vector<std::string>& expected) {
   auto request = forge::db::authenticated::range_request{
       .limit = 32,
       .include_values = false,
   };
   const auto proof = co_await authenticated.prove_range(version, request);
   const auto verified = forge::db::authenticated::verify_range(domain, proof.anchor, request, proof.tree, proof);
   BOOST_REQUIRE_EQUAL(verified.items.size(), expected.size());
   BOOST_TEST(verified.total_size == expected.size());
   BOOST_TEST(!verified.more);
   for (auto index = std::size_t{}; index < expected.size(); ++index) {
      BOOST_TEST(text(verified.items[index].key) == expected[index]);
      BOOST_TEST(verified.items[index].rank == index);
      const auto point = co_await authenticated.prove(version, bytes(expected[index]), false);
      const auto verified_point = forge::db::authenticated::verify_point(domain, point.anchor, point.key, point);
      BOOST_TEST(verified_point.exists);
      BOOST_TEST(verified_point.rank == index);
   }
   if (!proof.nodes.empty()) {
      auto next = std::size_t{};
      const auto shape = inspect_expanded_tree(proof.nodes, next);
      BOOST_TEST(next == proof.nodes.size());
      BOOST_TEST(shape.size == expected.size());
   }
   co_return;
}

std::uint64_t decode_be64(const std::vector<std::byte>& value) {
   if (value.size() != sizeof(std::uint64_t)) {
      throw std::runtime_error{"invalid authenticated reference record"};
   }
   auto result = std::uint64_t{};
   for (const auto byte : value) {
      result = (result << 8U) | std::to_integer<std::uint8_t>(byte);
   }
   return result;
}

forge::db::core::record_range prefix_range(std::byte prefix) {
   auto key = forge::db::core::record_key{std::vector<std::byte>{prefix}};
   return {
       .begin = key,
       .prefix = std::move(key),
       .has_end = false,
   };
}

} // namespace

BOOST_AUTO_TEST_SUITE(db_authenticated_test_suite)

BOOST_AUTO_TEST_CASE(authenticated_backend_failures_are_typed) {
   auto driver = std::make_shared<throwing_driver>();
   auto authenticated = forge::db::authenticated::store{
       driver,
       {
           .family = forge::db::core::family{"authenticated"},
           .domain = "forge.test.authenticated.backend-failure.v1",
       },
   };
   auto runtime = forge::asio::runtime{};
   BOOST_CHECK_THROW(static_cast<void>(forge::asio::blocking::run(runtime, authenticated.latest())),
                     forge::db::authenticated::exceptions::backend_failure);
}

BOOST_AUTO_TEST_CASE(authenticated_read_observer_failures_are_typed) {
   const auto root_path = make_test_root("forge_db_authenticated_observer_failure");
   auto runtime = forge::asio::runtime{};
   auto lane = forge::asio::affine::lane{};

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto driver = co_await open_driver(root_path / "store", lane);
      constexpr auto domain = "forge.test.authenticated.observer.v1";
      auto authenticated = make_store(driver, domain);
      auto active = co_await driver->begin_transaction();
      auto transaction = co_await authenticated.join(active, 0);
      static_cast<void>(co_await transaction.stage(std::vector{put("key", "value")}));
      co_await active.commit();

      const auto require_typed = [&](auto observer) -> boost::asio::awaitable<void> {
         auto observed = forge::db::authenticated::store{
             driver,
             {
                 .family = forge::db::core::family{"authenticated"},
                 .domain = domain,
                 .read_observer = std::move(observer),
             },
         };
         auto typed = false;
         try {
            static_cast<void>(co_await observed.get(0, bytes("key")));
         } catch (const forge::db::authenticated::exceptions::backend_failure&) {
            typed = true;
         }
         BOOST_TEST(typed);
      };

      co_await require_typed(
          [](const forge::db::core::record_key&) { throw std::runtime_error{"test authenticated observer failure"}; });
      co_await require_typed([](const forge::db::core::record_key&) { throw 7; });

      co_await driver->async_close();
      driver.reset();
      co_await lane.shutdown();
   }());

   std::filesystem::remove_all(root_path);
}

BOOST_AUTO_TEST_CASE(authenticated_codec_rejects_unbounded_and_invalid_inputs) {
   constexpr auto forge_support =
       forge::db::authenticated::support_for(forge::db::authenticated::proof_standard::forge_v3);
   constexpr auto ics23_support =
       forge::db::authenticated::support_for(forge::db::authenticated::proof_standard::cosmos_ics23_v1);
   static_assert(forge_support.native_codec && forge_support.native_verifier);
   static_assert(!ics23_support.native_codec && !ics23_support.native_verifier);

   auto point = forge::db::authenticated::point_proof{};
   point.path.push_back(forge::db::authenticated::proof_step{
       .child = static_cast<forge::db::authenticated::branch_side>(2),
       .sibling = forge::db::authenticated::proof_leaf{},
   });
   BOOST_CHECK_THROW(static_cast<void>(forge::db::authenticated::decode_point(forge::db::authenticated::encode(point))),
                     forge::db::authenticated::exceptions::invalid_proof);

   auto encoded_range = forge::db::authenticated::encode(forge::db::authenticated::range_proof{});
   BOOST_REQUIRE(!encoded_range.empty());
   encoded_range.pop_back();
   const auto oversized_count = forge::raw::pack(forge::unsigned_int{100'001});
   for (const auto byte : oversized_count) {
      encoded_range.push_back(static_cast<std::byte>(byte));
   }
   BOOST_CHECK_THROW(static_cast<void>(forge::db::authenticated::decode_range(encoded_range)),
                     forge::db::authenticated::exceptions::proof_limit_exceeded);

   auto truncated_max_count = forge::db::authenticated::encode(forge::db::authenticated::range_proof{});
   truncated_max_count.pop_back();
   for (const auto byte : {std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0x0f}}) {
      truncated_max_count.push_back(byte);
   }
   auto oversized_node_limits = forge::db::authenticated::limits{};
   oversized_node_limits.max_proof_nodes = std::numeric_limits<std::uint32_t>::max();
   BOOST_CHECK_THROW(
       static_cast<void>(forge::db::authenticated::decode_range(truncated_max_count, oversized_node_limits)),
       forge::db::authenticated::exceptions::proof_limit_exceeded);

   auto truncated_bounded_count = forge::db::authenticated::encode(forge::db::authenticated::range_proof{});
   truncated_bounded_count.pop_back();
   const auto bounded_count = forge::raw::pack(forge::unsigned_int{forge::db::authenticated::max_wire_proof_nodes});
   for (const auto byte : bounded_count) {
      truncated_bounded_count.push_back(static_cast<std::byte>(byte));
   }
   auto bounded_node_limits = forge::db::authenticated::limits{};
   bounded_node_limits.max_proof_nodes = forge::db::authenticated::max_wire_proof_nodes;
   BOOST_CHECK_THROW(
       static_cast<void>(forge::db::authenticated::decode_range(truncated_bounded_count, bounded_node_limits)),
       forge::db::authenticated::exceptions::invalid_proof);

   const auto valid = forge::db::authenticated::point_proof{};
   const auto encoded_valid = forge::db::authenticated::encode(valid);
   BOOST_TEST(forge::db::authenticated::wire_size(valid) == encoded_valid.size());
   BOOST_CHECK(forge::db::authenticated::decode_point(encoded_valid) == valid);

   auto exact_limits = forge::db::authenticated::limits{};
   exact_limits.max_proof_bytes = encoded_valid.size();
   BOOST_CHECK(forge::db::authenticated::decode_point(encoded_valid, exact_limits) == valid);
   --exact_limits.max_proof_bytes;
   BOOST_CHECK_THROW(static_cast<void>(forge::db::authenticated::decode_point(encoded_valid, exact_limits)),
                     forge::db::authenticated::exceptions::proof_limit_exceeded);

   auto invalid_limits = forge::db::authenticated::limits{};
   invalid_limits.max_proof_bytes = forge::db::authenticated::max_wire_proof_bytes + 1U;
   BOOST_CHECK_THROW(static_cast<void>(forge::db::authenticated::decode_point(encoded_valid, invalid_limits)),
                     forge::db::authenticated::exceptions::proof_limit_exceeded);

   auto count_boundary = forge::db::authenticated::range_proof{};
   for (auto index = std::uint32_t{}; index < 128U; ++index) {
      count_boundary.nodes.emplace_back(forge::db::authenticated::proof_leaf{
          .key = bytes(std::to_string(index)),
          .value_hash = hash_text(std::to_string(index)),
      });
   }
   const auto encoded_boundary = forge::db::authenticated::encode(count_boundary);
   BOOST_TEST(forge::db::authenticated::wire_size(count_boundary) == encoded_boundary.size());
   BOOST_CHECK(forge::db::authenticated::decode_range(encoded_boundary) == count_boundary);
}

BOOST_AUTO_TEST_CASE(authenticated_proof_depth_has_a_hard_cap) {
   const auto domain = std::string{"forge.test.authenticated.depth.v3"};
   const auto request = forge::db::authenticated::range_request{};
   auto proof = forge::db::authenticated::range_proof{
       .anchor =
           {
               .state_root = hash_text("hostile-depth"),
               .state_size = 1,
           },
       .request = request,
   };
   for (auto depth = std::uint32_t{}; depth <= forge::db::authenticated::hard_max_proof_depth; ++depth) {
      proof.nodes.emplace_back(forge::db::authenticated::range_inner{
          .height = 1,
          .size = 2,
          .min_key = bytes("a"),
          .max_key = bytes("b"),
          .separator = bytes("b"),
      });
   }
   proof.nodes.emplace_back(forge::db::authenticated::proof_leaf{
       .key = bytes("a"),
       .value_hash = hash_text("value"),
   });

   BOOST_CHECK_THROW(
       static_cast<void>(forge::db::authenticated::verify_range(domain, proof.anchor, request, proof.tree, proof)),
       forge::db::authenticated::exceptions::proof_limit_exceeded);

   auto invalid_limits = forge::db::authenticated::limits{};
   invalid_limits.max_proof_depth = forge::db::authenticated::hard_max_proof_depth + 1U;
   BOOST_CHECK_THROW(static_cast<void>(forge::db::authenticated::verify_range(domain, proof.anchor, request, proof.tree,
                                                                              proof, invalid_limits)),
                     forge::db::authenticated::exceptions::proof_limit_exceeded);
}

BOOST_AUTO_TEST_CASE(authenticated_hash_roles_and_persisted_format_are_breaking_v3) {
   constexpr auto base = std::string_view{"forge.test.authenticated.role"};
   const auto state =
       forge::db::authenticated::canonical_tree_domain(base, forge::db::authenticated::proof_tree::state);
   const auto changes =
       forge::db::authenticated::canonical_tree_domain(base, forge::db::authenticated::proof_tree::changes);
   const auto formerly_colliding_state = forge::db::authenticated::canonical_tree_domain(
       "forge.test.authenticated.role.changes", forge::db::authenticated::proof_tree::state);

   BOOST_TEST(forge::db::authenticated::hash_schema_version == 3U);
   BOOST_TEST(state != changes);
   BOOST_TEST(state.front() != changes.front());
   BOOST_TEST(std::string_view{state}.substr(1) == base);
   BOOST_TEST(std::string_view{changes}.substr(1) == base);
   BOOST_TEST(formerly_colliding_state != changes);
}

BOOST_AUTO_TEST_CASE(authenticated_generation_uses_exact_wire_budget) {
   const auto root_path = make_test_root("forge_db_authenticated_wire_budget");
   auto runtime = forge::asio::runtime{};
   auto lane = forge::asio::affine::lane{};

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto driver = co_await open_driver(root_path / "store", lane);

      const auto point_budget = forge::db::authenticated::wire_size(forge::db::authenticated::point_proof{});
      auto point_store = forge::db::authenticated::store{
          driver,
          {
              .family = forge::db::core::family{"authenticated"},
              .domain = "forge.test.authenticated.wire.point.v3",
              .bounds = {.max_proof_bytes = point_budget},
          },
      };
      const auto no_changes = std::vector<forge::db::authenticated::mutation>{};
      const auto empty_key = forge::db::authenticated::bytes{};
      auto point_db = co_await driver->begin_transaction();
      auto point_tx = co_await point_store.join(point_db, 0);
      static_cast<void>(co_await point_tx.stage(no_changes));
      co_await point_db.commit();
      const auto point = co_await point_store.prove(0, empty_key, false);
      BOOST_TEST(forge::db::authenticated::wire_size(point) == point_budget);
      BOOST_CHECK(forge::db::authenticated::decode_point(forge::db::authenticated::encode(point),
                                                         {.max_proof_bytes = point_budget}) == point);

      auto point_rejecting_store = forge::db::authenticated::store{
          driver,
          {
              .family = forge::db::core::family{"authenticated"},
              .domain = "forge.test.authenticated.wire.point.reject.v3",
              .bounds = {.max_proof_bytes = point_budget - 1U},
          },
      };
      auto point_rejecting_db = co_await driver->begin_transaction();
      auto point_rejecting_tx = co_await point_rejecting_store.join(point_rejecting_db, 0);
      static_cast<void>(co_await point_rejecting_tx.stage(no_changes));
      co_await point_rejecting_db.commit();
      auto rejected_point = false;
      try {
         static_cast<void>(co_await point_rejecting_store.prove(0, empty_key, false));
      } catch (const forge::db::authenticated::exceptions::proof_limit_exceeded&) {
         rejected_point = true;
      }
      BOOST_TEST(rejected_point);

      const auto range_budget = forge::db::authenticated::wire_size(forge::db::authenticated::range_proof{});
      auto range_store = forge::db::authenticated::store{
          driver,
          {
              .family = forge::db::core::family{"authenticated"},
              .domain = "forge.test.authenticated.wire.range.v3",
              .bounds = {.max_proof_bytes = range_budget},
          },
      };
      auto range_db = co_await driver->begin_transaction();
      auto range_tx = co_await range_store.join(range_db, 0);
      static_cast<void>(co_await range_tx.stage(no_changes));
      co_await range_db.commit();
      const auto range = co_await range_store.prove_range(0, forge::db::authenticated::range_request{});
      BOOST_TEST(forge::db::authenticated::wire_size(range) == range_budget);
      BOOST_CHECK(forge::db::authenticated::decode_range(forge::db::authenticated::encode(range),
                                                         {.max_proof_bytes = range_budget}) == range);

      auto range_rejecting_store = forge::db::authenticated::store{
          driver,
          {
              .family = forge::db::core::family{"authenticated"},
              .domain = "forge.test.authenticated.wire.range.reject.v3",
              .bounds = {.max_proof_bytes = range_budget - 1U},
          },
      };
      auto range_rejecting_db = co_await driver->begin_transaction();
      auto range_rejecting_tx = co_await range_rejecting_store.join(range_rejecting_db, 0);
      static_cast<void>(co_await range_rejecting_tx.stage(no_changes));
      co_await range_rejecting_db.commit();
      auto rejected_range = false;
      try {
         static_cast<void>(co_await range_rejecting_store.prove_range(0, forge::db::authenticated::range_request{}));
      } catch (const forge::db::authenticated::exceptions::proof_limit_exceeded&) {
         rejected_range = true;
      }
      BOOST_TEST(rejected_range);

      co_await driver->async_close();
      driver.reset();
      co_await lane.shutdown();
   }());

   std::filesystem::remove_all(root_path);
}

BOOST_AUTO_TEST_CASE(authenticated_range_budget_stops_value_fetches_early) {
   constexpr auto domain = std::string_view{"forge.test.authenticated.range-fetch-budget.v3"};
   const auto root_path = make_test_root("forge_db_authenticated_range_fetch_budget");
   auto runtime = forge::asio::runtime{};
   auto lane = forge::asio::affine::lane{};

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto driver = co_await open_driver(root_path / "store", lane);
      auto value_fetches = std::size_t{};
      auto authenticated = forge::db::authenticated::store{
          driver,
          {
              .family = forge::db::core::family{"authenticated"},
              .domain = std::string{domain},
              .bounds = {.max_proof_bytes = 8U << 10U},
              .read_observer =
                  [&value_fetches](const forge::db::core::record_key& key) {
                     if (!key.bytes().empty() && key.bytes().front() == std::byte{2}) {
                        ++value_fetches;
                     }
                  },
          },
      };
      auto changes = std::vector<forge::db::authenticated::mutation>{};
      changes.reserve(64);
      for (auto index = std::uint32_t{}; index < 64U; ++index) {
         auto key = std::string{"key-"};
         if (index < 10U) {
            key.push_back('0');
         }
         key += std::to_string(index);
         changes.push_back(put(std::move(key), std::string(64U << 10U, static_cast<char>('a' + (index % 26U)))));
      }

      auto active = co_await driver->begin_transaction();
      auto transaction = co_await authenticated.join(active, 0);
      static_cast<void>(co_await transaction.stage(changes));
      co_await active.commit();

      auto rejected = false;
      try {
         static_cast<void>(co_await authenticated.prove_range(0,
                                                              {
                                                                  .limit = 64,
                                                                  .include_values = true,
                                                              },
                                                              forge::db::authenticated::proof_tree::state));
      } catch (const forge::db::authenticated::exceptions::proof_limit_exceeded&) {
         rejected = true;
      }
      BOOST_TEST(rejected);
      BOOST_TEST(value_fetches == 1U);

      co_await driver->async_close();
      driver.reset();
      co_await lane.shutdown();
   }());

   std::filesystem::remove_all(root_path);
}

BOOST_AUTO_TEST_CASE(authenticated_proofs_reject_inconsistent_ordered_bounds) {
   constexpr auto domain = std::string_view{"forge.test.authenticated.ordered.v3"};
   const auto state_domain =
       forge::db::authenticated::canonical_tree_domain(domain, forge::db::authenticated::proof_tree::state);
   const auto a = forge::db::authenticated::proof_leaf{
       .key = bytes("a"),
       .value_hash = hash_text("1"),
   };
   const auto c = forge::db::authenticated::proof_leaf{
       .key = bytes("c"),
       .value_hash = hash_text("3"),
   };
   const auto malformed_separator = bytes("z");
   const auto anchor = forge::db::authenticated::root{
       .state_root =
           forge::db::authenticated::hash_inner(state_domain, 1, 2, a.key, c.key, malformed_separator,
                                                forge::db::authenticated::hash_leaf(state_domain, a.key, a.value_hash),
                                                forge::db::authenticated::hash_leaf(state_domain, c.key, c.value_hash)),
       .state_size = 2,
   };
   const auto point = forge::db::authenticated::point_proof{
       .anchor = anchor,
       .key = c.key,
       .terminal = a,
       .path = {{
           .child = forge::db::authenticated::branch_side::left,
           .height = 1,
           .subtree_size = 2,
           .min_key = a.key,
           .max_key = c.key,
           .separator = malformed_separator,
           .sibling = c,
       }},
   };
   BOOST_CHECK_THROW(static_cast<void>(forge::db::authenticated::verify_point(domain, anchor, c.key, point)),
                     forge::db::authenticated::exceptions::invalid_proof);

   const auto d = forge::db::authenticated::proof_leaf{
       .key = bytes("d"),
       .value_hash = hash_text("4"),
   };
   const auto malformed_opaque = forge::db::authenticated::proof_branch{
       .height = 1,
       .size = 2,
       .min_key = a.key,
       .max_key = c.key,
       .separator = a.key,
       .left_hash = forge::db::authenticated::hash_leaf(state_domain, a.key, a.value_hash),
       .right_hash = forge::db::authenticated::hash_leaf(state_domain, c.key, c.value_hash),
   };
   const auto malformed_opaque_hash = forge::db::authenticated::hash_inner(
       state_domain, malformed_opaque.height, malformed_opaque.size, malformed_opaque.min_key, malformed_opaque.max_key,
       malformed_opaque.separator, malformed_opaque.left_hash, malformed_opaque.right_hash);
   const auto opaque_anchor = forge::db::authenticated::root{
       .state_root =
           forge::db::authenticated::hash_inner(state_domain, 2, 3, a.key, d.key, d.key, malformed_opaque_hash,
                                                forge::db::authenticated::hash_leaf(state_domain, d.key, d.value_hash)),
       .state_size = 3,
   };
   const auto opaque_point = forge::db::authenticated::point_proof{
       .anchor = opaque_anchor,
       .key = d.key,
       .terminal = d,
       .path = {{
           .child = forge::db::authenticated::branch_side::right,
           .height = 2,
           .subtree_size = 3,
           .min_key = a.key,
           .max_key = d.key,
           .separator = d.key,
           .sibling = malformed_opaque,
       }},
   };
   BOOST_CHECK_THROW(
       static_cast<void>(forge::db::authenticated::verify_point(domain, opaque_anchor, d.key, opaque_point)),
       forge::db::authenticated::exceptions::invalid_proof);

   const auto request = forge::db::authenticated::range_request{};
   const auto range = forge::db::authenticated::range_proof{
       .anchor = anchor,
       .request = request,
       .nodes =
           {
               forge::db::authenticated::range_inner{
                   .height = 1,
                   .size = 2,
                   .min_key = a.key,
                   .max_key = c.key,
                   .separator = malformed_separator,
               },
               a,
               c,
           },
   };
   BOOST_CHECK_THROW(
       static_cast<void>(forge::db::authenticated::verify_range(domain, anchor, request, range.tree, range)),
       forge::db::authenticated::exceptions::invalid_proof);
}

BOOST_AUTO_TEST_CASE(authenticated_dragonfruit_forgery_is_rejected) {
   constexpr auto domain = std::string_view{"forge.test.authenticated.dragonfruit.v3"};
   const auto state_domain =
       forge::db::authenticated::canonical_tree_domain(domain, forge::db::authenticated::proof_tree::state);
   const auto left = forge::db::authenticated::proof_leaf{
       .key = {std::byte{0x11}},
       .value_hash = hash_text("left"),
   };
   const auto right = forge::db::authenticated::proof_leaf{
       .key = {std::byte{0x32}},
       .value_hash = hash_text("right"),
   };
   const auto anchor = forge::db::authenticated::root{
       .state_root = forge::db::authenticated::hash_inner(
           state_domain, 1, 2, left.key, right.key, right.key,
           forge::db::authenticated::hash_leaf(state_domain, left.key, left.value_hash),
           forge::db::authenticated::hash_leaf(state_domain, right.key, right.value_hash)),
       .state_size = 2,
   };
   const auto valid = forge::db::authenticated::point_proof{
       .anchor = anchor,
       .key = right.key,
       .terminal = right,
       .path = {{
           .child = forge::db::authenticated::branch_side::right,
           .height = 1,
           .subtree_size = 2,
           .min_key = left.key,
           .max_key = right.key,
           .separator = right.key,
           .sibling = left,
       }},
   };
   BOOST_TEST(forge::db::authenticated::verify_point(domain, anchor, right.key, valid).exists);

   const auto forged_leaf = forge::db::authenticated::proof_leaf{
       .key = {std::byte{0xff}},
       .value_hash = hash_text("forged"),
   };
   auto forged_point = valid;
   forged_point.path.push_back({
       .child = forge::db::authenticated::branch_side::left,
       .height = 2,
       .subtree_size = 3,
       .min_key = left.key,
       .max_key = forged_leaf.key,
       .separator = forged_leaf.key,
       .sibling = forged_leaf,
   });
   forged_point = forge::db::authenticated::decode_point(forge::db::authenticated::encode(forged_point));
   BOOST_CHECK_THROW(static_cast<void>(forge::db::authenticated::verify_point(domain, anchor, right.key, forged_point)),
                     forge::db::authenticated::exceptions::invalid_proof);

   auto forged_range = forge::db::authenticated::range_proof{
       .anchor = anchor,
       .nodes =
           {
               forge::db::authenticated::range_inner{
                   .height = 1,
                   .size = 2,
                   .min_key = left.key,
                   .max_key = right.key,
                   .separator = right.key,
               },
               left,
               right,
               forged_leaf,
           },
   };
   forged_range = forge::db::authenticated::decode_range(forge::db::authenticated::encode(forged_range));
   BOOST_CHECK_THROW(static_cast<void>(forge::db::authenticated::verify_range(domain, anchor, forged_range.request,
                                                                              forged_range.tree, forged_range)),
                     forge::db::authenticated::exceptions::invalid_proof);
}

BOOST_AUTO_TEST_CASE(authenticated_versions_are_deterministic_and_restart_safe) {
   const auto root_path = make_test_root("forge_db_authenticated_versions");
   auto runtime = forge::asio::runtime{};
   auto lane = forge::asio::affine::lane{{.thread_name = "authenticated-test"}};

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto driver = co_await open_driver(root_path / "store", lane);
      auto authenticated = make_store(driver);

      const auto initial = std::vector<forge::db::authenticated::mutation>{
          put("gamma", "3"),
          put("alpha", "old"),
          put("beta", "2"),
          put("alpha", "1"),
      };
      auto first_db = co_await driver->begin_transaction();
      auto first = co_await authenticated.join(first_db, 0);
      const auto preview = co_await first.preview(initial);
      const auto staged = co_await first.stage(initial, preview.commitment.state_root);
      BOOST_CHECK(staged == preview);
      BOOST_TEST(staged.commitment.state_size == 3U);
      BOOST_TEST(staged.commitment.change_count == 3U);
      co_await first_db.commit();

      {
         auto records = co_await driver->begin_read();
         for (const auto prefix : {std::byte{5}, std::byte{6}}) {
            const auto references =
                co_await records.scan_page(forge::db::core::family{"authenticated"}, prefix_range(prefix),
                                           {.limit = forge::db::core::max_page_limit});
            BOOST_REQUIRE(!references.entries.empty());
            for (const auto& entry : references.entries) {
               BOOST_TEST(decode_be64(entry.value) > 0U);
            }
         }
         for (const auto prefix : {std::byte{3}, std::byte{4}}) {
            const auto roots = co_await records.scan_page(forge::db::core::family{"authenticated"},
                                                          prefix_range(prefix), {.limit = 2});
            BOOST_REQUIRE(!roots.entries.empty());
            for (const auto& entry : roots.entries) {
               BOOST_REQUIRE(!entry.value.empty());
               BOOST_TEST(std::to_integer<std::uint8_t>(entry.value.front()) == 4U);
            }
         }
      }

      const auto latest = co_await authenticated.latest();
      BOOST_REQUIRE(latest.has_value());
      BOOST_CHECK(*latest == staged.commitment);
      BOOST_TEST(text(*(co_await authenticated.get(0, bytes("alpha")))) == "1");

      const auto membership = co_await authenticated.prove(0, bytes("beta"));
      const auto verified_membership = forge::db::authenticated::verify_point(
          "forge.test.authenticated.state.v1", membership.anchor, membership.key, membership);
      BOOST_TEST(verified_membership.exists);
      BOOST_REQUIRE(verified_membership.value.has_value());
      BOOST_TEST(text(*verified_membership.value) == "2");

      const auto absence = co_await authenticated.prove(0, bytes("delta"));
      const auto verified_absence = forge::db::authenticated::verify_point("forge.test.authenticated.state.v1",
                                                                           absence.anchor, absence.key, absence);
      BOOST_TEST(!verified_absence.exists);

      const auto hash_only = co_await authenticated.prove(0, bytes("beta"), false);
      const auto verified_hash_only = forge::db::authenticated::verify_point(
          "forge.test.authenticated.state.v1", hash_only.anchor, hash_only.key, hash_only);
      BOOST_TEST(verified_hash_only.exists);
      BOOST_REQUIRE(verified_hash_only.value_hash.has_value());
      BOOST_TEST(!verified_hash_only.value.has_value());

      const auto second_changes = std::vector<forge::db::authenticated::mutation>{
          erase("alpha"),
          put("beta", "22"),
          put("delta", "4"),
      };
      auto second_db = co_await driver->begin_transaction();
      auto second = co_await authenticated.join(second_db, 1);
      const auto second_staged = co_await second.stage(second_changes);
      BOOST_TEST(second_staged.commitment.state_size == 3U);
      co_await second_db.commit();

      BOOST_TEST(!(co_await authenticated.get(1, bytes("alpha"))).has_value());
      BOOST_TEST(text(*(co_await authenticated.get(1, bytes("beta")))) == "22");
      BOOST_TEST(text(*(co_await authenticated.get(0, bytes("beta")))) == "2");

      const auto first_page_proof = co_await authenticated.prove_range(1, forge::db::authenticated::range_request{
                                                                              .lower = bytes("b"),
                                                                              .upper = bytes("z"),
                                                                              .limit = 1,
                                                                          });
      BOOST_TEST(forge::db::authenticated::wire_size(first_page_proof) ==
                 forge::db::authenticated::encode(first_page_proof).size());
      const auto first_page =
          forge::db::authenticated::verify_range("forge.test.authenticated.state.v1", first_page_proof.anchor,
                                                 first_page_proof.request, first_page_proof.tree, first_page_proof);
      BOOST_REQUIRE_EQUAL(first_page.items.size(), 1U);
      BOOST_TEST(text(first_page.items.front().key) == "beta");
      BOOST_REQUIRE(first_page.items.front().value.has_value());
      BOOST_TEST(text(*first_page.items.front().value) == "22");
      BOOST_TEST(first_page.more);
      BOOST_REQUIRE(first_page.next_key.has_value());
      BOOST_TEST(text(*first_page.next_key) == "delta");

      const auto bounded_forward_proof = co_await authenticated.prove_range(
          1, forge::db::authenticated::range_request{.lower = bytes("b"), .upper = bytes("gamma"), .limit = 1});
      const auto bounded_forward = forge::db::authenticated::verify_range(
          "forge.test.authenticated.state.v1", bounded_forward_proof.anchor, bounded_forward_proof.request,
          bounded_forward_proof.tree, bounded_forward_proof);
      BOOST_REQUIRE_EQUAL(bounded_forward.items.size(), 1U);
      BOOST_TEST(text(bounded_forward.items.front().key) == "beta");
      BOOST_TEST(bounded_forward.more);
      BOOST_REQUIRE(bounded_forward.next_key.has_value());
      BOOST_TEST(text(*bounded_forward.next_key) == "delta");

      const auto reverse_page_proof = co_await authenticated.prove_range(1, forge::db::authenticated::range_request{
                                                                                .lower = bytes("b"),
                                                                                .upper = bytes("z"),
                                                                                .limit = 2,
                                                                                .reverse = true,
                                                                            });
      BOOST_CHECK(forge::db::authenticated::decode_range(forge::db::authenticated::encode(reverse_page_proof)) ==
                  reverse_page_proof);
      const auto reverse_page = forge::db::authenticated::verify_range(
          "forge.test.authenticated.state.v1", reverse_page_proof.anchor, reverse_page_proof.request,
          reverse_page_proof.tree, reverse_page_proof);
      BOOST_REQUIRE_EQUAL(reverse_page.items.size(), 2U);
      BOOST_TEST(text(reverse_page.items[0].key) == "gamma");
      BOOST_TEST(text(reverse_page.items[1].key) == "delta");
      BOOST_TEST(reverse_page.items[0].rank > reverse_page.items[1].rank);
      BOOST_TEST(reverse_page.more);
      BOOST_REQUIRE(reverse_page.next_key.has_value());
      BOOST_TEST(text(*reverse_page.next_key) == "delta");

      const auto scanned_reverse_page = co_await authenticated.scan_range(1, forge::db::authenticated::range_request{
                                                                                 .lower = bytes("b"),
                                                                                 .upper = bytes("z"),
                                                                                 .limit = 2,
                                                                                 .reverse = true,
                                                                             });
      BOOST_CHECK(scanned_reverse_page == reverse_page);

      const auto reverse_tail_proof = co_await authenticated.prove_range(1, forge::db::authenticated::range_request{
                                                                                .lower = bytes("b"),
                                                                                .upper = reverse_page.next_key,
                                                                                .limit = 2,
                                                                                .reverse = true,
                                                                            });
      const auto reverse_tail = forge::db::authenticated::verify_range(
          "forge.test.authenticated.state.v1", reverse_tail_proof.anchor, reverse_tail_proof.request,
          reverse_tail_proof.tree, reverse_tail_proof);
      BOOST_REQUIRE_EQUAL(reverse_tail.items.size(), 1U);
      BOOST_TEST(text(reverse_tail.items.front().key) == "beta");
      BOOST_TEST(!reverse_tail.more);

      const auto reverse_unbounded_proof =
          co_await authenticated.prove_range(1, forge::db::authenticated::range_request{.limit = 2, .reverse = true});
      const auto reverse_unbounded = forge::db::authenticated::verify_range(
          "forge.test.authenticated.state.v1", reverse_unbounded_proof.anchor, reverse_unbounded_proof.request,
          reverse_unbounded_proof.tree, reverse_unbounded_proof);
      BOOST_REQUIRE_EQUAL(reverse_unbounded.items.size(), 2U);
      BOOST_TEST(text(reverse_unbounded.items[0].key) == "gamma");
      BOOST_TEST(text(reverse_unbounded.items[1].key) == "delta");
      BOOST_TEST(reverse_unbounded.more);
      BOOST_REQUIRE(reverse_unbounded.next_key.has_value());
      BOOST_TEST(text(*reverse_unbounded.next_key) == "delta");

      const auto empty_reverse_proof = co_await authenticated.prove_range(
          1, forge::db::authenticated::range_request{
                 .lower = bytes("aardvark"), .upper = bytes("alpha"), .limit = 2, .reverse = true});
      const auto empty_reverse = forge::db::authenticated::verify_range(
          "forge.test.authenticated.state.v1", empty_reverse_proof.anchor, empty_reverse_proof.request,
          empty_reverse_proof.tree, empty_reverse_proof);
      BOOST_TEST(empty_reverse.items.empty());
      BOOST_TEST(!empty_reverse.more);
      BOOST_TEST(!empty_reverse.next_key.has_value());

      const auto scanned_first_page = co_await authenticated.scan_range(1, forge::db::authenticated::range_request{
                                                                               .lower = bytes("b"),
                                                                               .upper = bytes("z"),
                                                                               .limit = 1,
                                                                           });
      BOOST_CHECK(scanned_first_page == first_page);

      const auto hash_only_page = co_await authenticated.scan_range(1, forge::db::authenticated::range_request{
                                                                           .lower = bytes("b"),
                                                                           .upper = bytes("z"),
                                                                           .limit = 2,
                                                                           .include_values = false,
                                                                       });
      BOOST_REQUIRE_EQUAL(hash_only_page.items.size(), 2U);
      BOOST_TEST(!hash_only_page.items.front().value.has_value());
      BOOST_TEST(hash_only_page.items.front().value_hash == first_page.items.front().value_hash);

      const auto second_page_proof = co_await authenticated.prove_range(1, forge::db::authenticated::range_request{
                                                                               .lower = first_page.next_key,
                                                                               .upper = bytes("z"),
                                                                               .limit = 2,
                                                                           });
      const auto second_page =
          forge::db::authenticated::verify_range("forge.test.authenticated.state.v1", second_page_proof.anchor,
                                                 second_page_proof.request, second_page_proof.tree, second_page_proof);
      BOOST_REQUIRE_EQUAL(second_page.items.size(), 2U);
      BOOST_TEST(text(second_page.items.front().key) == "delta");
      BOOST_TEST(text(second_page.items.back().key) == "gamma");
      BOOST_TEST(!second_page.more);

      const auto empty_page_proof = co_await authenticated.prove_range(1, forge::db::authenticated::range_request{
                                                                              .lower = bytes("0"),
                                                                              .upper = bytes("a"),
                                                                          });
      const auto empty_page =
          forge::db::authenticated::verify_range("forge.test.authenticated.state.v1", empty_page_proof.anchor,
                                                 empty_page_proof.request, empty_page_proof.tree, empty_page_proof);
      BOOST_TEST(empty_page.items.empty());
      BOOST_TEST(!empty_page.more);

      const auto changes_proof = co_await authenticated.prove_range(1, forge::db::authenticated::range_request{},
                                                                    forge::db::authenticated::proof_tree::changes);
      const auto verified_changes =
          forge::db::authenticated::verify_range("forge.test.authenticated.state.v1", changes_proof.anchor,
                                                 changes_proof.request, changes_proof.tree, changes_proof);
      BOOST_REQUIRE_EQUAL(verified_changes.items.size(), 3U);
      BOOST_REQUIRE(verified_changes.items.front().value.has_value());
      BOOST_CHECK(verified_changes.items.front().value->front() == std::byte{0});
      const auto scanned_changes = co_await authenticated.scan_range(1, forge::db::authenticated::range_request{},
                                                                     forge::db::authenticated::proof_tree::changes);
      BOOST_CHECK(scanned_changes == verified_changes);

      auto rolled_back_db = co_await driver->begin_transaction();
      auto rolled_back = co_await authenticated.join(rolled_back_db, 2);
      const auto rolled_back_changes = std::vector<forge::db::authenticated::mutation>{put("zeta", "6")};
      static_cast<void>(co_await rolled_back.stage(rolled_back_changes));
      co_await rolled_back_db.rollback();
      BOOST_TEST(!(co_await authenticated.find_root(2)).has_value());
      BOOST_TEST((co_await authenticated.latest())->version == 1U);

      const auto expected_head = *(co_await authenticated.latest());
      co_await driver->async_flush(true);
      co_await driver->async_close();
      driver.reset();

      auto reopened = co_await open_driver(root_path / "store", lane);
      auto reopened_store = make_store(reopened);
      BOOST_CHECK(*(co_await reopened_store.latest()) == expected_head);
      const auto reopened_proof = co_await reopened_store.prove(1, bytes("delta"));
      BOOST_TEST(forge::db::authenticated::verify_point("forge.test.authenticated.state.v1", reopened_proof.anchor,
                                                        reopened_proof.key, reopened_proof)
                     .exists);

      co_await reopened->async_close();
      reopened.reset();
      co_await lane.shutdown();
   }());

   std::filesystem::remove_all(root_path);
}

BOOST_AUTO_TEST_CASE(authenticated_root_does_not_depend_on_batch_order) {
   const auto root_path = make_test_root("forge_db_authenticated_order");
   auto runtime = forge::asio::runtime{};
   auto first_lane = forge::asio::affine::lane{};
   auto second_lane = forge::asio::affine::lane{};

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto first_driver = co_await open_driver(root_path / "first", first_lane);
      auto second_driver = co_await open_driver(root_path / "second", second_lane);
      auto first_store = make_store(first_driver);
      auto second_store = make_store(second_driver);

      auto first_db = co_await first_driver->begin_transaction();
      auto first = co_await first_store.join(first_db, 0);
      const auto first_changes = std::vector<forge::db::authenticated::mutation>{
          put("c", "3"),
          put("a", "1"),
          put("b", "2"),
      };
      const auto first_root = co_await first.stage(first_changes);
      BOOST_TEST(first_root.commitment.state_root.str() ==
                 "377d101d9599961ee2d612969f3f9d7ffec23e6fffbd87f53c43bf71140eb0b7");
      BOOST_TEST(first_root.commitment.change_root.str() ==
                 "70c40f54ceb6ba93140fa53de25457d0f99b4f6db470c02c13677885f825bd85");
      co_await first_db.commit();

      auto second_db = co_await second_driver->begin_transaction();
      auto second = co_await second_store.join(second_db, 0);
      const auto second_changes = std::vector<forge::db::authenticated::mutation>{
          put("b", "2"),
          put("c", "3"),
          put("a", "1"),
      };
      const auto second_root = co_await second.stage(second_changes);
      co_await second_db.commit();

      BOOST_TEST(first_root.commitment.state_root == second_root.commitment.state_root);
      BOOST_TEST(first_root.commitment.change_root == second_root.commitment.change_root);

      co_await first_driver->async_close();
      co_await second_driver->async_close();
      first_driver.reset();
      second_driver.reset();
      co_await first_lane.shutdown();
      co_await second_lane.shutdown();
   }());

   std::filesystem::remove_all(root_path);
}

BOOST_AUTO_TEST_CASE(authenticated_randomized_history_matches_ordered_map) {
   constexpr auto domain = std::string_view{"forge.test.authenticated.randomized.v3"};
   const auto root_path = make_test_root("forge_db_authenticated_randomized");
   auto runtime = forge::asio::runtime{};
   auto lane = forge::asio::affine::lane{};

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto driver = co_await open_driver(root_path / "store", lane);
      auto authenticated = make_store(driver, std::string{domain});
      auto generator = std::mt19937_64{0x8f4d'21b6'c93a'705eULL};
      auto expected = std::map<std::string, std::string>{};
      auto history = std::vector<std::map<std::string, std::string>>{};

      for (auto version = std::uint64_t{}; version < 96U; ++version) {
         const auto key = std::string{"key-"} + std::to_string(generator() % 32U);
         auto changes = std::vector<forge::db::authenticated::mutation>{};
         if ((generator() % 4U) == 0U) {
            changes.push_back(erase(key));
            expected.erase(key);
         } else {
            const auto value = std::string{"value-"} + std::to_string(version) + "-" + std::to_string(generator());
            changes.push_back(put(key, value));
            expected.insert_or_assign(key, value);
         }

         auto active = co_await driver->begin_transaction();
         auto transaction = co_await authenticated.join(active, version);
         const auto staged = co_await transaction.stage(changes);
         co_await active.commit();
         BOOST_TEST(staged.commitment.version == version);
         BOOST_TEST(staged.commitment.state_size == expected.size());
         history.push_back(expected);

         const auto range_proof = co_await authenticated.prove_range(
             version, forge::db::authenticated::range_request{.limit = 64, .include_values = true});
         const auto range = forge::db::authenticated::verify_range(domain, range_proof.anchor, range_proof.request,
                                                                   range_proof.tree, range_proof);
         BOOST_REQUIRE_EQUAL(range.items.size(), expected.size());
         auto expected_item = expected.begin();
         for (auto index = std::size_t{}; index < range.items.size(); ++index, ++expected_item) {
            BOOST_TEST(text(range.items[index].key) == expected_item->first);
            BOOST_REQUIRE(range.items[index].value.has_value());
            BOOST_TEST(text(*range.items[index].value) == expected_item->second);
            BOOST_TEST(range.items[index].rank == index);
         }

         for (auto sample = 0U; sample < 4U; ++sample) {
            const auto query = std::string{"key-"} + std::to_string(generator() % 40U);
            const auto proof = co_await authenticated.prove(version, bytes(query));
            const auto verified = forge::db::authenticated::verify_point(domain, proof.anchor, proof.key, proof);
            const auto found = expected.find(query);
            BOOST_TEST(verified.exists == (found != expected.end()));
            if (found != expected.end()) {
               BOOST_REQUIRE(verified.value.has_value());
               BOOST_TEST(text(*verified.value) == found->second);
               BOOST_TEST(verified.rank == static_cast<std::uint64_t>(std::distance(expected.begin(), found)));
            }
         }
      }

      for (auto version = std::size_t{}; version < history.size(); version += 11U) {
         const auto& snapshot = history[version];
         for (auto key = 0U; key < 32U; ++key) {
            const auto query = std::string{"key-"} + std::to_string(key);
            const auto stored = co_await authenticated.get(version, bytes(query));
            const auto expected_value = snapshot.find(query);
            BOOST_TEST(stored.has_value() == (expected_value != snapshot.end()));
            if (expected_value != snapshot.end()) {
               BOOST_TEST(text(*stored) == expected_value->second);
            }
         }
      }

      co_await driver->async_close();
      driver.reset();
      co_await lane.shutdown();
   }());

   std::filesystem::remove_all(root_path);
}

BOOST_AUTO_TEST_CASE(authenticated_avl_rotations_delete_history_ranks_and_restart) {
   struct rotation_case {
      std::string name;
      std::vector<std::string> insertion_order;
      std::string erased;
      std::string domain;
      std::vector<forge::db::authenticated::root> roots;
   };

   auto cases = std::vector<rotation_case>{
       {.name = "ll", .insertion_order = {"c", "b", "a"}, .erased = "c"},
       {.name = "lr", .insertion_order = {"c", "a", "b"}, .erased = "c"},
       {.name = "rr", .insertion_order = {"a", "b", "c"}, .erased = "a"},
       {.name = "rl", .insertion_order = {"a", "c", "b"}, .erased = "a"},
   };
   for (auto& item : cases) {
      item.domain = "forge.test.authenticated.rotation." + item.name + ".v3";
   }

   const auto root_path = make_test_root("forge_db_authenticated_rotations");
   auto runtime = forge::asio::runtime{};
   auto lane = forge::asio::affine::lane{};

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto driver = co_await open_driver(root_path / "store", lane);
      for (auto& item : cases) {
         auto authenticated = make_store(driver, item.domain);
         auto expected = std::vector<std::string>{};
         for (auto version = std::size_t{}; version < item.insertion_order.size(); ++version) {
            auto active = co_await driver->begin_transaction();
            auto transaction = co_await authenticated.join(active, version);
            const auto changes = std::vector<forge::db::authenticated::mutation>{
                put(item.insertion_order[version], item.insertion_order[version]),
            };
            const auto staged = co_await transaction.stage(changes);
            co_await active.commit();
            item.roots.push_back(staged.commitment);
            expected.push_back(item.insertion_order[version]);
            std::ranges::sort(expected);
            co_await check_ranked_version(authenticated, item.domain, version, expected);
         }

         auto active = co_await driver->begin_transaction();
         auto transaction = co_await authenticated.join(active, 3);
         const auto changes = std::vector<forge::db::authenticated::mutation>{
             erase(item.erased),
         };
         const auto staged = co_await transaction.stage(changes);
         co_await active.commit();
         item.roots.push_back(staged.commitment);
         std::erase(expected, item.erased);
         co_await check_ranked_version(authenticated, item.domain, 3, expected);

         auto before_delete = item.insertion_order;
         std::ranges::sort(before_delete);
         co_await check_ranked_version(authenticated, item.domain, 2, before_delete);
      }

      co_await driver->async_flush(true);
      co_await driver->async_close();
      driver.reset();

      auto reopened = co_await open_driver(root_path / "store", lane);
      for (const auto& item : cases) {
         auto authenticated = make_store(reopened, item.domain);
         auto expected = std::vector<std::string>{};
         for (auto version = std::size_t{}; version < item.insertion_order.size(); ++version) {
            expected.push_back(item.insertion_order[version]);
            std::ranges::sort(expected);
            BOOST_REQUIRE((co_await authenticated.find_root(version)).has_value());
            BOOST_CHECK(*(co_await authenticated.find_root(version)) == item.roots[version]);
            co_await check_ranked_version(authenticated, item.domain, version, expected);
         }
         std::erase(expected, item.erased);
         BOOST_REQUIRE((co_await authenticated.find_root(3)).has_value());
         BOOST_CHECK(*(co_await authenticated.find_root(3)) == item.roots[3]);
         co_await check_ranked_version(authenticated, item.domain, 3, expected);
      }

      co_await reopened->async_close();
      reopened.reset();
      co_await lane.shutdown();
   }());

   std::filesystem::remove_all(root_path);
}

BOOST_AUTO_TEST_CASE(authenticated_first_version_is_caller_owned) {
   const auto root_path = make_test_root("forge_db_authenticated_initial_version");
   auto runtime = forge::asio::runtime{};
   auto lane = forge::asio::affine::lane{{.thread_name = "authenticated-version-test"}};

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto driver = co_await open_driver(root_path / "store", lane);
      auto authenticated = make_store(driver);

      auto active = co_await driver->begin_transaction();
      auto transaction = co_await authenticated.join(active, 42U);
      const auto staged = co_await transaction.stage(std::vector<forge::db::authenticated::mutation>{
          put("answer", "42"),
      });
      BOOST_TEST(staged.commitment.version == 42U);
      co_await active.commit();

      const auto latest = co_await authenticated.latest();
      BOOST_REQUIRE(latest.has_value());
      BOOST_TEST(latest->version == 42U);
      BOOST_TEST(text(*(co_await authenticated.get(42U, bytes("answer")))) == "42");

      co_await driver->async_close();
      co_await lane.shutdown();
   }());

   std::filesystem::remove_all(root_path);
}

BOOST_AUTO_TEST_CASE(authenticated_proof_rejects_tampering_and_missing_stage) {
   const auto root_path = make_test_root("forge_db_authenticated_negative");
   auto runtime = forge::asio::runtime{};
   auto lane = forge::asio::affine::lane{};

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto driver = co_await open_driver(root_path / "store", lane);
      auto authenticated = make_store(driver);

      auto unstaged_db = co_await driver->begin_transaction();
      static_cast<void>(co_await authenticated.join(unstaged_db, 0));
      auto rejected_unstaged = false;
      try {
         co_await unstaged_db.commit();
      } catch (const forge::db::authenticated::exceptions::transaction_not_staged&) {
         rejected_unstaged = true;
      }
      BOOST_TEST(rejected_unstaged);
      co_await unstaged_db.rollback();

      auto active = co_await driver->begin_transaction();
      auto authenticated_tx = co_await authenticated.join(active, 0);
      const auto changes = std::vector<forge::db::authenticated::mutation>{
          put("a", "1"),
          put("b", "2"),
          put("c", "3"),
      };
      static_cast<void>(co_await authenticated_tx.stage(changes));
      co_await active.commit();

      auto proof = co_await authenticated.prove(0, bytes("b"));
      BOOST_REQUIRE(!proof.path.empty());
      std::visit(
          [](auto& sibling) {
             using sibling_type = std::remove_cvref_t<decltype(sibling)>;
             if constexpr (std::same_as<sibling_type, forge::db::authenticated::proof_branch>) {
                ++sibling.size;
             } else {
                sibling.key.push_back(std::byte{0xff});
             }
          },
          proof.path.front().sibling);
      BOOST_CHECK_THROW(static_cast<void>(forge::db::authenticated::verify_point("forge.test.authenticated.state.v1",
                                                                                 proof.anchor, proof.key, proof)),
                        forge::db::authenticated::exceptions::invalid_proof);

      auto value_proof = co_await authenticated.prove(0, bytes("b"));
      BOOST_REQUIRE(value_proof.terminal->value.has_value());
      value_proof.terminal->value->front() ^= std::byte{1};
      BOOST_CHECK_THROW(static_cast<void>(forge::db::authenticated::verify_point(
                            "forge.test.authenticated.state.v1", value_proof.anchor, value_proof.key, value_proof)),
                        forge::db::authenticated::exceptions::invalid_proof);

      const auto expected_anchor = value_proof.anchor;
      const auto expected_key = value_proof.key;
      auto wrong_anchor = value_proof;
      ++wrong_anchor.anchor.version;
      BOOST_CHECK_THROW(static_cast<void>(forge::db::authenticated::verify_point(
                            "forge.test.authenticated.state.v1", expected_anchor, expected_key, wrong_anchor)),
                        forge::db::authenticated::exceptions::invalid_proof);
      BOOST_CHECK_THROW(static_cast<void>(forge::db::authenticated::verify_point(
                            "forge.test.authenticated.state.v1", expected_anchor, bytes("different"), value_proof)),
                        forge::db::authenticated::exceptions::invalid_proof);

      const auto bounded_proof = co_await authenticated.prove(0, bytes("b"));
      auto tight_limits = forge::db::authenticated::limits{};
      tight_limits.max_proof_bytes = 64;
      BOOST_CHECK_THROW(static_cast<void>(forge::db::authenticated::verify_point(
                            "forge.test.authenticated.state.v1", bounded_proof.anchor, bounded_proof.key, bounded_proof,
                            tight_limits)),
                        forge::db::authenticated::exceptions::proof_limit_exceeded);

      auto range_proof = co_await authenticated.prove_range(0, forge::db::authenticated::range_request{
                                                                   .lower = bytes("a"),
                                                                   .upper = bytes("z"),
                                                                   .limit = 1,
                                                               });
      BOOST_REQUIRE(!range_proof.nodes.empty());
      std::visit(
          [](auto& node) {
             using node_type = std::remove_cvref_t<decltype(node)>;
             if constexpr (std::same_as<node_type, forge::db::authenticated::proof_branch> ||
                           std::same_as<node_type, forge::db::authenticated::range_inner>) {
                ++node.size;
             } else {
                node.key.push_back(std::byte{0xfe});
             }
          },
          range_proof.nodes.front());
      BOOST_CHECK_THROW(static_cast<void>(forge::db::authenticated::verify_range(
                            "forge.test.authenticated.state.v1", range_proof.anchor, range_proof.request,
                            range_proof.tree, range_proof)),
                        forge::db::authenticated::exceptions::invalid_proof);

      const auto original_range = co_await authenticated.prove_range(0, forge::db::authenticated::range_request{
                                                                            .lower = bytes("a"),
                                                                            .upper = bytes("z"),
                                                                            .limit = 2,
                                                                        });
      auto altered_range = original_range;
      altered_range.request.limit = 1;
      BOOST_CHECK_THROW(static_cast<void>(forge::db::authenticated::verify_range(
                            "forge.test.authenticated.state.v1", original_range.anchor, original_range.request,
                            original_range.tree, altered_range)),
                        forge::db::authenticated::exceptions::invalid_proof);

      auto savepoint_db = co_await driver->begin_transaction();
      auto savepoint_tx = co_await authenticated.join(savepoint_db, 1);
      const auto savepoint = co_await savepoint_db.create_savepoint();
      const auto savepoint_changes = std::vector<forge::db::authenticated::mutation>{
          put("after-savepoint", "discarded"),
      };
      static_cast<void>(co_await savepoint_tx.stage(savepoint_changes));
      co_await savepoint_db.rollback_to_savepoint(savepoint);
      auto rejected_rolled_back_stage = false;
      try {
         co_await savepoint_db.commit();
      } catch (const forge::db::authenticated::exceptions::transaction_not_staged&) {
         rejected_rolled_back_stage = true;
      }
      BOOST_TEST(rejected_rolled_back_stage);
      co_await savepoint_db.rollback();

      auto tight_store = forge::db::authenticated::store{
          driver,
          {
              .family = forge::db::core::family{"authenticated"},
              .domain = "forge.test.authenticated.tight.v1",
              .bounds =
                  {
                      .max_proof_bytes = 128,
                  },
          },
      };
      auto tight_db = co_await driver->begin_transaction();
      auto tight_tx = co_await tight_store.join(tight_db, 0);
      const auto large_value = std::string(256, 'x');
      const auto tight_changes = std::vector<forge::db::authenticated::mutation>{
          put("large", large_value),
      };
      static_cast<void>(co_await tight_tx.stage(tight_changes));
      co_await tight_db.commit();
      auto rejected_point_generation = false;
      try {
         static_cast<void>(co_await tight_store.prove(0, bytes("large"), true));
      } catch (const forge::db::authenticated::exceptions::proof_limit_exceeded&) {
         rejected_point_generation = true;
      }
      BOOST_TEST(rejected_point_generation);
      auto rejected_range_generation = false;
      try {
         static_cast<void>(co_await tight_store.prove_range(0, forge::db::authenticated::range_request{}));
      } catch (const forge::db::authenticated::exceptions::proof_limit_exceeded&) {
         rejected_range_generation = true;
      }
      BOOST_TEST(rejected_range_generation);

      auto key_limited_store = forge::db::authenticated::store{
          driver,
          {
              .family = forge::db::core::family{"authenticated"},
              .domain = "forge.test.authenticated.key-limited.v1",
              .bounds =
                  {
                      .max_key_bytes = 4,
                  },
          },
      };
      auto key_limited_db = co_await driver->begin_transaction();
      auto key_limited_tx = co_await key_limited_store.join(key_limited_db, 0);
      static_cast<void>(co_await key_limited_tx.stage(std::vector<forge::db::authenticated::mutation>{
          put("key", "value"),
      }));
      co_await key_limited_db.commit();
      auto rejected_oversized_query = false;
      try {
         static_cast<void>(co_await key_limited_store.prove(0, bytes("oversized-query")));
      } catch (const forge::db::authenticated::exceptions::proof_limit_exceeded&) {
         rejected_oversized_query = true;
      }
      BOOST_TEST(rejected_oversized_query);

      auto invalid_config = forge::db::authenticated::store::config{
          .family = forge::db::core::family{"authenticated"},
          .domain = "forge.test.authenticated.invalid-config.v1",
      };
      invalid_config.bounds.max_proof_bytes = forge::db::authenticated::max_wire_proof_bytes + 1U;
      BOOST_CHECK_THROW(static_cast<void>(forge::db::authenticated::store{driver, invalid_config}),
                        forge::db::authenticated::exceptions::invalid_store);

      invalid_config.bounds = {};
      invalid_config.bounds.max_proof_depth = forge::db::authenticated::hard_max_proof_depth + 1U;
      BOOST_CHECK_THROW(static_cast<void>(forge::db::authenticated::store{driver, invalid_config}),
                        forge::db::authenticated::exceptions::invalid_store);

      invalid_config.bounds = {};
      invalid_config.bounds.max_key_bytes = forge::db::authenticated::max_framed_bytes + 1U;
      BOOST_CHECK_THROW(static_cast<void>(forge::db::authenticated::store{driver, invalid_config}),
                        forge::db::authenticated::exceptions::invalid_store);

      co_await driver->async_close();
      driver.reset();
      co_await lane.shutdown();
   }());

   std::filesystem::remove_all(root_path);
}

BOOST_AUTO_TEST_CASE(authenticated_pruning_is_bounded_and_preserves_retained_roots) {
   const auto root_path = make_test_root("forge_db_authenticated_pruning");
   auto runtime = forge::asio::runtime{};
   auto lane = forge::asio::affine::lane{};

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto driver = co_await open_driver(root_path / "store", lane);
      auto authenticated = make_store(driver);
      BOOST_TEST(!(co_await authenticated.earliest()).has_value());

      for (auto version = std::uint64_t{}; version < 3U; ++version) {
         auto active = co_await driver->begin_transaction();
         auto authenticated_tx = co_await authenticated.join(active, version);
         const auto changes = std::vector<forge::db::authenticated::mutation>{
             put("stable", "value"),
             put("version", std::to_string(version)),
             put("unique-" + std::to_string(version), "present"),
         };
         static_cast<void>(co_await authenticated_tx.stage(changes));
         co_await active.commit();
      }

      const auto pruned_version_proof = co_await authenticated.prove(0, bytes("stable"));
      BOOST_TEST(forge::db::authenticated::verify_point("forge.test.authenticated.state.v1",
                                                        pruned_version_proof.anchor, pruned_version_proof.key,
                                                        pruned_version_proof)
                     .exists);

      auto complete = false;
      auto calls = std::uint32_t{};
      auto versions_pruned = std::uint64_t{};
      while (!complete) {
         auto active = co_await driver->begin_transaction();
         const auto result = co_await authenticated.prune_through(active, 1,
                                                                  {
                                                                      .max_versions = 1,
                                                                      .max_garbage_records = 1,
                                                                  });
         BOOST_TEST(result.versions_pruned <= 1U);
         BOOST_TEST(result.nodes_collected + result.values_collected <= 1U);
         versions_pruned += result.versions_pruned;
         complete = result.complete;
         co_await active.commit();
         BOOST_REQUIRE(++calls < 256U);
      }

      BOOST_TEST(versions_pruned == 2U);
      BOOST_TEST(!(co_await authenticated.find_root(0)).has_value());
      BOOST_TEST(!(co_await authenticated.find_root(1)).has_value());
      BOOST_REQUIRE((co_await authenticated.find_root(2)).has_value());
      BOOST_REQUIRE((co_await authenticated.earliest()).has_value());
      BOOST_TEST((co_await authenticated.earliest())->version == 2U);
      BOOST_TEST(text(*(co_await authenticated.get(2, bytes("version")))) == "2");
      const auto retained = co_await authenticated.prove(2, bytes("stable"));
      BOOST_TEST(forge::db::authenticated::verify_point("forge.test.authenticated.state.v1", retained.anchor,
                                                        retained.key, retained)
                     .exists);
      BOOST_TEST(forge::db::authenticated::verify_point("forge.test.authenticated.state.v1",
                                                        pruned_version_proof.anchor, pruned_version_proof.key,
                                                        pruned_version_proof)
                     .exists);

      auto invalid = co_await driver->begin_transaction();
      auto rejected_latest = false;
      try {
         static_cast<void>(co_await authenticated.prune_through(invalid, 2));
      } catch (const forge::db::authenticated::exceptions::invalid_prune&) {
         rejected_latest = true;
      }
      BOOST_TEST(rejected_latest);
      co_await invalid.rollback();

      co_await driver->async_close();
      driver.reset();
      co_await lane.shutdown();
   }());

   std::filesystem::remove_all(root_path);
}

BOOST_AUTO_TEST_CASE(authenticated_versions_revert_with_forge_revision) {
   const auto root_path = make_test_root("forge_db_authenticated_revision");
   auto runtime = forge::asio::runtime{};
   auto lane = forge::asio::affine::lane{};

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto driver = co_await open_driver(root_path / "store", lane);
      auto objects = co_await forge::db::object::store::open(driver);
      auto revisions = co_await forge::db::revision::store::open(driver, objects);
      auto authenticated = make_store(driver);

      auto first_db = co_await driver->begin_transaction();
      const auto first_revision = co_await revisions.join(first_db);
      BOOST_TEST(first_revision.id() == 1U);
      auto first = co_await authenticated.join(first_db, 0);
      const auto first_changes = std::vector<forge::db::authenticated::mutation>{
          put("value", "first"),
      };
      static_cast<void>(co_await first.stage(first_changes));
      co_await first_db.commit();

      auto second_db = co_await driver->begin_transaction();
      const auto second_revision = co_await revisions.join(second_db);
      BOOST_TEST(second_revision.id() == 2U);
      auto second = co_await authenticated.join(second_db, 1);
      const auto second_changes = std::vector<forge::db::authenticated::mutation>{
          put("value", "second"),
      };
      static_cast<void>(co_await second.stage(second_changes));
      co_await second_db.commit();
      BOOST_TEST(text(*(co_await authenticated.get(1, bytes("value")))) == "second");

      auto guarded_prune = co_await driver->begin_transaction();
      auto rejected_guarded_prune = false;
      try {
         static_cast<void>(co_await authenticated.prune_through(guarded_prune, 0));
      } catch (const forge::db::authenticated::exceptions::invalid_prune&) {
         rejected_guarded_prune = true;
      }
      BOOST_TEST(rejected_guarded_prune);
      co_await guarded_prune.rollback();

      auto revert = co_await driver->begin_transaction();
      co_await revisions.revert(revert, 2);
      co_await revert.commit();

      BOOST_REQUIRE((co_await authenticated.latest()).has_value());
      BOOST_TEST((co_await authenticated.latest())->version == 0U);
      BOOST_TEST(!(co_await authenticated.find_root(1)).has_value());
      BOOST_TEST(text(*(co_await authenticated.get(0, bytes("value")))) == "first");

      co_await driver->async_close();
      driver.reset();
      co_await lane.shutdown();
   }());

   std::filesystem::remove_all(root_path);
}

BOOST_AUTO_TEST_CASE(authenticated_prune_preflights_all_retention_guards) {
   const auto root_path = make_test_root("forge_db_authenticated_prune_preflight");
   auto runtime = forge::asio::runtime{};
   auto lane = forge::asio::affine::lane{};

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto driver = co_await open_driver(root_path / "store", lane);
      auto objects = co_await forge::db::object::store::open(driver);
      auto revisions = co_await forge::db::revision::store::open(driver, objects);
      auto authenticated = make_store(driver);

      for (auto version = std::uint64_t{}; version < 3U; ++version) {
         auto active = co_await driver->begin_transaction();
         static_cast<void>(co_await revisions.join(active));
         auto authenticated_tx = co_await authenticated.join(active, version);
         static_cast<void>(co_await authenticated_tx.stage(std::vector<forge::db::authenticated::mutation>{
             put("version", std::to_string(version)),
         }));
         co_await active.commit();
      }

      auto remove_first_guard = co_await driver->begin_transaction();
      const auto guards =
          co_await remove_first_guard.scan_page(forge::db::core::family{"authenticated"}, prefix_range(std::byte{9}),
                                                {.limit = forge::db::core::max_page_limit});
      BOOST_REQUIRE_EQUAL(guards.entries.size(), 2U);
      co_await remove_first_guard.erase(forge::db::core::family{"authenticated"}, guards.entries.front().key);
      co_await remove_first_guard.commit();

      auto prune = co_await driver->begin_transaction();
      auto rejected = false;
      try {
         static_cast<void>(co_await authenticated.prune_through(prune, 1));
      } catch (const forge::db::authenticated::exceptions::invalid_prune&) {
         rejected = true;
      }
      BOOST_TEST(rejected);
      co_await prune.commit();

      BOOST_REQUIRE((co_await authenticated.find_root(0)).has_value());
      BOOST_REQUIRE((co_await authenticated.find_root(1)).has_value());
      BOOST_REQUIRE((co_await authenticated.find_root(2)).has_value());

      co_await driver->async_close();
      driver.reset();
      co_await lane.shutdown();
   }());

   std::filesystem::remove_all(root_path);
}

BOOST_AUTO_TEST_CASE(authenticated_checkpoint_frame_reuses_bounded_codec_without_proof_dataset_cap) {
   namespace auth = forge::db::authenticated;
   const auto policy = auth::checkpoint_limits{
       .max_records = 32U,
       .max_encoded_bytes = 4096U,
       .max_value_bytes = 4096U,
       .max_frame_bytes = 1024U,
       .max_working_bytes = 16384U,
       .max_depth = 16U,
   };
   BOOST_CHECK(auth::checkpoint_limits_are_valid(policy));
   BOOST_CHECK(!auth::checkpoint_limits_are_valid({}));
   auto record = auth::checkpoint_record{
       .tree = auth::proof_tree::changes,
       .hash = hash_text("checkpoint-leaf"),
       .node = auth::proof_leaf{.key = bytes("key"), .value_hash = hash_text("payload"), .value = bytes("payload")},
   };
   const auto encoded = auth::encode(record);
   BOOST_CHECK_EQUAL(auth::wire_size(record), encoded.size());
   BOOST_CHECK_EQUAL(std::to_integer<unsigned>(encoded.front()), auth::checkpoint_format_version);
   auto node_bounds = auth::limits{};
   // Dataset framing does not inherit the independent point/range proof-byte cap.
   node_bounds.max_proof_bytes = 1U;
   BOOST_CHECK(auth::decode_checkpoint_record(encoded, node_bounds, policy) == record);
   for (auto size = std::size_t{}; size < encoded.size(); ++size) {
      BOOST_CHECK_THROW(static_cast<void>(auth::decode_checkpoint_record(
                            std::span<const std::byte>{encoded}.first(size), node_bounds, policy)),
                        auth::exceptions::invalid_proof);
   }
   auto changed = encoded;
   changed.push_back(std::byte{});
   BOOST_CHECK_THROW(static_cast<void>(auth::decode_checkpoint_record(changed, node_bounds, policy)),
                     auth::exceptions::invalid_proof);
   changed = encoded;
   changed[0] = std::byte{2};
   BOOST_CHECK_THROW(static_cast<void>(auth::decode_checkpoint_record(changed, node_bounds, policy)),
                     auth::exceptions::invalid_proof);
   changed = encoded;
   changed[1] = std::byte{127};
   BOOST_CHECK_THROW(static_cast<void>(auth::decode_checkpoint_record(changed, node_bounds, policy)),
                     auth::exceptions::invalid_proof);
   changed = encoded;
   changed[2U + auth::digest::data_size()] = std::byte{127};
   BOOST_CHECK_THROW(static_cast<void>(auth::decode_checkpoint_record(changed, node_bounds, policy)),
                     auth::exceptions::invalid_proof);

   auto exact = policy;
   exact.max_frame_bytes = encoded.size();
   BOOST_CHECK(auth::decode_checkpoint_record(encoded, node_bounds, exact) == record);
   --exact.max_frame_bytes;
   BOOST_CHECK_THROW(static_cast<void>(auth::decode_checkpoint_record(encoded, node_bounds, exact)),
                     auth::exceptions::proof_limit_exceeded);
   auto small = node_bounds;
   small.max_value_bytes = 6U;
   BOOST_CHECK_THROW(static_cast<void>(auth::decode_checkpoint_record(encoded, small, policy)),
                     auth::exceptions::proof_limit_exceeded);
   small = node_bounds;
   small.max_key_bytes = 2U;
   BOOST_CHECK_THROW(static_cast<void>(auth::decode_checkpoint_record(encoded, small, policy)),
                     auth::exceptions::proof_limit_exceeded);
   BOOST_CHECK_THROW(static_cast<void>(auth::decode_checkpoint_record(encoded, node_bounds, {})),
                     auth::exceptions::proof_limit_exceeded);

   std::get<auth::proof_leaf>(record.node).value = auth::bytes{};
   BOOST_CHECK(auth::decode_checkpoint_record(auth::encode(record), node_bounds, policy) == record);
   std::get<auth::proof_leaf>(record.node).value.reset();
   BOOST_CHECK_THROW(static_cast<void>(auth::encode(record)), auth::exceptions::invalid_proof);
   // Raw is only used to construct an intentionally invalid frame in this test.
   const auto missing_payload = forge::raw::pack(record);
   changed.assign(1U, static_cast<std::byte>(auth::checkpoint_format_version));
   for (const auto byte : missing_payload)
      changed.push_back(static_cast<std::byte>(byte));
   BOOST_CHECK_THROW(static_cast<void>(auth::decode_checkpoint_record(changed, node_bounds, policy)),
                     auth::exceptions::invalid_proof);

   record.node = auth::proof_branch{
       .height = 1U,
       .size = 2U,
       .min_key = bytes("a"),
       .max_key = bytes("z"),
       .separator = bytes("z"),
       .left_hash = hash_text("a"),
       .right_hash = hash_text("z"),
   };
   const auto branch = auth::encode(record);
   BOOST_CHECK_EQUAL(auth::wire_size(record), branch.size());
   BOOST_CHECK(auth::decode_checkpoint_record(branch, node_bounds, policy) == record);
}

BOOST_AUTO_TEST_CASE(authenticated_checkpoint_preserves_historical_state_changes_then_reopens_advances_and_prunes) {
   namespace auth = forge::db::authenticated;
   const auto source_path = make_test_root("forge_authenticated_checkpoint_source");
   const auto target_path = make_test_root("forge_authenticated_checkpoint_target");
   auto runtime = forge::asio::runtime{};
   auto lane = forge::asio::affine::lane{};
   const auto policy = auth::checkpoint_limits{.max_records = 4096U,
                                               .max_encoded_bytes = 1U << 20U,
                                               .max_value_bytes = 1U << 20U,
                                               .max_frame_bytes = 1U << 16U,
                                               .max_working_bytes = 8U << 20U,
                                               .max_depth = 32U};
   const auto settings = auth::store::config{.family = forge::db::core::family{"authenticated"},
                                             .domain = "forge.test.checkpoint.v1",
                                             .bounds = {.max_key_bytes = 128U, .max_value_bytes = 4096U}};
   auto expected = auth::root{};
   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto driver = co_await open_driver(source_path, lane);
      auto source = auth::store{driver, settings};
      {
         auto active = co_await driver->begin_transaction();
         auto joined = co_await source.join(active, 41U);
         const auto mutations = std::vector{put("b", "B"), put("c", "C"), put("d", "D"), put("e", "E")};
         static_cast<void>(co_await joined.stage(mutations));
         co_await active.commit();
      }
      {
         auto active = co_await driver->begin_transaction();
         auto joined = co_await source.join(active, 42U);
         const auto mutations = std::vector{put("a", "A"), erase("z")};
         expected = (co_await joined.stage(mutations)).commitment;
         co_await active.commit();
      }
      BOOST_CHECK_EQUAL(expected.state_size, 5U);
      BOOST_CHECK_EQUAL(expected.change_count, 2U);
      const auto proof_a = co_await source.prove(42U, bytes("a"));
      const auto request = auth::range_request{.limit = 16U};
      const auto state_proof = co_await source.prove_range(42U, request);
      const auto changes_proof = co_await source.prove_range(42U, request, auth::proof_tree::changes);
      // Five leaves distinguish history: retained split abc/de versus sorted bulk ab/cde.
      auto target_driver = co_await open_driver(target_path, lane);
      {
         auto active = co_await target_driver->begin_transaction();
         auto bulk = auth::store{target_driver, settings};
         auto joined = co_await bulk.join(active, 42U);
         const auto mutations = std::vector{put("a", "A"), put("b", "B"), put("c", "C"), put("d", "D"), put("e", "E")};
         BOOST_CHECK((co_await joined.stage(mutations)).commitment.state_root != expected.state_root);
         co_await active.rollback();
      }
      auto frames = std::make_shared<std::vector<auth::checkpoint_record>>();
      auto pinned = co_await driver->begin_read();
      // Source Store wrapper dies BEFORE lazy export starts; the captured impl/view remain.
      auto pending = [&] {
         auto temporary = source;
         return temporary.export_checkpoint(
             pinned, expected,
             [frames](auth::checkpoint_record record) -> boost::asio::awaitable<void> {
                frames->push_back(std::move(record));
                co_return;
             },
             policy);
      }();
      // Change/delete/prune the live graph AFTER acquiring the exact export snapshot.
      {
         auto active = co_await driver->begin_transaction();
         auto joined = co_await source.join(active, 43U);
         const auto mutations = std::vector{put("f", "F"), erase("a")};
         static_cast<void>(co_await joined.stage(mutations));
         co_await active.commit();
      }
      {
         auto active = co_await driver->begin_transaction();
         static_cast<void>(co_await source.prune_through(active, 42U));
         co_await active.commit();
      }
      const auto exported = co_await std::move(pending);
      BOOST_CHECK(exported.anchor == expected);
      BOOST_CHECK_EQUAL(exported.records, 12U); // 9 state + 3 exact historical changes nodes.
      BOOST_CHECK_EQUAL(frames->size(), exported.records);
      BOOST_REQUIRE_EQUAL(frames->size(), 12U);
      const auto* historic_root = std::get_if<auth::proof_branch>(&(*frames)[8U].node);
      BOOST_REQUIRE(historic_root != nullptr);
      BOOST_CHECK((*frames)[8U].tree == auth::proof_tree::state);
      BOOST_CHECK((*frames)[8U].hash == expected.state_root);
      BOOST_CHECK(historic_root->separator == bytes("d"));
      pinned = {};
      BOOST_CHECK(!(co_await source.find_root(42U)));

      {
         auto target = auth::store{target_driver, settings};
         auto active = co_await target_driver->begin_transaction();
         auto joined = target.join_restore(active, expected, policy);
         BOOST_CHECK(!joined.restored());
         auto cursor = std::make_shared<std::pair<decltype(frames), std::size_t>>(frames, 0U);
         const auto source_lifetime = std::weak_ptr{cursor};
         auto read = auth::checkpoint_source{
             [cursor = std::move(cursor)]() -> boost::asio::awaitable<std::optional<auth::checkpoint_record>> {
                if (cursor->second == cursor->first->size())
                   co_return std::nullopt;
                co_return (*cursor->first)[cursor->second++];
             }};
         auto verified = std::make_shared<bool>(false);
         auto restoring = joined.restore_checkpoint(
             std::move(read),
             [verified, exported, source_lifetime](auth::checkpoint_summary summary) -> boost::asio::awaitable<void> {
                BOOST_CHECK(source_lifetime.expired()); // Before metadata/ready, including libc++ SBO moves.
                BOOST_CHECK(summary == exported);
                *verified = true;
                co_return;
             });
         read = {}; // The caller must also release any moved-from small-buffer source.
         const auto restored = co_await std::move(restoring);
         BOOST_CHECK(*verified);
         BOOST_CHECK(restored == exported);
         BOOST_REQUIRE(joined.restored());
         BOOST_CHECK(*joined.restored() == expected);
         BOOST_CHECK(!joined.staged());
         co_await active.commit();
         BOOST_REQUIRE(co_await target.earliest());
         BOOST_CHECK(*(co_await target.earliest()) == expected);
         BOOST_CHECK(*(co_await target.latest()) == expected);
         BOOST_CHECK((co_await target.prove(42U, bytes("a"))) == proof_a);
         BOOST_CHECK((co_await target.prove_range(42U, request)) == state_proof);
         BOOST_CHECK((co_await target.prove_range(42U, request, auth::proof_tree::changes)) == changes_proof);
         BOOST_CHECK(!(co_await target.find_root(41U)));
         BOOST_CHECK_THROW(static_cast<void>(co_await target.get(41U, bytes("a"))),
                           auth::exceptions::version_unavailable);
      }
      co_await target_driver->async_close();
      target_driver.reset();
      target_driver = co_await open_driver(target_path, lane);
      {
         auto target = auth::store{target_driver, settings};
         BOOST_CHECK(*(co_await target.find_root(42U)) == expected);
         auto active = co_await target_driver->begin_transaction();
         auto joined = co_await target.join(active, 43U);
         const auto mutations = std::vector{put("next", "N"), erase("a")};
         static_cast<void>(co_await joined.stage(mutations));
         co_await active.commit();
         auto prune = co_await target_driver->begin_transaction();
         static_cast<void>(co_await target.prune_through(prune, 42U));
         co_await prune.commit();
         BOOST_CHECK(!(co_await target.find_root(42U)));
         BOOST_CHECK(text(*(co_await target.get(43U, bytes("next")))) == "N");
         BOOST_CHECK(text(*(co_await target.get(43U, bytes("b")))) == "B");
         BOOST_CHECK(!(co_await target.get(43U, bytes("a"))));
      }
      co_await target_driver->async_close();
      target_driver.reset();
      co_await driver->async_close();
      driver.reset();
      co_await lane.shutdown();
   }());
   std::filesystem::remove_all(source_path);
   std::filesystem::remove_all(target_path);
}

BOOST_AUTO_TEST_CASE(authenticated_checkpoint_failure_is_sticky_and_caught_errors_cannot_commit_partial_records) {
   namespace auth = forge::db::authenticated;
   namespace core = forge::db::core;
   const auto source_path = make_test_root("forge_authenticated_checkpoint_fail_source");
   const auto target_path = make_test_root("forge_authenticated_checkpoint_fail_target");
   auto runtime = forge::asio::runtime{};
   auto lane = forge::asio::affine::lane{};
   const auto policy = auth::checkpoint_limits{.max_records = 4096U,
                                               .max_encoded_bytes = 1U << 20U,
                                               .max_value_bytes = 1U << 20U,
                                               .max_frame_bytes = 1U << 16U,
                                               .max_working_bytes = 8U << 20U,
                                               .max_depth = 32U};
   const auto settings = auth::store::config{.family = core::family{"authenticated"},
                                             .domain = "forge.test.checkpoint.fail.v1",
                                             .bounds = {.max_key_bytes = 128U, .max_value_bytes = 4096U}};
   class finalization_veto final : public core::transaction_participant {
    public:
      explicit finalization_veto(std::shared_ptr<bool> verified) : verified_{std::move(verified)} {}
      std::string_view name() const noexcept override {
         return "test.checkpoint.finalization-veto";
      }
      core::mutation_policy classify(const core::family&, const core::record_key&,
                                     core::mutation_kind) const noexcept override {
         return *verified_ ? core::mutation_policy::forbidden : core::mutation_policy::inherit;
      }

    private:
      std::shared_ptr<bool> verified_;
   };
   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto driver = co_await open_driver(source_path, lane);
      auto source = auth::store{driver, settings};
      {
         auto active = co_await driver->begin_transaction();
         auto joined = co_await source.join(active, 42U);
         const auto changes = std::vector{put("a", "A"), put("b", "B"), erase("z")};
         static_cast<void>(co_await joined.stage(changes));
         co_await active.commit();
      }
      const auto expected = *(co_await source.latest());
      auto frames = std::make_shared<std::vector<auth::checkpoint_record>>();
      static_cast<void>(co_await source.export_checkpoint(
          co_await driver->begin_read(), expected,
          [frames](auth::checkpoint_record value) -> boost::asio::awaitable<void> {
             frames->push_back(std::move(value));
             co_return;
          },
          policy));
      BOOST_REQUIRE(frames->size() > 3U);
      auto target_driver = co_await open_driver(target_path, lane);
      auto target = auth::store{target_driver, settings};
      for (auto scenario = 0U; scenario < 23U; ++scenario) {
         auto tape = std::make_shared<std::vector<auth::checkpoint_record>>(*frames);
         auto budget = policy;
         if (scenario == 0U)
            tape->pop_back();
         if (scenario == 1U)
            tape->front().tree = auth::proof_tree::changes;
         if (scenario == 2U)
            std::get<auth::proof_leaf>(tape->front().node).value->push_back(std::byte{7});
         if (scenario == 3U)
            tape->front().hash = hash_text("not-the-node");
         if (scenario == 4U) {
            for (auto& frame : *tape) {
               if (auto* branch = std::get_if<auth::proof_branch>(&frame.node)) {
                  ++branch->height;
                  break;
               }
            }
         }
         if (scenario == 5U)
            std::swap((*tape)[0], (*tape)[1]);
         if (scenario == 6U)
            (*tape)[1] = (*tape)[0];
         if (scenario == 7U)
            tape->push_back(tape->front());
         if (scenario == 10U)
            tape->clear();
         if (scenario == 11U)
            budget.max_records = 1U;
         if (scenario == 12U) {
            budget.max_encoded_bytes = 512U;
            budget.max_frame_bytes = 512U;
            budget.max_value_bytes = 512U;
         }
         if (scenario == 13U)
            budget.max_working_bytes = budget.max_frame_bytes;
         if (scenario >= 15U && scenario <= 18U) {
            for (auto& frame : *tape) {
               if (auto* branch = std::get_if<auth::proof_branch>(&frame.node)) {
                  if (scenario == 15U)
                     ++branch->size;
                  if (scenario == 16U)
                     branch->separator = bytes("wrong");
                  if (scenario == 17U)
                     branch->min_key = bytes("wrong");
                  if (scenario == 18U)
                     branch->left_hash = hash_text("wrong-child");
                  break;
               }
            }
         }
         if (scenario == 19U) {
            for (auto& frame : *tape) {
               if (frame.tree == auth::proof_tree::changes) {
                  if (auto* leaf = std::get_if<auth::proof_leaf>(&frame.node)) {
                     leaf->value = auth::bytes{std::byte{2}};
                     leaf->value_hash = auth::hash_value(*leaf->value);
                     frame.hash = auth::hash_leaf(auth::canonical_tree_domain(settings.domain, frame.tree), leaf->key,
                                                  leaf->value_hash);
                     break;
                  }
               }
            }
         }
         if (scenario == 20U)
            tape->insert(tape->begin() + 1, tape->front());
         if (scenario == 21U)
            budget.max_value_bytes = 1U;
         if (scenario == 22U)
            budget.max_depth = 1U;
         auto active = co_await target_driver->begin_transaction();
         if (scenario == 11U || scenario == 13U) {
            // Invalid total policy is rejected synchronously before guard/frame
            // publication or any data loading. The untouched transaction is usable.
            BOOST_CHECK_THROW(static_cast<void>(target.join_restore(active, expected, budget)),
                              auth::exceptions::proof_limit_exceeded);
            co_await active.commit();
            continue;
         }
         auto joined = target.join_restore(active, expected, budget);
         auto cursor = std::make_shared<std::size_t>(0U);
         auto verified = std::make_shared<bool>(false);
         const auto injected = std::make_exception_ptr(std::runtime_error{"owned checkpoint callback failure"});
         if (scenario == 14U)
            active.attach_participant(std::make_shared<finalization_veto>(verified));
         auto failed = false;
         auto original_error = std::exception_ptr{};
         try {
            static_cast<void>(co_await joined.restore_checkpoint(
                [tape, cursor, scenario, injected]() -> boost::asio::awaitable<std::optional<auth::checkpoint_record>> {
                   if (scenario == 8U && *cursor == 3U)
                      std::rethrow_exception(injected);
                   if (*cursor == tape->size())
                      co_return std::nullopt;
                   co_return (*tape)[(*cursor)++];
                },
                [verified, scenario, injected](auth::checkpoint_summary) -> boost::asio::awaitable<void> {
                   *verified = true;
                   if (scenario == 9U)
                      std::rethrow_exception(injected);
                   co_return;
                }));
         } catch (...) {
            failed = true;
            original_error = std::current_exception();
         }
         BOOST_REQUIRE(failed);
         if (scenario == 8U || scenario == 9U)
            BOOST_CHECK(original_error == injected);
         BOOST_CHECK(!joined.restored());
         BOOST_CHECK_THROW(static_cast<void>(joined.restore_checkpoint({}, {})), auth::exceptions::invalid_version);
         if (scenario == 9U || scenario == 14U)
            BOOST_CHECK(*verified);
         BOOST_CHECK_THROW(co_await active.commit(), auth::exceptions::transaction_not_staged);
         BOOST_CHECK_THROW(co_await active.commit(), core::exceptions::transaction_rollback_only);
         co_await active.rollback();
         auto check = co_await target_driver->begin_read();
         BOOST_CHECK((co_await check.scan_page(settings.family, core::record_range{.has_end = false}, {.limit = 1U}))
                         .entries.empty());
      }
      // Same native target is reusable after explicit whole rollback, not via a
      // savepoint or replacement participant on a poisoned Core transaction.
      {
         auto active = co_await target_driver->begin_transaction();
         auto joined = target.join_restore(active, expected, policy);
         auto cursor = std::make_shared<std::size_t>(0U);
         static_cast<void>(co_await joined.restore_checkpoint(
             [frames, cursor]() -> boost::asio::awaitable<std::optional<auth::checkpoint_record>> {
                if (*cursor == frames->size())
                   co_return std::nullopt;
                co_return (*frames)[(*cursor)++];
             },
             [](auth::checkpoint_summary) -> boost::asio::awaitable<void> { co_return; }));
         co_await active.commit();
      }
      BOOST_CHECK(*(co_await target.latest()) == expected);
      co_await target_driver->async_close();
      target_driver.reset();
      co_await driver->async_close();
      driver.reset();
      co_await lane.shutdown();
   }());
   std::filesystem::remove_all(source_path);
   std::filesystem::remove_all(target_path);
}

BOOST_AUTO_TEST_CASE(authenticated_checkpoint_pending_guard_denies_lazy_drop_savepoint_stage_and_reentrant_escape) {
   namespace auth = forge::db::authenticated;
   namespace core = forge::db::core;
   const auto path = make_test_root("forge_authenticated_checkpoint_guard");
   const auto foreign_path = make_test_root("forge_authenticated_checkpoint_foreign");
   auto runtime = forge::asio::runtime{};
   auto lane = forge::asio::affine::lane{};
   const auto policy = auth::checkpoint_limits{.max_records = 32U,
                                               .max_encoded_bytes = 4096U,
                                               .max_value_bytes = 4096U,
                                               .max_frame_bytes = 1024U,
                                               .max_working_bytes = 1U << 20U,
                                               .max_depth = 16U};
   const auto settings = auth::store::config{.family = core::family{"authenticated"},
                                             .domain = "forge.test.checkpoint.guard.v1",
                                             .bounds = {.max_key_bytes = 128U, .max_value_bytes = 4096U}};
   const auto empty = auth::root{
       .version = 42U,
       .state_root = auth::hash_empty(auth::canonical_tree_domain(settings.domain, auth::proof_tree::state)),
       .change_root = auth::hash_empty(auth::canonical_tree_domain(settings.domain, auth::proof_tree::changes))};
   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto driver = co_await open_driver(path, lane);
      auto foreign = co_await open_driver(foreign_path, lane);
      auto target = auth::store{driver, settings};
      {
         auto active = co_await foreign->begin_transaction();
         BOOST_CHECK_THROW(static_cast<void>(target.join_restore(active, empty, policy)),
                           auth::exceptions::invalid_store);
         co_await active.commit();
         BOOST_CHECK_THROW(static_cast<void>(co_await target.export_checkpoint(
                               co_await foreign->begin_read(), empty,
                               [](auth::checkpoint_record) -> boost::asio::awaitable<void> { co_return; }, policy)),
                           auth::exceptions::invalid_store);
      }
      for (auto scenario = 0U; scenario < 7U; ++scenario) {
         auto active = co_await driver->begin_transaction();
         auto joined = std::make_shared<auth::transaction>(target.join_restore(active, empty, policy));
         if (scenario == 0U)
            joined.reset();
         if (scenario == 1U)
            BOOST_CHECK_THROW(static_cast<void>(co_await active.create_savepoint()), auth::exceptions::invalid_version);
         if (scenario == 2U)
            BOOST_CHECK_THROW(static_cast<void>(co_await joined->preview({})), auth::exceptions::invalid_version);
         if (scenario == 3U)
            BOOST_CHECK_THROW(static_cast<void>(co_await joined->stage({})), auth::exceptions::invalid_version);
         if (scenario == 4U) {
            auto called = std::make_shared<bool>(false);
            {
               auto abandoned = joined->restore_checkpoint(
                   [called]() -> boost::asio::awaitable<std::optional<auth::checkpoint_record>> {
                      *called = true;
                      co_return std::nullopt;
                   },
                   [](auth::checkpoint_summary) -> boost::asio::awaitable<void> { co_return; });
            }
            BOOST_CHECK(!*called);
         }
         if (scenario == 5U) {
            BOOST_CHECK_THROW(
                static_cast<void>(co_await joined->restore_checkpoint(
                    []() -> boost::asio::awaitable<std::optional<auth::checkpoint_record>> { co_return std::nullopt; },
                    [joined](auth::checkpoint_summary) -> boost::asio::awaitable<void> {
                       BOOST_CHECK_THROW(static_cast<void>(co_await joined->preview({})),
                                         auth::exceptions::invalid_version);
                       co_return; // Catching the inner error MUST NOT restore eligibility.
                    })),
                auth::exceptions::invalid_version);
         }
         if (scenario == 6U) {
            static_cast<void>(co_await joined->restore_checkpoint(
                []() -> boost::asio::awaitable<std::optional<auth::checkpoint_record>> { co_return std::nullopt; },
                [](auth::checkpoint_summary) -> boost::asio::awaitable<void> { co_return; }));
            BOOST_REQUIRE(joined->restored());
            BOOST_CHECK_THROW(static_cast<void>(co_await joined->preview({})), auth::exceptions::invalid_version);
            BOOST_CHECK(!joined->restored());
         }
         BOOST_CHECK_THROW(co_await active.commit(), auth::exceptions::transaction_not_staged);
         BOOST_CHECK_THROW(co_await active.commit(), core::exceptions::transaction_rollback_only);
         co_await active.rollback();
         auto check = co_await driver->begin_read();
         BOOST_CHECK((co_await check.scan_page(settings.family, core::record_range{.has_end = false}, {.limit = 1U}))
                         .entries.empty());
      }
      {
         auto objects = co_await forge::db::object::store::open(driver);
         auto revisions = co_await forge::db::revision::store::open(driver, objects);
         for (auto order = 0U; order < 4U; ++order) {
            auto active = co_await driver->begin_transaction();
            auto joined = target.join_restore(active, empty, policy);
            auto attached = std::make_shared<bool>(false);
            if (order == 0U) {
               static_cast<void>(co_await revisions.join(active));
               *attached = true;
            }
            auto restoring = joined.restore_checkpoint(
                [&active, &revisions, attached,
                 order]() -> boost::asio::awaitable<std::optional<auth::checkpoint_record>> {
                   if (order == 2U) {
                      static_cast<void>(co_await revisions.join(active));
                      *attached = true;
                   }
                   co_return std::nullopt;
                },
                [&active, &revisions, attached, order](auth::checkpoint_summary) -> boost::asio::awaitable<void> {
                   if (order == 3U) {
                      static_cast<void>(co_await revisions.join(active));
                      *attached = true;
                   }
                   co_return;
                });
            if (order == 1U) {
               static_cast<void>(co_await revisions.join(active));
               *attached = true;
            }
            if (order < 2U) {
               BOOST_CHECK_THROW(static_cast<void>(co_await std::move(restoring)), auth::exceptions::invalid_store);
            } else {
               BOOST_CHECK_THROW(static_cast<void>(co_await std::move(restoring)),
                                 core::exceptions::mutation_forbidden);
            }
            BOOST_CHECK(*attached);
            BOOST_CHECK(!joined.restored());
            BOOST_CHECK_THROW(co_await active.commit(), auth::exceptions::transaction_not_staged);
            BOOST_CHECK_THROW(co_await active.commit(), core::exceptions::transaction_rollback_only);
            co_await active.rollback();
            auto check = co_await driver->begin_read();
            BOOST_CHECK((co_await check.scan_page(settings.family, core::record_range{.has_end = false}, {.limit = 1U}))
                            .entries.empty());
         }
      }
      for (auto occupied = 0U; occupied < 2U; ++occupied) {
         const auto occupied_path = make_test_root("forge_authenticated_checkpoint_occupied");
         auto occupied_driver = co_await open_driver(occupied_path, lane);
         auto occupied_target = auth::store{occupied_driver, settings};
         {
            auto active = co_await occupied_driver->begin_transaction();
            if (occupied == 0U) {
               // An unknown record anywhere in the dedicated family must veto restore.
               co_await active.put(settings.family, core::record_key{{std::byte{0xff}}}, bytes("not-a-checkpoint"));
            } else {
               auto other_settings = settings;
               other_settings.domain += ".other";
               auto other_namespace = auth::store{occupied_driver, std::move(other_settings)};
               auto joined = co_await other_namespace.join(active, 1U);
               const auto mutations = std::vector{put("other", "namespace")};
               static_cast<void>(co_await joined.stage(mutations));
            }
            co_await active.commit();
         }
         auto check = co_await occupied_driver->begin_read();
         const auto before =
             co_await check.scan_page(settings.family, core::record_range{.has_end = false}, {.limit = 64U});
         BOOST_REQUIRE(!before.entries.empty());
         BOOST_REQUIRE(!before.next);
         check = {};
         auto callbacks = std::make_shared<unsigned>(0U);
         {
            auto active = co_await occupied_driver->begin_transaction();
            auto joined = occupied_target.join_restore(active, empty, policy);
            BOOST_CHECK_THROW(static_cast<void>(co_await joined.restore_checkpoint(
                                  [callbacks]() -> boost::asio::awaitable<std::optional<auth::checkpoint_record>> {
                                     ++*callbacks;
                                     co_return std::nullopt;
                                  },
                                  [callbacks](auth::checkpoint_summary) -> boost::asio::awaitable<void> {
                                     ++*callbacks;
                                     co_return;
                                  })),
                              auth::exceptions::invalid_store);
            BOOST_CHECK(!joined.restored());
            BOOST_CHECK_THROW(co_await active.commit(), auth::exceptions::transaction_not_staged);
            BOOST_CHECK_THROW(co_await active.commit(), core::exceptions::transaction_rollback_only);
            co_await active.rollback();
         }
         BOOST_CHECK_EQUAL(*callbacks, 0U);
         check = co_await occupied_driver->begin_read();
         const auto after =
             co_await check.scan_page(settings.family, core::record_range{.has_end = false}, {.limit = 64U});
         auto unchanged = before.next == after.next && before.entries.size() == after.entries.size();
         for (auto index = std::size_t{}; index < std::min(before.entries.size(), after.entries.size()); ++index) {
            unchanged &= before.entries[index].key == after.entries[index].key &&
                         before.entries[index].value == after.entries[index].value;
         }
         BOOST_CHECK(unchanged);
         check = {};
         BOOST_CHECK(!(co_await occupied_target.latest()));
         co_await occupied_driver->async_close();
         occupied_driver.reset();
         std::filesystem::remove_all(occupied_path);
      }
      {
         auto active = co_await driver->begin_transaction();
         auto joined = co_await target.join(active, 1U);
         const auto mutations = std::vector{put("occupied", "value")};
         static_cast<void>(co_await joined.stage(mutations));
         co_await active.commit();
      }
      const auto original = *(co_await target.latest());
      {
         auto active = co_await driver->begin_transaction();
         auto joined = target.join_restore(active, empty, policy);
         BOOST_CHECK_THROW(
             static_cast<void>(co_await joined.restore_checkpoint(
                 []() -> boost::asio::awaitable<std::optional<auth::checkpoint_record>> { co_return std::nullopt; },
                 [](auth::checkpoint_summary) -> boost::asio::awaitable<void> { co_return; })),
             auth::exceptions::invalid_store);
         BOOST_CHECK_THROW(co_await active.commit(), auth::exceptions::transaction_not_staged);
         co_await active.rollback();
      }
      BOOST_CHECK(*(co_await target.latest()) == original);
      co_await driver->async_close();
      co_await foreign->async_close();
      driver.reset();
      foreign.reset();
      co_await lane.shutdown();
   }());
   std::filesystem::remove_all(path);
   std::filesystem::remove_all(foreign_path);
}

BOOST_AUTO_TEST_CASE(authenticated_checkpoint_cooperative_cancel_is_drained_before_caught_commit_is_rejected) {
   namespace auth = forge::db::authenticated;
   namespace core = forge::db::core;
   const auto path = make_test_root("forge_authenticated_checkpoint_cancel");
   auto runtime = forge::asio::runtime{}; // One IO worker serializes the cancellation signal/slot.
   auto lane = forge::asio::affine::lane{};
   const auto policy = auth::checkpoint_limits{.max_records = 32U,
                                               .max_encoded_bytes = 4096U,
                                               .max_value_bytes = 4096U,
                                               .max_frame_bytes = 1024U,
                                               .max_working_bytes = 1U << 20U,
                                               .max_depth = 16U};
   const auto settings = auth::store::config{.family = core::family{"authenticated"},
                                             .domain = "forge.test.checkpoint.cancel.v1",
                                             .bounds = {.max_key_bytes = 128U, .max_value_bytes = 4096U}};
   const auto empty = auth::root{
       .version = 42U,
       .state_root = auth::hash_empty(auth::canonical_tree_domain(settings.domain, auth::proof_tree::state)),
       .change_root = auth::hash_empty(auth::canonical_tree_domain(settings.domain, auth::proof_tree::changes))};
   struct state {
      forge::asio::notification entered;
      forge::asio::notification completed;
      std::atomic_bool source_entered = false;
      std::atomic_bool terminal = false;
      std::exception_ptr error;
      std::shared_ptr<core::transaction> active;
      std::shared_ptr<auth::transaction> joined;
   };
   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto driver = co_await open_driver(path, lane);
      auto target = auth::store{driver, settings};
      auto owned = std::make_shared<state>();
      owned->active = std::make_shared<core::transaction>(co_await driver->begin_transaction());
      owned->joined = std::make_shared<auth::transaction>(target.join_restore(*owned->active, empty, policy));
      auto cancellation = boost::asio::cancellation_signal{};
      const auto entered_epoch = owned->entered.epoch();
      const auto completed_epoch = owned->completed.epoch();
      boost::asio::co_spawn(co_await boost::asio::this_coro::executor,
                            owned->joined->restore_checkpoint(
                                [owned]() -> boost::asio::awaitable<std::optional<auth::checkpoint_record>> {
                                   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
                                   timer.expires_after(std::chrono::seconds{1});
                                   owned->source_entered.store(true, std::memory_order_release);
                                   owned->entered.notify();
                                   co_await timer.async_wait(boost::asio::use_awaitable);
                                   co_return std::nullopt;
                                },
                                [](auth::checkpoint_summary) -> boost::asio::awaitable<void> { co_return; }),
                            boost::asio::bind_cancellation_slot(
                                cancellation.slot(), [owned](std::exception_ptr error, auth::checkpoint_summary) {
                                   owned->error = error;
                                   owned->terminal.store(true, std::memory_order_release);
                                   owned->completed.notify();
                                }));
      static_cast<void>(co_await owned->entered.async_wait_until(entered_epoch, std::chrono::steady_clock::now() +
                                                                                    std::chrono::seconds{2}));
      cancellation.emit(boost::asio::cancellation_type::terminal);
      // Source has its own finite one-second timer even if cancellation is broken.
      // Heap-owned Core/guard remain retained by the terminal handler until it drains.
      static_cast<void>(co_await owned->completed.async_wait_until(completed_epoch, std::chrono::steady_clock::now() +
                                                                                        std::chrono::seconds{3}));
      if (!owned->terminal.load(std::memory_order_acquire)) {
         // Test-only fail-fast: never unwind runtime/lane owners with an unjoined child.
         std::terminate();
      }
      BOOST_REQUIRE(owned->terminal.load(std::memory_order_acquire));
      BOOST_CHECK(owned->source_entered.load(std::memory_order_acquire));
      BOOST_REQUIRE(owned->error);
      auto cancelled = false;
      try {
         std::rethrow_exception(owned->error);
      } catch (const boost::system::system_error& error) {
         cancelled = error.code() == boost::asio::error::operation_aborted;
      }
      BOOST_CHECK(cancelled);
      BOOST_CHECK(!owned->joined->restored());
      BOOST_CHECK_THROW(co_await owned->active->commit(), auth::exceptions::transaction_not_staged);
      BOOST_CHECK_THROW(co_await owned->active->commit(), core::exceptions::transaction_rollback_only);
      co_await owned->active->rollback();
      owned->joined.reset();
      owned->active.reset();
      BOOST_CHECK(!(co_await target.latest()));
      co_await driver->async_close();
      driver.reset();
      co_await lane.shutdown();
   }());
   std::filesystem::remove_all(path);
}

BOOST_AUTO_TEST_SUITE_END()
