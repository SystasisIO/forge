#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

import forge.api.core.registry;
import forge.app.application_builder;
import forge.app.application_shell;
import forge.asio.runtime;
import forge.asio.task;
import forge.codec.hex;
import forge.codec.json;
import forge.codec.yaml;
import forge.config.core.document;
import forge.config.core.value;
import forge.crypto.digest.sha256;
import forge.exceptions;
import forge.multiformats.multiaddr;
import forge.net.p2p.connection_gater;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.identify;
import forge.net.p2p.lifecycle;
import forge.net.p2p.node;
import forge.net.p2p.scoring;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.relay;
import forge.net.p2p.stream;
import forge.plugins.crypto.secrets.plugin;
import forge.plugins.db.store.plugin;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.configuration_api;
import forge.plugins.net.p2p.node.dht_api;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.events_api;
import forge.plugins.net.p2p.node.plugin;
import forge.plugins.net.p2p.node.protocol_api;
import forge.plugins.net.p2p.node.protocol_registration;
import forge.plugins.net.p2p.node.pubsub_api;
import forge.plugins.net.p2p.node.types;
import forge.variant.containers;
import forge.variant.value;

#include "forge_autonat_fixture.hxx"

namespace forge::test::libp2p_interop {

// One official shell owns every backend and callback. No raw node or second runtime.
class forge_plugin_interop_fixture final {
 public:
   using arguments = std::map<std::string, std::string>;
   explicit forge_plugin_interop_fixture(arguments args);
   ~forge_plugin_interop_fixture();
   forge_plugin_interop_fixture(const forge_plugin_interop_fixture&) = delete;
   forge_plugin_interop_fixture& operator=(const forge_plugin_interop_fixture&) = delete;
   int execute();
   static forge::variant build_info();
   static void save(const std::filesystem::path&, const forge::variant&);
   static int pubsub_self_test();
   static int relay_self_test();
   static int autonat_self_test();

 private:
   class admission_witness final : public forge::net::p2p::connection_gater {
    public:
      std::atomic_uint64_t accepted{0};
      std::atomic_uint64_t secured{0};
      bool intercept_accept(const forge::net::p2p::connection_endpoints&) noexcept override;
      bool intercept_secured(forge::net::p2p::connection_direction, const forge::net::p2p::peer_id&,
                             const forge::net::p2p::connection_endpoints&) noexcept override;
   };

   template<class T> T run(boost::asio::awaitable<T> operation, std::string_view phase) {
      set_phase(phase);
      auto future = boost::asio::co_spawn(_shell->runtime().context(), std::move(operation), boost::asio::use_future);
      if (future.wait_for(std::chrono::seconds{10}) != std::future_status::ready) {
         // The process owner records an abort as failure, never a successful join.
         std::cerr << "plugin fixture owned join budget exceeded: " << phase << '\n';
         std::terminate();
      }
      return future.get();
   }
   template<class T> std::shared_ptr<T> api() { return _shell->apis().get<T>(T::ref()).shared(); }
   std::string argument(std::string_view key, std::string fallback = {}) const;
   void start();
   void shutdown();
   void publish(bool final);
   static void publish_failure(forge::mutable_variant_object&, std::string_view);
   void set_phase(std::string_view);
   static forge::variant current_failure(std::string_view);
   void listen();
   void dial();
   forge::config::core::document configuration();
   boost::asio::awaitable<forge::variant> session_receipt(forge::net::p2p::peer_id, std::uint64_t,
                                                         forge::net::p2p::peer_authentication);
   boost::asio::awaitable<void> receive(forge::net::p2p::node::incoming_protocol_stream);
   boost::asio::awaitable<void> exchange();
   boost::asio::awaitable<void> dht_exchange();
   boost::asio::awaitable<forge::variant> dht_session_receipt(forge::net::p2p::node::session_info);
   forge::variant dht_session_facts(const forge::net::p2p::diagnostics::session&) const;
   void observe_dht_service(bool stopping = false);
   static boost::asio::awaitable<std::vector<std::uint8_t>> read_frame(forge::net::p2p::stream&);
   static std::vector<std::uint8_t> frame(std::span<const std::uint8_t>);
   static forge::variant payload_receipt(std::span<const std::uint8_t>);
   bool rejection_observed();
   struct pubsub_state;
   void configure_pubsub_document(forge::config::core::document&);
   void initialize_pubsub(const forge::config::core::document&);
   void start_pubsub();
   boost::asio::awaitable<forge::variant> pubsub_session_receipt(forge::net::p2p::node::session_info);
   static forge::variant pubsub_session_facts(const forge::net::p2p::diagnostics::session&,
                                             const forge::net::p2p::node::session_info&, std::string_view);
   bool poll_pubsub_control(bool stopping = false);
   void pubsub_command(const forge::variant&, std::uint64_t);
   void sample_pubsub(std::string_view);
   void prepare_pubsub();
   forge::variant pubsub_receipt(bool final) const;
   bool pubsub_failed() const;
   void pubsub_ready(forge::mutable_variant_object&) const;
   struct relay_state;
   void configure_relay_document(forge::config::core::document&);
   void start_relay();
   bool poll_relay();
   void prepare_relay();
   void verify_relay_detached();
   void sample_relay(std::string_view);
   forge::variant relay_receipt(bool final) const;
   bool relay_failed() const;
   static forge::variant relay_observation(const forge::net::p2p::diagnostics::snapshot&,
                                          std::int64_t elapsed_ms, std::int64_t unix_ms, std::string_view phase);
   static forge::variant relay_session_facts(const forge::net::p2p::diagnostics::snapshot&,
                                            const forge::net::p2p::node::session_info&,
                                            forge::net::p2p::peer_authentication);
   boost::asio::awaitable<forge::variant> relay_session_receipt(forge::net::p2p::node::session_info,
                                                              forge::net::p2p::peer_authentication);
   boost::asio::awaitable<forge::variant> relay_seed_receipt(forge::net::p2p::node::session_info);
   boost::asio::awaitable<void> receive_relay(forge::net::p2p::node::incoming_protocol_stream);
   struct autonat_state;
   void configure_autonat_document(forge::config::core::document&);
   void release_autonat_port();
   void start_autonat();
   void dial_autonat();
   void prepare_autonat();
   void verify_autonat_detached();
   forge::variant autonat_connections() const;
   forge::variant autonat_receipt(bool final) const;
   bool autonat_failed() const;
   void autonat_ready(forge::mutable_variant_object&) const;
   static forge::variant autonat_host(const forge::net::p2p::host_event&);
   static forge::variant autonat_snapshot(const forge::net::p2p::diagnostics::snapshot&);
   boost::asio::awaitable<forge::variant> autonat_control_receipt(forge::net::p2p::node::session_info);

   arguments _args;
   std::unique_ptr<forge::app::application_shell> _shell;
   forge::plugins::net::p2p::node::plugin* _node_plugin = nullptr; // Shell-owned; pre-start configure only.
   std::shared_ptr<pubsub_state> _pubsub;
   std::shared_ptr<relay_state> _relay;
   std::shared_ptr<autonat_state> _autonat;
   std::shared_ptr<forge::plugins::net::p2p::node::diagnostics_api> _relay_diagnostics;
   std::optional<forge::net::p2p::host_event_subscription> _relay_events;
   std::shared_ptr<admission_witness> _admission = std::make_shared<admission_witness>();
   std::optional<forge::plugins::net::p2p::node::protocol_registration> _echo;
   forge::variant _settings;
   forge::variant _exchange;
   forge::variant _service;
   forge::variant _service_diagnostics;
   std::map<std::uint64_t, forge::variant> _service_sessions;
   forge::variant _rejection;
   forge::variants _errors;
   std::mutex _mutex;
   std::string _local_peer;
   std::string _key_fingerprint;
   std::string _current_phase = "configuration";
   bool _accepted = false;
   bool _result_safe = true;
   bool _dht_value = false;
   bool _shutdown_attempted = false;
   bool _plugins_joined = false;
   bool _scheduler_joined = false;
   bool _rejected = false;
};

// Only immutable observations and harness admission state; the shell remains the sole backend owner.
struct forge_plugin_interop_fixture::pubsub_state {
   std::mutex mutex;
   forge::variants events;
   std::size_t capture_bytes = 0;
   bool overflow = false;
   bool capture_error = false;
   forge::variant capture_failure;
   std::atomic_uint64_t score_calls{0};
   std::string actor;
   std::string token;
   std::string topic;
   std::string version;
   bool typed = false;
   std::optional<forge::plugins::net::p2p::node::subscription> subscription;
   std::string prefix;
   std::size_t offset = 0;
   std::uint64_t commands = 0;
   bool prepared = false;
   bool drained = false;
   bool control_seen = false;
   std::set<std::uint64_t> connections;
   std::set<std::tuple<std::uint64_t, std::int64_t, std::string>> streams;
   std::chrono::steady_clock::time_point next_sample{};
   std::chrono::steady_clock::time_point deadline{};

   void record(std::string_view kind, std::string_view source, forge::mutable_variant_object fields) noexcept;
   bool failed();
   static boost::asio::awaitable<forge::net::p2p::pubsub::validation_result>
   deliver(std::shared_ptr<pubsub_state> state, forge::plugins::net::p2p::node::message message);
   void trace(const forge::net::p2p::pubsub::trace_event& event) noexcept;
};

// Test observations only; reservation ownership remains in the native AutoRelay manager.
struct forge_plugin_interop_fixture::relay_state {
   std::mutex mutex;
   std::string role;
   std::string seed_prefix;
   std::size_t seed_offset = 0;
   std::set<std::string> connected_seeds;
   forge::variants observations;
   forge::variants seed_connections;
   forge::variants echoes;
   forge::variant diagnostics_after_detach;
   forge::variant host_events;
   bool shutdown_observed = false;
   std::size_t capture_bytes = 0;
   std::size_t pending_echoes = 0;
   std::size_t admitted_echoes = 0;
   bool overflow = false;
   bool admission_closed = false;
   bool seed_seen = false;
   std::chrono::steady_clock::time_point started{};
   std::chrono::steady_clock::time_point next_sample{};

   void append(forge::variants&, forge::variant, std::size_t limit);
   void begin_echo();
   void finish_echo() noexcept;
   bool failed();
};

// Observation/lifecycle harness state only; the official shell owns the AutoNAT manager.
struct forge_plugin_interop_fixture::autonat_state {
   bool service = false;
   std::uint32_t version = 0;
   forge::net::p2p::endpoint bind;
   forge::net::p2p::endpoint probe;
   std::unique_ptr<boost::asio::ip::tcp::acceptor> port_reservation;
   std::shared_ptr<forge::net::p2p::connection_gater> gater;
   std::shared_ptr<autonat_connection_observation> connections;
   std::shared_ptr<forge::plugins::net::p2p::node::diagnostics_api> diagnostics;
   std::optional<forge::net::p2p::host_event_subscription> events;
   forge::variant reservation;
   forge::variant diagnostics_before_connect;
   forge::variant diagnostics_before_stop;
   forge::variant diagnostics_after_detach;
   forge::variant host_events;
   forge::variant control;
   forge::variant v2_address_state;
   forge::net::p2p::host_event host;
   std::string observer;
   std::uint64_t initial_probe_errors = 0;
   std::uint64_t initial_checks = 0;
   std::uint64_t initial_public = 0;
   std::uint64_t initial_private = 0;
   bool reached = false;
   bool detached = false;
};

} // namespace forge::test::libp2p_interop
