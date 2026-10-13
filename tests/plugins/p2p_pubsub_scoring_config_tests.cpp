module;

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/test/unit_test.hpp>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.asio.runtime;
import forge.codec.yaml;
import forge.config.core.component;
import forge.config.core.document;
import forge.config.core.value;
import forge.net.p2p.node;
import forge.net.p2p.endpoint;
import forge.net.p2p.peer_store;
import forge.net.p2p.pubsub;
import forge.net.p2p.scoring;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.exceptions;

#include "../../plugins/net/p2p/node/details/configuration_policy.hxx"
#include "../../plugins/net/p2p/node/details/pubsub_policy.hxx"

namespace {
namespace np = forge::plugins::net::p2p::node;
namespace pubsub = forge::net::p2p::pubsub;
using patch = np::pubsub_config::scoring_config;
using topic_patch = patch::topic_config;
using thresholds = patch::thresholds_config;
using limits = patch::limits_config;
using value = forge::config::core::value;
using namespace std::chrono_literals;
constexpr std::string_view section = "plugins.net.p2p.node";

template<class Config, class Native> struct double_field {
   const char* name;
   std::optional<double> Config::* configured;
   double Native::* native;
   double sample;
};
using topic_field = double_field<topic_patch, pubsub::topic_score_params>;
constexpr auto topic_fields = std::array{
   topic_field{"topic-weight", &topic_patch::topic_weight, &pubsub::topic_score_params::topic_weight, 2.},
   topic_field{"time-in-mesh-weight", &topic_patch::time_in_mesh_weight, &pubsub::topic_score_params::time_in_mesh_weight, .25},
   topic_field{"time-in-mesh-cap", &topic_patch::time_in_mesh_cap, &pubsub::topic_score_params::time_in_mesh_cap, 9.},
   topic_field{"first-message-deliveries-weight", &topic_patch::first_message_deliveries_weight,
               &pubsub::topic_score_params::first_message_deliveries_weight, 1.5},
   topic_field{"first-message-deliveries-decay", &topic_patch::first_message_deliveries_decay,
               &pubsub::topic_score_params::first_message_deliveries_decay, .8},
   topic_field{"first-message-deliveries-cap", &topic_patch::first_message_deliveries_cap,
               &pubsub::topic_score_params::first_message_deliveries_cap, 9.},
   topic_field{"mesh-message-deliveries-weight", &topic_patch::mesh_message_deliveries_weight,
               &pubsub::topic_score_params::mesh_message_deliveries_weight, -.5},
   topic_field{"mesh-message-deliveries-decay", &topic_patch::mesh_message_deliveries_decay,
               &pubsub::topic_score_params::mesh_message_deliveries_decay, .8},
   topic_field{"mesh-message-deliveries-cap", &topic_patch::mesh_message_deliveries_cap,
               &pubsub::topic_score_params::mesh_message_deliveries_cap, 9.},
   topic_field{"mesh-message-deliveries-threshold", &topic_patch::mesh_message_deliveries_threshold,
               &pubsub::topic_score_params::mesh_message_deliveries_threshold, 4.},
   topic_field{"mesh-failure-penalty-weight", &topic_patch::mesh_failure_penalty_weight,
               &pubsub::topic_score_params::mesh_failure_penalty_weight, -.5},
   topic_field{"mesh-failure-penalty-decay", &topic_patch::mesh_failure_penalty_decay,
               &pubsub::topic_score_params::mesh_failure_penalty_decay, .8},
   topic_field{"invalid-message-deliveries-weight", &topic_patch::invalid_message_deliveries_weight,
               &pubsub::topic_score_params::invalid_message_deliveries_weight, -.5},
   topic_field{"invalid-message-deliveries-decay", &topic_patch::invalid_message_deliveries_decay,
               &pubsub::topic_score_params::invalid_message_deliveries_decay, .8},
};
using global_field = double_field<patch, pubsub::scoring_params>;
constexpr auto global_fields = std::array{
   global_field{"topic-score-cap", &patch::topic_score_cap, &pubsub::scoring_params::topic_score_cap, 8.},
   global_field{"ip-colocation-factor-weight", &patch::ip_colocation_factor_weight,
                &pubsub::scoring_params::ip_colocation_factor_weight, -2.},
   global_field{"ip-colocation-factor-threshold", &patch::ip_colocation_factor_threshold,
                &pubsub::scoring_params::ip_colocation_factor_threshold, 4.},
   global_field{"behaviour-penalty-weight", &patch::behaviour_penalty_weight,
                &pubsub::scoring_params::behaviour_penalty_weight, -1.},
   global_field{"behaviour-penalty-threshold", &patch::behaviour_penalty_threshold,
                &pubsub::scoring_params::behaviour_penalty_threshold, 3.},
   global_field{"behaviour-penalty-decay", &patch::behaviour_penalty_decay,
                &pubsub::scoring_params::behaviour_penalty_decay, .8},
   global_field{"decay-to-zero", &patch::decay_to_zero, &pubsub::scoring_params::decay_to_zero, .02},
};
using threshold_field = double_field<thresholds, pubsub::score_thresholds>;
constexpr auto threshold_fields = std::array{
   threshold_field{"gossip-threshold", &thresholds::gossip_threshold, &pubsub::score_thresholds::gossip_threshold, -5.},
   threshold_field{"publish-threshold", &thresholds::publish_threshold, &pubsub::score_thresholds::publish_threshold, -20.},
   threshold_field{"graylist-threshold", &thresholds::graylist_threshold, &pubsub::score_thresholds::graylist_threshold, -90.},
   threshold_field{"accept-px-threshold", &thresholds::accept_px_threshold, &pubsub::score_thresholds::accept_px_threshold, 2.},
   threshold_field{"opportunistic-graft-threshold", &thresholds::opportunistic_graft_threshold,
                   &pubsub::score_thresholds::opportunistic_graft_threshold, 3.},
};

template<class Config, class Native> struct time_field {
   const char* name;
   std::optional<std::uint64_t> Config::* configured;
   std::chrono::milliseconds Native::* native;
   std::uint64_t sample;
};
constexpr auto topic_times = std::array{
   time_field<topic_patch, pubsub::topic_score_params>{"time-in-mesh-quantum-ms", &topic_patch::time_in_mesh_quantum_ms,
       &pubsub::topic_score_params::time_in_mesh_quantum, 500},
   time_field<topic_patch, pubsub::topic_score_params>{"mesh-message-deliveries-window-ms",
       &topic_patch::mesh_message_deliveries_window_ms, &pubsub::topic_score_params::mesh_message_deliveries_window, 0},
   time_field<topic_patch, pubsub::topic_score_params>{"mesh-message-deliveries-activation-ms",
       &topic_patch::mesh_message_deliveries_activation_ms, &pubsub::topic_score_params::mesh_message_deliveries_activation, 2000},
};
constexpr auto global_times = std::array{
   time_field<patch, pubsub::scoring_params>{"decay-interval-ms", &patch::decay_interval_ms,
       &pubsub::scoring_params::decay_interval, 2000},
   time_field<patch, pubsub::scoring_params>{"retain-score-ms", &patch::retain_score_ms,
       &pubsub::scoring_params::retain_score, 0},
   time_field<patch, pubsub::scoring_params>{"seen-message-ttl-ms", &patch::seen_message_ttl_ms,
       &pubsub::scoring_params::seen_message_ttl, 4000},
};

struct limit_field {
   const char* name;
   std::optional<std::uint64_t> limits::* configured;
   std::size_t pubsub::score_limits::* native;
   std::uint64_t sample;
};
constexpr auto limit_fields = std::array{
   limit_field{"max-connected-peers", &limits::max_connected_peers, &pubsub::score_limits::max_connected_peers, 7},
   limit_field{"max-retained-peers", &limits::max_retained_peers, &pubsub::score_limits::max_retained_peers, 2048},
   limit_field{"max-messages", &limits::max_messages, &pubsub::score_limits::max_messages, 512},
   limit_field{"max-delivery-peers", &limits::max_delivery_peers, &pubsub::score_limits::max_delivery_peers, 17},
};

patch enabled() { return {.enabled = true, .topics = {{.topic = "configured"}}}; }

pubsub::scoring_params mapped(patch settings = enabled()) {
   auto configured = np::pubsub_config{};
   configured.scoring = std::move(settings);
   np::detail::validate_pubsub_config(configured);
   auto options = np::detail::pubsub_options_for(configured);
   BOOST_REQUIRE(options.scoring);
   return std::move(*options.scoring);
}

const pubsub::topic_score_params& topic(const pubsub::scoring_params& params) {
   return params.topics.at(pubsub::topic{"configured"});
}

forge::config::core::document document_with(value::object_type scoring = {}) {
   auto document = forge::config::core::document{};
   document.set(std::string{section} + ".peer-store.store", "unbound-store");
   document.set(std::string{section} + ".identity.certificate-secret", "unbound/cert");
   document.set(std::string{section} + ".identity.private-key-secret", "unbound/key");
   document.set(std::string{section} + ".pubsub.scoring", std::move(scoring));
   return document;
}

template<class T> T run(forge::asio::runtime& runtime, boost::asio::awaitable<T> operation) {
   auto future = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
   if (future.wait_for(5s) != std::future_status::ready) { std::terminate(); }
   return future.get();
}

void configure(forge::asio::runtime& runtime, np::plugin& plugin,
               const forge::config::core::document& document) {
   run(runtime, plugin.configure({document, std::string{section}}));
}
} // namespace

BOOST_AUTO_TEST_CASE(p2p_pubsub_scoring_config_opt_in_and_native_defaults) {
   auto configured = np::pubsub_config{};
   BOOST_CHECK(!np::detail::pubsub_options_for(configured).scoring);
   configured.scoring = np::pubsub_config::scoring_config{};
   BOOST_CHECK(!np::detail::pubsub_options_for(configured).scoring);
   const auto actual = mapped();
   const auto defaults = pubsub::scoring_params{};
   const auto topic_defaults = pubsub::topic_score_params{};
   for (const auto& field : topic_fields) { BOOST_TEST((topic(actual).*(field.native)) == (topic_defaults.*(field.native))); }
   for (const auto& field : topic_times) { BOOST_CHECK((topic(actual).*(field.native)) == (topic_defaults.*(field.native))); }
   for (const auto& field : global_fields) { BOOST_TEST((actual.*(field.native)) == (defaults.*(field.native))); }
   for (const auto& field : global_times) { BOOST_CHECK((actual.*(field.native)) == (defaults.*(field.native))); }
   for (const auto& field : threshold_fields) {
      BOOST_TEST((actual.thresholds.*(field.native)) == (defaults.thresholds.*(field.native)));
   }
   for (const auto& field : limit_fields) { BOOST_TEST((actual.limits.*(field.native)) == (defaults.limits.*(field.native))); }
   BOOST_TEST(actual.app_specific_weight == 0.);
   BOOST_CHECK(!actual.app_specific_score);
   BOOST_TEST(actual.ip_colocation_factor_allowlist.empty());
   BOOST_TEST(actual.limits.max_message_id_size == defaults.limits.max_message_id_size);
   BOOST_TEST(actual.limits.max_peer_id_size == defaults.limits.max_peer_id_size);
   BOOST_TEST(actual.limits.max_topics == defaults.limits.max_topics);
   BOOST_TEST(actual.limits.max_topic_size == defaults.limits.max_topic_size);
   BOOST_TEST(actual.limits.max_ips_per_peer == defaults.limits.max_ips_per_peer);
   BOOST_TEST(actual.limits.max_ip_allowlist == defaults.limits.max_ip_allowlist);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_scoring_config_all_double_fields_map_remove_and_reject_nonfinite) {
   const auto defaults = mapped();
   const auto nonfinite = std::array{(std::numeric_limits<double>::quiet_NaN)(),
                                    (std::numeric_limits<double>::infinity)()};
   for (const auto& field : topic_fields) {
      BOOST_TEST_CONTEXT(field.name) {
         auto settings = enabled();
         settings.topics.front().*(field.configured) = field.sample;
         BOOST_TEST((topic(mapped(settings)).*(field.native)) == field.sample);
         settings.topics.front().*(field.configured) = std::nullopt;
         BOOST_TEST((topic(mapped(settings)).*(field.native)) == (topic(defaults).*(field.native)));
         for (const auto invalid : nonfinite) {
            settings.topics.front().*(field.configured) = invalid;
            BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config);
         }
      }
   }
   for (const auto& field : global_fields) {
      auto settings = enabled();
      settings.*(field.configured) = field.sample;
      BOOST_TEST((mapped(settings).*(field.native)) == field.sample);
      settings.*(field.configured) = std::nullopt;
      BOOST_TEST((mapped(settings).*(field.native)) == (defaults.*(field.native)));
      for (const auto invalid : nonfinite) {
         settings.*(field.configured) = invalid;
         BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config);
      }
   }
   for (const auto& field : threshold_fields) {
      auto settings = enabled();
      settings.thresholds.*(field.configured) = field.sample;
      BOOST_TEST((mapped(settings).thresholds.*(field.native)) == field.sample);
      settings.thresholds.*(field.configured) = std::nullopt;
      BOOST_TEST((mapped(settings).thresholds.*(field.native)) == (defaults.thresholds.*(field.native)));
      for (const auto invalid : nonfinite) {
         settings.thresholds.*(field.configured) = invalid;
         BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config);
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_scoring_config_all_durations_limits_and_native_relationships) {
   const auto defaults = mapped();
   for (const auto& field : topic_times) {
      auto settings = enabled();
      settings.topics.front().*(field.configured) = field.sample;
      BOOST_TEST((topic(mapped(settings)).*(field.native)).count() == field.sample);
      settings.topics.front().*(field.configured) = std::nullopt;
      BOOST_CHECK((topic(mapped(settings)).*(field.native)) == (topic(defaults).*(field.native)));
      settings.topics.front().*(field.configured) = (std::numeric_limits<std::uint64_t>::max)();
      BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config);
      settings.topics.front().*(field.configured) = std::uint64_t{10'000'000'000'000};
      BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config);
   }
   for (const auto& field : global_times) {
      auto settings = enabled();
      settings.*(field.configured) = field.sample;
      BOOST_TEST((mapped(settings).*(field.native)).count() == field.sample);
      settings.*(field.configured) = std::nullopt;
      BOOST_CHECK((mapped(settings).*(field.native)) == (defaults.*(field.native)));
      settings.*(field.configured) = (std::numeric_limits<std::uint64_t>::max)();
      BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config);
      settings.*(field.configured) = std::uint64_t{10'000'000'000'000};
      BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config);
   }
   for (const auto& field : limit_fields) {
      auto settings = enabled();
      settings.limits.*(field.configured) = field.sample;
      BOOST_TEST((mapped(settings).limits.*(field.native)) == field.sample);
      settings.limits.*(field.configured) = std::nullopt;
      BOOST_TEST((mapped(settings).limits.*(field.native)) == (defaults.limits.*(field.native)));
      settings.limits.*(field.configured) = 0;
      BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config);
   }
   auto maximum = enabled();
   for (const auto& field : limit_fields) { maximum.limits.*(field.configured) = (std::numeric_limits<std::uint64_t>::max)(); }
   if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
      BOOST_CHECK_THROW(mapped(maximum), np::exceptions::invalid_config);
   } else { BOOST_CHECK_NO_THROW(mapped(maximum)); } // Mapping only, not allocation at these sizes.
   auto bad = enabled();
   bad.limits.max_connected_peers = 8;
   bad.limits.max_retained_peers = 7;
   BOOST_CHECK_THROW(mapped(bad), np::exceptions::invalid_config);
   bad = enabled(); bad.thresholds.publish_threshold = -5.;
   BOOST_CHECK_THROW(mapped(bad), np::exceptions::invalid_config);
   bad = enabled(); bad.thresholds.graylist_threshold = -20.;
   BOOST_CHECK_THROW(mapped(bad), np::exceptions::invalid_config);
   bad = enabled(); bad.topics.front().first_message_deliveries_weight = 1.;
   bad.topics.front().first_message_deliveries_cap = 0.;
   BOOST_CHECK_THROW(mapped(bad), np::exceptions::invalid_config);
   bad = enabled(); bad.topics.front().mesh_message_deliveries_weight = -1.;
   bad.topics.front().mesh_message_deliveries_activation_ms = 999;
   BOOST_CHECK_THROW(mapped(bad), np::exceptions::invalid_config);
   bad = enabled(); bad.behaviour_penalty_decay = 1.;
   BOOST_CHECK_THROW(mapped(bad), np::exceptions::invalid_config);
   bad = enabled(); bad.decay_interval_ms = 999;
   BOOST_CHECK_THROW(mapped(bad), np::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_scoring_config_topic_identity_numeric_ips_and_disabled_validation) {
   auto settings = enabled();
   settings.ip_colocation_factor_allowlist = std::vector<std::string>{"127.0.0.1", "::1"};
   const auto actual = mapped(settings);
   BOOST_REQUIRE(actual.ip_colocation_factor_allowlist.size() == 2U);
   BOOST_TEST(actual.ip_colocation_factor_allowlist[0].is_v4());
   BOOST_TEST(actual.ip_colocation_factor_allowlist[1].is_v6());
   settings.ip_colocation_factor_allowlist.reset();
   BOOST_TEST(mapped(settings).ip_colocation_factor_allowlist.empty());
   for (const auto* bad : {"", "localhost", "127.0.0.999"}) {
      settings.ip_colocation_factor_allowlist = std::vector<std::string>{bad};
      BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config);
   }
   settings = enabled(); settings.topics.push_back(settings.topics.front());
   BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config);
   settings = enabled(); settings.topics.front().topic.clear();
   BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config);
   settings = enabled(); settings.topics.front().topic = std::string(256, 'x');
   BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config);
   settings = enabled();
   settings.enabled = false;
   settings.topic_score_cap = -1.;
   auto config = np::pubsub_config{};
   config.scoring = settings;
   BOOST_CHECK_THROW((void)np::detail::pubsub_options_for(config), np::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_scoring_config_real_yaml_strict_callbacks_and_types) {
   const auto parsed = forge::codec::yaml::read_document(
       "scoring:\n  enabled: true\n  topics:\n    - topic: configured\n      topic-weight: 2.0\n"
       "      first-message-deliveries-weight: 1.5\n      time-in-mesh-quantum-ms: 500\n"
       "  topic-score-cap: 8.0\n  ip-colocation-factor-allowlist: ['127.0.0.1', '::1']\n"
       "  thresholds: {gossip-threshold: -5.0, publish-threshold: -20.0, graylist-threshold: -90.0}\n"
       "  limits: {max-connected-peers: 7, max-retained-peers: 8, max-messages: 512, max-delivery-peers: 17}\n");
   BOOST_REQUIRE(parsed.ok());
   const auto* configured = parsed.value.try_get("scoring");
   BOOST_REQUIRE(configured);
   auto document = document_with();
   document.set(std::string{section} + ".pubsub.scoring", *configured);
   const auto decoded = np::decode_config({document, std::string{section}}).pubsub;
   BOOST_REQUIRE(decoded.scoring);
   const auto actual = mapped(*decoded.scoring);
   BOOST_TEST(topic(actual).topic_weight == 2.);
   BOOST_TEST(topic(actual).first_message_deliveries_weight == 1.5);
   BOOST_TEST(topic(actual).time_in_mesh_quantum.count() == 500);
   BOOST_TEST(actual.limits.max_connected_peers == 7U);
   BOOST_TEST(actual.limits.max_retained_peers == 8U);
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
   for (const auto* key : {"app-specific-score", "app-specific-weight", "thresholds.graylist",
                           "limits.max-topics", "limits.max-message-id-size", "retain-score-seconds"}) {
      auto bad = document;
      bad.set(std::string{section} + ".pubsub.scoring." + key, std::uint64_t{7});
      BOOST_CHECK_THROW(configure(runtime, plugin, bad), np::exceptions::invalid_config);
   }
   for (const auto& field : limit_fields) {
      for (const auto& invalid : {value{std::int64_t{-1}}, value{1.5}, value{true}}) {
         auto bad = document;
         bad.set(std::string{section} + ".pubsub.scoring.limits." + field.name, invalid);
         BOOST_CHECK_THROW(configure(runtime, plugin, bad), np::exceptions::invalid_config);
      }
   }
   auto bad = document;
   bad.set(std::string{section} + ".pubsub.scoring.topics", value::object_type{{"configured", value::object_type{}}});
   BOOST_CHECK_THROW(configure(runtime, plugin, bad), np::exceptions::invalid_config);
   bad.set(std::string{section} + ".pubsub.scoring.topics", value::array_type{
       value::object_type{{"topic", "configured"}, {"app-specific-weight", 2.}}});
   BOOST_CHECK_THROW(configure(runtime, plugin, bad), np::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_scoring_config_frozen_after_stop) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   const auto accepted = document_with({{"enabled", true}});
   configure(runtime, plugin, accepted);
   plugin.request_stop();
   for (const auto& [key, setting] : std::array{
            std::pair{"enabled", value{false}}, std::pair{"topic-score-cap", value{8.}},
            std::pair{"thresholds.gossip-threshold", value{-5.}},
            std::pair{"limits.max-connected-peers", value{std::uint64_t{7}}},
            std::pair{"topics", value{value::array_type{value::object_type{{"topic", "configured"}}}}}}) {
      auto candidate = accepted;
      candidate.set(std::string{section} + ".pubsub.scoring." + key, setting);
      candidate.set(std::string{section} + ".diagnostics.max-peers", std::uint64_t{2});
      BOOST_CHECK_THROW(configure(runtime, plugin, candidate), np::exceptions::route_conflict);
   }
   run(runtime, plugin.shutdown());
}
