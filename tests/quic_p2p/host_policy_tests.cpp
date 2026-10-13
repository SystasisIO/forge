#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <string>

import forge.net.p2p.exceptions;
import forge.net.p2p.node;

namespace {
namespace p2p = forge::net::p2p;
using milliseconds = std::chrono::milliseconds;

struct duration_field {
   const char* name;
   void (*set)(p2p::node::options&, milliseconds);
};

constexpr auto durations = std::array{
   duration_field{"session grace", [](auto& v, auto n) { v.limits.session_grace_period = n; }},
   duration_field{"session prune silence", [](auto& v, auto n) { v.limits.session_prune_silence = n; }},
   duration_field{"dial backoff base", [](auto& v, auto n) {
      v.limits.dial_backoff_base = n;
      v.limits.dial_backoff_max = n;
   }},
   duration_field{"dial backoff step", [](auto& v, auto n) { v.limits.dial_backoff_step = n; }},
   duration_field{"dial backoff maximum", [](auto& v, auto n) { v.limits.dial_backoff_max = n; }},
   duration_field{"Identify timeout", [](auto& v, auto n) { v.identify.timeout = n; }},
};
} // namespace

BOOST_AUTO_TEST_CASE(p2p_host_policy_durations_must_fit_both_native_clocks_without_an_operational_cap) {
   constexpr auto largest = std::min(
       std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::duration::max()),
       std::chrono::duration_cast<milliseconds>(std::chrono::system_clock::duration::max()));
   for (const auto& field : durations) {
      BOOST_TEST_CONTEXT(field.name) {
         auto options = p2p::node::options{};
         field.set(options, largest);
         BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
         for (const auto invalid : {milliseconds{10'000'000'000'000LL}, largest + milliseconds{1},
                                    milliseconds::max()}) {
            field.set(options, invalid);
            BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
         }
      }
   }
   auto options = p2p::node::options{};
   options.limits.session_grace_period = milliseconds::zero();
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
}

BOOST_AUTO_TEST_CASE(p2p_host_policy_local_versions_respect_effective_identify_bound_and_allow_empty) {
   auto options = p2p::node::options{};
   options.identify.max_version_size = 16;
   options.protocol_version = std::string(16, 'p');
   options.agent_version = std::string(16, 'a');
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   options.protocol_version += 'p';
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   options.protocol_version.clear();
   options.agent_version += 'a';
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   options.agent_version.clear();
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
}
