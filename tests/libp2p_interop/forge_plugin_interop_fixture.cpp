#include "forge_plugin_interop_fixture.hxx"
#include "forge_interop_build_info.hxx"
#include "dht_value_fixture.hxx"

namespace forge::test::libp2p_interop {
namespace {
namespace np = forge::plugins::net::p2p::node;
using object = forge::mutable_variant_object;
using namespace std::chrono_literals;
}

forge_plugin_interop_fixture::forge_plugin_interop_fixture(arguments args) : _args(std::move(args)) {
   _dht_value = dht_value_fixture::is_scenario(argument("scenario"));
   _result_safe = argument("scenario") != "pubsub_exchange" && argument("scenario") != "autorelay" &&
       argument("scenario") != "autonat_v1" && argument("scenario") != "autonat_v2";
   auto descriptor = np::plugin::descriptor();
   if (argument("scenario") == "pubsub_exchange") {
      auto factory = std::move(descriptor.factory);
      descriptor.factory = [this, factory = std::move(factory)] {
         auto plugin = factory();
         _node_plugin = static_cast<np::plugin*>(plugin.get());
         return plugin;
      };
   }
   auto builder = forge::app::application_builder{};
   builder.name("forge-plugin-interop")
       .runtime(forge::asio::runtime_options{.worker_threads = 2})
       .plugin(forge::plugins::db::store::plugin::descriptor())
       .plugin(forge::plugins::crypto::secrets::plugin::descriptor())
       .plugin(std::move(descriptor));
   _shell = std::move(builder).build();
}

forge_plugin_interop_fixture::~forge_plugin_interop_fixture() {
   try { shutdown(); }
   catch (...) { std::cerr << "plugin fixture cleanup failed\n"; }
}

std::string forge_plugin_interop_fixture::argument(std::string_view key, std::string fallback) const {
   const auto found = _args.find(std::string{key});
   return found == _args.end() ? std::move(fallback) : found->second;
}

void forge_plugin_interop_fixture::set_phase(std::string_view phase) {
   auto lock = std::lock_guard{_mutex};
   _current_phase = phase;
}

forge::variant forge_plugin_interop_fixture::current_failure(std::string_view phase) {
   auto category = std::string{"untyped"};
   auto code = 0;
   try { throw; }
   catch (const forge::exceptions::base& failure) {
      category = failure.code().category().name();
      code = failure.code().value();
   } catch (const boost::system::system_error& failure) {
      category = failure.code().category().name();
      code = failure.code().value();
   } catch (const std::system_error& failure) {
      category = failure.code().category().name();
      code = failure.code().value();
   } catch (...) {}
   return object{}("phase", std::string{phase})("error_category", category)("error_code", code)
       ("secret_material", "redacted");
}

bool forge_plugin_interop_fixture::admission_witness::intercept_accept(
    const forge::net::p2p::connection_endpoints&) noexcept {
   ++accepted;
   return true;
}

bool forge_plugin_interop_fixture::admission_witness::intercept_secured(
    forge::net::p2p::connection_direction direction, const forge::net::p2p::peer_id&,
    const forge::net::p2p::connection_endpoints&) noexcept {
   if (direction == forge::net::p2p::connection_direction::inbound) { ++secured; }
   return true;
}

void forge_plugin_interop_fixture::save(const std::filesystem::path& path, const forge::variant& value) {
   auto result = forge::codec::json::write_value(value, {.pretty = true});
   if (!result.ok()) { throw std::runtime_error{"plugin fixture JSON encoding failed"}; }
   auto temporary = path;
   temporary += ".tmp";
   {
      auto output = std::ofstream{temporary, std::ios::binary | std::ios::trunc};
      output.exceptions(std::ios::badbit | std::ios::failbit);
      output << result.text << '\n';
   }
   std::filesystem::rename(temporary, path);
}

forge::variant forge_plugin_interop_fixture::build_info() {
   auto backends = forge::variants{};
#if FORGE_HAS_MDBX
   backends.emplace_back("mdbx");
#endif
#if FORGE_HAS_ROCKSDB
   backends.emplace_back("rocksdb");
#endif
   return object{}("schema_version", 2u)("fixture_kind", "configured-plugin")("store_backends", backends)
       ("forge", object{}("head", FORGE_INTEROP_BUILD_FORGE_HEAD)
           ("worktree_sha256", FORGE_INTEROP_BUILD_WORKTREE_SHA256)
           ("dirty", FORGE_INTEROP_BUILD_WORKTREE_DIRTY != 0)
           ("exact_identity", std::string{"git:"} + FORGE_INTEROP_BUILD_FORGE_HEAD +
                                ";worktree-sha256:" + FORGE_INTEROP_BUILD_WORKTREE_SHA256))
       ("compiler", object{}("path", FORGE_INTEROP_BUILD_COMPILER_PATH)("id", FORGE_INTEROP_BUILD_COMPILER_ID)
           ("version", FORGE_INTEROP_BUILD_COMPILER_VERSION))("build_profile", FORGE_INTEROP_BUILD_PROFILE);
}

void forge_plugin_interop_fixture::start() {
   set_phase("configuration");
   const auto document = configuration();
   _shell->configure(document);
   run(_shell->initialize(), "official shell initialize");
   api<np::configuration_api>()->set_connection_gater(_autonat ? _autonat->gater : _admission);
   if (_pubsub) { initialize_pubsub(document); }
   if (_autonat) { release_autonat_port(); }
   run(_shell->startup(), "official shell startup");
   _local_peer = api<np::api>()->local_peer().to_string();
   _accepted = true;
   _settings = object{_settings}("source", "official-shell.accepted-document");
   if (_autonat) { start_autonat(); }
   else if (_relay) { start_relay(); }
   else if (_pubsub) { start_pubsub(); }
   else if (!_dht_value) {
      _echo.emplace(api<np::protocol_api>()->register_protocol({"/forge/interop/relay-echo/1"},
          [this](forge::net::p2p::node::incoming_protocol_stream incoming) {
             return receive(std::move(incoming));
          }));
   }
}

void forge_plugin_interop_fixture::shutdown() {
   if (_shutdown_attempted || !_shell) { return; }
   _shutdown_attempted = true;
   // Captures and API providers stay owned through both joins, including failure paths.
   auto failure = std::exception_ptr{};
   if (_autonat && _autonat->port_reservation) {
      try {
         _autonat->port_reservation->close();
         _autonat->port_reservation.reset();
         _autonat->reservation = object{_autonat->reservation}("released_on_failed_setup", true);
      } catch (...) {
         failure = std::current_exception();
         auto lock = std::lock_guard{_mutex};
         _errors.emplace_back(current_failure("autonat.failed-setup.port-release"));
      }
   }
   if (_dht_value && _accepted) {
      try { observe_dht_service(true); }
      catch (...) {
         failure = std::current_exception();
         auto lock = std::lock_guard{_mutex};
         _errors.emplace_back(current_failure("service.pre-stop.diagnostics"));
      }
   }
   if (_pubsub && _accepted) {
      try { prepare_pubsub(); }
      catch (...) {
         if (!failure) { failure = std::current_exception(); }
         auto lock = std::lock_guard{_mutex};
         _errors.emplace_back(current_failure("pubsub.pre-stop.drain"));
      }
   }
   if (_relay && _accepted) {
      try { prepare_relay(); }
      catch (...) {
         if (!failure) { failure = std::current_exception(); }
         auto lock = std::lock_guard{_mutex};
         _errors.emplace_back(current_failure("autorelay.pre-stop.capture"));
      }
   }
   if (_autonat && _accepted) {
      try { prepare_autonat(); }
      catch (...) {
         if (!failure) { failure = std::current_exception(); }
         auto lock = std::lock_guard{_mutex};
         _errors.emplace_back(current_failure("autonat.pre-stop.capture"));
      }
   }
   _shell->request_stop();
   try {
      run(_shell->shutdown(), "official shell reverse shutdown");
      _plugins_joined = true;
   } catch (...) {
      if (!failure) { failure = std::current_exception(); }
      auto lock = std::lock_guard{_mutex};
      _errors.emplace_back(current_failure("official shell reverse shutdown"));
   }
   try {
      run(_shell->scheduler().shutdown(), "official shell scheduler join");
      _scheduler_joined = true;
   } catch (...) {
      if (!failure) { failure = std::current_exception(); }
      auto lock = std::lock_guard{_mutex};
      _errors.emplace_back(current_failure("official shell scheduler join"));
   }
   _echo.reset();
   if (_relay && _accepted && _plugins_joined && _scheduler_joined) {
      try { verify_relay_detached(); }
      catch (...) {
         if (!failure) { failure = std::current_exception(); }
         auto lock = std::lock_guard{_mutex};
         _errors.emplace_back(current_failure("autorelay.post-join.diagnostics-detached"));
      }
   }
   if (relay_failed()) {
      try { throw std::runtime_error{"AutoRelay capture overflow or pending echo after join"}; }
      catch (...) {
         if (!failure) { failure = std::current_exception(); }
         auto lock = std::lock_guard{_mutex};
         _errors.emplace_back(current_failure("autorelay.post-join.capture"));
      }
   }
   if (_autonat && _accepted && _plugins_joined && _scheduler_joined) {
      try { verify_autonat_detached(); }
      catch (...) {
         if (!failure) { failure = std::current_exception(); }
         auto lock = std::lock_guard{_mutex};
         _errors.emplace_back(current_failure("autonat.post-join.diagnostics-detached"));
      }
   }
   if (autonat_failed()) {
      try { throw std::runtime_error{"AutoNAT actual connection capture failed"}; }
      catch (...) {
         if (!failure) { failure = std::current_exception(); }
         auto lock = std::lock_guard{_mutex};
         _errors.emplace_back(current_failure("autonat.post-join.capture"));
      }
   }
   if (failure) { std::rethrow_exception(failure); }
}

void forge_plugin_interop_fixture::publish_failure(object& result, std::string_view error) {
   result.set("error", std::string{error}).set("complete", false).set("status", "error");
}

void forge_plugin_interop_fixture::publish(bool final) {
   const auto pubsub = pubsub_receipt(final);
   const auto relay = relay_receipt(final);
   const auto autonat = autonat_receipt(final);
   const auto capture_failed = pubsub_failed() || relay_failed() || autonat_failed();
   auto lock = std::lock_guard{_mutex};
   auto result = object{}("schema_version", 1u)("suite", "plugin-node")
       ("implementation", "forge")("fixture_kind", "configured-plugin")
       ("role", argument("command") == "listen" ? "listener" : "dialer")
       ("scenario", argument("scenario"))("local_peer_id", _local_peer)
       ("status", _errors.empty() && !capture_failed ? (_rejected ? (argument("command") == "dial" ? "rejection_candidate" : "rejected") : "ok") : "error")
       ("current_phase", _current_phase)("control_kind", argument("pnet-control"))
       ("correlation_token", argument("pnet-correlation"))
       ("final", final)("configuration_accepted", _accepted)("accepted_configuration", _settings)
       ("store_backend", argument("store-backend"))("psk_fingerprint", _key_fingerprint)
       ("exchange", _exchange)("service", _service)("diagnostics", _service_diagnostics)
       ("rejection", _rejection)("errors", _errors)
       ("lifecycle", object{}("plugins_joined", _plugins_joined)("scheduler_joined", _scheduler_joined));
   if (_pubsub) {
      for (const auto& field : pubsub.get_object()) { result.set(field.key(), field.value()); }
      if (!_errors.empty()) { publish_failure(result, "pubsub_fixture_operation_failed"); }
   }
   if (_relay) {
      for (const auto& field : relay.get_object()) { result.set(field.key(), field.value()); }
      if (!_errors.empty()) {
         publish_failure(result, "autorelay_fixture_operation_failed");
      }
   }
   if (_autonat) {
      for (const auto& field : autonat.get_object()) { result.set(field.key(), field.value()); }
      if (!_errors.empty()) { publish_failure(result, "autonat_fixture_operation_failed"); }
   }
   save(argument("result-file"), result);
}

bool forge_plugin_interop_fixture::rejection_observed() {
   const auto state = api<np::diagnostics_api>()->snapshot();
   if (_admission->accepted == 0 || state.metrics.handshakes_failed == 0 ||
       state.resources.system.inbound_connections != 0) { return false; }
   _rejection = object{}("source", "node.diagnostics-api-and-configured-gater")
       ("attempted_connections", _admission->accepted.load())("secured_callbacks", _admission->secured.load())
       ("handshakes_failed", state.metrics.handshakes_failed)("handshakes_completed", state.metrics.handshakes_completed)
       ("sessions_opened", state.metrics.sessions_opened)("protocol_streams_opened", state.metrics.protocol_streams_opened)
       ("protocol_streams_accepted", state.metrics.protocol_streams_accepted)
       ("inbound_connections", static_cast<std::uint64_t>(state.resources.system.inbound_connections));
   return _admission->secured == 0 && state.metrics.handshakes_completed == 0 &&
          state.metrics.sessions_opened == 0 && state.metrics.protocol_streams_opened == 0 &&
          state.metrics.protocol_streams_accepted == 0;
}

void forge_plugin_interop_fixture::listen() {
   set_phase(_autonat ? "autonat.autonomous-service" : _relay ? "autorelay.observe" : _pubsub ? "pubsub.control" : _dht_value ? "listener.await_dht_service" : "listener.await_echo_or_private_ingress");
   if (_dht_value) { observe_dht_service(); }
   auto addresses = forge::variants{};
   const auto local = forge::net::p2p::peer_id::from_string(_local_peer);
   for (auto endpoint : api<np::api>()->local_endpoints()) {
      if (endpoint.peer && *endpoint.peer != local) {
         throw std::runtime_error{"official local endpoint has a different peer identity"};
      }
      endpoint.peer = local;
      addresses.emplace_back(endpoint.to_string());
   }
   auto ready = object{}("implementation", "forge")("fixture_kind", "configured-plugin")
       ("role", "listener")("status", "ready")("peer_id", _local_peer)("listen_addrs", addresses)
       ("configuration_accepted", true)("store_backend", argument("store-backend"));
   if (_pubsub) { pubsub_ready(ready); }
   if (_autonat) { autonat_ready(ready); }
   if (_relay) {
      ready("role", _relay->role)("transport", argument("transport"))("scenario", "autorelay")
          ("accepted_configuration", _settings);
   }
   save(argument("ready-file"), ready);
   if (_pubsub || _relay || _autonat) { publish(false); }
   const auto deadline = std::chrono::steady_clock::now() + (_relay ? 60s : 45s);
   while (!std::filesystem::exists(argument("stop-file"))) {
      if (std::chrono::steady_clock::now() >= deadline) { throw std::runtime_error{"plugin listener phase deadline"}; }
      auto failed = false;
      { auto lock = std::lock_guard{_mutex}; failed = !_errors.empty(); }
      if (failed || pubsub_failed() || relay_failed() || autonat_failed()) {
         // A failed callback has no exchange receipt. Expose its typed failure
         // instead of hiding it until the runner's completion budget expires.
         publish(false);
         return;
      }
      if (_autonat) {
         // Native handlers serve requests; no manual probe or protocol handler is installed here.
      } else if (_relay) {
         if (poll_relay()) { publish(false); }
      } else if (_pubsub) {
         if (poll_pubsub_control()) { publish(false); }
      } else if (_dht_value) {
         observe_dht_service();
         publish(false);
      } else if (!argument("pnet-control").empty()) {
         _rejected = rejection_observed();
         if (_rejected) { publish(false); }
      } else {
         auto completed = false;
         { auto lock = std::lock_guard{_mutex}; completed = !_exchange.is_null(); }
         if (completed) { publish(false); }
      }
      std::this_thread::sleep_for(5ms);
   }
   if (_pubsub) { static_cast<void>(poll_pubsub_control(true)); }
   if (!argument("pnet-control").empty() && !(_rejected = rejection_observed())) {
      throw std::runtime_error{"private listener did not observe pre-security rejection and resource release"};
   }
}

void forge_plugin_interop_fixture::dial() {
   if (_autonat) {
      dial_autonat();
      return; // Raw AutoNAT dial CLI exits after exchange; it has no stop-file barrier.
   } else if (_dht_value) {
      run(dht_exchange(), "service.authenticated_dht_value");
      observe_dht_service();
   } else if (argument("pnet-control").empty()) {
      run(exchange(), "configured connect and binary echo");
   } else {
      auto observed = false;
      try {
         const auto peer = forge::net::p2p::peer_id::from_string(argument("peer-id"));
         static_cast<void>(run(api<np::api>()->connect(forge::multiformats::multiaddr::parse(argument("addr")),
             {.expected_peer = peer, .allow_relay = false, .timeout = 2s, .direct_attempt_timeout = 2s, .max_direct_endpoints = 1,
              .allow_hole_punch = false}), "negative private connect"));
      } catch (const forge::exceptions::base& failure) {
         const auto state = api<np::diagnostics_api>()->snapshot();
         _rejection = object{}("source", "node.connect-and-diagnostics-api")
             ("error_category", failure.code().category().name())("error_code", failure.code().value())
             ("path_direct_attempts", state.metrics.path_direct_attempts)("direct_failures", state.metrics.direct_failures)
             ("handshakes_completed", state.metrics.handshakes_completed)("sessions_opened", state.metrics.sessions_opened)
             ("protocol_streams_opened", state.metrics.protocol_streams_opened);
         observed = failure.code().value() != 0 && state.metrics.handshakes_completed == 0 && state.metrics.sessions_opened == 0 &&
                    state.metrics.protocol_streams_opened == 0;
      }
      if (!observed) { throw std::runtime_error{"private control did not fail before authentication"}; }
      _rejected = true;
   }
   publish(false);
   set_phase("dialer.await_owned_stop");
   const auto deadline = std::chrono::steady_clock::now() + 35s;
   while (!std::filesystem::exists(argument("stop-file"))) {
      if (std::chrono::steady_clock::now() >= deadline) { throw std::runtime_error{"plugin dial stop barrier deadline"}; }
      std::this_thread::sleep_for(5ms);
   }
}

int forge_plugin_interop_fixture::execute() {
   try {
      start();
      if (_relay || argument("command") == "listen") { listen(); } else { dial(); }
   } catch (...) {
      // Exception messages may contain Secrets context. Only a fixed redacted phase is public.
      auto lock = std::lock_guard{_mutex};
      _errors.emplace_back(current_failure(_current_phase));
   }
   try { shutdown(); }
   catch (...) {} // Each failed join has its own typed, redacted phase receipt.
   if (_result_safe) { publish(true); }
   return _errors.empty() && !pubsub_failed() && !relay_failed() && !autonat_failed() ? 0 : 1;
}
} // namespace forge::test::libp2p_interop

int main(int argc, char** argv) {
   using fixture = forge::test::libp2p_interop::forge_plugin_interop_fixture;
   try {
      if (argc == 2 && std::string_view{argv[1]} == "build-info") {
         const auto result = forge::codec::json::write_value(fixture::build_info());
         if (!result.ok()) { throw std::runtime_error{"build-info encoding failed"}; }
         std::cout << result.text << '\n';
         return 0;
      }
      if (argc == 2 && std::string_view{argv[1]} == "pubsub-self-test") { return fixture::pubsub_self_test(); }
      if (argc == 2 && std::string_view{argv[1]} == "relay-self-test") { return fixture::relay_self_test(); }
      if (argc == 2 && std::string_view{argv[1]} == "autonat-self-test") { return fixture::autonat_self_test(); }
      const auto pubsub = argc >= 2 && std::string_view{argv[1]} == "pubsub-live";
      const auto relay = argc >= 2 && (std::string_view{argv[1]} == "autorelay-destination" ||
                                      std::string_view{argv[1]} == "autorelay-service");
      if (argc < 2 || (!pubsub && !relay && std::string_view{argv[1]} != "listen" && std::string_view{argv[1]} != "dial")) {
         throw std::runtime_error{"expected build-info/listen/dial/pubsub-live/autorelay-destination/autorelay-service"};
      }
      auto args = fixture::arguments{{"command", argv[1]}};
      const auto allowed = std::vector<std::string>{"ready-file", "result-file", "stop-file", "store-dir",
          "store-backend", "transport", "scenario", "addr", "peer-id", "payload", "dns-server",
          "pnet-key-file", "pnet-fingerprint", "pnet-control", "pnet-correlation", "features",
          "version", "actor", "case-token", "control-file", "pubsub-policy", "seed-file", "bind-ip", "probe-addr"};
      for (auto index = 2; index < argc; index += 2) {
         const auto flag = std::string{argv[index]};
         if (!flag.starts_with("--") || index + 1 >= argc) { throw std::runtime_error{"invalid fixture argument"}; }
         const auto key = flag.substr(2);
         if (std::find(allowed.begin(), allowed.end(), key) == allowed.end() || !args.emplace(key, argv[index + 1]).second) {
            throw std::runtime_error{"unknown or duplicate fixture argument"};
         }
      }
      if (pubsub) {
         if (args.contains("scenario")) { throw std::runtime_error{"PubSub scenario is fixed"}; }
         args["scenario"] = "pubsub_exchange";
         args["command"] = "listen";
      } else {
         for (const auto* field : {"version", "actor", "case-token", "control-file", "pubsub-policy"}) {
            if (args.contains(field)) { throw std::runtime_error{"PubSub-only argument outside pubsub-live"}; }
         }
      }
      if (!relay && args.contains("seed-file")) { throw std::runtime_error{"AutoRelay seed outside AutoRelay command"}; }
      for (const auto* field : {"result-file", "store-dir", "store-backend", "transport", "scenario"}) {
         if (!args.contains(field) || args.at(field).empty()) { throw std::runtime_error{"missing fixture argument"}; }
      }
      const auto autonat = args.at("scenario") == "autonat_v1" || args.at("scenario") == "autonat_v2";
      if (!autonat && (args.contains("bind-ip") || args.contains("probe-addr"))) {
         throw std::runtime_error{"AutoNAT-only argument outside AutoNAT"};
      }
      if ((!autonat || args.at("command") != "dial") && (!args.contains("stop-file") || args.at("stop-file").empty())) {
         throw std::runtime_error{"missing stop-file"};
      }
      if ((args.at("command") == "listen" || relay) && !args.contains("ready-file")) { throw std::runtime_error{"missing ready-file"}; }
      if (args.at("command") == "dial" && (!args.contains("addr") || !args.contains("peer-id"))) {
         throw std::runtime_error{"missing dial target"};
      }
      return fixture{std::move(args)}.execute();
   } catch (...) {
      std::cerr << "configured-plugin fixture setup failed (secret material redacted)\n";
      return 1;
   }
}
