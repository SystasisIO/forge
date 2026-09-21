module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/address.hpp>

module forge.net.p2p.node;

import forge.exceptions;
import forge.multiformats.exceptions;
import forge.multiformats.multiaddr;
import forge.net.dns.exceptions;
import forge.net.dns.resolver;
import forge.net.dns.types;
import forge.net.p2p.address_resolution;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;

#include "details/dns_address_expander.hxx"
#include "details/host_addresses.hxx"

namespace forge::net::p2p::detail {

dns_address_expansion_operation::dns_address_expansion_operation(
    address_resolution::policy policy_value, dns_address_expander::resolver_callbacks callbacks_value,
    std::optional<peer_id> expected_peer_value)
    : policy(std::move(policy_value)), callbacks(std::move(callbacks_value)), expected_peer(std::move(expected_peer_value)),
      state{.limits = policy.bounds} {}

namespace {

namespace dns = forge::net::dns;
namespace multiformats = forge::multiformats;

using protocol_code = multiformats::protocol_code;
using multiaddr = multiformats::multiaddr;
using multiaddr_component = multiformats::multiaddr_component;

[[nodiscard]] boost::asio::awaitable<dns::address_response>
async_resolve_address_callback(dns::resolver* resolver, std::string name, dns::address_family family,
                               dns::query_options options, std::stop_token stop) {
   co_return co_await resolver->async_resolve_addresses(std::move(name), family, std::move(options), stop);
}

[[nodiscard]] boost::asio::awaitable<dns::text_response>
async_resolve_txt_callback(dns::resolver* resolver, std::string name, dns::query_options options, std::stop_token stop) {
   co_return co_await resolver->async_resolve_txt(std::move(name), std::move(options), stop);
}

constexpr auto dnsaddr_prefix_size = std::string_view{"dnsaddr="}.size();
constexpr auto forge_max_address_roots = std::size_t{100};
constexpr auto max_dns_lookups = std::size_t{32};
constexpr auto max_dns_txt_answers = std::size_t{4096};
constexpr auto max_dnsaddr_candidates_per_lookup = std::size_t{16};
constexpr auto max_dns_record_bytes = std::size_t{65536};
constexpr auto max_dns_total_answer_bytes = std::size_t{1024 * 1024};
constexpr auto max_resolved_addresses = std::size_t{100};
constexpr auto max_recursion_depth = std::size_t{4};
constexpr auto max_multiaddr_size = std::size_t{4096};

[[noreturn]] void throw_invalid_options(std::string message) {
   FORGE_THROW_EXCEPTION(exceptions::invalid_options, std::move(message));
}

[[noreturn]] void throw_unsupported_protocol(std::string message) {
   FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, std::move(message));
}

[[noreturn]] void throw_timeout() {
   FORGE_THROW_EXCEPTION(exceptions::timeout, "P2P DNS address resolution deadline expired");
}

[[noreturn]] void throw_canceled() {
   FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P DNS address resolution canceled");
}

void validate_policy_value(const address_resolution::policy& value) {
   const auto& bounds = value.bounds;
   if (bounds.max_roots == 0 || bounds.max_dns_lookups == 0 || bounds.max_txt_records == 0 ||
       bounds.max_resolved_addresses == 0 ||
       bounds.max_recursion_depth == 0 || bounds.max_multiaddr_size == 0) {
      throw_invalid_options("P2P DNS address resolution limits must be greater than zero");
   }
   if (bounds.max_txt_records > max_dnsaddr_candidates_per_lookup) {
      throw_invalid_options("P2P DNS address resolution TXT candidate limit exceeds donor ceiling");
   }
   if (bounds.max_roots > forge_max_address_roots) {
      throw_invalid_options("P2P DNS address resolution root limit exceeds the Forge operation ceiling");
   }
   if (bounds.max_dns_lookups > max_dns_lookups || bounds.max_resolved_addresses > max_resolved_addresses ||
       bounds.max_recursion_depth > max_recursion_depth || bounds.max_multiaddr_size > max_multiaddr_size) {
      throw_invalid_options("P2P DNS address resolution limit exceeds immutable donor ceiling");
   }
   if (bounds.max_multiaddr_size > max_dns_record_bytes - dnsaddr_prefix_size) {
      throw_invalid_options("P2P DNS address resolution multiaddr size exceeds DNS record ceiling");
   }

   const auto max_record_bytes = bounds.max_multiaddr_size + dnsaddr_prefix_size;
   if (bounds.max_resolved_addresses > max_dns_total_answer_bytes / max_record_bytes) {
      throw_invalid_options("P2P DNS address resolution aggregate answer limit exceeds DNS ceiling");
   }
}

void check_cancellation(std::chrono::steady_clock::time_point deadline, std::stop_token stop) {
   if (stop.stop_requested()) {
      throw_canceled();
   }
   if (std::chrono::steady_clock::now() >= deadline) {
      throw_timeout();
   }
}

[[nodiscard]] std::size_t checked_add(std::size_t left, std::size_t right, std::string_view context) {
   if (right > std::numeric_limits<std::size_t>::max() - left) {
      throw_invalid_options("P2P DNS address resolution size overflow while " + std::string{context});
   }
   return left + right;
}

[[nodiscard]] std::size_t checked_multiply(std::size_t left, std::size_t right, std::string_view context) {
   if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
      throw_invalid_options("P2P DNS address resolution size overflow while " + std::string{context});
   }
   return left * right;
}

[[nodiscard]] std::size_t varint_size(std::size_t value) noexcept {
   auto size = std::size_t{1};
   while (value >= 0x80U) {
      value >>= 7U;
      ++size;
   }
   return size;
}

[[nodiscard]] std::size_t component_size(const multiaddr_component& component) {
   auto result = varint_size(multiformats::code_value(component.code));
   switch (component.code) {
   case protocol_code::ip4:
      return checked_add(result, 4, "sizing an IPv4 component");
   case protocol_code::ip6:
      return checked_add(result, 16, "sizing an IPv6 component");
   case protocol_code::tcp:
   case protocol_code::udp:
      return checked_add(result, 2, "sizing a transport component");
   case protocol_code::dns:
   case protocol_code::dns4:
   case protocol_code::dns6:
   case protocol_code::dnsaddr:
   case protocol_code::ip6zone:
   case protocol_code::p2p:
      // For p2p, the text form is a conservative upper bound on decoded bytes.
      return checked_add(result, checked_add(varint_size(component.value.size()), component.value.size(),
                                             "sizing a variable component"),
                         "sizing a variable component");
   case protocol_code::p2p_circuit:
   case protocol_code::quic:
   case protocol_code::quic_v1:
   case protocol_code::ws:
   case protocol_code::wss:
      return result;
   }
   throw_unsupported_protocol("unsupported multiaddr protocol during DNS address resolution");
}

void ensure_size_at_most(const std::vector<multiaddr_component>& components, std::size_t maximum) {
   auto total = std::size_t{0};
   for (const auto& component : components) {
      total = checked_add(total, component_size(component), "sizing a multiaddr");
      if (total > maximum) {
         throw_invalid_options("P2P DNS address resolution multiaddr exceeds configured size limit");
      }
   }
}

void ensure_joined_size_at_most(const std::vector<multiaddr_component>& prefix,
                                const std::vector<multiaddr_component>& candidate,
                                const std::vector<multiaddr_component>& suffix, std::size_t maximum) {
   auto total = std::size_t{0};
   const auto append = [&](const std::vector<multiaddr_component>& values) {
      for (const auto& component : values) {
         total = checked_add(total, component_size(component), "sizing a multiaddr");
         if (total > maximum) {
            throw_invalid_options("P2P DNS address resolution multiaddr exceeds configured size limit");
         }
      }
   };
   append(prefix);
   append(candidate);
   append(suffix);
}

[[nodiscard]] multiaddr replace_component(const multiaddr& source, std::size_t index, multiaddr_component replacement,
                                          std::size_t maximum) {
   const auto& components = source.components();
   auto projected = std::vector<multiaddr_component>{};
   projected.reserve(components.size());
   for (std::size_t position = 0; position < components.size(); ++position) {
      projected.push_back(position == index ? replacement : components[position]);
   }
   ensure_size_at_most(projected, maximum);

   auto result = multiaddr{};
   for (auto& component : projected) {
      result.push(std::move(component));
   }
   return result;
}

[[nodiscard]] bool ends_with_components(const std::vector<multiaddr_component>& value,
                                        const std::vector<multiaddr_component>& suffix) noexcept {
   if (suffix.size() > value.size()) {
      return false;
   }
   return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
}

[[nodiscard]] std::vector<multiaddr_component> component_slice(const std::vector<multiaddr_component>& value,
                                                                std::size_t begin, std::size_t end) {
   auto result = std::vector<multiaddr_component>{};
   result.reserve(end - begin);
   result.insert(result.end(), value.begin() + static_cast<std::ptrdiff_t>(begin),
                 value.begin() + static_cast<std::ptrdiff_t>(end));
   return result;
}

[[nodiscard]] std::vector<multiaddr_component>
remove_suffix_components(const std::vector<multiaddr_component>& value, const std::vector<multiaddr_component>& suffix) {
   return component_slice(value, 0, value.size() - suffix.size());
}

[[nodiscard]] multiaddr join_components(const std::vector<multiaddr_component>& prefix,
                                        const std::vector<multiaddr_component>& candidate,
                                        const std::vector<multiaddr_component>& suffix, std::size_t maximum) {
   ensure_joined_size_at_most(prefix, candidate, suffix, maximum);
   auto result = multiaddr{};
   for (const auto& component : prefix) {
      result.push(component);
   }
   for (const auto& component : candidate) {
      result.push(component);
   }
   for (const auto& component : suffix) {
      result.push(component);
   }
   return result;
}

[[nodiscard]] std::optional<std::pair<std::size_t, protocol_code>>
first_unresolved_component(const multiaddr& value) {
   const auto& components = value.components();
   for (std::size_t index = 0; index < components.size(); ++index) {
      switch (components[index].code) {
      case protocol_code::dns:
      case protocol_code::dns4:
      case protocol_code::dns6:
      case protocol_code::dnsaddr:
         return std::pair{index, components[index].code};
      default:
         break;
      }
   }
   return std::nullopt;
}

[[nodiscard]] dns::address_family family_for(protocol_code value) {
   switch (value) {
   case protocol_code::dns:
      return dns::address_family::any;
   case protocol_code::dns4:
      return dns::address_family::ipv4;
   case protocol_code::dns6:
      return dns::address_family::ipv6;
   default:
      throw_unsupported_protocol("multiaddr protocol is not an address DNS protocol");
   }
}

[[nodiscard]] bool accepts_family(const boost::asio::ip::address& value, dns::address_family family) noexcept {
   switch (family) {
   case dns::address_family::any:
      return true;
   case dns::address_family::ipv4:
      return value.is_v4();
   case dns::address_family::ipv6:
      return value.is_v6();
   }
   return false;
}

[[nodiscard]] dns::query_options query_options(const address_resolution::limits& limits,
                                                std::chrono::steady_clock::time_point deadline,
                                                std::size_t max_answers) {
   const auto max_record_bytes = checked_add(limits.max_multiaddr_size, dnsaddr_prefix_size, "sizing DNS records");
   if (max_answers == 0 || max_answers > max_dns_total_answer_bytes / max_record_bytes) {
      throw_invalid_options("P2P DNS address resolution aggregate answer limit exceeds DNS ceiling");
   }
   const auto max_total_answer_bytes = checked_multiply(max_answers, max_record_bytes, "sizing DNS answer bytes");
   return dns::query_options{
       .deadline = deadline,
       .max_answers = max_answers,
       .max_cnames = std::min<std::size_t>(8, max_answers),
       .max_record_bytes = max_record_bytes,
       .max_total_answer_bytes = max_total_answer_bytes,
   };
}

[[nodiscard]] std::size_t raw_txt_answer_limit(const address_resolution::limits& limits) {
   const auto max_record_bytes = checked_add(limits.max_multiaddr_size, dnsaddr_prefix_size, "sizing DNS records");
   return std::min(max_dns_txt_answers, max_dns_total_answer_bytes / max_record_bytes);
}

void consume_lookup(dns_address_expansion_state& state) {
   if (state.dns_lookups == state.limits.max_dns_lookups) {
      throw_invalid_options("P2P DNS address resolution exceeded configured DNS lookup limit");
   }
   ++state.dns_lookups;
}

[[nodiscard]] bool candidate_peer_matches(const multiaddr& value, const std::optional<peer_id>& expected) {
   if (!expected) {
      return true;
   }
   for (const auto& component : value.components()) {
      if (component.code != protocol_code::p2p) {
         continue;
      }
      try {
         if (peer_id::from_string(component.value) != *expected) {
            return false;
         }
      } catch (const forge::exceptions::base&) {
         return false;
      }
   }
   return true;
}

void merge_root_indices(std::vector<std::size_t>& destination, const std::vector<std::size_t>& source) {
   auto merged = std::vector<std::size_t>{};
   merged.reserve(destination.size() + source.size());
   std::set_union(destination.begin(), destination.end(), source.begin(), source.end(), std::back_inserter(merged));
   destination = std::move(merged);
}

void append_candidate_endpoint(std::vector<endpoint>& destination, const endpoint& value) {
   const auto key = value.to_string();
   if (std::any_of(destination.begin(), destination.end(), [&key](const auto& current) {
          return current.to_string() == key;
       })) {
      return;
   }
   destination.push_back(value);
}

void append_resolved_target(dns_address_expansion_state& state, const endpoint& value,
                            const std::vector<std::size_t>& root_indices) {
   const auto key = value.to_string();
   if (const auto found = state.target_indices.find(key); found != state.target_indices.end()) {
      merge_root_indices(state.targets[found->second].root_indices, root_indices);
      return;
   }
   if (state.targets.size() == state.limits.max_resolved_addresses) {
      throw_invalid_options("P2P DNS address resolution exceeded configured result limit");
   }
   state.target_indices.emplace(key, state.targets.size());
   state.targets.push_back({.concrete = value, .root_indices = root_indices});
}

void append_resolved_targets(dns_address_expansion_state& state, const std::vector<endpoint>& values,
                             const std::vector<std::size_t>& root_indices) {
   for (const auto& value : values) {
      append_resolved_target(state, value, root_indices);
   }
}

void append_endpoint(dns_address_expansion_state& state, dns_address_candidate_state& candidate,
                     const multiaddr& value, const std::optional<peer_id>& expected, bool discovered) {
   if (discovered && host_addresses::has_interface_zone(value)) {
      return;
   }
   auto parsed = std::optional<endpoint>{};
   try {
      parsed.emplace(parse_endpoint(value.to_string()));
   } catch (const forge::exceptions::base&) {
      if (discovered) {
         return;
      }
      throw_invalid_options("P2P DNS address resolution source is not a valid P2P endpoint");
   }

   if (!parsed->is_direct_tcp() && !parsed->is_direct_quic()) {
      if (discovered) {
         return;
      }
      throw_unsupported_protocol("P2P DNS address resolution requires a direct TCP or QUIC endpoint");
   }
   if (parsed->transport.host_type != endpoint::host_kind::ip4 && parsed->transport.host_type != endpoint::host_kind::ip6) {
      if (discovered) {
         return;
      }
      throw_invalid_options("P2P DNS address resolution left an unresolved endpoint host");
   }
   if (expected && parsed->peer && *parsed->peer != *expected) {
      return;
   }
   append_candidate_endpoint(candidate.endpoints, *parsed);
   append_resolved_target(state, *parsed, candidate.root_indices);
}

void record_branch_failure(dns_address_expansion_state& state, dns_address_branch_failure value) noexcept {
   if (static_cast<int>(value) > static_cast<int>(state.strongest_branch_failure)) {
      state.strongest_branch_failure = value;
   }
}

template <typename Response, typename Operation>
[[nodiscard]] boost::asio::awaitable<std::optional<Response>>
perform_lookup(dns_address_expansion_state& state, Operation operation) {
   try {
      co_return co_await operation();
   } catch (const forge::exceptions::base& error) {
      if (dns::exceptions::is(error, dns::exceptions::code::timeout)) {
         throw_timeout();
      }
      if (dns::exceptions::is(error, dns::exceptions::code::canceled)) {
         throw_canceled();
      }
      if (dns::exceptions::is(error, dns::exceptions::code::closed)) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P DNS resolver closed");
      }
      if (dns::exceptions::is(error, dns::exceptions::code::codec_error)) {
         FORGE_THROW_EXCEPTION(exceptions::codec_error, "P2P DNS resolver returned malformed data");
      }
      if (dns::exceptions::is(error, dns::exceptions::code::invalid_options)) {
         throw_invalid_options("P2P DNS resolver rejected bounded lookup options");
      }
      if (dns::exceptions::is(error, dns::exceptions::code::resource_limit)) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P DNS resolver exhausted bounded resources");
      }
      if (dns::exceptions::is(error, dns::exceptions::code::not_found)) {
         record_branch_failure(state, dns_address_branch_failure::not_found);
         co_return std::nullopt;
      }
      if (dns::exceptions::is(error, dns::exceptions::code::temporary_failure)) {
         record_branch_failure(state, dns_address_branch_failure::temporary_failure);
         co_return std::nullopt;
      }
      FORGE_THROW_EXCEPTION(exceptions::internal, "P2P DNS resolver failed internally");
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "P2P DNS resolver failed unexpectedly");
   }
}

[[nodiscard]] std::optional<peer_id>
consistent_expected_peer(const std::vector<multiaddr>& roots, std::optional<peer_id> expected_peer,
                         std::chrono::steady_clock::time_point deadline, std::stop_token stop) {
   for (const auto& root : roots) {
      check_cancellation(deadline, stop);
      for (const auto& component : root.components()) {
         if (component.code != protocol_code::p2p) {
            continue;
         }
         auto embedded_peer = peer_id{};
         try {
            embedded_peer = peer_id::from_string(component.value);
         } catch (const forge::exceptions::base&) {
            throw_invalid_options("P2P DNS address resolution root contains an invalid peer id");
         }
         if (expected_peer && *expected_peer != embedded_peer) {
            throw_invalid_options("P2P DNS address resolution roots disagree on the expected peer");
         }
         expected_peer = std::move(embedded_peer);
      }
   }
   return expected_peer;
}

[[nodiscard]] std::vector<source_root> canonical_source_roots(std::vector<multiaddr> roots) {
   auto result = std::vector<source_root>{};
   auto root_indices = std::map<std::string, std::size_t>{};
   result.reserve(roots.size());
   for (auto index = std::size_t{}; index < roots.size(); ++index) {
      const auto key = roots[index].to_string();
      if (const auto found = root_indices.find(key); found != root_indices.end()) {
         result[found->second].input_indices.push_back(index);
         continue;
      }
      root_indices.emplace(key, result.size());
      result.push_back({.canonical = std::move(roots[index]), .input_indices = {index}});
   }
   return result;
}

[[nodiscard]] boost::asio::awaitable<std::vector<endpoint>>
async_expand_candidate(const std::shared_ptr<dns_address_expansion_operation>& operation, multiaddr candidate,
                       std::vector<std::size_t> root_indices, std::chrono::steady_clock::time_point deadline,
                       std::stop_token stop, std::size_t depth, bool discovered) {
   auto& state = operation->state;
   check_cancellation(deadline, stop);
   const auto active_key = candidate.to_string();
   auto found = state.candidates.try_emplace(active_key).first;
   auto& cached = found->second;
   merge_root_indices(cached.root_indices, root_indices);
   if (cached.completed) {
      append_resolved_targets(state, cached.endpoints, cached.root_indices);
      co_return cached.endpoints;
   }
   if (cached.active) {
      // A recursive revisit cannot resolve until its active ancestor completes.
      // Its roots are already merged above and will be applied at completion.
      co_return std::vector<endpoint>{};
   }
   cached.active = true;
   try {
      const auto unresolved = first_unresolved_component(candidate);
      if (!unresolved) {
         append_endpoint(state, cached, candidate, operation->expected_peer, discovered);
      } else {
         if (depth >= state.limits.max_recursion_depth) {
            throw_invalid_options("P2P DNS address resolution exceeded configured recursion depth");
         }

         const auto [component_index, code] = *unresolved;
         const auto& component = candidate.components()[component_index];
         consume_lookup(state);
         if (code != protocol_code::dnsaddr) {
            const auto family = family_for(code);
            const auto answer_limit = state.limits.max_resolved_addresses;
            auto response = co_await perform_lookup<dns::address_response>(state, [operation, name = component.value,
                                                                                   family,
                                                                                   options = query_options(
                                                                                       operation->policy.bounds,
                                                                                       deadline, answer_limit),
                                                                                   stop]() mutable {
               return operation->callbacks.resolve_addresses(std::move(name), family, std::move(options), stop);
            });
            check_cancellation(deadline, stop);
            if (response) {
               if (response->answers.size() > answer_limit) {
                  throw_invalid_options("P2P DNS address resolution exceeded configured result limit");
               }
               for (const auto& answer : response->answers) {
                  if (!accepts_family(answer.value, family)) {
                     continue;
                  }
                  const auto replacement = multiaddr_component{
                      .code = answer.value.is_v4() ? protocol_code::ip4 : protocol_code::ip6,
                      .value = answer.value.to_string(),
                  };
                  const auto child = co_await async_expand_candidate(
                      operation,
                      replace_component(candidate, component_index, replacement, state.limits.max_multiaddr_size),
                      cached.root_indices, deadline, stop, depth + 1, true);
                  for (const auto& value : child) {
                     append_candidate_endpoint(cached.endpoints, value);
                  }
               }
            }
         } else {
            const auto lookup_name = "_dnsaddr." + component.value;
            const auto raw_answer_limit = raw_txt_answer_limit(operation->policy.bounds);
            auto response = co_await perform_lookup<dns::text_response>(state, [operation, name = lookup_name,
                                                                                options = query_options(
                                                                                    operation->policy.bounds, deadline,
                                                                                    raw_answer_limit),
                                                                                stop]() mutable {
               return operation->callbacks.resolve_txt(std::move(name), std::move(options), stop);
            });
            check_cancellation(deadline, stop);
            if (response) {
               if (response->answers.size() > raw_answer_limit) {
                  throw_invalid_options("P2P DNS address resolution exceeded raw TXT answer limit");
               }

               const auto prefix = component_slice(candidate.components(), 0, component_index);
               const auto suffix = component_slice(candidate.components(), component_index + 1,
                                                   candidate.components().size());
               auto matching_candidates = std::size_t{0};
               auto matching_keys = std::set<std::string>{};
               for (const auto& answer : response->answers) {
                  constexpr auto dnsaddr_prefix = std::string_view{"dnsaddr="};
                  if (answer.value.size() >
                      checked_add(state.limits.max_multiaddr_size, dnsaddr_prefix.size(), "sizing DNS TXT records")) {
                     throw_invalid_options("P2P DNS address resolution TXT record exceeds configured size limit");
                  }
                  if (answer.value.size() <= dnsaddr_prefix.size() ||
                      !std::equal(dnsaddr_prefix.begin(), dnsaddr_prefix.end(), answer.value.begin())) {
                     continue;
                  }
                  auto text = std::string{answer.value.begin() + static_cast<std::ptrdiff_t>(dnsaddr_prefix.size()),
                                          answer.value.end()};
                  if (text.size() > state.limits.max_multiaddr_size) {
                     throw_invalid_options("P2P DNS address resolution TXT candidate exceeds configured size limit");
                  }
                  auto parsed = multiaddr{};
                  try {
                     parsed = multiaddr::parse(text);
                  } catch (const multiformats::exceptions::invalid_format&) {
                     // DNSADDR records are independently supplied candidates; malformed
                     // records must not poison other valid records in the same answer.
                     continue;
                  }
                  if (host_addresses::has_interface_zone(parsed)) {
                     // TXT answers are remote data. Interface scope belongs only to
                     // the local host that supplied a literal root.
                     continue;
                  }
                  ensure_size_at_most(parsed.components(), state.limits.max_multiaddr_size);
                  if (!ends_with_components(parsed.components(), suffix) ||
                      !candidate_peer_matches(parsed, operation->expected_peer)) {
                     continue;
                  }
                  auto expanded = join_components(prefix, remove_suffix_components(parsed.components(), suffix), suffix,
                                                  state.limits.max_multiaddr_size);
                  const auto expanded_key = expanded.to_string();
                  if (matching_candidates == state.limits.max_txt_records ||
                      !matching_keys.insert(expanded_key).second) {
                     continue;
                  }
                  ++matching_candidates;
                  const auto child = co_await async_expand_candidate(operation, std::move(expanded), cached.root_indices,
                                                                     deadline, stop, depth + 1, true);
                  for (const auto& value : child) {
                     append_candidate_endpoint(cached.endpoints, value);
                  }
               }
            }
         }
      }
   } catch (...) {
      cached.active = false;
      throw;
   }
   cached.active = false;
   cached.completed = true;
   append_resolved_targets(state, cached.endpoints, cached.root_indices);
   co_return cached.endpoints;
}

} // namespace

void dns_address_expander::validate_policy(const address_resolution::policy& value) {
   validate_policy_value(value);
}

dns_address_expander::dns_address_expander(address_resolution::policy policy, resolver_callbacks callbacks)
    : policy_(std::move(policy)), callbacks_(std::move(callbacks)) {
   validate_policy(policy_);
   if (!callbacks_.resolve_addresses || !callbacks_.resolve_txt) {
      throw_invalid_options("P2P DNS address expander requires both resolver callbacks");
   }
}

dns_address_expander::dns_address_expander(dns::resolver& resolver, address_resolution::policy policy)
    : dns_address_expander(
          std::move(policy),
          resolver_callbacks{
              .resolve_addresses =
                  [resolver = &resolver](std::string name, dns::address_family family, dns::query_options options,
                                         std::stop_token stop) {
               return async_resolve_address_callback(resolver, std::move(name), family, std::move(options), stop);
            },
              .resolve_txt =
                  [resolver = &resolver](std::string name, dns::query_options options, std::stop_token stop) {
               return async_resolve_txt_callback(resolver, std::move(name), std::move(options), stop);
            },
          }) {}

boost::asio::awaitable<dns_address_expansion_result>
dns_address_expander::async_expand(std::vector<multiaddr> roots, std::optional<peer_id> expected_peer,
                                   std::chrono::steady_clock::time_point deadline, std::stop_token stop) {
   auto policy = policy_;
   auto callbacks = callbacks_;
   return async_expand_owned(std::move(policy), std::move(callbacks), std::move(roots), std::move(expected_peer),
                             deadline, stop);
}

boost::asio::awaitable<dns_address_expansion_result>
dns_address_expander::async_expand(multiaddr root, std::optional<peer_id> expected_peer,
                                   std::chrono::steady_clock::time_point deadline, std::stop_token stop) {
   return async_expand(std::vector<multiaddr>{std::move(root)}, std::move(expected_peer), deadline, stop);
}

boost::asio::awaitable<dns_address_expansion_result>
dns_address_expander::async_expand_owned(address_resolution::policy policy, resolver_callbacks callbacks,
                                         std::vector<multiaddr> roots, std::optional<peer_id> expected_peer,
                                         std::chrono::steady_clock::time_point deadline, std::stop_token stop) {
   check_cancellation(deadline, stop);
   if (roots.empty()) {
      throw_invalid_options("P2P DNS address resolution requires at least one root");
   }
   if (roots.size() > policy.bounds.max_roots) {
      throw_invalid_options("P2P DNS address resolution root count exceeds configured limit");
   }
   for (const auto& root : roots) {
      check_cancellation(deadline, stop);
      ensure_size_at_most(root.components(), policy.bounds.max_multiaddr_size);
   }
   expected_peer = consistent_expected_peer(roots, std::move(expected_peer), deadline, stop);

   auto operation = std::make_shared<dns_address_expansion_operation>(std::move(policy), std::move(callbacks),
                                                                       std::move(expected_peer));
   auto source_roots = canonical_source_roots(std::move(roots));
   for (auto index = std::size_t{}; index < source_roots.size(); ++index) {
      co_await async_expand_candidate(operation, source_roots[index].canonical, {index}, deadline, stop, 0, false);
   }
   check_cancellation(deadline, stop);
   if (operation->state.targets.empty()) {
      if (operation->state.strongest_branch_failure == dns_address_branch_failure::temporary_failure) {
         FORGE_THROW_EXCEPTION(exceptions::temporary_failure, "P2P DNS address resolution temporarily failed");
      }
      if (operation->state.strongest_branch_failure == dns_address_branch_failure::not_found) {
         FORGE_THROW_EXCEPTION(exceptions::peer_not_found, "P2P DNS address resolution found no peer endpoint");
      }
      throw_invalid_options("P2P DNS address resolution produced no direct endpoint");
   }
   co_return dns_address_expansion_result{
       .roots = std::move(source_roots),
       .targets = std::move(operation->state.targets),
       .expected_peer = std::move(operation->expected_peer),
   };
}

} // namespace forge::net::p2p::detail
