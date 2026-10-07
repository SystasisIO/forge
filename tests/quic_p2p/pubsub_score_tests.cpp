module;

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <latch>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/ip/address.hpp>

module forge.net.p2p.node;

import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.pubsub;

#include "../../libraries/net/p2p/details/pubsub_peer_score.hxx"

namespace {

namespace p2p = forge::net::p2p;
namespace ps = p2p::pubsub;
using engine = p2p::detail::pubsub_peer_score;
using namespace std::chrono_literals;
constexpr auto start = engine::clock::time_point{} + 1h;
const auto subject = ps::topic{"blocks"};

p2p::peer_id peer(std::uint8_t value) {
   return p2p::make_peer_id({.type = p2p::public_key::type::ed25519, .data = std::vector<std::uint8_t>(32, value)});
}

ps::scoring_params params() {
   auto result = ps::scoring_params{};
   result.topics.emplace(subject, ps::topic_score_params{});
   return result;
}

std::array<std::uint8_t, 2> id(std::uint8_t value) {
   return {0, value}; // Protocol-neutral bytes, including NUL; not a text/sequence-number assumption.
}

ps::peer_score_snapshot inspect(engine& scores, std::uint8_t value, engine::clock::time_point now = start) {
   auto result = scores.inspect(peer(value), now);
   BOOST_REQUIRE(result);
   BOOST_REQUIRE_EQUAL(result->topics.size(), 1U);
   return std::move(*result);
}

engine::validation validation(engine& scores, std::uint8_t source, std::uint8_t message,
                              engine::clock::time_point now = start) {
   auto result = scores.validation_start(peer(source), subject, id(message), now);
   BOOST_REQUIRE(result);
   return std::move(*result);
}

void accept(engine& scores, std::uint8_t source, std::uint8_t message, engine::clock::time_point now = start) {
   const auto ticket = validation(scores, source, message, now);
   BOOST_REQUIRE(scores.validation_complete(ticket, ps::validation_result::accept, now));
}

} // namespace

BOOST_AUTO_TEST_SUITE(pubsub_score_tests)

BOOST_AUTO_TEST_CASE(scoring_is_opt_in_and_enabled_config_is_fully_validated) {
   auto options = ps::options{};
   BOOST_TEST(!options.scoring.has_value());
   BOOST_CHECK_NO_THROW(ps::validate(options));
   BOOST_CHECK_NO_THROW(static_cast<void>(ps::codec::encode(ps::rpc{}, options)));
   options.scoring = params();
   BOOST_CHECK_NO_THROW(ps::validate(*options.scoring));
   options.scoring->topics.at(subject).invalid_message_deliveries_weight = 1.0;
   BOOST_CHECK_THROW(ps::validate(options), p2p::exceptions::invalid_options);
   options.scoring.reset();
   BOOST_CHECK_NO_THROW(ps::validate(options));
   BOOST_CHECK_NO_THROW(static_cast<void>(ps::codec::encode(ps::rpc{}, options)));
   // Existing diagnostics keep their independent, unmodified cumulative counter meanings.
   const auto counters = ps::score{.value = 2.0, .invalid_messages = 4, .duplicate_messages = 5, .delivered_messages = 6};
   BOOST_TEST(counters.invalid_messages == 4U);
   BOOST_TEST(counters.duplicate_messages == 5U);
   BOOST_TEST(counters.delivered_messages == 6U);
}

BOOST_AUTO_TEST_CASE(rejects_nonfinite_values_in_every_score_parameter_and_threshold) {
   const auto topic_fields = std::array{
       &ps::topic_score_params::topic_weight, &ps::topic_score_params::time_in_mesh_weight,
       &ps::topic_score_params::time_in_mesh_cap, &ps::topic_score_params::first_message_deliveries_weight,
       &ps::topic_score_params::first_message_deliveries_decay, &ps::topic_score_params::first_message_deliveries_cap,
       &ps::topic_score_params::mesh_message_deliveries_weight, &ps::topic_score_params::mesh_message_deliveries_decay,
       &ps::topic_score_params::mesh_message_deliveries_cap, &ps::topic_score_params::mesh_message_deliveries_threshold,
       &ps::topic_score_params::mesh_failure_penalty_weight, &ps::topic_score_params::mesh_failure_penalty_decay,
       &ps::topic_score_params::invalid_message_deliveries_weight, &ps::topic_score_params::invalid_message_deliveries_decay};
   const auto peer_fields = std::array{
       &ps::scoring_params::topic_score_cap, &ps::scoring_params::app_specific_weight,
       &ps::scoring_params::ip_colocation_factor_weight, &ps::scoring_params::ip_colocation_factor_threshold,
       &ps::scoring_params::behaviour_penalty_weight, &ps::scoring_params::behaviour_penalty_threshold,
       &ps::scoring_params::behaviour_penalty_decay, &ps::scoring_params::decay_to_zero};
   const auto threshold_fields = std::array{
       &ps::score_thresholds::gossip_threshold, &ps::score_thresholds::publish_threshold,
       &ps::score_thresholds::graylist_threshold, &ps::score_thresholds::accept_px_threshold,
       &ps::score_thresholds::opportunistic_graft_threshold};
   for (const auto bad : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                          -std::numeric_limits<double>::infinity()}) {
      for (const auto field : topic_fields) {
         auto config = params();
         config.topics.at(subject).*field = bad;
         BOOST_CHECK_THROW(ps::validate(config), p2p::exceptions::invalid_options);
      }
      for (const auto field : peer_fields) {
         auto config = params();
         config.*field = bad;
         BOOST_CHECK_THROW(ps::validate(config), p2p::exceptions::invalid_options);
      }
      for (const auto field : threshold_fields) {
         auto config = params();
         config.thresholds.*field = bad;
         BOOST_CHECK_THROW(ps::validate(config), p2p::exceptions::invalid_options);
      }
   }
}

BOOST_AUTO_TEST_CASE(rejects_bad_signs_windows_decays_order_and_resource_limits) {
   const auto mutations = std::vector<std::function<void(ps::scoring_params&)>>{
       [](auto& p) { p.topics.at(subject).topic_weight = -1; },
       [](auto& p) { p.topics.at(subject).time_in_mesh_weight = -1; },
       [](auto& p) { p.topics.at(subject).first_message_deliveries_weight = -1; },
       [](auto& p) { p.topics.at(subject).mesh_message_deliveries_weight = 1; },
       [](auto& p) { p.topics.at(subject).mesh_failure_penalty_weight = 1; },
       [](auto& p) { p.topics.at(subject).invalid_message_deliveries_weight = 1; },
       [](auto& p) { p.topics.at(subject).time_in_mesh_quantum = 0ms; },
       [](auto& p) { p.topics.at(subject).time_in_mesh_weight = 1; p.topics.at(subject).time_in_mesh_cap = 0; },
       [](auto& p) { p.topics.at(subject).first_message_deliveries_cap = -1; },
       [](auto& p) { p.topics.at(subject).first_message_deliveries_decay = 1; },
       [](auto& p) { p.topics.at(subject).mesh_message_deliveries_decay = 0; },
       [](auto& p) { p.topics.at(subject).mesh_failure_penalty_decay = -1; },
       [](auto& p) { p.topics.at(subject).invalid_message_deliveries_decay = 1; },
       [](auto& p) { p.topics.at(subject).mesh_message_deliveries_window = -1ms; },
       [](auto& p) { p.topics.at(subject).mesh_message_deliveries_weight = -1;
                    p.topics.at(subject).mesh_message_deliveries_activation = 999ms; },
       [](auto& p) { p.topics.at(subject).mesh_message_deliveries_weight = -1;
                    p.topics.at(subject).mesh_message_deliveries_threshold = 0; },
       [](auto& p) { p.ip_colocation_factor_weight = 1; },
       [](auto& p) { p.ip_colocation_factor_threshold = 0; },
       [](auto& p) { p.behaviour_penalty_weight = 1; },
       [](auto& p) { p.behaviour_penalty_threshold = -1; },
       [](auto& p) { p.behaviour_penalty_decay = 0; },
       [](auto& p) { p.app_specific_weight = 1; },
       [](auto& p) { p.thresholds.gossip_threshold = 1; },
       [](auto& p) { p.thresholds.publish_threshold = 0; },
       [](auto& p) { p.thresholds.graylist_threshold = -1; },
       [](auto& p) { p.thresholds.accept_px_threshold = -1; },
       [](auto& p) { p.thresholds.opportunistic_graft_threshold = -1; },
       [](auto& p) { p.decay_interval = 999ms; },
       [](auto& p) { p.decay_to_zero = 0; },
       [](auto& p) { p.retain_score = -1ms; },
       [](auto& p) { p.seen_message_ttl = 0ms; },
       [](auto& p) { p.seen_message_ttl = std::chrono::milliseconds::max(); },
       [](auto& p) { p.limits.max_connected_peers = 0; },
       [](auto& p) { p.limits.max_retained_peers = 1; },
       [](auto& p) { p.limits.max_messages = 0; },
       [](auto& p) { p.limits.max_delivery_peers = 0; },
       [](auto& p) { p.limits.max_message_id_size = 0; },
       [](auto& p) { p.limits.max_peer_id_size = 0; },
       [](auto& p) { p.limits.max_topics = 0; },
       [](auto& p) { p.limits.max_topic_size = 1; },
       [](auto& p) { p.limits.max_ips_per_peer = 0; },
       [](auto& p) { p.limits.max_ip_allowlist = 0; },
   };
   for (const auto& mutate : mutations) {
      auto config = params();
      mutate(config);
      BOOST_CHECK_THROW(ps::validate(config), p2p::exceptions::invalid_options);
   }
}

BOOST_AUTO_TEST_CASE(p1_fractional_quantum_p2_caps_and_positive_topic_cap_do_not_cap_p4_or_p5) {
   auto config = params();
   auto& topic = config.topics.at(subject);
   topic.topic_weight = 2;
   topic.time_in_mesh_weight = 1;
   topic.time_in_mesh_cap = 2;
   topic.first_message_deliveries_weight = 3;
   topic.first_message_deliveries_cap = 2;
   topic.first_message_deliveries_decay = 0.5;
   topic.invalid_message_deliveries_weight = -2;
   config.topic_score_cap = 5;
   config.app_specific_score = [](const auto&) { return 4.0; };
   config.app_specific_weight = 2;
   auto scores = engine{config, start};
   BOOST_REQUIRE(scores.connect(peer(1), {}, start));
   BOOST_REQUIRE(scores.graft(peer(1), subject, start));
   BOOST_REQUIRE(scores.set_application_score(peer(1), config.app_specific_score(peer(1)), start));
   accept(scores, 1, 1);
   accept(scores, 1, 2);
   accept(scores, 1, 3);
   BOOST_TEST(inspect(scores, 1).topics.front().first_message_deliveries == 2.0);
   BOOST_TEST(scores.score(peer(1), start) == 13.0);
   const auto fractional = inspect(scores, 1, start + 1500ms);
   BOOST_TEST(fractional.topics.front().weighted_score == 9.0);
   BOOST_TEST(fractional.value == 13.0);
   const auto capped = inspect(scores, 1, start + 2500ms);
   BOOST_TEST(capped.topics.front().weighted_score == 7.0);
   BOOST_REQUIRE(scores.reject_invalid(peer(1), subject, start + 2500ms));
   BOOST_REQUIRE(scores.reject_invalid(peer(1), subject, start + 2500ms));
   BOOST_TEST(scores.score(peer(1), start + 2500ms) == -1.0);
}

BOOST_AUTO_TEST_CASE(p3_strict_activation_and_p3b_sticky_failure_survive_prune_disconnect_and_reconnect) {
   auto config = params();
   auto& topic = config.topics.at(subject);
   topic.mesh_message_deliveries_weight = -2;
   topic.mesh_message_deliveries_threshold = 3;
   topic.mesh_message_deliveries_activation = 1s;
   topic.mesh_failure_penalty_weight = -3;
   topic.mesh_failure_penalty_decay = 0.5;
   config.retain_score = 10s;
   auto scores = engine{config, start};
   BOOST_REQUIRE(scores.connect(peer(1), {}, start));
   BOOST_REQUIRE(scores.graft(peer(1), subject, start));
   BOOST_TEST(scores.score(peer(1), start + 1s) == 0.0);
   BOOST_TEST(scores.score(peer(1), start + 2s) == -18.0);
   BOOST_REQUIRE(scores.prune(peer(1), subject, start + 2s));
   BOOST_TEST(!scores.prune(peer(1), subject, start + 2s));
   BOOST_TEST(scores.score(peer(1), start + 2s) == -27.0);
   BOOST_REQUIRE(scores.graft(peer(1), subject, start + 2s));
   BOOST_TEST(scores.score(peer(1), start + 3s) == -13.5);
   BOOST_TEST(scores.score(peer(1), start + 4s) == -24.75);
   BOOST_REQUIRE(scores.disconnect(peer(1), start + 4s));
   const auto retained = inspect(scores, 1, start + 8s);
   BOOST_TEST(!retained.connected);
   BOOST_TEST(!retained.topics.front().mesh_deliveries_active);
   BOOST_TEST(retained.topics.front().mesh_failure_penalty == 11.25);
   BOOST_TEST(retained.value == -33.75);
   BOOST_REQUIRE(scores.connect(peer(1), {}, start + 8s));
   BOOST_TEST(scores.score(peer(1), start + 9s) == -16.875);
}

BOOST_AUTO_TEST_CASE(topic_weights_and_cap_apply_to_the_aggregate_not_individual_topics_or_negative_scores) {
   auto config = params();
   const auto other = ps::topic{"transactions"};
   config.topics.at(subject).first_message_deliveries_weight = 4;
   auto second = ps::topic_score_params{};
   second.topic_weight = 2;
   second.first_message_deliveries_weight = 4;
   second.invalid_message_deliveries_weight = -3;
   config.topics.emplace(other, second);
   config.topic_score_cap = 10;
   auto scores = engine{config, start};
   BOOST_REQUIRE(scores.connect(peer(1), {}, start));
   for (const auto message : {1, 2, 3}) {
      accept(scores, 1, message);
   }
   const auto ticket = scores.validation_start(peer(1), other, id(4), start);
   BOOST_REQUIRE(ticket);
   BOOST_REQUIRE(scores.validation_complete(*ticket, ps::validation_result::accept, start));
   BOOST_TEST(scores.score(peer(1), start) == 10.0);
   for (auto message = 0; message < 3; ++message) {
      BOOST_REQUIRE(scores.reject_invalid(peer(1), other, start));
   }
   BOOST_TEST(scores.score(peer(1), start) == -34.0);
   const auto receipt = scores.inspect(peer(1), start);
   BOOST_REQUIRE(receipt);
   BOOST_TEST(receipt->topics.size() == 2U);
   BOOST_TEST(receipt->topic_score == -34.0);
}

BOOST_AUTO_TEST_CASE(validation_duplicates_are_attributed_once_and_window_starts_at_validation_completion) {
   auto config = params();
   config.topics.at(subject).first_message_deliveries_weight = 1;
   config.topics.at(subject).mesh_message_deliveries_cap = 2;
   auto scores = engine{config, start};
   for (const auto p : {1, 2, 3, 4}) {
      BOOST_REQUIRE(scores.connect(peer(p), {}, start));
      BOOST_REQUIRE(scores.graft(peer(p), subject, start));
   }
   const auto ticket = validation(scores, 1, 1);
   BOOST_REQUIRE(scores.duplicate_delivery(peer(2), subject, id(1), start + 1s));
   BOOST_REQUIRE(scores.duplicate_delivery(peer(2), subject, id(1), start + 1s));
   BOOST_REQUIRE(scores.validation_complete(ticket, ps::validation_result::retry, start + 2s));
   BOOST_TEST(scores.snapshot(start + 2s).pending_validations == 1U);
   BOOST_REQUIRE(scores.validation_complete(ticket, ps::validation_result::accept, start + 10s));
   BOOST_TEST(!scores.validation_complete(ticket, ps::validation_result::reject, start + 10s));
   BOOST_REQUIRE(scores.duplicate_delivery(peer(3), subject, id(1), start + 10s + 10ms));
   BOOST_REQUIRE(scores.duplicate_delivery(peer(3), subject, id(1), start + 10s + 10ms));
   BOOST_REQUIRE(scores.duplicate_delivery(peer(4), subject, id(1), start + 10s + 11ms));
   BOOST_TEST(!scores.duplicate_delivery(peer(4), ps::topic{"foreign"}, id(1), start + 10s + 11ms));
   const auto first = inspect(scores, 1, start + 10s + 11ms).topics.front();
   const auto second = inspect(scores, 2, start + 10s + 11ms).topics.front();
   BOOST_TEST(first.first_message_deliveries == 1.0);
   BOOST_TEST(first.mesh_message_deliveries == 1.0);
   BOOST_TEST(second.first_message_deliveries == 0.0);
   BOOST_TEST(second.mesh_message_deliveries == 1.0);
   BOOST_TEST(inspect(scores, 3, start + 10s + 11ms).topics.front().mesh_message_deliveries == 1.0);
   BOOST_TEST(inspect(scores, 4, start + 10s + 11ms).topics.front().mesh_message_deliveries == 0.0);
   accept(scores, 1, 2, start + 10s + 11ms);
   accept(scores, 1, 3, start + 10s + 11ms);
   BOOST_TEST(inspect(scores, 1, start + 10s + 11ms).topics.front().mesh_message_deliveries == 2.0);
}

BOOST_AUTO_TEST_CASE(p4_penalizes_actual_invalid_senders_but_ignore_and_retry_have_no_invalid_penalty) {
   auto scores = engine{params(), start};
   for (const auto p : {1, 2, 3}) {
      BOOST_REQUIRE(scores.connect(peer(p), {}, start));
   }
   const auto bad = validation(scores, 1, 1);
   BOOST_REQUIRE(scores.duplicate_delivery(peer(2), subject, id(1), start));
   BOOST_REQUIRE(scores.duplicate_delivery(peer(2), subject, id(1), start));
   BOOST_REQUIRE(scores.validation_complete(bad, ps::validation_result::reject, start));
   BOOST_TEST(!scores.validation_complete(bad, ps::validation_result::reject, start));
   BOOST_REQUIRE(scores.duplicate_delivery(peer(2), subject, id(1), start));
   BOOST_TEST(scores.score(peer(1), start) == -1.0);
   BOOST_TEST(scores.score(peer(2), start) == -4.0);
   const auto ignored = validation(scores, 1, 2);
   BOOST_REQUIRE(scores.duplicate_delivery(peer(2), subject, id(2), start));
   BOOST_REQUIRE(scores.validation_complete(ignored, ps::validation_result::ignore, start));
   BOOST_REQUIRE(scores.duplicate_delivery(peer(3), subject, id(2), start));
   BOOST_TEST(!scores.validation_complete(ignored, ps::validation_result::reject, start));
   const auto retried = validation(scores, 1, 3);
   BOOST_REQUIRE(scores.duplicate_delivery(peer(2), subject, id(3), start));
   BOOST_REQUIRE(scores.validation_complete(retried, ps::validation_result::retry, start));
   BOOST_TEST(inspect(scores, 1).topics.front().invalid_message_deliveries == 1.0);
   BOOST_TEST(inspect(scores, 2).topics.front().invalid_message_deliveries == 2.0);
   BOOST_TEST(scores.score(peer(3), start) == 0.0);
   BOOST_REQUIRE(scores.validation_complete(retried, ps::validation_result::accept, start));
   BOOST_REQUIRE(scores.reject_invalid(peer(3), subject, start));
   BOOST_TEST(scores.score(peer(3), start) == -1.0);
}

BOOST_AUTO_TEST_CASE(pending_first_sender_duplicate_is_one_extra_p4_on_reject_without_an_extra_peer_slot) {
   auto config = params();
   config.limits.max_delivery_peers = 1;
   auto scores = engine{config, start};
   BOOST_REQUIRE(scores.connect(peer(1), {}, start));
   BOOST_REQUIRE(scores.connect(peer(2), {}, start));
   const auto ticket = validation(scores, 1, 1);
   BOOST_REQUIRE(scores.duplicate_delivery(peer(1), subject, id(1), start));
   BOOST_REQUIRE(scores.duplicate_delivery(peer(1), subject, id(1), start));
   BOOST_TEST(!scores.duplicate_delivery(peer(2), subject, id(1), start));
   BOOST_REQUIRE(scores.validation_complete(ticket, ps::validation_result::retry, start));
   BOOST_TEST(inspect(scores, 1).topics.front().invalid_message_deliveries == 0.0);
   BOOST_REQUIRE(scores.validation_complete(ticket, ps::validation_result::reject, start));
   BOOST_TEST(inspect(scores, 1).topics.front().invalid_message_deliveries == 2.0);
   BOOST_TEST(scores.score(peer(1), start) == -4.0);
   BOOST_TEST(scores.score(peer(2), start) == 0.0);
   BOOST_TEST(!scores.validation_complete(ticket, ps::validation_result::reject, start));
   BOOST_TEST(inspect(scores, 1).topics.front().invalid_message_deliveries == 2.0);
   BOOST_REQUIRE(scores.duplicate_delivery(peer(1), subject, id(1), start));
   BOOST_TEST(inspect(scores, 1).topics.front().invalid_message_deliveries == 3.0);
}

BOOST_AUTO_TEST_CASE(pending_first_sender_duplicate_does_not_double_valid_credit_or_penalize_ignore) {
   auto config = params();
   config.topics.at(subject).first_message_deliveries_weight = 1;
   config.limits.max_delivery_peers = 2;
   auto scores = engine{config, start};
   for (const auto p : {1, 2, 3}) {
      BOOST_REQUIRE(scores.connect(peer(p), {}, start));
      BOOST_REQUIRE(scores.graft(peer(p), subject, start));
   }
   const auto accepted = validation(scores, 1, 1);
   BOOST_REQUIRE(scores.duplicate_delivery(peer(2), subject, id(1), start));
   BOOST_TEST(!scores.duplicate_delivery(peer(3), subject, id(1), start));
   // First-source attribution remains possible even when all distinct peer slots are occupied.
   BOOST_REQUIRE(scores.duplicate_delivery(peer(1), subject, id(1), start));
   BOOST_REQUIRE(scores.duplicate_delivery(peer(1), subject, id(1), start));
   BOOST_REQUIRE(scores.validation_complete(accepted, ps::validation_result::accept, start));
   BOOST_REQUIRE(scores.duplicate_delivery(peer(1), subject, id(1), start));
   const auto first = inspect(scores, 1).topics.front();
   BOOST_TEST(first.first_message_deliveries == 1.0);
   BOOST_TEST(first.mesh_message_deliveries == 1.0);
   BOOST_TEST(first.invalid_message_deliveries == 0.0);
   BOOST_TEST(inspect(scores, 2).topics.front().first_message_deliveries == 0.0);
   BOOST_TEST(inspect(scores, 2).topics.front().mesh_message_deliveries == 1.0);
   const auto ignored = validation(scores, 1, 2);
   BOOST_REQUIRE(scores.duplicate_delivery(peer(1), subject, id(2), start));
   BOOST_REQUIRE(scores.validation_complete(ignored, ps::validation_result::ignore, start));
   BOOST_REQUIRE(scores.duplicate_delivery(peer(1), subject, id(2), start));
   BOOST_TEST(inspect(scores, 1).topics.front().invalid_message_deliveries == 0.0);
}

BOOST_AUTO_TEST_CASE(unconfigured_topics_are_neutral_valid_work_not_invalid_or_rejected_admission) {
   auto scores = engine{params(), start};
   const auto other = ps::topic{"unscored-but-signed"};
   BOOST_REQUIRE(scores.connect(peer(1), {}, start));
   BOOST_REQUIRE(scores.connect(peer(2), {}, start));
   BOOST_REQUIRE(scores.graft(peer(1), other, start));
   const auto valid = scores.validation_start(peer(1), other, id(1), start);
   BOOST_REQUIRE(valid);
   BOOST_REQUIRE(scores.duplicate_delivery(peer(2), other, id(1), start));
   BOOST_REQUIRE(scores.validation_complete(*valid, ps::validation_result::accept, start));
   BOOST_REQUIRE(scores.duplicate_delivery(peer(2), other, id(1), start));
   const auto invalid = scores.validation_start(peer(1), other, id(2), start);
   BOOST_REQUIRE(invalid);
   BOOST_REQUIRE(scores.validation_complete(*invalid, ps::validation_result::reject, start));
   BOOST_REQUIRE(scores.reject_invalid(peer(2), other, start));
   BOOST_REQUIRE(scores.prune(peer(1), other, start));
   BOOST_TEST(scores.score(peer(1), start) == 0.0);
   BOOST_TEST(scores.score(peer(2), start) == 0.0);
   BOOST_TEST(inspect(scores, 1).topics.front().invalid_message_deliveries == 0.0);
   BOOST_TEST(scores.snapshot(start).capacity_rejections == 0U);
}

BOOST_AUTO_TEST_CASE(p5_callback_is_only_sampled_by_caller_and_p7_uses_thresholded_quadratic_decay) {
   auto config = params();
   auto calls = 0;
   engine* active = nullptr;
   config.app_specific_weight = 2;
   config.app_specific_score = [&](const auto& p) {
      ++calls;
      return active->score(p, start) + 8.0; // Reentrant read is legal because caller holds no engine lock.
   };
   config.behaviour_penalty_weight = -3;
   config.behaviour_penalty_threshold = 2;
   config.behaviour_penalty_decay = 0.5;
   auto scores = engine{config, start};
   active = &scores;
   BOOST_REQUIRE(scores.connect(peer(1), {}, start));
   BOOST_TEST(calls == 0);
   BOOST_REQUIRE(scores.set_application_score(peer(1), config.app_specific_score(peer(1)), start));
   BOOST_REQUIRE(scores.add_behaviour_penalty(peer(1), 5, start));
   BOOST_TEST(scores.score(peer(1), start) == -11.0);
   BOOST_TEST(scores.score(peer(1), start + 1s) == 15.25);
   BOOST_TEST(scores.score(peer(1), start + 2s) == 16.0);
   BOOST_TEST(calls == 1);
   BOOST_CHECK_THROW(static_cast<void>(scores.set_application_score(peer(1),
       std::numeric_limits<double>::quiet_NaN(), start + 2s)), p2p::exceptions::invalid_options);
   BOOST_CHECK_THROW(static_cast<void>(scores.add_behaviour_penalty(peer(1), -1, start + 2s)),
                     p2p::exceptions::invalid_options);
   BOOST_CHECK_THROW(static_cast<void>(scores.add_behaviour_penalty(peer(1),
       std::numeric_limits<double>::infinity(), start + 2s)), p2p::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(p6_uses_real_canonical_direct_ips_allowlist_and_retained_peer_membership) {
   auto config = params();
   config.ip_colocation_factor_weight = -2;
   config.ip_colocation_factor_threshold = 1;
   config.limits.max_ips_per_peer = 1;
   config.retain_score = 2s;
   const auto shared = boost::asio::ip::make_address("11.0.0.1");
   const auto allowed = boost::asio::ip::make_address("11.0.0.9");
   config.ip_colocation_factor_allowlist.push_back(allowed);
   auto scores = engine{config, start};
   for (const auto p : {1, 2, 3}) {
      BOOST_REQUIRE(scores.connect(peer(p), std::array{shared}, start));
   }
   BOOST_TEST(scores.score(peer(1), start) == -8.0);
   BOOST_REQUIRE(scores.connect(peer(4), std::array{boost::asio::ip::make_address("127.0.0.1")}, start));
   BOOST_REQUIRE(scores.connect(peer(5), {}, start)); // Authenticated relay: no fabricated carrier colocation.
   for (const auto p : {6, 7}) {
      BOOST_REQUIRE(scores.connect(peer(p), std::array{allowed}, start));
      BOOST_TEST(scores.score(peer(p), start) == 0.0);
   }
   BOOST_TEST(inspect(scores, 4).ips.empty());
   BOOST_TEST(inspect(scores, 5).ips.empty());
   BOOST_REQUIRE(scores.connect(peer(8), std::array{boost::asio::ip::make_address("::ffff:11.0.0.1")}, start));
   BOOST_TEST(scores.score(peer(1), start) == -18.0);
   BOOST_TEST(!scores.update_ips(peer(2), std::array{shared, allowed}, start));
   BOOST_TEST(scores.score(peer(2), start) == -18.0);
   BOOST_REQUIRE(scores.disconnect(peer(1), start));
   BOOST_TEST(scores.score(peer(2), start + 2s) == -18.0); // Strict donor expiry: still retained at boundary.
   BOOST_TEST(scores.score(peer(2), start + 2s + 1ms) == -8.0);
   BOOST_REQUIRE(scores.update_ips(peer(2), std::array{boost::asio::ip::make_address("11.0.0.2")}, start + 2s + 1ms));
   BOOST_TEST(scores.score(peer(2), start + 2s + 1ms) == 0.0);
   BOOST_TEST(scores.score(peer(3), start + 2s + 1ms) == -2.0);
}

BOOST_AUTO_TEST_CASE(retention_reservation_never_evicts_negative_peers_under_admission_pressure) {
   auto config = params();
   config.limits.max_connected_peers = 2;
   config.limits.max_retained_peers = 2;
   config.retain_score = 5s;
   auto scores = engine{config, start};
   for (const auto p : {1, 2}) {
      BOOST_REQUIRE(scores.connect(peer(p), {}, start));
      BOOST_REQUIRE(scores.reject_invalid(peer(p), subject, start));
      BOOST_REQUIRE(scores.disconnect(peer(p), start));
   }
   BOOST_TEST(!scores.connect(peer(3), {}, start));
   BOOST_TEST(scores.snapshot(start).retained_peers == 2U);
   BOOST_TEST(scores.score(peer(1), start + 4s) == -1.0); // No disconnected decay.
   BOOST_REQUIRE(scores.connect(peer(1), {}, start + 4s));
   BOOST_TEST(scores.score(peer(1), start + 4s) == -1.0);
   BOOST_REQUIRE(scores.disconnect(peer(1), start + 4s));
   BOOST_TEST(!scores.connect(peer(3), {}, start + 5s));
   BOOST_REQUIRE(scores.connect(peer(3), {}, start + 5s + 1ms));
   BOOST_TEST(scores.score(peer(1), start + 9s) == -1.0);
   scores.tick(start + 9s + 1ms);
   BOOST_TEST(!scores.inspect(peer(1), start + 9s + 1ms).has_value());
}

BOOST_AUTO_TEST_CASE(positive_scores_are_not_retained_and_zero_retention_is_explicit) {
   auto config = params();
   config.topics.at(subject).first_message_deliveries_weight = 1;
   auto scores = engine{config, start};
   BOOST_REQUIRE(scores.connect(peer(1), {}, start));
   accept(scores, 1, 1);
   BOOST_REQUIRE(scores.disconnect(peer(1), start));
   BOOST_TEST(!scores.inspect(peer(1), start).has_value());
   config.retain_score = 0ms;
   auto no_retention = engine{config, start};
   BOOST_REQUIRE(no_retention.connect(peer(2), {}, start));
   BOOST_REQUIRE(no_retention.reject_invalid(peer(2), subject, start));
   BOOST_REQUIRE(no_retention.disconnect(peer(2), start));
   BOOST_TEST(no_retention.snapshot(start).retained_peers == 0U);
}

BOOST_AUTO_TEST_CASE(ip_index_tracks_reconnect_and_positive_removal_without_double_membership) {
   auto config = params();
   config.ip_colocation_factor_weight = -1;
   config.ip_colocation_factor_threshold = 1;
   config.app_specific_weight = 1;
   config.app_specific_score = [](const auto&) { return 5.0; };
   const auto shared = boost::asio::ip::make_address("2001:4860::1");
   const auto other = boost::asio::ip::make_address("2001:4860::2");
   auto scores = engine{config, start};
   BOOST_REQUIRE(scores.connect(peer(1), std::array{shared, shared}, start));
   BOOST_REQUIRE(scores.connect(peer(2), std::array{shared}, start));
   BOOST_TEST(scores.score(peer(2), start) == -1.0);
   BOOST_REQUIRE(scores.disconnect(peer(1), start));
   BOOST_REQUIRE(scores.connect(peer(1), std::array{other}, start));
   BOOST_TEST(scores.score(peer(2), start) == 0.0);
   BOOST_REQUIRE(scores.connect(peer(1), std::array{shared}, start));
   BOOST_REQUIRE(scores.connect(peer(1), std::array{shared}, start));
   BOOST_TEST(scores.score(peer(2), start) == -1.0);
   BOOST_REQUIRE(scores.set_application_score(peer(1), 5, start));
   BOOST_REQUIRE(scores.disconnect(peer(1), start));
   BOOST_TEST(!scores.inspect(peer(1), start).has_value());
   BOOST_TEST(scores.score(peer(2), start) == 0.0);
}

BOOST_AUTO_TEST_CASE(expiry_indices_keep_reconnected_peers_and_later_message_owners_alive) {
   auto config = params();
   config.retain_score = 2s;
   config.seen_message_ttl = 3s;
   auto scores = engine{config, start};
   BOOST_REQUIRE(scores.connect(peer(1), {}, start));
   BOOST_REQUIRE(scores.connect(peer(2), {}, start));
   BOOST_REQUIRE(scores.reject_invalid(peer(1), subject, start));
   BOOST_REQUIRE(scores.disconnect(peer(1), start));
   const auto first = validation(scores, 2, 1);
   BOOST_REQUIRE(scores.connect(peer(1), {}, start + 1s));
   const auto later = validation(scores, 2, 2, start + 1s);
   BOOST_TEST(scores.snapshot(start + 2s + 1ms).connected_peers == 2U);
   BOOST_REQUIRE(scores.validation_complete(first, ps::validation_result::reject, start + 3s));
   BOOST_TEST(scores.snapshot(start + 3s + 1ms).delivery_records == 1U);
   BOOST_TEST(!scores.validation_complete(first, ps::validation_result::accept, start + 3s + 1ms));
   BOOST_REQUIRE(scores.validation_complete(later, ps::validation_result::reject, start + 3s + 1ms));
   BOOST_TEST(inspect(scores, 2, start + 3s + 1ms).topics.front().invalid_message_deliveries == 2.0);
   BOOST_TEST(scores.snapshot(start + 4s + 1ms).delivery_records == 0U);
}

BOOST_AUTO_TEST_CASE(message_pressure_refuses_new_work_without_eviction_or_unattributed_credit) {
   auto config = params();
   config.limits.max_messages = 2;
   config.limits.max_delivery_peers = 2;
   config.limits.max_message_id_size = 2;
   config.seen_message_ttl = 2s;
   auto scores = engine{config, start};
   for (const auto p : {1, 2, 3}) {
      BOOST_REQUIRE(scores.connect(peer(p), {}, start));
      BOOST_REQUIRE(scores.graft(peer(p), subject, start));
   }
   const auto first = validation(scores, 1, 1);
   const auto second = validation(scores, 1, 2);
   BOOST_TEST(!scores.validation_start(peer(1), subject, id(3), start).has_value());
   BOOST_TEST(!scores.validation_start(peer(1), subject, std::array<std::uint8_t, 3>{0, 1, 2}, start).has_value());
   BOOST_REQUIRE(scores.duplicate_delivery(peer(2), subject, id(1), start));
   BOOST_TEST(!scores.duplicate_delivery(peer(3), subject, id(1), start));
   BOOST_REQUIRE(scores.validation_complete(first, ps::validation_result::accept, start));
   BOOST_TEST(inspect(scores, 2).topics.front().mesh_message_deliveries == 1.0);
   BOOST_TEST(inspect(scores, 3).topics.front().mesh_message_deliveries == 0.0);
   BOOST_TEST(scores.snapshot(start).delivery_records == 2U);
   BOOST_TEST(scores.snapshot(start).capacity_rejections == 3U);
   scores.tick(start + 2s + 1ms);
   BOOST_TEST(scores.snapshot(start + 2s + 1ms).delivery_records == 0U);
   BOOST_TEST(!scores.validation_complete(second, ps::validation_result::reject, start + 2s + 1ms));
   BOOST_REQUIRE(scores.validation_start(peer(1), subject, id(3), start + 2s + 1ms));
}

BOOST_AUTO_TEST_CASE(late_validation_cannot_mutate_replacement_message_or_replacement_peer_generation) {
   auto config = params();
   config.seen_message_ttl = 2s;
   config.app_specific_weight = 1;
   config.app_specific_score = [](const auto&) { return 2.0; };
   auto scores = engine{config, start};
   BOOST_REQUIRE(scores.connect(peer(1), {}, start));
   BOOST_REQUIRE(scores.connect(peer(2), {}, start));
   const auto old = validation(scores, 1, 1);
   const auto now = start + 2s + 1ms;
   const auto fresh = validation(scores, 1, 1, now);
   BOOST_TEST(fresh.generation != old.generation);
   BOOST_TEST(!scores.validation_complete(old, ps::validation_result::reject, now));
   BOOST_REQUIRE(scores.validation_complete(fresh, ps::validation_result::reject, now));
   BOOST_TEST(scores.score(peer(1), now) == -1.0);
   const auto stale_peer = validation(scores, 1, 2, now);
   BOOST_REQUIRE(scores.duplicate_delivery(peer(2), subject, id(2), now));
   BOOST_REQUIRE(scores.set_application_score(peer(1), 2, now));
   BOOST_REQUIRE(scores.disconnect(peer(1), now)); // Positive total removes the old generation.
   BOOST_REQUIRE(scores.connect(peer(1), {}, now));
   BOOST_REQUIRE(scores.validation_complete(stale_peer, ps::validation_result::reject, now));
   BOOST_TEST(scores.score(peer(1), now) == 0.0);
   BOOST_TEST(scores.score(peer(2), now) == -1.0);
}

BOOST_AUTO_TEST_CASE(decay_catchup_cutoff_clock_order_and_finite_extreme_quadratic_values) {
   auto config = params();
   config.behaviour_penalty_weight = -1;
   config.behaviour_penalty_decay = 0.5;
   config.decay_to_zero = 0.1;
   auto scores = engine{config, start};
   BOOST_REQUIRE(scores.connect(peer(1), {}, start));
   BOOST_REQUIRE(scores.add_behaviour_penalty(peer(1), 1, start));
   BOOST_TEST(scores.score(peer(1), start + 3s) == -0.015625);
   BOOST_TEST(scores.score(peer(1), start + 4s) == 0.0);
   BOOST_CHECK_NO_THROW(scores.tick(start + 3s)); // Stale observations cannot rewind decay.
   BOOST_TEST(scores.score(peer(1), start + 3s) == 0.0);
   scores.tick(start + 24h); // Bounded pow catchup, not 86,400 worker/timer iterations.
   BOOST_TEST(scores.score(peer(1), start + 24h) == 0.0);
   config.behaviour_penalty_weight = -1e-300;
   auto extreme = engine{config, start};
   BOOST_REQUIRE(extreme.connect(peer(1), {}, start));
   BOOST_REQUIRE(extreme.add_behaviour_penalty(peer(1), 1e200, start));
   BOOST_CHECK_CLOSE(extreme.score(peer(1), start), -1e100, 0.000001);
   BOOST_REQUIRE(extreme.add_behaviour_penalty(peer(1), (std::numeric_limits<double>::max)(), start));
   BOOST_TEST(std::isfinite(extreme.score(peer(1), start)));
}

BOOST_AUTO_TEST_CASE(concurrent_bounded_event_accounting_does_not_invoke_application_callback) {
   auto config = params();
   auto samples = std::atomic<unsigned>{0};
   config.app_specific_weight = 1;
   config.app_specific_score = [&](const auto&) { ++samples; return 0.0; };
   config.behaviour_penalty_weight = -1;
   auto scores = engine{config, start};
   BOOST_REQUIRE(scores.connect(peer(1), {}, start));
   auto okay = std::atomic<bool>{true};
   auto workers = std::vector<std::jthread>{};
   for (auto index = 0; index < 4; ++index) {
      workers.emplace_back([&] {
         for (auto n = 0; n < 100; ++n) {
            if (!scores.add_behaviour_penalty(peer(1), 1, start) ||
                !scores.reject_invalid(peer(1), subject, start)) {
               okay = false;
            }
         }
      });
   }
   for (auto& worker : workers) {
      worker.join();
   }
   BOOST_TEST(okay.load());
   BOOST_TEST(samples.load() == 0U);
   const auto receipt = inspect(scores, 1);
   BOOST_TEST(receipt.behaviour_penalty == 400.0);
   BOOST_TEST(receipt.topics.front().invalid_message_deliveries == 400.0);
   BOOST_TEST(receipt.value == -320'000.0);
}

BOOST_AUTO_TEST_CASE(concurrent_real_clock_observations_are_clamped_after_waiting_for_engine_ownership) {
   auto scores = engine{params(), engine::clock::now()};
   BOOST_REQUIRE(scores.connect(peer(1), {}, engine::clock::now()));
   auto observed = std::promise<engine::clock::time_point>{};
   auto release = std::latch{1};
   auto error = std::exception_ptr{};
   auto earlier = std::jthread{[&] {
      const auto sampled = engine::clock::now();
      observed.set_value(sampled);
      release.wait();
      try { static_cast<void>(scores.inspect(peer(1), sampled)); }
      catch (...) { error = std::current_exception(); }
   }};
   const auto first = observed.get_future().get();
   const auto later = engine::clock::now();
   scores.tick(later);
   release.count_down();
   earlier.join();
   BOOST_CHECK(later >= first);
   BOOST_CHECK(!error);
   auto okay = std::atomic_bool{true};
   auto readers = std::vector<std::jthread>{};
   for (auto n = 0; n < 4; ++n) {
      readers.emplace_back([&] {
         try {
            for (auto count = 0; count < 100; ++count) {
               scores.tick(engine::clock::now());
               static_cast<void>(scores.inspect(peer(1), engine::clock::now()));
            }
         } catch (...) { okay = false; }
      });
   }
   for (auto& reader : readers) { reader.join(); }
   BOOST_TEST(okay.load());
}

BOOST_AUTO_TEST_SUITE_END()
