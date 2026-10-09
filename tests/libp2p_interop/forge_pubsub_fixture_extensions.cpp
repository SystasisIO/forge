#include <chrono>
#include <coroutine>
#include <cstdint>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>

import forge.asio.blocking;
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
using namespace std::chrono_literals;
}

void forge_pubsub_fixture::validate_extension(std::string_view mode, pubsub::version version) {
   if (mode.empty() || (mode == "idontwant" && version == pubsub::version::v1_2) ||
       ((mode == "partial" || mode == "advertisement") && version == pubsub::version::v1_3)) { return; }
   throw std::runtime_error{"unsupported PubSub extension mode/version"};
}

boost::asio::awaitable<pubsub::validation_result> forge_pubsub_fixture::validate_message(pubsub::event event) {
   const auto text = std::string_view{reinterpret_cast<const char*>(event.value.data.data()), event.value.data.size()};
   auto result = pubsub::validation_result::accept;
   if (_actor == "victim" && text.starts_with("reject:" + _token + ':')) { result = pubsub::validation_result::reject; }
   if (_actor == "victim" && text.starts_with("ignore:" + _token + ':')) { result = pubsub::validation_result::ignore; }
   if (_extension != "idontwant") { co_return result; }
   auto held = false;
   auto observation = std::uint64_t{};
   auto epoch = forge::asio::notification::epoch_type{};
   {
      const auto lock = std::scoped_lock{_mutex};
      if (_prepared || _extension_admission_closed || _extension_stop.stop_requested()) {
         co_return pubsub::validation_result::ignore;
      }
      held = !_hold_released && !_held_payload.empty() && text == _held_payload;
      if (!held) { co_return result; }
      if (_hold_observation != 0) { throw std::runtime_error{"duplicate held payload validation"}; }
      epoch = _extension_notification.epoch();
      record_locked("validation_held", "forge.fixture.extensions.validation", forge::mutable_variant_object{}
          ("extension_mode", _extension)("command_sequence", _hold_command)("committed", false)
          ("propagation_peer", event.source.to_string())("author_peer", event.value.from ? event.value.from->to_string() : "")
          ("topic", event.value.subject.value)("seqno_hex", forge::codec::hex::encode(event.value.seqno))
          ("message_id", forge::codec::hex::encode(pubsub::codec::message_id(event.value)))
          ("message_id_basis", "native_pubsub_codec_message_id_from_callback_message")
          ("payload_sha256", forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{event.value.data}).str())
          ("payload_bytes", event.value.data.size()));
      if (_overflow || !_capture_error.empty()) { throw std::runtime_error{"validation hold capture failed"}; }
      _hold_observation = observation = _events.size();
      ++_extension_validators;
   }
   auto failure = std::string{};
   try {
      co_await _extension_notification.async_wait_until(epoch, std::chrono::steady_clock::now() + 10s,
                                                       _extension_stop.get_token());
   } catch (const std::exception&) { failure = "validation hold expired or cancelled"; }
   {
      const auto lock = std::scoped_lock{_mutex};
      --_extension_validators;
      if (!_hold_released && failure.empty()) { failure = "validation hold woke without release"; }
      auto fields = forge::mutable_variant_object{}("extension_mode", _extension)
          ("held_observation_sequence", observation)("committed", false)("released", failure.empty())
          ("propagation_peer", event.source.to_string())("topic", event.value.subject.value)
          ("author_peer", event.value.from ? event.value.from->to_string() : "")
          ("seqno_hex", forge::codec::hex::encode(event.value.seqno))
          ("message_id", forge::codec::hex::encode(pubsub::codec::message_id(event.value)))
          ("message_id_basis", "native_pubsub_codec_message_id_from_callback_message")
          ("payload_sha256", forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{event.value.data}).str())
          ("payload_bytes", event.value.data.size());
      if (!failure.empty()) {
         fields("error", failure);
         result = pubsub::validation_result::ignore;
      }
      record_locked("validation_resumed", "forge.fixture.extensions.validation", std::move(fields));
      if (!failure.empty() && _capture_error.empty()) { _capture_error = failure; }
   }
   _extension_drain_notification.notify();
   co_return result;
}

boost::asio::awaitable<void> forge_pubsub_fixture::prepare_extension(const forge::variant& input) {
   {
      const auto lock = std::scoped_lock{_mutex};
      check_prepare_locked(input);
      if (_extension_admission_closed) { throw std::runtime_error{"extension application admission already closed"}; }
      _extension_admission_closed = true;
   }
   try {
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      auto registration = std::optional<pubsub::partial_topic>{};
      while (true) {
         auto epoch = forge::asio::notification::epoch_type{};
         {
            const auto lock = std::scoped_lock{_mutex};
            if (_overflow || !_capture_error.empty()) { throw std::runtime_error{"extension drain observed capture failure"}; }
            if (_extension_work == 0 && _extension_validators == 0) {
               registration = _partial_registration;
               break;
            }
            epoch = _extension_drain_notification.epoch();
         }
         co_await _extension_drain_notification.async_wait_until(epoch, deadline, _extension_stop.get_token());
      }
      if (registration) {
         auto failure = std::string{};
         try { co_await _node->async_unsubscribe(*registration); }
         catch (const std::exception& error) {
            failure = std::string{error.what()}.substr(0, 512);
            for (auto& byte : failure) {
               if (static_cast<unsigned char>(byte) < 32 || static_cast<unsigned char>(byte) > 126) { byte = '?'; }
            }
            if (failure.empty()) { failure = "partial unsubscribe failed"; }
         }
         {
            const auto lock = std::scoped_lock{_mutex};
            record_locked("partial_unsubscribe_return", "forge.node.async_unsubscribe.partial_topic",
                forge::mutable_variant_object{}("topic", registration->subject().value)("admission_closed", true)
                ("active_work", _extension_work)("error", failure.empty() ? forge::variant{} : forge::variant{failure})
                ("scope", "scoped_unsubscribe_not_native_callback_join"));
            if (failure.empty()) {
               _partial_registration.reset();
               _partial.owned = false;
            }
         }
         if (!failure.empty()) { throw std::runtime_error{failure}; }
      }
      {
         const auto lock = std::scoped_lock{_mutex};
         check_prepare_locked(input);
         if (_extension_work || _extension_validators || _partial_registration) {
            throw std::runtime_error{"extension drain has live work or registration"};
         }
         record_locked("extension_stopped", "forge.fixture.extensions.application_drain", forge::mutable_variant_object{}
             ("admission_closed", true)("active_work", _extension_work)("active_validators", _extension_validators)
             ("inputs", _extension_inputs)("partial_registration_active", false)("drain_error", forge::variant{})
             ("scope", "fixture_admitted_work_not_native_callback_or_node_join"));
         if (_overflow || !_capture_error.empty()) { throw std::runtime_error{"extension drain capture failed"}; }
         _extension_drained = true;
      }
   } catch (const std::exception& error) {
      const auto lock = std::scoped_lock{_mutex};
      if (_extension_drain_error.empty()) {
         _extension_drain_error = std::string{error.what()}.substr(0, 512);
         for (auto& byte : _extension_drain_error) {
            if (static_cast<unsigned char>(byte) < 32 || static_cast<unsigned char>(byte) > 126) { byte = '?'; }
         }
         if (_extension_drain_error.empty()) { _extension_drain_error = "extension drain failed"; }
      }
      throw;
   }
}

bool forge_pubsub_fixture::extension_command(const forge::variant& input, std::uint64_t sequence,
                                            forge::asio::runtime& runtime) {
   const auto& object = input.get_object();
   const auto& kind = object["kind"].get_string();
   if (_extension.empty()) { return false; }
   if (kind == "partial_offer" && object.size() == 3 && object["have"].is_integer() && _extension == "partial") {
      const auto have = object["have"].as_uint64();
      if (have > 7) { throw std::runtime_error{"partial_offer have outside 0..7"}; }
      forge::asio::blocking::run(runtime, partial_offer(static_cast<std::uint8_t>(have), sequence));
      return true;
   }
   if ((kind == "publish_extension" || kind == "validation_hold") && object.size() == 3 && object["payload"].is_string()) {
      const auto& text = object["payload"].get_string();
      if (text.empty() || text.size() > 4096 || text.find('\0') != std::string::npos) {
         throw std::runtime_error{"extension payload outside fixture bound"};
      }
      const auto payload = std::vector<std::uint8_t>{text.begin(), text.end()};
      if (kind == "publish_extension") {
         forge::asio::blocking::run(runtime, _node->async_publish({"forge-pr11:" + _token}, payload));
         record("publish", "forge.node.async_publish", forge::mutable_variant_object{}
             ("command_sequence", sequence)("topic", "forge-pr11:" + _token)
             ("payload_sha256", forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{payload}).str()));
      } else {
         const auto lock = std::scoped_lock{_mutex};
         if (_extension != "idontwant" || !_held_payload.empty() || !text.starts_with("accept:" + _token + ':')) {
            throw std::runtime_error{"invalid or duplicate validation hold"};
         }
         _held_payload = text;
         _hold_command = sequence;
         record_locked("validation_hold_armed", "forge.fixture.extensions.command", forge::mutable_variant_object{}
             ("extension_mode", _extension)("command_sequence", sequence)("topic", "forge-pr11:" + _token)
             ("payload_sha256", forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{payload}).str())("payload_bytes", payload.size()));
      }
      return true;
   }
   if (kind == "validation_release" && object.size() == 2 && _extension == "idontwant") {
      {
         const auto lock = std::scoped_lock{_mutex};
         if (!_hold_observation || _hold_released) { throw std::runtime_error{"validation release without held observation"}; }
         record_locked("validation_release", "forge.fixture.extensions.command", forge::mutable_variant_object{}
             ("extension_mode", _extension)("command_sequence", sequence)("topic", "forge-pr11:" + _token)
             ("held_observation_sequence", _hold_observation));
         _hold_released = true;
      }
      _extension_notification.notify();
      return true;
   }
   return false;
}

} // namespace forge::test::libp2p_interop
