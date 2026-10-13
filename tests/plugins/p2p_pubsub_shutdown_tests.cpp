#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../quic_p2p/libp2p_identity_fixture.hxx"

import forge.api.core.descriptor;
import forge.api.core.registry;
import forge.app.events;
import forge.app.plugin_context;
import forge.asio.notification;
import forge.asio.runtime;
import forge.asio.task;
import forge.config.core.component;
import forge.config.core.document;
import forge.config.core.value;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.identify;
import forge.net.p2p.node;
import forge.net.p2p.pubsub;
import forge.net.p2p.stream;
import forge.plugins.crypto.secrets.api;
import forge.plugins.crypto.secrets.types;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.configuration_api;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.plugin;
import forge.plugins.net.p2p.node.pubsub_api;
import forge.plugins.net.p2p.node.types;

namespace {
namespace p2p = forge::net::p2p;
namespace core = forge::net::p2p::pubsub;
namespace np = forge::plugins::net::p2p::node;
namespace secrets = forge::plugins::crypto::secrets;
using namespace std::chrono_literals;

class identity_source final : public secrets::api {
 public:
   forge::tests::p2p::identity_fixture identity;
   explicit identity_source(std::string name) : identity{forge::tests::p2p::make_identity_fixture(name)} {}
   boost::asio::awaitable<secrets::snapshot> status(secrets::query) override {
      co_return secrets::snapshot{.configured_secrets = 2};
   }
   boost::asio::awaitable<secrets::get_result> get_bytes(secrets::get_request request) override {
      const auto* bytes = request.secret_id == "shutdown/cert" ? &identity.certificate_pem
          : request.secret_id == "shutdown/key" ? &identity.private_key_pem : nullptr;
      if (!bytes) { throw std::invalid_argument{"unknown shutdown fixture secret"}; }
      co_return secrets::get_result{.secret_id = request.secret_id, .bytes = {bytes->begin(), bytes->end()}};
   }
   boost::asio::awaitable<secrets::derive_result> derive_hkdf_sha256(secrets::derive_request) override {
      throw std::logic_error{"not a fixture operation"}; co_return secrets::derive_result{};
   }
   boost::asio::awaitable<secrets::aead_encrypt_result> encrypt_aes_gcm(secrets::aead_encrypt_request) override {
      throw std::logic_error{"not a fixture operation"}; co_return secrets::aead_encrypt_result{};
   }
   boost::asio::awaitable<secrets::aead_decrypt_result> decrypt_aes_gcm(secrets::aead_decrypt_request) override {
      throw std::logic_error{"not a fixture operation"}; co_return secrets::aead_decrypt_result{};
   }
};

template<class Future> decltype(auto) joined(Future& result, std::string_view phase) {
   if (result.wait_for(5s) != std::future_status::ready) {
      // Never unwind a node/runtime while an owned callback or destructor is live.
      std::cerr << "official PubSub shutdown fixture exceeded its owned join budget: " << phase << '\n';
      std::terminate();
   }
   return result.get();
}

struct callback_barrier {
   forge::asio::notification released;
   std::atomic_bool release_requested{false};
   std::atomic_bool exited{false};
   std::atomic_uint calls{0};
   std::promise<np::message> entered;
   std::future<np::message> arrival = entered.get_future();

   boost::asio::awaitable<core::validation_result> receive(np::message value) {
      // Deliberately cooperative user code: cancellation cannot stand in for
      // joining the callback. Only the explicit test release permits its exit.
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
      if (calls.fetch_add(1) != 0) { co_return core::validation_result::ignore; }
      entered.set_value(std::move(value));
      for (;;) {
         const auto epoch = released.epoch();
         if (release_requested.load()) { break; }
         co_await released.async_wait(epoch);
      }
      exited.store(true);
      co_return core::validation_result::accept;
   }
   void release() noexcept { release_requested.store(true); released.notify(); }
};

struct destruction_barrier {
   std::mutex mutex;
   std::condition_variable changed;
   bool entered = false;
   bool released = false;
   std::atomic_bool finished{false};

   void wait() noexcept {
      auto lock = std::unique_lock{mutex};
      entered = true;
      changed.notify_all();
      if (!changed.wait_for(lock, 5s, [&] { return released; })) {
         std::cerr << "official PubSub shutdown capture was not released\n";
         std::terminate();
      }
      finished.store(true);
   }
   void await_entry() {
      auto lock = std::unique_lock{mutex};
      if (!changed.wait_for(lock, 5s, [&] { return entered; })) {
         throw std::runtime_error{"official node shutdown did not retire the callback capture"};
      }
   }
   void release() noexcept {
      { const auto lock = std::scoped_lock{mutex}; released = true; }
      changed.notify_all();
   }
};

struct retiring_capture {
   std::shared_ptr<destruction_barrier> barrier;
   explicit retiring_capture(std::shared_ptr<destruction_barrier> value) : barrier(std::move(value)) {}
   ~retiring_capture() { barrier->wait(); }
};

struct network_fixture {
   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 4}};
   forge::asio::task::scheduler scheduler{runtime};
   forge::api::core::registry apis;
   forge::app::event_journal events{};
   forge::app::plugin_context context{scheduler, apis, events};
   np::plugin node;
   std::shared_ptr<np::api> host;
   std::shared_ptr<np::pubsub_api> pubsub;
   std::string label;

   explicit network_fixture(std::string name) : label(std::move(name)) {
      // Test mode permits memory persistence only; TCP still uses real identity
      // material and libp2p TLS. Unsigned PubSub isolates callback ownership.
      apis.install<secrets::api>(std::make_shared<identity_source>(label));
      auto config = forge::config::core::document{};
      config.set("plugins.net.p2p.node.allow-insecure-test-mode", true);
      config.set("plugins.net.p2p.node.identity.certificate-secret", "shutdown/cert");
      config.set("plugins.net.p2p.node.identity.private-key-secret", "shutdown/key");
      config.set("plugins.net.p2p.node.listen", forge::config::core::value::array_type{"/ip4/127.0.0.1/tcp/0"});
      config.set("plugins.net.p2p.node.topology.mode", "static-only");
      config.set("plugins.net.p2p.node.pubsub.enabled", true);
      config.set("plugins.net.p2p.node.pubsub.sign-publishes", false);
      run(node.configure({config, "plugins.net.p2p.node"}), "configure");
      auto installer = forge::api::core::installer{apis};
      run(node.provide(installer), "provide");
      auto options = core::options{};
      options.signatures = core::signature_policy::strict_no_sign;
      apis.get<np::configuration_api>(np::configuration_api::ref())->set_pubsub_options(std::move(options));
      run(node.initialize(context), "initialize");
      run(node.after_initialize(), "after_initialize");
      run(node.startup(), "startup");
      host = apis.get<np::api>(np::api::ref()).shared();
      pubsub = apis.get<np::pubsub_api>(np::pubsub_api::ref()).shared();
   }
   ~network_fixture() {
      node.request_stop();
      try { run(node.shutdown(), "fixture cleanup: node.shutdown"); }
      catch (const std::exception& error) { BOOST_ERROR("official PubSub node cleanup: " << error.what()); }
      run(scheduler.shutdown(), "fixture cleanup: scheduler.shutdown");
   }
   template<class T> std::future<T> start(boost::asio::awaitable<T> operation) {
      return boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
   }
   template<class T> T run(boost::asio::awaitable<T> operation, std::string_view phase) {
      auto result = start(std::move(operation));
      return joined(result, label + ": " + std::string{phase});
   }
};

boost::asio::awaitable<bool> await_mesh(std::shared_ptr<np::pubsub_api> receiver,
                                      std::shared_ptr<np::pubsub_api> sender) {
   const auto deadline = std::chrono::steady_clock::now() + 4s;
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   for (;;) {
      if (receiver->snapshot().core.mesh_edges != 0 && sender->snapshot().core.mesh_edges != 0) { co_return true; }
      if (std::chrono::steady_clock::now() >= deadline) { co_return false; }
      // RPC tracing precedes state mutation; only the actual mesh predicate
      // witnesses readiness. Keep its bound below the unchanged outer join.
      timer.expires_at(std::min(deadline, std::chrono::steady_clock::now() + 5ms));
      co_await timer.async_wait(boost::asio::use_awaitable);
   }
}

class shutdown_call {
 public:
   shutdown_call(np::plugin& node, std::shared_ptr<callback_barrier> callback,
                 std::shared_ptr<destruction_barrier> destruction)
       : callback_(std::move(callback)), destruction_(std::move(destruction)),
         work_(boost::asio::make_work_guard(context_)),
         result_(boost::asio::co_spawn(context_, node.shutdown(), boost::asio::use_future).share()) {}
   ~shutdown_call() {
      callback_->release();
      destruction_->release();
      resume();
      try { joined(result_, "receiver shutdown caller: fail-safe cleanup join"); }
      catch (const std::exception& error) { BOOST_ERROR("official node shutdown caller: " << error.what()); }
      work_.reset();
      context_.stop();
      worker_.join();
   }
   void poll() { context_.poll(); }
   void resume() {
      if (!worker_.joinable()) { worker_ = std::thread{[this] { context_.run(); }}; }
   }
   bool pending() const { return result_.wait_for(0s) == std::future_status::timeout; }
   void finish() { joined(result_, "receiver shutdown caller: final join after both barrier releases"); }
 private:
   std::shared_ptr<callback_barrier> callback_;
   std::shared_ptr<destruction_barrier> destruction_;
   boost::asio::io_context context_;
   boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_;
   std::shared_future<void> result_;
   std::thread worker_;
};
} // namespace

BOOST_AUTO_TEST_CASE(p2p_pubsub_official_node_shutdown_joins_real_delivery_and_capture_retirement) {
   auto receiver = network_fixture{"shutdown-pubsub-receiver"};
   auto sender = network_fixture{"shutdown-pubsub-sender"};
   const auto subject = core::topic{"shutdown-owned-delivery"};
   auto callback = std::make_shared<callback_barrier>();
   auto destruction = std::make_shared<destruction_barrier>();
   auto capture = std::make_shared<retiring_capture>(destruction);
   const auto weak_capture = std::weak_ptr{capture};
   // Release before fixture teardown even if a mandatory witness fails.
   auto release = boost::scope::scope_exit{[callback, destruction] {
      callback->release(); destruction->release();
   }};

   const auto subscription = receiver.run(receiver.pubsub->subscribe(subject,
       [callback, capture](np::message value) -> boost::asio::awaitable<core::validation_result> {
          (void)capture;
          return callback->receive(std::move(value));
       }), "subscribe held full callback");
   BOOST_REQUIRE(subscription.id != 0);
   capture.reset(); // Only the actual consumer registration owns this user capture.
   static_cast<void>(sender.run(sender.pubsub->subscribe(subject,
       [](np::message) -> boost::asio::awaitable<core::validation_result> {
          co_return core::validation_result::accept;
       }), "subscribe sender full callback"));
   const auto address = receiver.host->local_endpoint();
   BOOST_REQUIRE(address);
   const auto connected = sender.run(sender.host->connect(*address), "TCP connect/TLS/Identify");
   BOOST_REQUIRE(connected.remote_peer == receiver.host->local_peer());
   BOOST_REQUIRE(connected.identify_state == p2p::identify::state::identified);
   BOOST_REQUIRE(connected.muxer.value == "/yamux/1.0.0");
   const auto connections = sender.apis.get<np::diagnostics_api>(np::diagnostics_api::ref())->snapshot();
   BOOST_REQUIRE(std::ranges::any_of(connections.sessions, [&](const auto& session) {
      return session.id == connected.id && session.remote_peer == receiver.host->local_peer() && !session.closed &&
          session.authentication == p2p::peer_authentication::libp2p_tls &&
          session.identify_state == p2p::identify::state::identified;
   }));
   BOOST_REQUIRE(sender.run(await_mesh(receiver.pubsub, sender.pubsub), "await both actual mesh edges"));

   auto publication = sender.start(sender.pubsub->publish(subject, std::vector<std::uint8_t>{4, 2}));
   const auto observed = joined(callback->arrival, "receiver actual full delivery: callback entry");
   // Mandatory real delivery witness, not a direct handler invocation.
   BOOST_CHECK(observed.source == sender.host->local_peer());
   BOOST_CHECK(observed.subject == subject);
   BOOST_CHECK(observed.data == (std::vector<std::uint8_t>{4, 2}));
   static_cast<void>(joined(publication, "sender publish completion while receiver callback is held"));
   BOOST_TEST(callback->calls.load() == 1U);
   BOOST_TEST(!callback->exited.load());
   BOOST_TEST(!weak_capture.expired());

   auto stopping = shutdown_call{receiver.node, callback, destruction};
   // Drain this caller context to its real async join, while the callback remains
   // held on the receiver runtime. Closed admission proves shutdown reached stop.
   stopping.poll();
   BOOST_CHECK_THROW((void)receiver.pubsub->snapshot(), p2p::exceptions::canceled);
   BOOST_CHECK(stopping.pending());
   BOOST_TEST(!callback->exited.load());
   BOOST_TEST(!destruction->finished.load());

   callback->release();
   stopping.resume();
   destruction->await_entry(); // Mandatory destructor entry before the pending assertion.
   BOOST_TEST(callback->exited.load());
   BOOST_CHECK(stopping.pending());
   BOOST_TEST(!destruction->finished.load());
   destruction->release();
   stopping.finish();
   BOOST_TEST(destruction->finished.load());
   BOOST_TEST(weak_capture.expired());
   BOOST_TEST(callback->calls.load() == 1U);
}
