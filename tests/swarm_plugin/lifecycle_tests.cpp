#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include "../swarm_storage/support.hpp"
#include "official_fixture.hxx"

import forge.plugins.swarm.node.exceptions;
import forge.api.core.binding;
import forge.api.core.exceptions;
import forge.api.p2p.publication;
import forge.app.events;
import forge.app.plugin_context;
import forge.asio.task;
import forge.asio.runtime;
import forge.multiformats.multihash;
import forge.net.p2p.dht;
import forge.net.p2p.identify;
import forge.net.p2p.protocol;
import forge.plugins.net.p2p.node.diagnostics_api;

namespace {
namespace fixture = swarm_plugin_test;
namespace sn = forge::plugins::swarm::node;
namespace runtime = forge::swarm::runtime;
namespace core = forge::swarm::core;
using namespace std::chrono_literals;

class product_policy final : public runtime::admission {
 public:
   std::mutex mutex;
   std::shared_ptr<runtime::quota> grant;
   bool metadata_only = false;
   std::atomic_uint calls{0};
   std::atomic_bool authenticated{true};
   boost::asio::awaitable<std::shared_ptr<runtime::quota>> async_authorize(core::swarm_id,
      forge::api::auth::authenticated_caller caller, runtime::access_kind kind,
      std::stop_token, std::chrono::steady_clock::time_point) override {
      ++calls;
      if (!caller.transport_authenticated()) authenticated = false;
      const auto lock = std::scoped_lock{mutex};
      co_return metadata_only && kind != runtime::access_kind::metadata ? nullptr : grant;
   }
};

void stop(fixture::application& app) {
   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

boost::asio::awaitable<bool> identified(fixture::application& app, forge::net::p2p::peer_id peer) {
   auto diagnostics = app.apis().get<forge::plugins::net::p2p::node::diagnostics_api>(
      {.id = {"forge.plugins.net.p2p.node.diagnostics"}, .major = 1});
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   const auto deadline = std::chrono::steady_clock::now() + 5s;
   for (;;) {
      const auto snapshot = diagnostics->snapshot();
      if (std::ranges::any_of(snapshot.sessions, [&](const auto& session) {
             return session.remote_peer == peer && !session.closed &&
                session.identify_state == forge::net::p2p::identify::state::identified;
          })) co_return true;
      if (std::chrono::steady_clock::now() >= deadline) co_return false;
      timer.expires_after(10ms);
      co_await timer.async_wait(boost::asio::use_awaitable);
   }
}

void connect(fixture::application& client, fixture::application& server) {
   const auto endpoint = server.node()->local_endpoint();
   BOOST_REQUIRE(endpoint);
   forge::asio::blocking::run(client.runtime(), client.node()->connect(*endpoint));
   BOOST_REQUIRE(forge::asio::blocking::run(client.runtime(), identified(client, server.node()->local_peer())));
   BOOST_REQUIRE(forge::asio::blocking::run(server.runtime(), identified(server, client.node()->local_peer())));
}

boost::asio::awaitable<std::size_t> metadata(runtime::capabilities::connection& connection, core::swarm_id id) {
   auto call = co_await connection.remote().async_open<&forge::swarm::api::peer::metadata>(
      forge::swarm::protocol::metadata_request{id, {}}, forge::api::auth::authenticated_caller{});
   auto failure = std::exception_ptr{};
   auto bytes = std::size_t{};
   try { while (auto chunk = co_await call.async_read()) bytes += chunk->data.size(); }
   catch (...) { failure = std::current_exception(); }
   try { co_await call.async_finish(); } catch (...) { failure = std::current_exception(); }
   if (failure) std::rethrow_exception(failure);
   co_return bytes;
}

boost::asio::awaitable<void> content_denied(runtime::capabilities::connection& connection, core::swarm_id id) {
   auto call = co_await connection.remote().async_open<&forge::swarm::api::peer::exchange>(
      forge::swarm::protocol::exchange_request{id, {}}, forge::api::auth::authenticated_caller{});
   auto failure = std::exception_ptr{};
   try {
      const auto output = co_await call.async_read();
      BOOST_CHECK(!output);
   } catch (...) { failure = std::current_exception(); }
   try { co_await call.async_finish(); } catch (...) { failure = std::current_exception(); }
   if (failure) std::rethrow_exception(failure);
}

forge::net::p2p::dht::key key(core::swarm_id id) {
   return {forge::multiformats::multihash::sha2_256(forge::raw::pack(
      std::tuple{std::string{"forge.swarm.providers/v1"}, std::string{}, id})).encode()};
}

void transfer(bool funded) {
   auto source = swarm_test::fixture{};
   auto destination = swarm_test::fixture{};
   const auto bytes = swarm_test::data(2U * core::block_size);
   const auto manifest = swarm_test::describe({bytes});
   const auto id = core::calculate_swarm_id(manifest);
   auto store = std::shared_ptr<forge::swarm::storage::file_store>{
      swarm_test::run(forge::swarm::storage::file_store::async_open(source.options(manifest)))};
   swarm_test::fill(*store, manifest, {bytes});
   auto output = std::shared_ptr<forge::swarm::storage::file_store>{
      swarm_test::run(forge::swarm::storage::file_store::async_open(destination.options(manifest)))};
   auto policy = std::make_shared<product_policy>();
   auto server_options = funded ? sn::plugin_options{} : fixture::free_options();
   if (funded) server_options.admission = policy;
   auto server = fixture::application{funded ? "swarm-funded-source" : "swarm-free-source", server_options};
   auto client = fixture::application{funded ? "swarm-funded-client" : "swarm-free-client", fixture::free_options()};
   server.configure(fixture::configuration()); client.configure(fixture::configuration());
   auto cleanup = boost::scope::scope_exit{[&] { stop(client); stop(server); }};
   forge::asio::blocking::run(server.runtime(), server.startup());
   forge::asio::blocking::run(client.runtime(), client.startup());
   connect(client, server);
   BOOST_CHECK_THROW((void)server.swarm()->async_find(id), sn::exceptions::metadata_unavailable);
   if (funded) {
      const auto principal = forge::api::auth::authenticated_caller{forge::api::auth::caller_source::p2p_peer,
         core::sha256::hash(client.node()->local_peer().to_bytes())};
      const auto lock = std::scoped_lock{policy->mutex};
      policy->grant = std::make_shared<runtime::quota>(runtime::quota::options{
         .content = id, .requester = principal, .bytes = bytes.size()});
   }
   auto sharing = forge::asio::blocking::run(server.runtime(), server.swarm()->async_share(manifest, store));
   auto routes = std::vector<std::unique_ptr<runtime::capabilities::route>>{};
   routes.push_back(client.swarm()->make_route(server.node()->local_peer(), fixture::addresses(server)));
   auto download = forge::asio::blocking::run(client.runtime(), client.swarm()->async_download(
      manifest, output, {}, std::move(routes), std::chrono::steady_clock::now() + 10s));
   forge::asio::blocking::run(client.runtime(), download.async_wait());
   BOOST_CHECK(download.inspect().complete);
   BOOST_CHECK_EQUAL(download.inspect().traffic.received, bytes.size());
   BOOST_CHECK_EQUAL(download.inspect().traffic.verified, bytes.size());
   BOOST_CHECK(output->snapshot().published);
   BOOST_CHECK(swarm_test::read_file(destination.root / "result") == bytes);
   forge::asio::blocking::run(client.runtime(), download.async_close());
   forge::asio::blocking::run(server.runtime(), sharing.async_close());
   if (funded) {
      BOOST_CHECK(policy->authenticated.load());
      BOOST_CHECK_GT(policy->calls.load(), 0U);
      BOOST_CHECK_EQUAL(policy->grant->inspect().traffic.queued, bytes.size());
      BOOST_CHECK_EQUAL(policy->grant->inspect().traffic.sent, 0U);
      BOOST_CHECK_EQUAL(policy->grant->inspect().held, 0U);
   }
   // Per-operation and plugin stop never stop these caller-owned stores/pools.
   auto retained = swarm_test::run(store->async_read({0, 0}, 0, 17));
   BOOST_CHECK_EQUAL(retained.size(), 17U);
   BOOST_CHECK(!source.disk.snapshot().stopped);
   BOOST_CHECK(!source.cpu.snapshot().stopped);
   swarm_test::run(output->async_close()); swarm_test::run(store->async_close());
}
} // namespace

BOOST_AUTO_TEST_CASE(swarm_plugin_api_rejects_before_startup_and_after_shutdown) {
   auto runtime = forge::asio::runtime{};
   auto plugin = sn::plugin{};
   auto apis = forge::api::core::registry{};
   auto installer = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(installer));
   auto api = apis.get<sn::api>({.id = {"forge.plugins.swarm.node"}, .major = 1});
   BOOST_CHECK_THROW((void)api->async_share({}, {}), sn::exceptions::not_running);
   BOOST_CHECK_THROW((void)api->async_find({}), sn::exceptions::not_running);
   plugin.request_stop();
   forge::asio::blocking::run(runtime, plugin.shutdown());
   BOOST_CHECK_THROW((void)api->async_download({}, {}), sn::exceptions::not_running);
   BOOST_CHECK_THROW((void)api->make_route({}, {}), sn::exceptions::not_running);
}

BOOST_AUTO_TEST_CASE(swarm_plugin_missing_cpu_and_object_db_executor_fail_without_hidden_workers) {
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto events = forge::app::event_journal{};
   auto apis = forge::api::core::registry{};
   auto context = forge::app::plugin_context{tasks, apis, events};
   auto plugin = sn::plugin{fixture::free_options()};
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, plugin.initialize(context)), sn::exceptions::invalid_config);
   forge::asio::blocking::run(runtime, plugin.shutdown());
   auto cpu = forge::asio::compute::pool{{.worker_threads = 1}};
   auto options = fixture::free_options();
   options.cpu = cpu.get_executor();
   options.initial_config.metadata.mode = "object";
   auto named = sn::plugin{options};
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, named.initialize(context)), sn::exceptions::invalid_config);
   forge::asio::blocking::run(runtime, named.shutdown());
   BOOST_CHECK(!cpu.snapshot().stopped);
}

BOOST_AUTO_TEST_CASE(swarm_plugin_missing_policy_fails_closed_and_free_cannot_override_product_policy) {
   auto app = fixture::application{"swarm-no-policy"};
   app.configure(fixture::configuration());
   auto cleanup = boost::scope::scope_exit{[&] { stop(app); }};
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), app.startup()), sn::exceptions::invalid_config);
   auto options = fixture::free_options();
   options.admission = std::make_shared<product_policy>();
   auto plugin = sn::plugin{options};
   auto runtime = forge::asio::runtime{};
   auto document = forge::config::core::document{};
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, plugin.configure({document, "plugins.swarm.node"})),
      sn::exceptions::invalid_config);
   forge::asio::blocking::run(runtime, plugin.shutdown());
}

BOOST_AUTO_TEST_CASE(swarm_plugin_official_free_transfer_uses_owned_route) { transfer(false); }
BOOST_AUTO_TEST_CASE(swarm_plugin_official_funded_transfer_preserves_trusted_identity_and_quota) { transfer(true); }

BOOST_AUTO_TEST_CASE(swarm_plugin_independent_reader_discovers_unknown_provider_and_downloads_real_file) {
   auto bootstrap = fixture::application{"swarm-dht-bootstrap", fixture::free_options()};
   bootstrap.configure(fixture::configuration());
   auto bootstrap_cleanup = boost::scope::scope_exit{[&] { stop(bootstrap); }};
   forge::asio::blocking::run(bootstrap.runtime(), bootstrap.startup());
   const auto endpoint = bootstrap.node()->local_endpoint();
   BOOST_REQUIRE(endpoint);
   const auto client_config = [&] {
      using value = forge::config::core::value;
      auto result = fixture::configuration();
      result.set("plugins.net.p2p.node.bootstrap", value::array_type{endpoint->to_string()});
      result.set("plugins.net.p2p.node.bootstrap-policy.requirement", "require-connection");
      result.set("plugins.net.p2p.node.dht.profiles", value::array_type{value{value::object_type{
         {"kind", "amino-v1"}, {"mode", "client"}, {"protocol", "/ipfs/kad/1.0.0"},
         {"peers", true}, {"providers", true}, {"values", true}}}});
      return result;
   };
   auto provider = fixture::application{"swarm-dht-provider", fixture::free_options()};
   auto reader = fixture::application{"swarm-dht-reader", fixture::free_options()};
   provider.configure(client_config()); reader.configure(client_config());
   auto cleanup = boost::scope::scope_exit{[&] { stop(reader); stop(provider); }};
   forge::asio::blocking::run(provider.runtime(), provider.startup());
   BOOST_REQUIRE(forge::asio::blocking::run(provider.runtime(), identified(provider, bootstrap.node()->local_peer())));
   auto source = swarm_test::fixture{};
   auto destination = swarm_test::fixture{};
   const auto bytes = swarm_test::data(3U * core::block_size + 19U);
   const auto manifest = swarm_test::describe({bytes});
   auto store = std::shared_ptr<forge::swarm::storage::file_store>{
      swarm_test::run(forge::swarm::storage::file_store::async_open(source.options(manifest)))};
   auto output = std::shared_ptr<forge::swarm::storage::file_store>{
      swarm_test::run(forge::swarm::storage::file_store::async_open(destination.options(manifest)))};
   swarm_test::fill(*store, manifest, {bytes});
   auto share = forge::asio::blocking::run(provider.runtime(), provider.swarm()->async_share(manifest, store));
   forge::asio::blocking::run(reader.runtime(), reader.startup());
   BOOST_REQUIRE(forge::asio::blocking::run(reader.runtime(), identified(reader, bootstrap.node()->local_peer())));
   auto diagnostics = reader.apis().get<forge::plugins::net::p2p::node::diagnostics_api>(
      {.id = {"forge.plugins.net.p2p.node.diagnostics"}, .major = 1});
   const auto before = diagnostics->snapshot();
   const auto provider_id = provider.node()->local_peer();
   BOOST_REQUIRE(std::ranges::none_of(before.peers, [&](const auto& peer) { return peer.peer == provider_id; }));
   BOOST_REQUIRE(std::ranges::none_of(before.sessions, [&](const auto& session) { return session.remote_peer == provider_id; }));
   // No explicit routes or target connect: the plugin must retain the DHT
   // AddrInfo across discovery and pass it through the native scoped dial.
   auto download = forge::asio::blocking::run(reader.runtime(), reader.swarm()->async_download(
      manifest, output, {}, {}, std::chrono::steady_clock::now() + 10s));
   forge::asio::blocking::run(reader.runtime(), download.async_wait());
   BOOST_CHECK(download.inspect().complete);
   BOOST_CHECK_EQUAL(download.inspect().traffic.verified, bytes.size());
   BOOST_CHECK(output->snapshot().published);
   BOOST_CHECK(swarm_test::read_file(destination.root / "result") == bytes);
   BOOST_CHECK_EQUAL(diagnostics->routing_status({"/ipfs/kad/1.0.0"}).active, 1U);
   forge::asio::blocking::run(reader.runtime(), download.async_close());
   forge::asio::blocking::run(provider.runtime(), share.async_close());
   swarm_test::run(output->async_close()); swarm_test::run(store->async_close());
}

BOOST_AUTO_TEST_CASE(swarm_plugin_official_metadata_and_content_share_fail_closed_product_policy) {
   auto source = swarm_test::fixture{};
   const auto bytes = swarm_test::data(core::block_size);
   const auto manifest = swarm_test::describe({bytes});
   const auto id = core::calculate_swarm_id(manifest);
   auto store = std::shared_ptr<forge::swarm::storage::file_store>{
      swarm_test::run(forge::swarm::storage::file_store::async_open(source.options(manifest)))};
   swarm_test::fill(*store, manifest, {bytes});
   auto policy = std::make_shared<product_policy>();
   auto options = sn::plugin_options{}; options.admission = policy;
   auto server = fixture::application{"swarm-policy-server", options};
   auto client = fixture::application{"swarm-policy-client", fixture::free_options()};
   server.configure(fixture::configuration()); client.configure(fixture::configuration());
   auto cleanup = boost::scope::scope_exit{[&] { stop(client); stop(server); }};
   forge::asio::blocking::run(server.runtime(), server.startup());
   forge::asio::blocking::run(client.runtime(), client.startup());
   connect(client, server);
   auto share = forge::asio::blocking::run(server.runtime(), server.swarm()->async_share(manifest, store));
   auto route = client.swarm()->make_route(server.node()->local_peer(), fixture::addresses(server));
   auto connection = forge::asio::blocking::run(client.runtime(), route->async_connect({}, std::chrono::steady_clock::now() + 5s));
   BOOST_CHECK_THROW(forge::asio::blocking::run(client.runtime(), metadata(*connection, id)),
      forge::api::core::exceptions::invocation_denied);
   {
      const auto lock = std::scoped_lock{policy->mutex};
      policy->metadata_only = true;
      policy->grant = std::make_shared<runtime::quota>(runtime::quota::options{
         .content = id, .requester = {forge::api::auth::caller_source::p2p_peer,
            core::sha256::hash(client.node()->local_peer().to_bytes())}, .bytes = 1024U * 1024U});
   }
   BOOST_CHECK_GT(forge::asio::blocking::run(client.runtime(), metadata(*connection, id)), 0U);
   BOOST_CHECK_THROW(forge::asio::blocking::run(client.runtime(), content_denied(*connection, id)),
      forge::api::core::exceptions::invocation_denied);
   BOOST_CHECK(policy->authenticated.load());
   forge::asio::blocking::run(client.runtime(), connection->async_close());
   forge::asio::blocking::run(server.runtime(), share.async_close());
   swarm_test::run(store->async_close());
}

BOOST_AUTO_TEST_CASE(swarm_plugin_route_awaitable_owns_context_and_repeats_after_facade_release) {
   auto server = fixture::application{"swarm-route-server", fixture::free_options()};
   auto client = fixture::application{"swarm-route-client", fixture::free_options()};
   server.configure(fixture::configuration()); client.configure(fixture::configuration());
   auto cleanup = boost::scope::scope_exit{[&] { stop(client); stop(server); }};
   forge::asio::blocking::run(server.runtime(), server.startup());
   forge::asio::blocking::run(client.runtime(), client.startup());
   auto route = client.swarm()->make_route(server.node()->local_peer(), fixture::addresses(server));
   auto first = route->async_connect({}, std::chrono::steady_clock::now() + 5s);
   auto second = route->async_connect({}, std::chrono::steady_clock::now() + 5s);
   route.reset();
   auto one = forge::asio::blocking::run(client.runtime(), std::move(first));
   auto two = forge::asio::blocking::run(client.runtime(), std::move(second));
   BOOST_CHECK(one->remote_identity() == two->remote_identity());
   BOOST_CHECK(one->remote_identity().fingerprint == core::sha256::hash(server.node()->local_peer().to_bytes()));
   forge::asio::blocking::run(client.runtime(), one->async_close());
   forge::asio::blocking::run(client.runtime(), two->async_close());
}

BOOST_AUTO_TEST_CASE(swarm_plugin_official_publication_replacement_cancels_idle_engine_and_withdraws_provider) {
   auto source = swarm_test::fixture{};
   const auto bytes = swarm_test::data(core::block_size);
   const auto manifest = swarm_test::describe({bytes});
   const auto id = core::calculate_swarm_id(manifest);
   auto store = std::shared_ptr<forge::swarm::storage::file_store>{
      swarm_test::run(forge::swarm::storage::file_store::async_open(source.options(manifest)))};
   swarm_test::fill(*store, manifest, {bytes});
   auto app = fixture::application{"swarm-replace", fixture::free_options()};
   auto client = fixture::application{"swarm-replace-client", fixture::free_options()};
   app.configure(fixture::configuration()); client.configure(fixture::configuration());
   auto cleanup = boost::scope::scope_exit{[&] { stop(client); stop(app); }};
   forge::asio::blocking::run(app.runtime(), app.startup());
   forge::asio::blocking::run(client.runtime(), client.startup());
   connect(client, app);
   auto old_api = app.swarm();
   auto share = forge::asio::blocking::run(app.runtime(), old_api->async_share(manifest, store));
   BOOST_CHECK(!forge::asio::blocking::run(app.runtime(), app.dht()->find_providers(
      {"/ipfs/kad/1.0.0"}, key(id))).empty());
   auto next_engine = std::make_shared<runtime::engine>(app.scheduler(), app.compute(), std::make_shared<runtime::free_admission>());
   auto registry = forge::api::core::registry{};
   registry.install<forge::swarm::api::peer>(next_engine);
   auto next = app.node()->publish_api(forge::api::core::binding().serve(registry)
      .export_api<forge::swarm::api::peer>({.id = {"forge.swarm.peer"}, .major = 1}).build(), {"/forge/swarm/1.0.0"});
   BOOST_CHECK(share.inspect().canceled);
   forge::asio::blocking::run(app.runtime(), share.async_close());
   BOOST_CHECK(forge::asio::blocking::run(app.runtime(), app.dht()->find_providers(
      {"/ipfs/kad/1.0.0"}, key(id))).empty());
   app.swarm_owner().request_stop();
   forge::asio::blocking::run(app.runtime(), app.swarm_owner().shutdown());
   BOOST_CHECK(next.active());
   BOOST_CHECK_THROW((void)old_api->async_share(manifest, store), sn::exceptions::not_running);
   auto next_share = forge::asio::blocking::run(app.runtime(), next_engine->async_share(manifest, store));
   auto route = client.swarm()->make_route(app.node()->local_peer(), fixture::addresses(app));
   auto connection = forge::asio::blocking::run(client.runtime(), route->async_connect({}, std::chrono::steady_clock::now() + 5s));
   BOOST_CHECK_GT(forge::asio::blocking::run(client.runtime(), metadata(*connection, id)), 0U);
   forge::asio::blocking::run(client.runtime(), connection->async_close());
   forge::asio::blocking::run(app.runtime(), next_share.async_close());
   forge::asio::blocking::run(app.runtime(), next.async_close());
   forge::asio::blocking::run(app.runtime(), next_engine->async_close());
   BOOST_CHECK(!source.cpu.snapshot().stopped);
   swarm_test::run(store->async_close());
}
