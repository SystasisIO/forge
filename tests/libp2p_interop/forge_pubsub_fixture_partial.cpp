#include <algorithm>
#include <array>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <iterator>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>

import forge.asio.notification;
import forge.asio.runtime;
import forge.codec.hex;
import forge.crypto.digest.sha256;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.pubsub;
import forge.variant.value;
import forge.variant.containers;

#include "forge_pubsub_fixture.hxx"

namespace forge::test::libp2p_interop {
namespace {
namespace pubsub = forge::net::p2p::pubsub;
using format = forge_pubsub_partial;
constexpr auto application = "forge.fixture.extensions.application";

std::string short_error(std::string_view text) {
   auto output = std::string{text.substr(0, 512)};
   for (auto& byte : output) {
      if (static_cast<unsigned char>(byte) < 32 || static_cast<unsigned char>(byte) > 126) { byte = '?'; }
   }
   return output.empty() ? "partial application operation failed" : output;
}

forge::variant captured(const std::optional<std::vector<std::uint8_t>>& bytes, std::size_t bound) {
   if (!bytes) { return {}; }
   return forge::variant{forge::codec::hex::encode(std::span<const std::uint8_t>{*bytes}.first(std::min(bytes->size(), bound)))};
}

forge::variant size_of(const std::optional<std::vector<std::uint8_t>>& bytes) {
   return bytes ? forge::variant{bytes->size()} : forge::variant{};
}
}

forge_pubsub_fixture::partial_work::~partial_work() { owner->finish_partial(); }

bool forge_pubsub_fixture::admit_partial_locked() {
   if (_extension != "partial" || _extension_admission_closed || _extension_stop.stop_requested() ||
       _overflow || !_capture_error.empty()) { return false; }
   if (_extension_inputs >= 64 || _extension_work >= 64) {
      _capture_error = "partial application input/work bound exceeded";
      throw std::runtime_error{_capture_error};
   }
   ++_extension_inputs;
   ++_extension_work;
   return true;
}

void forge_pubsub_fixture::finish_partial() noexcept {
   {
      const auto lock = std::scoped_lock{_mutex};
      --_extension_work;
   }
   _extension_drain_notification.notify();
}

void forge_pubsub_fixture::partial_failure(std::string_view error) {
   const auto lock = std::scoped_lock{_mutex};
   if (_capture_error.empty()) { _capture_error = short_error(error); }
}

std::uint8_t forge_pubsub_fixture::received_have_locked() const {
   auto bits = std::uint8_t{};
   for (auto index = 0u; index < 3; ++index) {
      if (_partial.received[index]) { bits |= static_cast<std::uint8_t>(1u << index); }
   }
   return bits;
}

forge::mutable_variant_object forge_pubsub_fixture::partial_fields_locked() const {
   return forge::mutable_variant_object{}("group_id_hex", forge::codec::hex::encode(format::group(_token)))
       ("revision", _partial.local.revision)("have", _partial.local.have)("want", _partial.local.want)
       ("received_have", received_have_locked())("metadata_hex", forge::codec::hex::encode(format::encode(_partial.local)))
       ("group_basis", "case_token_and_fixed_sequence_one_not_content_hash");
}

void forge_pubsub_fixture::offer_partial_locked(std::uint8_t have) {
   if (have > format::mask) { throw std::runtime_error{"partial_offer have outside 0..7"}; }
   if (_extension_admission_closed || _extension_stop.stop_requested()) {
      throw std::runtime_error{"partial application admission closed"};
   }
   auto next = _partial;
   next.initialized = true;
   next.local_have = have;
   for (auto index = std::uint8_t{}; index < 3; ++index) {
      next.generated[index] = (have & (1u << index)) ?
          format::encode(format::part{index, format::expected_part(_token, index)}) : std::vector<std::uint8_t>{};
   }
   const auto available = static_cast<std::uint8_t>(have | received_have_locked());
   if (available != next.local.have && _partial.initialized) {
      if (next.local.revision == std::numeric_limits<std::uint32_t>::max()) {
         throw std::runtime_error{"partial local revision exhausted"};
      }
      ++next.local.revision;
   }
   next.local.have = available;
   next.local.want = format::mask ^ available;
   _partial = std::move(next);
}

bool forge_pubsub_fixture::apply_partial_locked(const forge::net::p2p::peer_id& peer,
                                              const pubsub::partial_message& value, std::uint64_t observation) {
   if (_extension_admission_closed || _extension_stop.stop_requested()) { return false; }
   if (!value.subject || value.subject->value != "forge-pr11:" + _token ||
       !value.group_id || *value.group_id != format::group(_token) || !value.metadata) {
      throw std::runtime_error{"partial topic/group/metadata outside application contract"};
   }
   const auto metadata = format::decode_metadata(*value.metadata);
   auto next = _partial;
   const auto key = peer.to_string();
   if (!next.peers.contains(key) && next.peers.size() >= 16) {
      throw std::runtime_error{"partial peer state bound exceeded"};
   }
   auto& remote = next.peers.try_emplace(key, partial_peer{.peer = peer}).first->second;
   if (remote.remote && (metadata.revision < remote.remote->revision ||
       (metadata.revision == remote.remote->revision && metadata != *remote.remote))) {
      throw std::runtime_error{"older or conflicting same-revision partial metadata"};
   }
   remote.remote = metadata; // Replacement, never accumulated availability.
   remote.sent &= metadata.want;
   next.initialized = true; // Receiving does not create or renew gossip ownership.
   auto applied = false;
   if (value.data) {
      const auto part = format::decode_part(*value.data);
      if (part.data != format::expected_part(_token, part.index) || !(metadata.have & (1u << part.index))) {
         throw std::runtime_error{"partial bytes/metadata do not describe expected part"};
      }
      auto& received = next.received[part.index];
      if (received && received->encoded != *value.data) { throw std::runtime_error{"conflicting received partial bytes"}; }
      if (!received) {
         if (!observation) { throw std::runtime_error{"received part lacks actual input reference"}; }
         received = received_part{*value.data, peer, observation};
         applied = true;
      }
   }
   auto available = next.local_have;
   for (auto index = 0u; index < 3; ++index) {
      if (next.received[index]) { available |= static_cast<std::uint8_t>(1u << index); }
   }
   if (available != next.local.have) {
      if (next.local.revision == std::numeric_limits<std::uint32_t>::max()) {
         throw std::runtime_error{"partial local revision exhausted"};
      }
      ++next.local.revision;
      next.local.have = available;
      next.local.want = format::mask ^ available;
   }
   _partial = std::move(next);
   return applied;
}

void forge_pubsub_fixture::reconstruct_partial_locked() {
   if (_partial.reconstructed || received_have_locked() != format::mask) { return; }
   auto inputs = std::array<std::vector<std::uint8_t>, 3>{};
   auto parts = forge::variants{};
   auto references = forge::variants{};
   auto peers = forge::variants{};
   for (auto index = 0u; index < 3; ++index) {
      const auto& received = *_partial.received[index];
      inputs[index] = received.encoded;
      parts.emplace_back(forge::codec::hex::encode(received.encoded));
      references.emplace_back(received.observation);
      peers.emplace_back(received.peer.to_string());
   }
   const auto payload = format::reconstruct(_token, inputs);
   record_locked("partial_reconstructed", application, forge::mutable_variant_object{}
       ("group_id_hex", forge::codec::hex::encode(format::group(_token)))("parts_hex", std::move(parts))
       ("part_incoming_sequences", std::move(references))("part_peer_ids", std::move(peers))
       ("payload_hex", forge::codec::hex::encode(payload))
       ("payload_sha256", forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{payload}).str())
       ("payload_bytes", payload.size())("authority", "application_reconstruction_not_router_delivery_or_signature"));
   if (_overflow || !_capture_error.empty()) { throw std::runtime_error{"partial reconstruction capture failed"}; }
   _partial.reconstructed = true;
}

std::vector<forge_pubsub_fixture::partial_action> forge_pubsub_fixture::plan_partial_locked(
    const forge::net::p2p::peer_id& peer, std::uint64_t observation, bool gossip) {
   auto actions = std::vector<partial_action>{};
   if (!_partial.initialized || _extension_admission_closed || _extension_stop.stop_requested() ||
       _overflow || !_capture_error.empty()) { return actions; }
   const auto key = peer.to_string();
   if (!_partial.peers.contains(key) && _partial.peers.size() >= 16) {
      throw std::runtime_error{"partial peer state bound exceeded"};
   }
   auto& state = _partial.peers.try_emplace(key, partial_peer{.peer = peer}).first->second;
   auto base = pubsub::partial_message{.subject = pubsub::topic{"forge-pr11:" + _token},
       .group_id = format::group(_token), .metadata = format::encode(_partial.local)};
   const auto wanted = static_cast<std::uint8_t>(!gossip && state.remote ? state.remote->want & _partial.local.have : 0);
   for (auto index = std::uint8_t{}; index < 3; ++index) {
      const auto bit = static_cast<std::uint8_t>(1u << index);
      if (!(wanted & bit) || (state.sent & bit)) { continue; }
      auto value = base;
      value.data = (_partial.local_have & bit) ? _partial.generated[index] : _partial.received[index]->encoded;
      actions.push_back(partial_action{peer, std::move(value), observation, false});
      state.sent |= bit;
   }
   const auto request = !gossip && state.remote && (state.remote->have & _partial.local.want) != 0 &&
       (state.announced_revision != _partial.local.revision || state.requested_revision != state.remote->revision);
   if (actions.empty() && ((gossip && state.announced_revision != _partial.local.revision) || request)) {
      actions.push_back(partial_action{peer, std::move(base), observation, gossip});
   }
   if (!actions.empty()) {
      state.announced_revision = _partial.local.revision;
      if (!gossip && state.remote) { state.requested_revision = state.remote->revision; }
   }
   return actions;
}

boost::asio::awaitable<void> forge_pubsub_fixture::subscribe_partial() {
   auto callbacks = pubsub::partial_options{.requests_partial = true};
   callbacks.receive = [this](pubsub::partial_event event, std::stop_token stop) -> boost::asio::awaitable<void> {
      co_await receive_partial(std::move(event), stop);
   };
   callbacks.gossip = [this](pubsub::partial_gossip_event event, std::stop_token stop) -> boost::asio::awaitable<void> {
      co_await gossip_partial(std::move(event), stop);
   };
   auto token = co_await _node->async_subscribe({"forge-pr11:" + _token},
       [this](pubsub::event event) -> boost::asio::awaitable<pubsub::validation_result> {
          co_return co_await validate_message(std::move(event));
       }, std::move(callbacks));
   const auto lock = std::scoped_lock{_mutex};
   _partial_registration = std::move(token);
}

boost::asio::awaitable<void> forge_pubsub_fixture::partial_offer(std::uint8_t have, std::uint64_t command) {
   auto registration = pubsub::partial_topic{};
   {
      const auto lock = std::scoped_lock{_mutex};
      if (_extension != "partial" || !_partial_registration) { throw std::runtime_error{"partial_offer requires native partial subscription"}; }
      offer_partial_locked(have);
      registration = *_partial_registration;
   }
   auto error = std::string{};
   try { co_await _node->async_advertise_partial(registration, format::group(_token)); }
   catch (const std::exception& failure) { error = short_error(failure.what()); }
   {
      const auto lock = std::scoped_lock{_mutex};
      record_locked("partial_advertise_return", "forge.node.async_advertise_partial", forge::mutable_variant_object{}
          ("command_sequence", command)("topic", registration.subject().value)
          ("group_id_hex", forge::codec::hex::encode(format::group(_token)))
          ("error", error.empty() ? forge::variant{} : forge::variant{error})
          ("authority", "local_group_registration_not_wire_send"));
      if (error.empty()) {
         _partial.owned = true;
         auto fields = partial_fields_locked();
         fields("command_sequence", command)("local_have", have)("advertised", true);
         record_locked("partial_offer", application, std::move(fields));
      }
      if (error.empty() && (_overflow || !_capture_error.empty())) {
         throw std::runtime_error{"partial offer capture failed"};
      }
   }
   if (!error.empty()) { partial_failure(error); throw std::runtime_error{error}; }
}

boost::asio::awaitable<void> forge_pubsub_fixture::send_partial(std::vector<partial_action> actions,
                                                             pubsub::partial_topic registration, std::stop_token stop) {
   auto cancellation = std::stop_source{};
   auto cancel_native = std::stop_callback{stop, [&] { cancellation.request_stop(); }};
   auto cancel_fixture = std::stop_callback{_extension_stop.get_token(), [&] { cancellation.request_stop(); }};
   for (const auto& action : actions) {
      if (cancellation.stop_requested()) { throw std::runtime_error{"partial send cancelled"}; }
      {
         const auto lock = std::scoped_lock{_mutex};
         if (_overflow || !_capture_error.empty()) { throw std::runtime_error{"partial send after capture failure"}; }
         record_locked("partial_publish_action", "forge.node.async_send_partial.application_submission", forge::mutable_variant_object{}
             ("peer_id", action.peer.to_string())("topic", registration.subject().value)
             ("group_id_hex", forge::codec::hex::encode(*action.value.group_id))
             ("observation_sequence", action.observation)("gossip_metadata_only", action.gossip_metadata_only)
             ("body_present", action.value.data.has_value())
             ("body_hex", action.value.data ? forge::codec::hex::encode(*action.value.data) : "")
             ("metadata_hex", forge::codec::hex::encode(*action.value.metadata))
             ("authority", "application_submission_not_wire_write"));
         if (_overflow || !_capture_error.empty()) { throw std::runtime_error{"partial send action capture failed"}; }
      }
      auto error = std::string{};
      try { co_await _node->async_send_partial(registration, action.peer, action.value, cancellation.get_token()); }
      catch (const std::exception& failure) { error = short_error(failure.what()); }
      record("partial_publish_return", "forge.node.async_send_partial", forge::mutable_variant_object{}
          ("peer_id", action.peer.to_string())("topic", registration.subject().value)
          ("group_id_hex", forge::codec::hex::encode(*action.value.group_id))("observation_sequence", action.observation)
          ("error", error.empty() ? forge::variant{} : forge::variant{error})
          ("authority", "native_public_API_return_not_delivery"));
      if (!error.empty()) { partial_failure(error); throw std::runtime_error{error}; }
   }
}

boost::asio::awaitable<void> forge_pubsub_fixture::receive_partial(pubsub::partial_event event, std::stop_token stop) {
   auto actions = std::vector<partial_action>{};
   auto work = std::optional<partial_work>{};
   try {
      {
         const auto lock = std::scoped_lock{_mutex};
         if (stop.stop_requested() || !admit_partial_locked()) { co_return; }
         work.emplace(this);
      }
      {
         const auto lock = std::scoped_lock{_mutex};
         if (_extension_admission_closed) { co_return; }
         record_locked("partial_incoming", "forge.pubsub.partial_handler", forge::mutable_variant_object{}
             ("peer_id", event.source.to_string())("topic", event.value.subject ? event.value.subject->value : "")
             ("group_present", event.value.group_id.has_value())("group_id_hex", captured(event.value.group_id, 20))
             ("group_bytes", size_of(event.value.group_id))("body_present", event.value.data.has_value())
             ("body_hex", captured(event.value.data, 260))("body_bytes", size_of(event.value.data))
             ("metadata_present", event.value.metadata.has_value())("metadata_hex", captured(event.value.metadata, 7))
             ("metadata_bytes", size_of(event.value.metadata))
             ("authority", "native_application_callback_not_validation_or_delivery"));
         if (_overflow || !_capture_error.empty()) { throw std::runtime_error{"partial input capture failed"}; }
         const auto observation = static_cast<std::uint64_t>(_events.size());
         auto applied = false;
         try { applied = apply_partial_locked(event.source, event.value, observation); }
         catch (const std::exception& error) {
            record_locked("partial_rejected", application, forge::mutable_variant_object{}
                ("peer_id", event.source.to_string())("group_id_hex", captured(event.value.group_id, 20))
                ("observation_sequence", observation)("error", short_error(error.what()))
                ("scope", "application_only_no_router_validation_result"));
            co_return;
         }
         auto fields = partial_fields_locked();
         fields("peer_id", event.source.to_string())("body_applied", applied)("incoming_sequence", observation)
             ("authority", "application_checked_bytes_not_signed_message_validation");
         record_locked("partial_applied", application, std::move(fields));
         reconstruct_partial_locked();
         actions = plan_partial_locked(event.source, observation, false);
      }
      co_await send_partial(std::move(actions), event.registration, stop);
   } catch (const std::exception& error) { partial_failure(error.what()); }
}

boost::asio::awaitable<void> forge_pubsub_fixture::gossip_partial(pubsub::partial_gossip_event event, std::stop_token stop) {
   auto actions = std::vector<partial_action>{};
   auto work = std::optional<partial_work>{};
   try {
      {
         const auto lock = std::scoped_lock{_mutex};
         if (stop.stop_requested() || !admit_partial_locked()) { co_return; }
         work.emplace(this);
      }
      {
         const auto lock = std::scoped_lock{_mutex};
         if (_extension_admission_closed) { co_return; }
         if (event.registration.subject().value != "forge-pr11:" + _token || event.groups.size() != 1 ||
             event.groups.front() != format::group(_token) || event.peers.size() > 16) {
            throw std::runtime_error{"gossip callback outside owned bounded partial group"};
         }
         auto peers = forge::variants{};
         for (const auto& peer : event.peers) { peers.emplace_back(peer.to_string()); }
         record_locked("partial_gossip", "forge.pubsub.partial_gossip_handler", forge::mutable_variant_object{}
             ("group_id_hex", forge::codec::hex::encode(event.groups.front()))("peer_ids", std::move(peers))
             ("authority", "native_gossip_recipient_callback_not_wire_send"));
         if (_overflow || !_capture_error.empty()) { throw std::runtime_error{"partial gossip capture failed"}; }
         const auto observation = static_cast<std::uint64_t>(_events.size());
         // A native callback may race the public advertisement's return; do not fabricate that return.
         if (!_partial.owned) { co_return; }
         for (const auto& peer : event.peers) {
            auto next = plan_partial_locked(peer, observation, true);
            actions.insert(actions.end(), std::make_move_iterator(next.begin()), std::make_move_iterator(next.end()));
         }
      }
      co_await send_partial(std::move(actions), event.registration, stop);
   } catch (const std::exception& error) { partial_failure(error.what()); }
}

} // namespace forge::test::libp2p_interop
