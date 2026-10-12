#include <boost/test/unit_test.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>

#include <forge/exceptions/macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

import forge.asio.blocking;
import forge.asio.runtime;
import forge.multiformats.multihash;
import forge.multiformats.types;
import forge.net.p2p.endpoint;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.protocol;
import forge.test.net.p2p.dht_record_store_fixture;

namespace forge::net::p2p {
namespace {

template<class T>
T bounded_record_result(forge::asio::runtime& runtime, boost::asio::awaitable<T> operation) {
   auto result = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
   if (result.wait_for(std::chrono::seconds{5}) != std::future_status::ready) {
      std::cerr << "default DHT record store fixture did not join its operation\n";
      std::terminate();
   }
   return result.get();
}

[[nodiscard]] peer_id test_peer(std::uint8_t value) {
   const auto payload = forge::multiformats::bytes{value};
   return peer_id::from_bytes(forge::multiformats::multihash::identity(payload).encode());
}

[[nodiscard]] dht::key test_key(std::uint8_t value) {
   return dht::key{.bytes = {'/', value}};
}

[[nodiscard]] dht::profile test_profile() {
   return custom_dht_profile(
       protocol_id{.value = "/forge/test/kad/1.0.0"}, dht::mode::server,
       dht::profile_capabilities{.peers = true, .providers = true, .values = true},
       {dht::value_policy{
           .key_prefix = {'/'},
           .validate =
               [](const dht::record& value, dht::value_validation_context) {
                  if (value.value.empty()) {
                     FORGE_THROW_EXCEPTION(exceptions::record_rejected, "empty test DHT value");
                  }
               },
           .select =
               [](std::span<const dht::record> candidates) {
                  if (candidates.empty()) {
                     throw std::runtime_error{"empty test DHT selection"};
                  }
                  auto selected = std::size_t{};
                  for (auto index = std::size_t{1}; index < candidates.size(); ++index) {
                     if (candidates[selected].value < candidates[index].value) {
                        selected = index;
                     }
                  }
                  return selected;
               },
           .expiry = [](const dht::record&, dht::value_expiry_context context) { return context.supplied_expires_at; },
       }});
}

[[nodiscard]] dht::record_store::value_record test_value(dht::key key, std::uint8_t value,
                                                         std::chrono::system_clock::time_point expires_at) {
   return dht::record_store::value_record{
       .record = dht::record{.key_value = std::move(key), .value = {value}},
       .expires_at = expires_at,
   };
}

[[nodiscard]] dht::record_store::provider_record
test_provider(dht::key key, peer_id provider, std::chrono::system_clock::time_point provider_expires_at,
              std::chrono::system_clock::time_point addresses_expires_at, std::uint16_t port = 4401) {
   auto address = parse_endpoint("/ip4/127.0.0.1/udp/" + std::to_string(port) + "/quic-v1/p2p/" + provider.to_string());
   return dht::record_store::provider_record{
       .key = std::move(key),
       .provider = std::move(provider),
       .endpoints = {address.to_multiaddr()},
       .provider_expires_at = provider_expires_at,
       .addresses_expires_at = addresses_expires_at,
   };
}

} // namespace

BOOST_AUTO_TEST_SUITE(dht_record_store_tests)

BOOST_AUTO_TEST_CASE(dht_record_store_default_constructor_preserves_custom_k_one) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto profile = test_profile();
   profile.limits.replication = 1;
   profile.limits.alpha = 1;
   auto store = dht::record_store{std::move(profile)};
   auto cleanup = boost::scope::scope_exit{[&] {
      try { bounded_record_result(runtime, store.async_close()); }
      catch (...) { BOOST_ERROR("custom default DHT store cleanup failed"); }
   }};
   const auto now = std::chrono::system_clock::now();
   const auto key = test_key('k');
   const auto expiry = now + std::chrono::hours{1};
   bounded_record_result(runtime, store.async_upsert_provider(test_provider(key, test_peer(1), expiry, expiry), now));
   BOOST_CHECK_THROW(bounded_record_result(runtime, store.async_upsert_provider(
       test_provider(key, test_peer(2), expiry, expiry), now)), exceptions::backpressure_rejected);
   const auto providers = store.find_providers(key, 20, now);
   BOOST_REQUIRE_EQUAL(providers.size(), 1U);
   BOOST_CHECK(providers.front().provider == test_peer(1));
}

BOOST_AUTO_TEST_CASE(dht_record_store_default_constructor_preserves_amino_k_twenty) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto store = dht::record_store{amino_v1()};
   auto cleanup = boost::scope::scope_exit{[&] {
      try { bounded_record_result(runtime, store.async_close()); }
      catch (...) { BOOST_ERROR("Amino default DHT store cleanup failed"); }
   }};
   const auto now = std::chrono::system_clock::now();
   const auto key = test_key('a');
   const auto expiry = now + std::chrono::hours{1};
   for (auto id = std::uint8_t{1}; id <= 20; ++id) {
      bounded_record_result(runtime, store.async_upsert_provider(test_provider(key, test_peer(id), expiry, expiry), now));
   }
   BOOST_CHECK_THROW(bounded_record_result(runtime, store.async_upsert_provider(
       test_provider(key, test_peer(21), expiry, expiry), now)), exceptions::backpressure_rejected);
   const auto providers = store.find_providers(key, 21, now);
   BOOST_REQUIRE_EQUAL(providers.size(), 20U);
   for (const auto& provider : providers) { BOOST_CHECK(provider.provider != test_peer(21)); }
}

BOOST_AUTO_TEST_CASE(dht_record_store_validates_and_selects_before_durable_publication) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto store = dht::record_store{test_profile()};
   const auto now = std::chrono::system_clock::now();
   const auto key = test_key('s');

   const auto first =
       forge::asio::blocking::run(runtime, store.async_put(test_value(key, 2, now + std::chrono::hours{1}), now));
   BOOST_TEST(first.selected.record.value == (std::vector<std::uint8_t>{2}));
   BOOST_TEST(static_cast<int>(first.outcome) == static_cast<int>(dht::record_store::put_outcome::incoming_stored));
   const auto rejected =
       forge::asio::blocking::run(runtime, store.async_put(test_value(key, 1, now + std::chrono::hours{2}), now));
   BOOST_TEST(rejected.selected.record.value == (std::vector<std::uint8_t>{2}));
   BOOST_TEST(static_cast<int>(rejected.outcome) ==
              static_cast<int>(dht::record_store::put_outcome::existing_preferred));
   const auto selected =
       forge::asio::blocking::run(runtime, store.async_put(test_value(key, 3, now + std::chrono::hours{3}), now));
   BOOST_TEST(selected.selected.record.value == (std::vector<std::uint8_t>{3}));
   BOOST_TEST(static_cast<int>(selected.outcome) == static_cast<int>(dht::record_store::put_outcome::incoming_stored));
   BOOST_TEST(store.find_value(key, now)->record.value == (std::vector<std::uint8_t>{3}));
}

BOOST_AUTO_TEST_CASE(dht_record_store_rolls_back_operational_value_when_persistence_throws) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<forge::test::net::p2p::dht_record_store_persistence>();
   auto store = dht::record_store{test_profile(), dht::record_store::options{.persistence = persistence}};
   const auto now = std::chrono::system_clock::now();
   const auto key = test_key('r');
   forge::asio::blocking::run(runtime, store.async_put(test_value(key, 1, now + std::chrono::hours{1}), now));

   persistence->fail_next_apply();
   BOOST_CHECK_THROW(
       (forge::asio::blocking::run(runtime, store.async_put(test_value(key, 2, now + std::chrono::hours{2}), now))),
       std::runtime_error);
   const auto stored = store.find_value(key, now);
   BOOST_REQUIRE(stored);
   BOOST_TEST(stored->record.value == (std::vector<std::uint8_t>{1}));
   const auto status = store.persistence_state();
   BOOST_TEST(status.failure_count == 1U);
   BOOST_TEST(status.degraded);
   BOOST_TEST(!status.durability_uncertain);
   BOOST_TEST(status.last_failure.find("injected DHT record persistence failure") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(dht_record_store_propagates_operational_validator_failures) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto profile = test_profile();
   profile.value_policies.front().validate = [](const dht::record&, dht::value_validation_context) {
      throw std::runtime_error{"validator unavailable"};
   };
   auto store = dht::record_store{std::move(profile)};
   const auto now = std::chrono::system_clock::now();

   BOOST_CHECK_THROW((forge::asio::blocking::run(
                         runtime, store.async_put(test_value(test_key('o'), 1, now + std::chrono::hours{1}), now))),
                     std::runtime_error);
}

BOOST_AUTO_TEST_CASE(dht_record_store_discards_code_based_rejection_only_at_network_ingress) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto profile = test_profile();
   profile.value_policies.front().validate = [](const dht::record&, dht::value_validation_context) {
      FORGE_THROW_CODE(exceptions::code::record_rejected, "rejected by test DHT policy");
   };
   auto store = dht::record_store{std::move(profile)};
   const auto now = std::chrono::system_clock::now();
   const auto key = test_key('n');

   const auto result = forge::asio::blocking::run(
       runtime, store.async_put_received(test_value(key, 1, now + std::chrono::hours{1}), now));
   BOOST_CHECK(!result);
   BOOST_CHECK(!store.find_value(key, now));
}

BOOST_AUTO_TEST_CASE(dht_record_store_propagates_typed_rejection_from_persistence) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<forge::test::net::p2p::dht_record_store_persistence>();
   auto store = dht::record_store{test_profile(), dht::record_store::options{.persistence = persistence}};
   const auto now = std::chrono::system_clock::now();
   const auto key = test_key('p');

   persistence->reject_next_apply_as_record();
   BOOST_CHECK_THROW((forge::asio::blocking::run(
                         runtime, store.async_put_received(test_value(key, 1, now + std::chrono::hours{1}), now))),
                     exceptions::record_rejected);
   BOOST_CHECK(!store.find_value(key, now));
   BOOST_TEST(store.persistence_state().degraded);
}

BOOST_AUTO_TEST_CASE(dht_record_store_publishes_post_commit_uncertain_value_and_flush_recovers_status) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<forge::test::net::p2p::dht_record_store_persistence>();
   auto store = dht::record_store{test_profile(), dht::record_store::options{.persistence = persistence}};
   const auto now = std::chrono::system_clock::now();
   const auto key = test_key('u');

   persistence->make_next_apply_durability_uncertain();
   BOOST_CHECK_THROW(
       (forge::asio::blocking::run(runtime, store.async_put(test_value(key, 7, now + std::chrono::hours{1}), now))),
       exceptions::durability_uncertain);

   const auto operational = store.find_value(key, now);
   BOOST_REQUIRE(operational);
   BOOST_TEST(operational->record.value == (std::vector<std::uint8_t>{7}));
   const auto durable =
       forge::asio::blocking::run(runtime, persistence->async_hydrate(dht::record_store::hydration_request{
                                               .kind = dht::record_store::hydration_kind::values}));
   BOOST_REQUIRE_EQUAL(durable.values.size(), 1U);
   BOOST_TEST(durable.values.front().record.value == (std::vector<std::uint8_t>{7}));

   auto status = store.persistence_state();
   BOOST_TEST(status.failure_count == 1U);
   BOOST_TEST(status.degraded);
   BOOST_TEST(status.durability_uncertain);
   BOOST_TEST(status.last_failure.find("post-commit DHT durability failure") != std::string::npos);

   forge::asio::blocking::run(runtime, store.async_hydrate(now));
   status = store.persistence_state();
   BOOST_TEST(status.degraded);
   BOOST_TEST(status.durability_uncertain);

   const auto pruned = forge::asio::blocking::run(runtime, store.async_prune_expired(now));
   BOOST_TEST(pruned.values.empty());
   BOOST_TEST(pruned.providers.empty());
   BOOST_TEST(pruned.provider_address_updates.empty());
   status = store.persistence_state();
   BOOST_TEST(status.degraded);
   BOOST_TEST(status.durability_uncertain);

   forge::asio::blocking::run(runtime, store.async_flush());
   status = store.persistence_state();
   BOOST_TEST(status.failure_count == 1U);
   BOOST_TEST(!status.degraded);
   BOOST_TEST(!status.durability_uncertain);
   BOOST_TEST(status.last_failure.empty());
}

BOOST_AUTO_TEST_CASE(dht_record_store_publishes_post_commit_uncertain_provider) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<forge::test::net::p2p::dht_record_store_persistence>();
   auto store = dht::record_store{test_profile(), dht::record_store::options{.persistence = persistence}};
   const auto now = std::chrono::system_clock::now();
   const auto key = test_key('o');
   const auto provider = test_peer(11);

   persistence->make_next_apply_durability_uncertain();
   BOOST_CHECK_THROW((forge::asio::blocking::run(
                         runtime, store.async_upsert_provider(test_provider(key, provider, now + std::chrono::hours{1},
                                                                            now + std::chrono::minutes{5}),
                                                              now))),
                     exceptions::durability_uncertain);

   const auto operational = store.find_providers(key, 1, now);
   BOOST_REQUIRE_EQUAL(operational.size(), 1U);
   BOOST_CHECK(operational.front().provider == provider);
   const auto durable =
       forge::asio::blocking::run(runtime, persistence->async_hydrate(dht::record_store::hydration_request{
                                               .kind = dht::record_store::hydration_kind::providers}));
   BOOST_REQUIRE_EQUAL(durable.providers.size(), 1U);
   BOOST_CHECK(durable.providers.front().provider == provider);
   BOOST_TEST(store.persistence_state().degraded);
   BOOST_TEST(store.persistence_state().durability_uncertain);
}

BOOST_AUTO_TEST_CASE(dht_record_store_publishes_post_commit_uncertain_provider_removal) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<forge::test::net::p2p::dht_record_store_persistence>();
   auto store = dht::record_store{test_profile(), dht::record_store::options{.persistence = persistence}};
   const auto now = std::chrono::system_clock::now();
   const auto key = test_key('d');
   const auto provider = test_peer(12);
   forge::asio::blocking::run(
       runtime, store.async_upsert_provider(
                    test_provider(key, provider, now + std::chrono::hours{1}, now + std::chrono::minutes{5}), now));

   persistence->make_next_apply_durability_uncertain();
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_remove_provider(dht::record_store::provider_key{
                                                              .key = key, .provider = provider}))),
                     exceptions::durability_uncertain);

   BOOST_TEST(store.find_providers(key, 1, now).empty());
   const auto durable =
       forge::asio::blocking::run(runtime, persistence->async_hydrate(dht::record_store::hydration_request{
                                               .kind = dht::record_store::hydration_kind::providers}));
   BOOST_TEST(durable.providers.empty());
   BOOST_TEST(store.persistence_state().degraded);
   BOOST_TEST(store.persistence_state().durability_uncertain);
}

BOOST_AUTO_TEST_CASE(dht_record_store_removes_durable_provider_absent_from_operational_state) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<forge::test::net::p2p::dht_record_store_persistence>();
   auto store = dht::record_store{test_profile(), dht::record_store::options{.persistence = persistence}};
   const auto now = std::chrono::system_clock::now();
   const auto key = test_key('b');
   const auto provider = test_peer(13);
   auto batch = dht::record_store::mutation_batch{};
   batch.provider_upserts.push_back(
       test_provider(key, provider, now + std::chrono::hours{1}, now + std::chrono::minutes{5}));
   const auto seeded = forge::asio::blocking::run(runtime, persistence->async_apply(std::move(batch)));
   BOOST_TEST(seeded.durability_confirmed);
   BOOST_TEST(store.find_providers(key, 1, now).empty());

   forge::asio::blocking::run(
       runtime, store.async_remove_provider(dht::record_store::provider_key{.key = key, .provider = provider}));

   const auto durable =
       forge::asio::blocking::run(runtime, persistence->async_hydrate(dht::record_store::hydration_request{
                                               .kind = dht::record_store::hydration_kind::providers}));
   BOOST_TEST(durable.providers.empty());
}

BOOST_AUTO_TEST_CASE(dht_record_store_publishes_post_commit_uncertain_prune) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<forge::test::net::p2p::dht_record_store_persistence>();
   auto store = dht::record_store{test_profile(), dht::record_store::options{.persistence = persistence}};
   const auto now = std::chrono::system_clock::now();
   const auto key = test_key('n');
   forge::asio::blocking::run(runtime, store.async_put(test_value(key, 9, now + std::chrono::minutes{1}), now));

   persistence->make_next_prune_durability_uncertain();
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_prune_expired(now + std::chrono::minutes{1}))),
                     exceptions::durability_uncertain);

   BOOST_TEST(!store.find_value(key, now + std::chrono::minutes{1}));
   const auto durable =
       forge::asio::blocking::run(runtime, persistence->async_hydrate(dht::record_store::hydration_request{
                                               .kind = dht::record_store::hydration_kind::values}));
   BOOST_TEST(durable.values.empty());
   const auto status = store.persistence_state();
   BOOST_TEST(status.failure_count == 1U);
   BOOST_TEST(status.degraded);
   BOOST_TEST(status.durability_uncertain);
   BOOST_TEST(status.last_failure.find("post-commit DHT prune durability failure") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(dht_record_store_rejects_invalid_committed_prune_results_before_runtime_mutation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<forge::test::net::p2p::dht_record_store_persistence>();
   auto store = dht::record_store{test_profile(), dht::record_store::options{.persistence = persistence}};
   const auto now = std::chrono::system_clock::now();
   const auto key = test_key('i');
   const auto provider = test_peer(19);
   const auto live_value = test_value(key, 4, now + std::chrono::hours{1});
   const auto live_provider = test_provider(key, provider, now + std::chrono::hours{2}, now + std::chrono::hours{1});
   forge::asio::blocking::run(runtime, store.async_put(live_value, now));
   forge::asio::blocking::run(runtime, store.async_upsert_provider(live_provider, now));

   auto invalid = dht::record_store::prune_result{};
   invalid.values.push_back(key);
   auto replacement = live_provider;
   replacement.endpoints.clear();
   replacement.addresses_expires_at = {};
   invalid.provider_address_updates.push_back(std::move(replacement));
   persistence->return_next_prune_result(std::move(invalid));

   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_prune_expired(now))), exceptions::internal);
   BOOST_REQUIRE(store.find_value(key, now));
   const auto providers = store.find_providers(key, 1, now);
   BOOST_REQUIRE_EQUAL(providers.size(), 1U);
   BOOST_REQUIRE_EQUAL(providers.front().endpoints.size(), 1U);
   BOOST_TEST(store.persistence_state().degraded);
   BOOST_TEST(store.persistence_state().durability_uncertain);

   forge::asio::blocking::run(runtime, store.async_flush());
   auto status = store.persistence_state();
   BOOST_TEST(status.degraded);
   BOOST_TEST(status.durability_uncertain);
   BOOST_TEST(status.last_failure.find("invalid committed prune result") != std::string::npos);

   forge::asio::blocking::run(runtime, store.async_hydrate(now));
   status = store.persistence_state();
   BOOST_TEST(status.degraded);
   BOOST_TEST(status.durability_uncertain);

   forge::asio::blocking::run(runtime, store.async_flush());
   status = store.persistence_state();
   BOOST_TEST(!status.degraded);
   BOOST_TEST(!status.durability_uncertain);
   BOOST_TEST(status.last_failure.empty());
}

BOOST_AUTO_TEST_CASE(dht_record_store_keeps_provider_identity_after_address_expiry) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto store = dht::record_store{test_profile()};
   const auto now = std::chrono::system_clock::now();
   const auto key = test_key('p');
   const auto provider = test_peer(1);
   forge::asio::blocking::run(
       runtime, store.async_upsert_provider(
                    test_provider(key, provider, now + std::chrono::minutes{2}, now + std::chrono::minutes{1}), now));

   const auto without_addresses = store.find_providers(key, 1, now + std::chrono::minutes{1});
   BOOST_REQUIRE_EQUAL(without_addresses.size(), 1U);
   BOOST_CHECK(without_addresses.front().provider == provider);
   BOOST_TEST(without_addresses.front().endpoints.empty());
   BOOST_TEST(store.find_providers(key, 1, now + std::chrono::minutes{2}).empty());
}

BOOST_AUTO_TEST_CASE(dht_record_store_enforces_value_provider_and_per_key_capacity) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto now = std::chrono::system_clock::now();
   auto store = dht::record_store{
       test_profile(), dht::record_store::options{.max_values = 1, .max_providers = 2, .max_providers_per_key = 1}};

   forge::asio::blocking::run(runtime, store.async_put(test_value(test_key('a'), 1, now + std::chrono::hours{1}), now));
   BOOST_CHECK_THROW((forge::asio::blocking::run(
                         runtime, store.async_put(test_value(test_key('b'), 1, now + std::chrono::hours{1}), now))),
                     exceptions::backpressure_rejected);

   const auto first_key = test_key('c');
   forge::asio::blocking::run(
       runtime,
       store.async_upsert_provider(
           test_provider(first_key, test_peer(2), now + std::chrono::hours{1}, now + std::chrono::minutes{5}), now));
   BOOST_CHECK_THROW(
       (forge::asio::blocking::run(
           runtime, store.async_upsert_provider(test_provider(first_key, test_peer(3), now + std::chrono::hours{1},
                                                              now + std::chrono::minutes{5}),
                                                now))),
       exceptions::backpressure_rejected);
   forge::asio::blocking::run(
       runtime, store.async_upsert_provider(test_provider(test_key('d'), test_peer(3), now + std::chrono::hours{1},
                                                          now + std::chrono::minutes{5}),
                                            now));
   BOOST_CHECK_THROW(
       (forge::asio::blocking::run(
           runtime, store.async_upsert_provider(test_provider(test_key('e'), test_peer(4), now + std::chrono::hours{1},
                                                              now + std::chrono::minutes{5}),
                                                now))),
       exceptions::backpressure_rejected);
}

BOOST_AUTO_TEST_CASE(dht_record_store_enforces_record_and_total_byte_capacity) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto now = std::chrono::system_clock::now();
   auto record_limited = dht::record_store{test_profile(), dht::record_store::options{.max_record_bytes = 2}};
   BOOST_CHECK_THROW(
       (forge::asio::blocking::run(
           runtime, record_limited.async_put(test_value(test_key('x'), 1, now + std::chrono::hours{1}), now))),
       exceptions::backpressure_rejected);

   auto total_limited =
       dht::record_store{test_profile(), dht::record_store::options{.max_total_bytes = 5, .max_record_bytes = 4}};
   forge::asio::blocking::run(runtime,
                              total_limited.async_put(test_value(test_key('y'), 1, now + std::chrono::hours{1}), now));
   BOOST_CHECK_THROW(
       (forge::asio::blocking::run(
           runtime, total_limited.async_put(test_value(test_key('z'), 1, now + std::chrono::hours{1}), now))),
       exceptions::backpressure_rejected);
}

BOOST_AUTO_TEST_CASE(dht_record_store_hydrates_bounded_pages_across_reopen) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = dht::record_store::make_memory_persistence();
   const auto now = std::chrono::system_clock::now();
   {
      auto writer = dht::record_store{test_profile(), dht::record_store::options{.persistence = persistence}};
      for (auto index = std::uint8_t{}; index < 3; ++index) {
         forge::asio::blocking::run(runtime,
                                    writer.async_put(test_value(test_key(static_cast<std::uint8_t>('h' + index)),
                                                                index + 1, now + std::chrono::hours{1}),
                                                     now));
      }
      forge::asio::blocking::run(
          runtime, writer.async_upsert_provider(test_provider(test_key('q'), test_peer(5), now + std::chrono::hours{1},
                                                              now + std::chrono::minutes{5}),
                                                now));
      forge::asio::blocking::run(runtime, writer.async_flush());
   }

   auto reader = dht::record_store{test_profile(),
                                   dht::record_store::options{.persistence = persistence, .hydration_page_limit = 1}};
   forge::asio::blocking::run(runtime, reader.async_hydrate(now));
   BOOST_REQUIRE(reader.find_value(test_key('h'), now));
   BOOST_REQUIRE(reader.find_value(test_key('i'), now));
   BOOST_REQUIRE(reader.find_value(test_key('j'), now));
   BOOST_REQUIRE_EQUAL(reader.find_providers(test_key('q'), 1, now).size(), 1U);
}

BOOST_AUTO_TEST_CASE(dht_record_store_prunes_expired_rows_before_hydration_capacity) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = dht::record_store::make_memory_persistence();
   const auto now = std::chrono::system_clock::now();
   const auto live_value = test_key('v');
   const auto live_provider_key = test_key('p');
   const auto live_provider = test_peer(79);

   auto batch = dht::record_store::mutation_batch{};
   batch.value_upserts.push_back(test_value(test_key('a'), 1, now - std::chrono::minutes{2}));
   batch.value_upserts.push_back(test_value(test_key('b'), 2, now - std::chrono::minutes{1}));
   batch.value_upserts.push_back(test_value(live_value, 3, now + std::chrono::hours{1}));
   batch.provider_upserts.push_back(
       test_provider(test_key('c'), test_peer(77), now - std::chrono::minutes{2}, now - std::chrono::minutes{3}));
   batch.provider_upserts.push_back(
       test_provider(test_key('d'), test_peer(78), now - std::chrono::minutes{1}, now - std::chrono::minutes{2}));
   batch.provider_upserts.push_back(
       test_provider(live_provider_key, live_provider, now + std::chrono::hours{1}, now + std::chrono::minutes{30}));
   forge::asio::blocking::run(runtime, persistence->async_apply(std::move(batch)));

   auto store = dht::record_store{test_profile(), dht::record_store::options{
                                                      .persistence = persistence,
                                                      .max_values = 1,
                                                      .max_providers = 1,
                                                      .max_providers_per_key = 1,
                                                      .hydration_page_limit = 1,
                                                      .prune_page_limit = 1,
                                                  }};
   forge::asio::blocking::run(runtime, store.async_hydrate(now));

   BOOST_REQUIRE(store.find_value(live_value, now));
   BOOST_REQUIRE_EQUAL(store.find_providers(live_provider_key, 1, now).size(), 1U);
   BOOST_CHECK(store.find_providers(live_provider_key, 1, now).front().provider == live_provider);

   const auto durable_values =
       forge::asio::blocking::run(runtime, persistence->async_hydrate(dht::record_store::hydration_request{
                                               .kind = dht::record_store::hydration_kind::values,
                                               .limit = 10,
                                           }));
   const auto durable_providers =
       forge::asio::blocking::run(runtime, persistence->async_hydrate(dht::record_store::hydration_request{
                                               .kind = dht::record_store::hydration_kind::providers,
                                               .limit = 10,
                                           }));
   BOOST_REQUIRE_EQUAL(durable_values.values.size(), 1U);
   BOOST_REQUIRE_EQUAL(durable_providers.providers.size(), 1U);
   BOOST_CHECK(durable_values.values.front().record.key_value == live_value);
   BOOST_CHECK(durable_providers.providers.front().provider == live_provider);

   auto over_capacity_persistence = dht::record_store::make_memory_persistence();
   auto over_capacity_batch = dht::record_store::mutation_batch{};
   over_capacity_batch.value_upserts.push_back(test_value(test_key('y'), 1, now + std::chrono::hours{1}));
   over_capacity_batch.value_upserts.push_back(test_value(test_key('z'), 2, now + std::chrono::hours{1}));
   forge::asio::blocking::run(runtime, over_capacity_persistence->async_apply(std::move(over_capacity_batch)));
   auto over_capacity = dht::record_store{test_profile(), dht::record_store::options{
                                                              .persistence = over_capacity_persistence,
                                                              .max_values = 1,
                                                              .hydration_page_limit = 1,
                                                              .prune_page_limit = 1,
                                                          }};
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, over_capacity.async_hydrate(now))),
                     exceptions::backpressure_rejected);
}

BOOST_AUTO_TEST_CASE(dht_record_store_rejects_nonprogressing_hydration_prune) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<forge::test::net::p2p::dht_record_store_persistence>();
   auto invalid = dht::record_store::prune_result{.may_have_more = true};
   persistence->return_next_prune_result(std::move(invalid));

   auto store = dht::record_store{test_profile(), dht::record_store::options{.persistence = persistence}};
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_hydrate(std::chrono::system_clock::now()))),
                     exceptions::internal);
   const auto status = store.persistence_state();
   BOOST_TEST(status.degraded);
   BOOST_TEST(status.durability_uncertain);
}

BOOST_AUTO_TEST_CASE(dht_record_store_bounds_repeating_nonempty_hydration_prune) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<forge::test::net::p2p::dht_record_store_persistence>();
   auto repeating = dht::record_store::prune_result{.values = {test_key('r')}, .may_have_more = true};
   persistence->repeat_prune_result(std::move(repeating));

   auto store = dht::record_store{test_profile(), dht::record_store::options{
                                                      .persistence = persistence,
                                                      .max_hydration_pages = 2,
                                                  }};
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_hydrate(std::chrono::system_clock::now()))),
                     exceptions::backpressure_rejected);
   const auto status = store.persistence_state();
   BOOST_TEST(status.degraded);
   BOOST_TEST(!status.durability_uncertain);
}

BOOST_AUTO_TEST_CASE(dht_record_store_bounds_live_hydration_pages_and_releases_persistence_gate) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = dht::record_store::make_memory_persistence();
   const auto now = std::chrono::system_clock::now();
   auto batch = dht::record_store::mutation_batch{};
   batch.value_upserts.push_back(test_value(test_key('a'), 1, now + std::chrono::hours{1}));
   batch.value_upserts.push_back(test_value(test_key('b'), 2, now + std::chrono::hours{1}));
   forge::asio::blocking::run(runtime, persistence->async_apply(std::move(batch)));

   auto store = dht::record_store{test_profile(), dht::record_store::options{
                                                      .persistence = persistence,
                                                      .max_values = 2,
                                                      .hydration_page_limit = 1,
                                                      .max_hydration_pages = 1,
                                                  }};
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_hydrate(now))),
                     exceptions::backpressure_rejected);
   auto status = store.persistence_state();
   BOOST_TEST(status.degraded);
   BOOST_TEST(!status.durability_uncertain);

   forge::asio::blocking::run(runtime, store.async_flush());
   status = store.persistence_state();
   BOOST_TEST(!status.degraded);
   BOOST_TEST(!status.durability_uncertain);
}

BOOST_AUTO_TEST_CASE(dht_record_store_requires_local_provider_reconfirmation_after_restart) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = dht::record_store::make_memory_persistence();
   const auto now = std::chrono::system_clock::now();
   const auto key = test_key('r');
   const auto local = test_peer(71);
   const auto remote = test_peer(72);
   {
      auto store = dht::record_store{test_profile(), dht::record_store::options{.persistence = persistence}};
      auto local_record = test_provider(key, local, now + std::chrono::hours{48}, now + std::chrono::hours{24});
      local_record.local_owned = true;
      forge::asio::blocking::run(runtime, store.async_upsert_provider(std::move(local_record), now));
      forge::asio::blocking::run(
          runtime, store.async_upsert_provider(
                       test_provider(key, remote, now + std::chrono::hours{48}, now + std::chrono::hours{24}), now));
      BOOST_TEST(store.find_providers(key, 20, now).size() == 2U);
      // Simulate process loss: no graceful close/withdrawal owns this transition.
   }

   auto reopened = dht::record_store{test_profile(), dht::record_store::options{
                                                         .persistence = persistence,
                                                         .max_providers = 1,
                                                         .max_local_providers = 1,
                                                         .hydration_page_limit = 1,
                                                     }};
   forge::asio::blocking::run(runtime, reopened.async_hydrate(now));
   const auto providers = reopened.find_providers(key, 20, now);
   BOOST_REQUIRE_EQUAL(providers.size(), 1U);
   BOOST_TEST(providers.front().provider.to_string() == remote.to_string());
   BOOST_TEST(!providers.front().local_owned);

   const auto durable =
       forge::asio::blocking::run(runtime, persistence->async_hydrate(dht::record_store::hydration_request{
                                               .kind = dht::record_store::hydration_kind::providers,
                                               .limit = 20,
                                           }));
   BOOST_REQUIRE_EQUAL(durable.providers.size(), 1U);
   BOOST_TEST(durable.providers.front().provider.to_string() == remote.to_string());
}

BOOST_AUTO_TEST_CASE(dht_record_store_confirmed_hydration_prune_repairs_sticky_durability) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<forge::test::net::p2p::dht_record_store_persistence>();
   auto store = dht::record_store{test_profile(), dht::record_store::options{.persistence = persistence}};
   const auto now = std::chrono::system_clock::now();
   const auto key = test_key('u');

   persistence->make_next_apply_durability_uncertain();
   BOOST_CHECK_THROW(
       (forge::asio::blocking::run(runtime, store.async_put(test_value(key, 1, now + std::chrono::minutes{1}), now))),
       exceptions::durability_uncertain);
   BOOST_TEST(store.persistence_state().durability_uncertain);

   forge::asio::blocking::run(runtime, store.async_hydrate(now + std::chrono::minutes{2}));
   const auto status = store.persistence_state();
   BOOST_TEST(!status.degraded);
   BOOST_TEST(!status.durability_uncertain);
   BOOST_TEST(!store.find_value(key, now + std::chrono::minutes{2}));
}

BOOST_AUTO_TEST_CASE(dht_record_store_confirmed_hydration_prune_repairs_durability_before_capacity_failure) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<forge::test::net::p2p::dht_record_store_persistence>();
   auto store = dht::record_store{test_profile(), dht::record_store::options{
                                                      .persistence = persistence,
                                                      .max_values = 1,
                                                      .hydration_page_limit = 1,
                                                  }};
   const auto now = std::chrono::system_clock::now();

   persistence->make_next_apply_durability_uncertain();
   BOOST_CHECK_THROW((forge::asio::blocking::run(
                         runtime, store.async_put(test_value(test_key('u'), 1, now + std::chrono::minutes{1}), now))),
                     exceptions::durability_uncertain);
   BOOST_TEST(store.persistence_state().durability_uncertain);

   auto live = dht::record_store::mutation_batch{};
   live.value_upserts.push_back(test_value(test_key('v'), 2, now + std::chrono::hours{1}));
   live.value_upserts.push_back(test_value(test_key('w'), 3, now + std::chrono::hours{1}));
   forge::asio::blocking::run(runtime, persistence->async_apply(std::move(live)));

   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_hydrate(now + std::chrono::minutes{2}))),
                     exceptions::backpressure_rejected);
   const auto status = store.persistence_state();
   BOOST_TEST(status.degraded);
   BOOST_TEST(!status.durability_uncertain);
}

BOOST_AUTO_TEST_CASE(dht_record_store_hydration_reconciles_expired_rows_and_provider_addresses) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = dht::record_store::make_memory_persistence();
   const auto now = std::chrono::system_clock::now();
   const auto live_key = test_key('l');
   const auto expired_key = test_key('x');
   const auto expired_provider_key = test_key('e');
   const auto address_expired_key = test_key('a');
   const auto expired_provider = test_peer(73);
   const auto address_expired_provider = test_peer(74);

   auto batch = dht::record_store::mutation_batch{};
   batch.value_upserts.push_back(test_value(live_key, 1, now + std::chrono::hours{1}));
   batch.value_upserts.push_back(test_value(expired_key, 2, now - std::chrono::minutes{1}));
   batch.provider_upserts.push_back(test_provider(expired_provider_key, expired_provider, now - std::chrono::minutes{1},
                                                  now - std::chrono::minutes{2}));
   batch.provider_upserts.push_back(test_provider(address_expired_key, address_expired_provider,
                                                  now + std::chrono::hours{1}, now - std::chrono::minutes{1}));
   forge::asio::blocking::run(runtime, persistence->async_apply(std::move(batch)));

   auto store = dht::record_store{test_profile(), dht::record_store::options{.persistence = persistence}};
   forge::asio::blocking::run(runtime, store.async_hydrate(now));

   BOOST_REQUIRE(store.find_value(live_key, now));
   BOOST_TEST(!store.find_value(expired_key, now));
   BOOST_TEST(store.find_providers(expired_provider_key, 1, now).empty());
   const auto operational = store.find_providers(address_expired_key, 1, now);
   BOOST_REQUIRE_EQUAL(operational.size(), 1U);
   BOOST_TEST(operational.front().endpoints.empty());

   const auto durable_values =
       forge::asio::blocking::run(runtime, persistence->async_hydrate(dht::record_store::hydration_request{
                                               .kind = dht::record_store::hydration_kind::values,
                                               .limit = 20,
                                           }));
   BOOST_REQUIRE_EQUAL(durable_values.values.size(), 1U);
   BOOST_CHECK(durable_values.values.front().record.key_value == live_key);

   const auto durable_providers =
       forge::asio::blocking::run(runtime, persistence->async_hydrate(dht::record_store::hydration_request{
                                               .kind = dht::record_store::hydration_kind::providers,
                                               .limit = 20,
                                           }));
   BOOST_REQUIRE_EQUAL(durable_providers.providers.size(), 1U);
   BOOST_CHECK(durable_providers.providers.front().provider == address_expired_provider);
   BOOST_TEST(durable_providers.providers.front().endpoints.empty());
   BOOST_CHECK(durable_providers.providers.front().addresses_expires_at == std::chrono::system_clock::time_point{});

   const auto pruned = forge::asio::blocking::run(runtime, store.async_prune_expired(now));
   BOOST_TEST(pruned.values.empty());
   BOOST_TEST(pruned.providers.empty());
   BOOST_TEST(pruned.provider_address_updates.empty());
   BOOST_TEST(!store.persistence_state().degraded);
   BOOST_TEST(!store.persistence_state().durability_uncertain);
}

BOOST_AUTO_TEST_CASE(dht_record_store_marks_oversized_committed_prune_result_for_reconciliation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<forge::test::net::p2p::dht_record_store_persistence>();
   auto store =
       dht::record_store{test_profile(), dht::record_store::options{.persistence = persistence, .prune_page_limit = 1}};
   const auto now = std::chrono::system_clock::now();

   auto oversized = dht::record_store::prune_result{};
   oversized.values.push_back(test_key('1'));
   oversized.values.push_back(test_key('2'));
   persistence->return_next_prune_result(std::move(oversized));

   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_prune_expired(now))), exceptions::internal);
   auto status = store.persistence_state();
   BOOST_TEST(status.degraded);
   BOOST_TEST(status.durability_uncertain);
   BOOST_TEST(status.last_failure.find("invalid committed prune result") != std::string::npos);

   forge::asio::blocking::run(runtime, store.async_flush());
   status = store.persistence_state();
   BOOST_TEST(status.degraded);
   BOOST_TEST(status.durability_uncertain);

   forge::asio::blocking::run(runtime, store.async_hydrate(now));
   status = store.persistence_state();
   BOOST_TEST(status.degraded);
   BOOST_TEST(status.durability_uncertain);

   forge::asio::blocking::run(runtime, store.async_flush());
   status = store.persistence_state();
   BOOST_TEST(!status.degraded);
   BOOST_TEST(!status.durability_uncertain);
}

BOOST_AUTO_TEST_CASE(dht_record_store_prunes_expiry_in_bounded_steps_and_closes_idempotently) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto store = dht::record_store{test_profile(), dht::record_store::options{.prune_page_limit = 1}};
   const auto now = std::chrono::system_clock::now();
   const auto value_key = test_key('v');
   const auto provider_key = test_key('w');
   forge::asio::blocking::run(runtime, store.async_put(test_value(value_key, 1, now + std::chrono::minutes{1}), now));
   forge::asio::blocking::run(
       runtime, store.async_upsert_provider(test_provider(provider_key, test_peer(6), now + std::chrono::minutes{2},
                                                          now + std::chrono::minutes{1}),
                                            now));

   const auto values = forge::asio::blocking::run(runtime, store.async_prune_expired(now + std::chrono::minutes{1}));
   BOOST_REQUIRE_EQUAL(values.values.size(), 1U);
   BOOST_TEST(values.may_have_more);
   BOOST_TEST(values.durability.durability_confirmed);
   BOOST_TEST(!store.find_value(value_key, now + std::chrono::minutes{1}));
   const auto addresses = forge::asio::blocking::run(runtime, store.async_prune_expired(now + std::chrono::minutes{1}));
   BOOST_REQUIRE_EQUAL(addresses.provider_address_updates.size(), 1U);
   BOOST_TEST(!addresses.may_have_more);
   BOOST_REQUIRE_EQUAL(store.find_providers(provider_key, 1, now + std::chrono::minutes{1}).size(), 1U);
   BOOST_TEST(store.find_providers(provider_key, 1, now + std::chrono::minutes{1}).front().endpoints.empty());
   const auto providers = forge::asio::blocking::run(runtime, store.async_prune_expired(now + std::chrono::minutes{2}));
   BOOST_REQUIRE_EQUAL(providers.providers.size(), 1U);
   BOOST_TEST(store.find_providers(provider_key, 1, now + std::chrono::minutes{2}).empty());

   forge::asio::blocking::run(runtime, store.async_close());
   forge::asio::blocking::run(runtime, store.async_close());
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_flush())), exceptions::closed);
}

BOOST_AUTO_TEST_CASE(dht_record_store_prunes_fully_expired_provider_without_address_update) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto store = dht::record_store{test_profile()};
   const auto now = std::chrono::system_clock::now();
   const auto key = test_key('j');
   const auto provider = test_peer(12);
   forge::asio::blocking::run(
       runtime, store.async_upsert_provider(
                    test_provider(key, provider, now + std::chrono::minutes{2}, now + std::chrono::minutes{1}), now));

   const auto pruned = forge::asio::blocking::run(runtime, store.async_prune_expired(now + std::chrono::minutes{2}));
   BOOST_REQUIRE_EQUAL(pruned.providers.size(), 1U);
   BOOST_CHECK(pruned.providers.front().key == key);
   BOOST_CHECK(pruned.providers.front().provider == provider);
   BOOST_TEST(pruned.provider_address_updates.empty());
   BOOST_TEST(!pruned.may_have_more);
   BOOST_TEST(store.find_providers(key, 1, now + std::chrono::minutes{2}).empty());
   BOOST_TEST(!store.persistence_state().degraded);
}

BOOST_AUTO_TEST_CASE(dht_record_store_close_retries_after_flush_failure) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<forge::test::net::p2p::dht_record_store_persistence>();
   auto store = dht::record_store{test_profile(), {.persistence = persistence}};

   persistence->fail_next_flush();
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_close())), exceptions::durability_uncertain);
   BOOST_TEST(store.persistence_state().closing);
   BOOST_TEST(!store.persistence_state().closed);

   forge::asio::blocking::run(runtime, store.async_close());
   BOOST_TEST(store.persistence_state().closed);
}

BOOST_AUTO_TEST_CASE(dht_record_store_close_retries_after_backend_close_failure) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<forge::test::net::p2p::dht_record_store_persistence>();
   auto store = dht::record_store{test_profile(), {.persistence = persistence}};

   persistence->fail_next_close();
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_close())), std::runtime_error);
   BOOST_TEST(store.persistence_state().closing);
   BOOST_TEST(!store.persistence_state().closed);

   forge::asio::blocking::run(runtime, store.async_close());
   BOOST_TEST(store.persistence_state().closed);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace forge::net::p2p
