module;

#include <boost/test/unit_test.hpp>
#include <forge/exceptions/macros.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/address.hpp>

module forge.net.p2p.node;

import forge.asio.blocking;
import forge.asio.runtime;
import forge.multiformats.multiaddr;
import forge.net.dns.exceptions;
import forge.net.dns.resolver;
import forge.net.dns.types;
import forge.net.p2p.address_resolution;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;

#include "../../libraries/net/p2p/details/dns_address_expander.hxx"

namespace {

namespace dns = forge::net::dns;
namespace p2p = forge::net::p2p;

using address_key = std::pair<std::string, dns::address_family>;

[[noreturn]] void throw_scripted_dns_failure(dns::exceptions::code value) {
   switch (value) {
   case dns::exceptions::code::invalid_options:
      FORGE_THROW_EXCEPTION(dns::exceptions::invalid_options, "scripted invalid DNS options");
   case dns::exceptions::code::not_found:
      FORGE_THROW_EXCEPTION(dns::exceptions::not_found, "scripted DNS name not found");
   case dns::exceptions::code::temporary_failure:
      FORGE_THROW_EXCEPTION(dns::exceptions::temporary_failure, "scripted temporary DNS failure");
   case dns::exceptions::code::resource_limit:
      FORGE_THROW_EXCEPTION(dns::exceptions::resource_limit, "scripted DNS resource limit");
   default:
      FORGE_THROW_EXCEPTION(dns::exceptions::internal, "unsupported scripted DNS failure");
   }
}

struct lookup_script {
   std::map<address_key, dns::address_response> address_responses;
   std::map<address_key, dns::exceptions::code> address_failures;
   std::map<std::string, dns::text_response> text_responses;
   std::vector<address_key> address_requests;
   std::vector<std::string> text_requests;
   std::vector<dns::query_options> text_options;

   [[nodiscard]] boost::asio::awaitable<dns::address_response>
   resolve_addresses(std::string name, dns::address_family family, dns::query_options, std::stop_token) {
      const auto key = address_key{std::move(name), family};
      address_requests.push_back(key);
      if (const auto found = address_failures.find(key); found != address_failures.end()) {
         throw_scripted_dns_failure(found->second);
      }
      if (const auto found = address_responses.find(key); found != address_responses.end()) {
         co_return found->second;
      }
      co_return dns::address_response{};
   }

   [[nodiscard]] boost::asio::awaitable<dns::text_response>
   resolve_txt(std::string name, dns::query_options options, std::stop_token) {
      text_requests.push_back(name);
      text_options.push_back(std::move(options));
      if (const auto found = text_responses.find(name); found != text_responses.end()) {
         co_return found->second;
      }
      co_return dns::text_response{};
   }
};

[[nodiscard]] dns::address_response addresses(std::initializer_list<std::string_view> values) {
   auto result = dns::address_response{};
   for (const auto value : values) {
      result.answers.push_back({.value = boost::asio::ip::make_address(value), .ttl = std::chrono::seconds{60}});
   }
   return result;
}

[[nodiscard]] dns::text_answer text_record(std::string value) {
   return dns::text_answer{.value = {value.begin(), value.end()}, .ttl = std::chrono::seconds{60}};
}

[[nodiscard]] p2p::detail::dns_address_expander make_expander(
    const std::shared_ptr<lookup_script>& script, p2p::address_resolution::policy policy = {}) {
   return p2p::detail::dns_address_expander{
       std::move(policy),
       {.resolve_addresses =
            [script](std::string name, dns::address_family family, dns::query_options options,
                     std::stop_token stop) -> boost::asio::awaitable<dns::address_response> {
               co_return co_await script->resolve_addresses(std::move(name), family, std::move(options), stop);
            },
        .resolve_txt =
            [script](std::string name, dns::query_options options,
                     std::stop_token stop) -> boost::asio::awaitable<dns::text_response> {
               co_return co_await script->resolve_txt(std::move(name), std::move(options), stop);
            }}};
}

[[nodiscard]] p2p::detail::dns_address_expansion_result
expand_roots_result(forge::asio::runtime& runtime, p2p::detail::dns_address_expander& value,
                    std::vector<std::string> sources, std::optional<p2p::peer_id> expected_peer = {},
                    std::stop_token stop = {},
                    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) {
   auto roots = std::vector<forge::multiformats::multiaddr>{};
   roots.reserve(sources.size());
   for (auto& source : sources) {
      roots.push_back(forge::multiformats::multiaddr::parse(std::move(source)));
   }
   return forge::asio::blocking::run(runtime,
                                     value.async_expand(std::move(roots), std::move(expected_peer), deadline, stop));
}

std::vector<p2p::endpoint>
expand_roots(forge::asio::runtime& runtime, p2p::detail::dns_address_expander& value, std::vector<std::string> sources,
             std::optional<p2p::peer_id> expected_peer = {}, std::stop_token stop = {},
             std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) {
   auto result = expand_roots_result(runtime, value, std::move(sources), std::move(expected_peer), stop, deadline);
   auto endpoints = std::vector<p2p::endpoint>{};
   endpoints.reserve(result.targets.size());
   for (auto& target : result.targets) {
      endpoints.push_back(std::move(target.concrete));
   }
   return endpoints;
}

std::vector<p2p::endpoint>
expand(forge::asio::runtime& runtime, p2p::detail::dns_address_expander& value, std::string source,
       std::optional<p2p::peer_id> expected_peer = {}, std::stop_token stop = {},
       std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) {
   return expand_roots(runtime, value, {std::move(source)}, std::move(expected_peer), stop, deadline);
}

} // namespace

BOOST_AUTO_TEST_CASE(p2p_dns_address_expansion_preserves_components_and_filters_families) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<lookup_script>();
   script->address_responses.emplace(address_key{"dual.test", dns::address_family::any},
                                     addresses({"192.0.2.10", "2001:db8::10"}));
   script->address_responses.emplace(address_key{"v4.test", dns::address_family::ipv4},
                                     addresses({"192.0.2.11", "2001:db8::11"}));
   script->address_responses.emplace(address_key{"v6.test", dns::address_family::ipv6},
                                     addresses({"2001:db8::12", "192.0.2.12"}));
   auto resolver = make_expander(script);

   const auto dual = expand(runtime, resolver, "/dns/dual.test/udp/4001/quic-v1");
   BOOST_REQUIRE_EQUAL(dual.size(), 2U);
   BOOST_TEST(dual[0].to_string() == "/ip4/192.0.2.10/udp/4001/quic-v1");
   BOOST_TEST(dual[1].to_string() == "/ip6/2001:db8::10/udp/4001/quic-v1");

   const auto ipv4 = expand(runtime, resolver, "/dns4/v4.test/tcp/4001");
   BOOST_REQUIRE_EQUAL(ipv4.size(), 1U);
   BOOST_TEST(ipv4.front().to_string() == "/ip4/192.0.2.11/tcp/4001");

   const auto ipv6 = expand(runtime, resolver, "/dns6/v6.test/tcp/4001");
   BOOST_REQUIRE_EQUAL(ipv6.size(), 1U);
   BOOST_TEST(ipv6.front().to_string() == "/ip6/2001:db8::12/tcp/4001");
   BOOST_REQUIRE_EQUAL(script->address_requests.size(), 3U);
   BOOST_TEST(static_cast<int>(script->address_requests[0].second) == static_cast<int>(dns::address_family::any));
   BOOST_TEST(static_cast<int>(script->address_requests[1].second) == static_cast<int>(dns::address_family::ipv4));
   BOOST_TEST(static_cast<int>(script->address_requests[2].second) == static_cast<int>(dns::address_family::ipv6));
}

BOOST_AUTO_TEST_CASE(p2p_dns_address_expansion_combines_and_deduplicates_roots_deterministically) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<lookup_script>();
   script->address_responses.emplace(address_key{"first-root.test", dns::address_family::any},
                                     addresses({"192.0.2.101"}));
   script->address_responses.emplace(address_key{"second-root.test", dns::address_family::any},
                                     addresses({"192.0.2.101", "2001:db8::101"}));
   auto resolver = make_expander(script);

   const auto result = expand_roots_result(runtime, resolver,
                                           {"/dns/first-root.test/tcp/4001", "/dns/second-root.test/tcp/4001",
                                            "/dns/first-root.test/tcp/4001"});
   BOOST_REQUIRE_EQUAL(result.roots.size(), 2U);
   BOOST_REQUIRE_EQUAL(result.roots[0].input_indices.size(), 2U);
   BOOST_TEST(result.roots[0].input_indices[0] == 0U);
   BOOST_TEST(result.roots[0].input_indices[1] == 2U);
   BOOST_REQUIRE_EQUAL(result.roots[1].input_indices.size(), 1U);
   BOOST_TEST(result.roots[1].input_indices[0] == 1U);
   BOOST_REQUIRE_EQUAL(result.targets.size(), 2U);
   BOOST_TEST(result.targets[0].concrete.to_string() == "/ip4/192.0.2.101/tcp/4001");
   BOOST_TEST(result.targets[1].concrete.to_string() == "/ip6/2001:db8::101/tcp/4001");
   BOOST_REQUIRE_EQUAL(script->address_requests.size(), 2U);
   BOOST_TEST(script->address_requests[0].first == "first-root.test");
   BOOST_TEST(script->address_requests[1].first == "second-root.test");
}

BOOST_AUTO_TEST_CASE(p2p_dns_address_expansion_merges_shared_child_root_provenance_per_operation) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<lookup_script>();
   script->text_responses.emplace("_dnsaddr.first-provenance.test",
                                  dns::text_response{.answers = {text_record("dnsaddr=/dns/shared-provenance.test/tcp/4001")}});
   script->text_responses.emplace("_dnsaddr.second-provenance.test",
                                  dns::text_response{.answers = {text_record("dnsaddr=/dns/shared-provenance.test/tcp/4001")}});
   script->address_responses.emplace(address_key{"shared-provenance.test", dns::address_family::any},
                                     addresses({"192.0.2.110"}));
   auto resolver = make_expander(script);

   const auto first = expand_roots_result(runtime, resolver,
                                          {"/dnsaddr/first-provenance.test", "/dnsaddr/second-provenance.test"});
   BOOST_REQUIRE_EQUAL(first.roots.size(), 2U);
   BOOST_TEST(first.roots[0].canonical.to_string() == "/dnsaddr/first-provenance.test");
   BOOST_TEST(first.roots[1].canonical.to_string() == "/dnsaddr/second-provenance.test");
   BOOST_REQUIRE_EQUAL(first.targets.size(), 1U);
   BOOST_TEST(first.targets.front().concrete.to_string() == "/ip4/192.0.2.110/tcp/4001");
   BOOST_REQUIRE_EQUAL(first.targets.front().root_indices.size(), 2U);
   BOOST_TEST(first.targets.front().root_indices[0] == 0U);
   BOOST_TEST(first.targets.front().root_indices[1] == 1U);
   BOOST_REQUIRE_EQUAL(script->address_requests.size(), 1U);

   const auto second = expand_roots_result(runtime, resolver,
                                           {"/dnsaddr/first-provenance.test", "/dnsaddr/second-provenance.test"});
   BOOST_REQUIRE_EQUAL(second.targets.size(), 1U);
   BOOST_REQUIRE_EQUAL(script->address_requests.size(), 2U);
}

BOOST_AUTO_TEST_CASE(p2p_dns_address_expansion_merges_shared_targets_at_the_global_result_limit) {
   auto runtime = forge::asio::runtime{};

   {
      auto script = std::make_shared<lookup_script>();
      script->address_responses.emplace(address_key{"first-limited-provenance.test", dns::address_family::any},
                                        addresses({"192.0.2.111"}));
      script->address_responses.emplace(address_key{"second-limited-provenance.test", dns::address_family::any},
                                        addresses({"192.0.2.111"}));
      auto policy = p2p::address_resolution::policy{};
      policy.bounds.max_resolved_addresses = 1;
      auto resolver = make_expander(script, policy);

      const auto result = expand_roots_result(
          runtime, resolver,
          {"/dns/first-limited-provenance.test/tcp/4001", "/dns/second-limited-provenance.test/tcp/4001"});
      BOOST_REQUIRE_EQUAL(result.targets.size(), 1U);
      BOOST_REQUIRE_EQUAL(result.targets.front().root_indices.size(), 2U);
      BOOST_TEST(result.targets.front().root_indices[0] == 0U);
      BOOST_TEST(result.targets.front().root_indices[1] == 1U);
      BOOST_REQUIRE_EQUAL(script->address_requests.size(), 2U);
   }

   {
      auto script = std::make_shared<lookup_script>();
      script->text_responses.emplace(
          "_dnsaddr.first-limited-provenance.test",
          dns::text_response{.answers = {text_record("dnsaddr=/ip4/192.0.2.112/tcp/4001")}});
      script->text_responses.emplace(
          "_dnsaddr.second-limited-provenance.test",
          dns::text_response{.answers = {text_record("dnsaddr=/ip4/192.0.2.112/tcp/4001")}});
      auto policy = p2p::address_resolution::policy{};
      policy.bounds.max_resolved_addresses = 1;
      auto resolver = make_expander(script, policy);

      const auto result = expand_roots_result(
          runtime, resolver,
          {"/dnsaddr/first-limited-provenance.test", "/dnsaddr/second-limited-provenance.test"});
      BOOST_REQUIRE_EQUAL(result.targets.size(), 1U);
      BOOST_REQUIRE_EQUAL(result.targets.front().root_indices.size(), 2U);
      BOOST_TEST(result.targets.front().root_indices[0] == 0U);
      BOOST_TEST(result.targets.front().root_indices[1] == 1U);
      BOOST_REQUIRE_EQUAL(script->text_requests.size(), 2U);
   }
}

BOOST_AUTO_TEST_CASE(p2p_dns_address_expansion_shares_root_budgets) {
   auto runtime = forge::asio::runtime{};

   {
      auto script = std::make_shared<lookup_script>();
      script->address_responses.emplace(address_key{"first-lookup.test", dns::address_family::any},
                                        addresses({"192.0.2.102"}));
      script->address_responses.emplace(address_key{"second-lookup.test", dns::address_family::any},
                                        addresses({"192.0.2.103"}));
      auto policy = p2p::address_resolution::policy{};
      policy.bounds.max_dns_lookups = 1;
      auto resolver = make_expander(script, policy);

      BOOST_CHECK_THROW(expand_roots(runtime, resolver,
                                     {"/dns/first-lookup.test/tcp/4001", "/dns/second-lookup.test/tcp/4001"}),
                        p2p::exceptions::invalid_options);
      BOOST_REQUIRE_EQUAL(script->address_requests.size(), 1U);
   }

   {
      auto script = std::make_shared<lookup_script>();
      script->text_responses.emplace("_dnsaddr.first-candidate.test",
                                     dns::text_response{.answers = {text_record("dnsaddr=/ip4/192.0.2.104/tcp/4001")}});
      script->text_responses.emplace("_dnsaddr.second-candidate.test",
                                     dns::text_response{.answers = {text_record("dnsaddr=/ip4/192.0.2.105/tcp/4001")}});
      auto policy = p2p::address_resolution::policy{};
      policy.bounds.max_txt_records = 1;
      auto resolver = make_expander(script, policy);

      const auto values = expand_roots(runtime, resolver,
                                       {"/dnsaddr/first-candidate.test", "/dnsaddr/second-candidate.test"});
      BOOST_REQUIRE_EQUAL(values.size(), 2U);
      BOOST_TEST(values[0].to_string() == "/ip4/192.0.2.104/tcp/4001");
      BOOST_TEST(values[1].to_string() == "/ip4/192.0.2.105/tcp/4001");
      BOOST_REQUIRE_EQUAL(script->text_requests.size(), 2U);
   }

   {
      auto script = std::make_shared<lookup_script>();
      script->address_responses.emplace(address_key{"first-result.test", dns::address_family::any},
                                        addresses({"192.0.2.106"}));
      script->address_responses.emplace(address_key{"second-result.test", dns::address_family::any},
                                        addresses({"192.0.2.107"}));
      auto policy = p2p::address_resolution::policy{};
      policy.bounds.max_resolved_addresses = 1;
      auto resolver = make_expander(script, policy);

      BOOST_CHECK_THROW(expand_roots(runtime, resolver,
                                     {"/dns/first-result.test/tcp/4001", "/dns/second-result.test/tcp/4001"}),
                        p2p::exceptions::invalid_options);
      BOOST_REQUIRE_EQUAL(script->address_requests.size(), 2U);
   }

   {
      auto script = std::make_shared<lookup_script>();
      script->text_responses.emplace(
          "_dnsaddr.first-result.test",
          dns::text_response{.answers = {text_record("dnsaddr=/ip4/192.0.2.108/tcp/4001")}});
      script->text_responses.emplace(
          "_dnsaddr.second-result.test",
          dns::text_response{.answers = {text_record("dnsaddr=/ip4/192.0.2.109/tcp/4001")}});
      auto policy = p2p::address_resolution::policy{};
      policy.bounds.max_resolved_addresses = 1;
      auto resolver = make_expander(script, policy);

      BOOST_CHECK_THROW(expand_roots(runtime, resolver,
                                     {"/dnsaddr/first-result.test", "/dnsaddr/second-result.test"}),
                        p2p::exceptions::invalid_options);
      BOOST_REQUIRE_EQUAL(script->text_requests.size(), 2U);
   }
}

BOOST_AUTO_TEST_CASE(p2p_dns_address_expansion_rejects_conflicting_root_peers_and_honors_cancellation) {
   const auto first = p2p::peer_id::from_string("QmcgpsyWgH8Y8ajJz1Cu72KnS5uo2Aa2LpzU7kinSupNKC");
   const auto second = p2p::peer_id::from_string("QmNLfbof5rLekrACjeuLk9JmGZD2HDBHCU4z16iYKmx5SE");
   auto runtime = forge::asio::runtime{};

   {
      auto script = std::make_shared<lookup_script>();
      auto resolver = make_expander(script);
      BOOST_CHECK_THROW(
          expand_roots(runtime, resolver,
                       {"/dns/first-peer.test/tcp/4001/p2p/" + first.to_string(),
                        "/dns/second-peer.test/tcp/4001/p2p/" + second.to_string()}),
          p2p::exceptions::invalid_options);
      BOOST_TEST(script->address_requests.empty());
   }

   {
      auto script = std::make_shared<lookup_script>();
      auto resolver = make_expander(script);
      auto stopped = std::stop_source{};
      static_cast<void>(stopped.request_stop());
      BOOST_CHECK_THROW(expand_roots(runtime, resolver,
                                     {"/dns/canceled-first.test/tcp/4001", "/dns/canceled-second.test/tcp/4001"},
                                     {}, stopped.get_token()),
                        p2p::exceptions::canceled);
      BOOST_TEST(script->address_requests.empty());
   }
}

BOOST_AUTO_TEST_CASE(p2p_dns_address_expansion_bounds_root_preflight) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<lookup_script>();
   auto policy = p2p::address_resolution::policy{};
   policy.bounds.max_roots = 1;
   auto resolver = make_expander(script, policy);

   BOOST_CHECK_THROW(expand_roots(runtime, resolver,
                                  {"/ip4/192.0.2.108/tcp/4001", "/ip4/192.0.2.109/tcp/4001"}),
                     p2p::exceptions::invalid_options);
   BOOST_TEST(script->address_requests.empty());
   BOOST_TEST(script->text_requests.empty());
}

BOOST_AUTO_TEST_CASE(p2p_dns_address_expansion_maps_synchronous_resolver_failure) {
   auto runtime = forge::asio::runtime{};
   auto resolver = p2p::detail::dns_address_expander{
       {},
       {.resolve_addresses = [](std::string, dns::address_family, dns::query_options,
                                std::stop_token) -> boost::asio::awaitable<dns::address_response> {
           throw std::runtime_error{"synchronous resolver failure"};
        },
        .resolve_txt = [](std::string, dns::query_options,
                          std::stop_token) -> boost::asio::awaitable<dns::text_response> {
           co_return dns::text_response{};
        }}};

   BOOST_CHECK_THROW(expand(runtime, resolver, "/dns/synchronous-failure.test/tcp/4001"),
                     p2p::exceptions::internal);
}

BOOST_AUTO_TEST_CASE(p2p_dnsaddr_expansion_preserves_suffix_and_filters_expected_peer) {
   const auto known = p2p::peer_id::from_string("QmcgpsyWgH8Y8ajJz1Cu72KnS5uo2Aa2LpzU7kinSupNKC");
   const auto other = p2p::peer_id::from_string("QmNLfbof5rLekrACjeuLk9JmGZD2HDBHCU4z16iYKmx5SE");
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<lookup_script>();
   script->text_responses.emplace(
       "_dnsaddr.bootstrap.test",
       dns::text_response{.answers = {text_record("dnsaddr=/ip4/192.0.2.21/tcp/4001/p2p/" + other.to_string()),
                                      text_record("dnsaddr=/ip4/192.0.2.22/tcp/4001"),
                                      text_record("dnsaddr=/ip4/192.0.2.23/tcp/4001/p2p/" + known.to_string())}});
   script->text_responses.emplace(
       "_dnsaddr.anonymous.test",
       dns::text_response{.answers = {text_record("dnsaddr=/ip6/2001:db8::21/udp/4001/quic-v1/p2p/" + other.to_string()),
                                      text_record("dnsaddr=/ip6/2001:db8::22/udp/4001/quic-v1")}});
   auto resolver = make_expander(script);

   const auto filtered = expand(runtime, resolver, "/dnsaddr/bootstrap.test/p2p/" + known.to_string(), known);
   BOOST_REQUIRE_EQUAL(filtered.size(), 1U);
   BOOST_TEST(filtered.front().to_string() == "/ip4/192.0.2.23/tcp/4001/p2p/" + known.to_string());

   const auto anonymous = expand(runtime, resolver, "/dnsaddr/anonymous.test", known);
   BOOST_REQUIRE_EQUAL(anonymous.size(), 1U);
   BOOST_TEST(anonymous.front().to_string() == "/ip6/2001:db8::22/udp/4001/quic-v1");
   BOOST_TEST(!anonymous.front().peer.has_value());
}

BOOST_AUTO_TEST_CASE(p2p_dnsaddr_expansion_preserves_a_nonleading_prefix) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<lookup_script>();
   script->text_responses.emplace(
       "_dnsaddr.prefixed.test", dns::text_response{.answers = {text_record("dnsaddr=/tcp/4001")}});
   auto resolver = make_expander(script);

   const auto values = expand(runtime, resolver, "/ip4/192.0.2.70/dnsaddr/prefixed.test");
   BOOST_REQUIRE_EQUAL(values.size(), 1U);
   BOOST_TEST(values.front().to_string() == "/ip4/192.0.2.70/tcp/4001");
}

BOOST_AUTO_TEST_CASE(p2p_dnsaddr_expansion_rejects_nested_suffix_mismatch) {
   const auto known = p2p::peer_id::from_string("QmcgpsyWgH8Y8ajJz1Cu72KnS5uo2Aa2LpzU7kinSupNKC");
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<lookup_script>();
   script->text_responses.emplace(
       "_dnsaddr.outer.test",
       dns::text_response{.answers = {text_record("dnsaddr=/dnsaddr/inner.test/p2p/" + known.to_string())}});
   script->text_responses.emplace(
       "_dnsaddr.inner.test", dns::text_response{.answers = {text_record("dnsaddr=/ip4/192.0.2.80/tcp/4001")}});
   auto resolver = make_expander(script);

   BOOST_CHECK_THROW(expand(runtime, resolver, "/dnsaddr/outer.test/p2p/" + known.to_string(), known),
                     p2p::exceptions::invalid_options);
   BOOST_REQUIRE_EQUAL(script->text_requests.size(), 2U);
   BOOST_TEST(script->text_requests[0] == "_dnsaddr.outer.test");
   BOOST_TEST(script->text_requests[1] == "_dnsaddr.inner.test");
}

BOOST_AUTO_TEST_CASE(p2p_dnsaddr_skips_malformed_records_and_deduplicates_results) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<lookup_script>();
   script->text_responses.emplace(
       "_dnsaddr.records.test",
       dns::text_response{.answers = {text_record("not-a-dnsaddr-record"), text_record("dnsaddr=not-a-multiaddr"),
                                      text_record("dnsaddr=/ip4/192.0.2.30/tcp/4001"),
                                      text_record("dnsaddr=/ip4/192.0.2.30/tcp/4001")}});
   auto resolver = make_expander(script);

   const auto values = expand(runtime, resolver, "/dnsaddr/records.test");
   BOOST_REQUIRE_EQUAL(values.size(), 1U);
   BOOST_TEST(values.front().to_string() == "/ip4/192.0.2.30/tcp/4001");
}

BOOST_AUTO_TEST_CASE(p2p_dnsaddr_skips_interface_scoped_txt_candidates) {
   auto runtime = forge::asio::runtime{};

   {
      auto script = std::make_shared<lookup_script>();
      script->text_responses.emplace(
          "_dnsaddr.mixed-zone.test",
          dns::text_response{.answers = {text_record("dnsaddr=/ip6zone/remote0/ip6/2001:4860::1/tcp/4001"),
                                         text_record("dnsaddr=/ip4/192.0.2.32/tcp/4001")}});
      auto resolver = make_expander(script);
      const auto values = expand(runtime, resolver, "/dnsaddr/mixed-zone.test");
      BOOST_REQUIRE_EQUAL(values.size(), 1U);
      BOOST_TEST(values.front().to_string() == "/ip4/192.0.2.32/tcp/4001");
   }

   {
      auto script = std::make_shared<lookup_script>();
      script->text_responses.emplace(
          "_dnsaddr.zone-only.test",
          dns::text_response{.answers = {text_record("dnsaddr=/ip6zone/remote0/ip6/2001:4860::2/tcp/4001")}});
      auto resolver = make_expander(script);
      BOOST_CHECK_THROW(expand(runtime, resolver, "/dnsaddr/zone-only.test"), p2p::exceptions::invalid_options);
   }

   {
      auto script = std::make_shared<lookup_script>();
      script->text_responses.emplace(
          "_dnsaddr.outer-zone.test",
          dns::text_response{.answers = {text_record("dnsaddr=/dnsaddr/inner-zone.test")}});
      script->text_responses.emplace(
          "_dnsaddr.inner-zone.test",
          dns::text_response{.answers = {text_record("dnsaddr=/ip6zone/remote0/ip6/2001:4860::3/tcp/4001"),
                                         text_record("dnsaddr=/ip4/192.0.2.33/tcp/4001")}});
      auto resolver = make_expander(script);
      const auto values = expand(runtime, resolver, "/dnsaddr/outer-zone.test");
      BOOST_REQUIRE_EQUAL(values.size(), 1U);
      BOOST_TEST(values.front().to_string() == "/ip4/192.0.2.33/tcp/4001");
      BOOST_REQUIRE_EQUAL(script->text_requests.size(), 2U);
   }

   {
      auto resolver = make_expander(std::make_shared<lookup_script>());
      const auto values = expand(runtime, resolver, "/ip6zone/local0/ip6/fe80::1/tcp/4001");
      BOOST_REQUIRE_EQUAL(values.size(), 1U);
      BOOST_TEST(values.front().to_string() == "/ip6zone/local0/ip6/fe80::1/tcp/4001");
   }
}

BOOST_AUTO_TEST_CASE(p2p_dnsaddr_scoped_child_cannot_share_literal_cache_or_provenance) {
   auto runtime = forge::asio::runtime{};
   const auto literal = std::string{"/ip6zone/local0/ip6/2001:4860::1/tcp/4001"};
   const auto dns_root = std::string{"/ip6zone/local0/dnsaddr/scoped-cache.test"};
   for (const auto literal_first : {false, true}) {
      BOOST_TEST_CONTEXT("literal first=" << literal_first) {
         auto script = std::make_shared<lookup_script>();
         script->text_responses.emplace("_dnsaddr.scoped-cache.test",
             dns::text_response{.answers = {text_record("dnsaddr=/ip6/2001:4860::1/tcp/4001")}});
         auto resolver = make_expander(script);
         const auto roots = literal_first ? std::vector<std::string>{literal, dns_root}
                                          : std::vector<std::string>{dns_root, literal};
         const auto result = expand_roots_result(runtime, resolver, roots);
         BOOST_REQUIRE_EQUAL(result.targets.size(), 1U);
         BOOST_TEST(result.targets.front().concrete.to_string() == literal);
         BOOST_REQUIRE_EQUAL(result.targets.front().root_indices.size(), 1U);
         BOOST_TEST(result.targets.front().root_indices.front() == (literal_first ? 0U : 1U));
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_dnsaddr_filters_raw_records_before_per_lookup_candidate_limit) {
   const auto known = p2p::peer_id::from_string("QmcgpsyWgH8Y8ajJz1Cu72KnS5uo2Aa2LpzU7kinSupNKC");
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<lookup_script>();
   auto response = dns::text_response{};
   for (auto index = std::size_t{0}; index < 17; ++index) {
      response.answers.push_back(text_record("not-a-dnsaddr-record"));
   }
   response.answers.push_back(text_record("dnsaddr=/ip4/192.0.2.31/tcp/4001/p2p/" + known.to_string()));
   script->text_responses.emplace("_dnsaddr.filtered.test", std::move(response));
   auto resolver = make_expander(script);

   const auto values = expand(runtime, resolver, "/dnsaddr/filtered.test/p2p/" + known.to_string(), known);
   BOOST_REQUIRE_EQUAL(values.size(), 1U);
   BOOST_TEST(values.front().to_string() == "/ip4/192.0.2.31/tcp/4001/p2p/" + known.to_string());
   BOOST_REQUIRE_EQUAL(script->text_options.size(), 1U);
   BOOST_TEST(script->text_options.front().max_answers > 16U);
   BOOST_TEST(script->text_options.front().max_answers <= 4096U);
   BOOST_TEST(script->text_options.front().max_record_bytes <= 65536U);
   BOOST_TEST(script->text_options.front().max_total_answer_bytes <= 1024U * 1024U);
}

BOOST_AUTO_TEST_CASE(p2p_dnsaddr_truncates_matching_candidates_per_lookup_in_dns_order) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<lookup_script>();
   auto response = dns::text_response{};
   for (auto index = std::size_t{0}; index < 18; ++index) {
      response.answers.push_back(
          text_record("dnsaddr=/ip4/192.0.2." + std::to_string(100 + index) + "/tcp/4001"));
   }
   script->text_responses.emplace("_dnsaddr.truncated.test", std::move(response));
   auto resolver = make_expander(script);

   const auto values = expand(runtime, resolver, "/dnsaddr/truncated.test");
   BOOST_REQUIRE_EQUAL(values.size(), 16U);
   BOOST_TEST(values.front().to_string() == "/ip4/192.0.2.100/tcp/4001");
   BOOST_TEST(values.back().to_string() == "/ip4/192.0.2.115/tcp/4001");
}

BOOST_AUTO_TEST_CASE(p2p_dns_address_expansion_enforces_cycle_and_global_bounds) {
   auto runtime = forge::asio::runtime{};

   {
      auto script = std::make_shared<lookup_script>();
      script->text_responses.emplace("_dnsaddr.cycle.test",
                                     dns::text_response{.answers = {text_record("dnsaddr=/dnsaddr/cycle.test")}});
      auto resolver = make_expander(script);
      BOOST_CHECK_THROW(expand(runtime, resolver, "/dnsaddr/cycle.test"), p2p::exceptions::invalid_options);
      BOOST_REQUIRE_EQUAL(script->text_requests.size(), 1U);
   }

   {
      auto script = std::make_shared<lookup_script>();
      script->text_responses.emplace("_dnsaddr.first.test",
                                     dns::text_response{.answers = {text_record("dnsaddr=/dnsaddr/second.test")}});
      auto policy = p2p::address_resolution::policy{};
      policy.bounds.max_recursion_depth = 1;
      auto resolver = make_expander(script, policy);
      BOOST_CHECK_THROW(expand(runtime, resolver, "/dnsaddr/first.test"), p2p::exceptions::invalid_options);
   }

   {
      auto script = std::make_shared<lookup_script>();
      script->text_responses.emplace("_dnsaddr.lookup.test",
                                     dns::text_response{.answers = {text_record("dnsaddr=/dnsaddr/next.test")}});
      auto policy = p2p::address_resolution::policy{};
      policy.bounds.max_dns_lookups = 1;
      auto resolver = make_expander(script, policy);
      BOOST_CHECK_THROW(expand(runtime, resolver, "/dnsaddr/lookup.test"), p2p::exceptions::invalid_options);
      BOOST_REQUIRE_EQUAL(script->text_requests.size(), 1U);
   }

   {
      auto script = std::make_shared<lookup_script>();
      script->text_responses.emplace("_dnsaddr.txt-limit.test",
                                     dns::text_response{.answers = {text_record("dnsaddr=/ip4/192.0.2.40/tcp/4001"),
                                                                      text_record("dnsaddr=/ip4/192.0.2.41/tcp/4001")}});
      auto policy = p2p::address_resolution::policy{};
      policy.bounds.max_txt_records = 1;
      auto resolver = make_expander(script, policy);
      const auto values = expand(runtime, resolver, "/dnsaddr/txt-limit.test");
      BOOST_REQUIRE_EQUAL(values.size(), 1U);
      BOOST_TEST(values.front().to_string() == "/ip4/192.0.2.40/tcp/4001");
   }

   {
      auto script = std::make_shared<lookup_script>();
      script->address_responses.emplace(address_key{"many.test", dns::address_family::any},
                                        addresses({"192.0.2.50", "192.0.2.51"}));
      auto policy = p2p::address_resolution::policy{};
      policy.bounds.max_resolved_addresses = 1;
      auto resolver = make_expander(script, policy);
      BOOST_CHECK_THROW(expand(runtime, resolver, "/dns/many.test/tcp/4001"), p2p::exceptions::invalid_options);
   }

   {
      auto script = std::make_shared<lookup_script>();
      script->address_responses.emplace(address_key{"a", dns::address_family::any}, addresses({"2001:db8::50"}));
      auto policy = p2p::address_resolution::policy{};
      policy.bounds.max_multiaddr_size = 10;
      auto resolver = make_expander(script, policy);
      BOOST_CHECK_THROW(expand(runtime, resolver, "/dns/a/tcp/1"), p2p::exceptions::invalid_options);
   }

   {
      auto script = std::make_shared<lookup_script>();
      auto policy = p2p::address_resolution::policy{};
      policy.bounds.max_txt_records = 17;
      BOOST_CHECK_THROW((void)make_expander(script, policy), p2p::exceptions::invalid_options);
   }

   {
      auto script = std::make_shared<lookup_script>();
      auto policy = p2p::address_resolution::policy{};
      policy.bounds.max_multiaddr_size = 4097;
      BOOST_CHECK_THROW((void)make_expander(script, policy), p2p::exceptions::invalid_options);
   }

   {
      auto script = std::make_shared<lookup_script>();
      auto policy = p2p::address_resolution::policy{};
      policy.bounds.max_resolved_addresses = 101;
      BOOST_CHECK_THROW((void)make_expander(script, policy), p2p::exceptions::invalid_options);
   }
}

BOOST_AUTO_TEST_CASE(p2p_dns_address_resolution_uses_immutable_donor_ceilings) {
   auto script = std::make_shared<lookup_script>();
   auto boundary = p2p::address_resolution::policy{};
   boundary.bounds.max_dns_lookups = 32;
   boundary.bounds.max_txt_records = 16;
   boundary.bounds.max_resolved_addresses = 100;
   boundary.bounds.max_recursion_depth = 4;
   boundary.bounds.max_multiaddr_size = 4096;
   BOOST_CHECK_NO_THROW((void)make_expander(script, boundary));

   auto lookups = boundary;
   lookups.bounds.max_dns_lookups = 33;
   BOOST_CHECK_THROW((void)make_expander(script, lookups), p2p::exceptions::invalid_options);

   auto depth = boundary;
   depth.bounds.max_recursion_depth = 5;
   BOOST_CHECK_THROW((void)make_expander(script, depth), p2p::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(p2p_dns_address_resolution_uses_forge_root_ceiling) {
   auto script = std::make_shared<lookup_script>();
   auto boundary = p2p::address_resolution::policy{};
   boundary.bounds.max_roots = 100;
   BOOST_CHECK_NO_THROW((void)make_expander(script, boundary));

   boundary.bounds.max_roots = 101;
   BOOST_CHECK_THROW((void)make_expander(script, boundary), p2p::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(p2p_dns_address_resolution_maps_bounded_dns_errors) {
   auto runtime = forge::asio::runtime{};

   {
      auto script = std::make_shared<lookup_script>();
      script->address_failures.emplace(address_key{"limited.test", dns::address_family::any},
                                       dns::exceptions::code::resource_limit);
      auto resolver = make_expander(script);
      BOOST_CHECK_THROW(expand(runtime, resolver, "/dns/limited.test/tcp/4001"),
                        p2p::exceptions::backpressure_rejected);
   }

   {
      auto script = std::make_shared<lookup_script>();
      script->address_failures.emplace(address_key{"invalid.test", dns::address_family::any},
                                       dns::exceptions::code::invalid_options);
      auto resolver = make_expander(script);
      BOOST_CHECK_THROW(expand(runtime, resolver, "/dns/invalid.test/tcp/4001"), p2p::exceptions::invalid_options);
   }
}

BOOST_AUTO_TEST_CASE(p2p_dns_address_resolution_retains_branch_local_dns_failures) {
   auto runtime = forge::asio::runtime{};

   {
      auto script = std::make_shared<lookup_script>();
      script->text_responses.emplace(
          "_dnsaddr.mixed.test",
          dns::text_response{.answers = {text_record("dnsaddr=/dns/missing.test/tcp/4001"),
                                         text_record("dnsaddr=/ip4/192.0.2.90/tcp/4001")}});
      script->address_failures.emplace(address_key{"missing.test", dns::address_family::any},
                                       dns::exceptions::code::not_found);
      auto resolver = make_expander(script);
      const auto values = expand(runtime, resolver, "/dnsaddr/mixed.test");
      BOOST_REQUIRE_EQUAL(values.size(), 1U);
      BOOST_TEST(values.front().to_string() == "/ip4/192.0.2.90/tcp/4001");
   }

   {
      auto script = std::make_shared<lookup_script>();
      script->text_responses.emplace(
          "_dnsaddr.all-failed.test",
          dns::text_response{.answers = {text_record("dnsaddr=/dns/missing.test/tcp/4001"),
                                         text_record("dnsaddr=/dns/transient.test/tcp/4001")}});
      script->address_failures.emplace(address_key{"missing.test", dns::address_family::any},
                                       dns::exceptions::code::not_found);
      script->address_failures.emplace(address_key{"transient.test", dns::address_family::any},
                                       dns::exceptions::code::temporary_failure);
      auto resolver = make_expander(script);
      BOOST_CHECK_THROW(expand(runtime, resolver, "/dnsaddr/all-failed.test"), p2p::exceptions::temporary_failure);
   }

   {
      auto script = std::make_shared<lookup_script>();
      script->address_failures.emplace(address_key{"missing.test", dns::address_family::any},
                                       dns::exceptions::code::not_found);
      auto resolver = make_expander(script);
      BOOST_CHECK_THROW(expand(runtime, resolver, "/dns/missing.test/tcp/4001"), p2p::exceptions::peer_not_found);
   }
}

BOOST_AUTO_TEST_CASE(p2p_dns_address_expansion_preserves_timeout_and_cancellation_and_rejects_all_invalid) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<lookup_script>();
   auto resolver = make_expander(script);

   auto stopped = std::stop_source{};
   stopped.request_stop();
   BOOST_CHECK_THROW(expand(runtime, resolver, "/dns/canceled.test/tcp/4001", {}, stopped.get_token()),
                     p2p::exceptions::canceled);
   BOOST_CHECK_THROW(expand(runtime, resolver, "/dns/expired.test/tcp/4001", {}, {},
                            std::chrono::steady_clock::now() - std::chrono::milliseconds{1}),
                     p2p::exceptions::timeout);
   BOOST_CHECK(script->address_requests.empty());

   script->text_responses.emplace(
       "_dnsaddr.invalid.test",
       dns::text_response{.answers = {text_record("dnsaddr=not-a-multiaddr"), text_record("not-a-dnsaddr-record")}});
   BOOST_CHECK_THROW(expand(runtime, resolver, "/dnsaddr/invalid.test"), p2p::exceptions::invalid_options);
   BOOST_CHECK_THROW(expand(runtime, resolver, "/ip4/192.0.2.60/udp/4001"), p2p::exceptions::invalid_options);
   BOOST_CHECK_THROW(expand(runtime, resolver, "/ip4/192.0.2.60/tcp/4001/ws"),
                     p2p::exceptions::unsupported_protocol);
}
