module;

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <future>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>
#include "../quic_p2p/libp2p_identity_fixture.hxx"

module forge.plugins.net.p2p.node.plugin;

import forge.api.core.descriptor;
import forge.api.core.handle;
import forge.api.core.registry;
import forge.api.core.types;
import forge.app.events;
import forge.app.plugin_context;
import forge.app.signals;
import forge.asio.notification;
import forge.asio.exceptions;
import forge.asio.gate;
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
import forge.net.p2p.lifecycle;
import forge.net.p2p.node;
import forge.net.p2p.pubsub;
import forge.net.p2p.stream;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.pubsub_api;
import forge.plugins.net.p2p.node.types;
import forge.plugins.crypto.secrets.api;
import forge.plugins.crypto.secrets.types;

#include "../../../plugins/net/p2p/node/details/pubsub_service.hxx"
#include "../../../plugins/net/p2p/node/details/pubsub_policy.hxx"
#include "details/pubsub_service_fixture.hxx"

namespace {
namespace p2p = forge::net::p2p;
namespace core = forge::net::p2p::pubsub;
namespace facade = forge::plugins::net::p2p::node;
namespace np = forge::plugins::net::p2p::node;
using namespace std::chrono_literals;
namespace secrets = forge::plugins::crypto::secrets;

class identity_source final : public secrets::api {
 public:
   forge::tests::p2p::identity_fixture identity;
   explicit identity_source(std::string name) : identity{forge::tests::p2p::make_identity_fixture(name)} {}
   boost::asio::awaitable<secrets::snapshot> status(secrets::query) override { co_return secrets::snapshot{.configured_secrets = 2}; }
   boost::asio::awaitable<secrets::get_result> get_bytes(secrets::get_request request) override {
      const auto* bytes = request.secret_id == "partial/cert" ? &identity.certificate_pem
          : request.secret_id == "partial/key" ? &identity.private_key_pem : nullptr;
      if (!bytes) { throw std::invalid_argument{"unknown Partial fixture secret"}; }
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

template<class T> T joined(std::future<T>& value) {
   if (value.wait_for(5s) != std::future_status::ready) { std::terminate(); }
   return value.get();
}

struct barrier {
   forge::asio::notification released;
   const std::uint64_t epoch = released.epoch();
   std::promise<void> entered;
   std::future<void> arrival = entered.get_future();
   boost::asio::awaitable<void> wait() {
      entered.set_value();
      static_cast<void>(co_await released.async_wait(epoch));
   }
   void await_entered() { joined(arrival); }
   void release() { released.notify(); }
};

struct destruction_barrier {
   std::mutex mutex;
   std::condition_variable changed;
   bool entered = false;
   bool released = false;

   void wait() noexcept {
      auto lock = std::unique_lock{mutex};
      entered = true;
      changed.notify_all();
      if (!changed.wait_for(lock, 5s, [&] { return released; })) { std::terminate(); }
   }
   void await_entry() {
      auto lock = std::unique_lock{mutex};
      if (!changed.wait_for(lock, 5s, [&] { return entered; })) {
         throw std::runtime_error{"Partial capture destructor was not entered"};
      }
   }
   void release() noexcept {
      const auto lock = std::scoped_lock{mutex};
      released = true;
      changed.notify_all();
   }
};

struct retiring_capture {
   std::shared_ptr<destruction_barrier> barrier;
   explicit retiring_capture(std::shared_ptr<destruction_barrier> value) : barrier{std::move(value)} {}
   ~retiring_capture() { barrier->wait(); }
};

facade::handler accepting() {
   return [](facade::message) -> boost::asio::awaitable<core::validation_result> { co_return core::validation_result::accept; };
}

core::partial_options callbacks() {
   return {
      .requests_partial = true,
      .receive = [](core::partial_event, std::stop_token) -> boost::asio::awaitable<void> { co_return; },
      .gossip = [](core::partial_gossip_event, std::stop_token) -> boost::asio::awaitable<void> { co_return; }};
}

// Holds private operation completion after real native mutations. No application API source is installed.
// Direct callback invocations below prove service ownership, not native wire interoperability.
class observed_operations final : public std::enable_shared_from_this<observed_operations> {
 public:
   std::shared_ptr<p2p::node> native;
   std::mutex mutex;
   std::map<std::string, core::handler> full;
   std::map<std::string, core::partial_options> partial;
   std::map<std::string, core::partial_topic> tokens;
   std::map<std::string, std::shared_ptr<barrier>> sending;
   std::shared_ptr<barrier> enabling;
   std::shared_ptr<barrier> disabling;
   std::atomic_bool fail_enable{false};
   std::atomic_bool fail_disable{false};
   std::atomic_bool fail_restore{false};
   std::atomic_uint leaves{0};
   std::atomic_uint enable_calls{0};
   std::atomic_uint send_calls{0};
   std::atomic_uint send_stops{0};
   np::detail::pubsub_operations operations() {
      auto out = np::detail::pubsub_operations{};
      const auto self = shared_from_this();
      out.subscribe = [self](auto owner, auto subject, auto callback) {
         return self->async_join_topic(std::move(owner), std::move(subject), std::move(callback));
      };
      out.unsubscribe = [self](auto owner, auto subject) {
         return self->async_leave_topic(std::move(owner), std::move(subject));
      };
      out.enable_partial = [self](auto owner, auto subject, auto callback, auto options) {
         return self->async_enable_partial(std::move(owner), std::move(subject), std::move(callback), std::move(options));
      };
      out.disable_partial = [self](auto owner, auto token) {
         return self->async_disable_partial(std::move(owner), std::move(token));
      };
      out.send_partial = [self](auto owner, auto token, auto peer, auto value, auto stop) {
         return self->async_send_partial(std::move(owner), std::move(token), std::move(peer), std::move(value), stop);
      };
      return out;
   }
   p2p::peer_id local_peer() const { return native->local_peer(); }
   core::snapshot snapshot() const { return native->pubsub_snapshot(); }
   boost::asio::awaitable<core::subscription> async_join_topic(
       std::shared_ptr<p2p::node> owner, core::topic topic, core::handler handler) {
      if (fail_restore.load()) { throw std::runtime_error{"controlled restore failure"}; }
      auto result = co_await owner->async_subscribe(topic, handler);
      {
         const auto lock = std::scoped_lock{mutex};
         full[topic.value] = std::move(handler);
      }
      co_return result;
   }
   boost::asio::awaitable<void> async_leave_topic(std::shared_ptr<p2p::node> owner, core::topic topic) {
      ++leaves;
      co_await owner->async_unsubscribe(topic);
      const auto lock = std::scoped_lock{mutex};
      full.erase(topic.value);
      partial.erase(topic.value);
   }
   boost::asio::awaitable<core::partial_topic> async_enable_partial(
       std::shared_ptr<p2p::node> owner, core::topic topic, core::handler handler, core::partial_options options) {
      ++enable_calls;
      auto result = co_await owner->async_subscribe(topic, handler, options);
      {
         const auto lock = std::scoped_lock{mutex};
         full[topic.value] = std::move(handler);
         partial[topic.value] = std::move(options);
         tokens[topic.value] = result;
      }
      if (enabling) { co_await enabling->wait(); }
      if (fail_enable.load()) { throw std::runtime_error{"controlled error after native enable"}; }
      co_return result;
   }
   boost::asio::awaitable<void> async_disable_partial(std::shared_ptr<p2p::node> owner, core::partial_topic token) {
      co_await owner->async_disable_partial(token);
      if (disabling) { co_await disabling->wait(); }
      if (fail_disable.load()) { throw std::runtime_error{"controlled error after native downgrade"}; }
   }
   boost::asio::awaitable<void> async_send_partial(std::shared_ptr<p2p::node> owner, core::partial_topic token,
       p2p::peer_id peer, core::partial_message value, std::stop_token stop) {
      ++send_calls;
      auto hold = std::shared_ptr<barrier>{};
      {
         const auto lock = std::scoped_lock{mutex};
         const auto found = sending.find(peer.value);
         if (found != sending.end()) { hold = found->second; }
      }
      const auto stopped = std::stop_callback{stop, [this] { ++send_stops; }};
      // Controlled private operation admission, not a claim of a stalled native socket write.
      if (hold) { co_await hold->wait(); }
      co_await owner->async_send_partial(std::move(token), std::move(peer), std::move(value), stop);
   }
   core::partial_options observed(core::topic subject) {
      const auto lock = std::scoped_lock{mutex};
      return partial.at(subject.value);
   }
   core::handler full_handler(core::topic subject) {
      const auto lock = std::scoped_lock{mutex};
      return full.at(subject.value);
   }
};

struct fixture {
   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 4}};
   forge::asio::task::scheduler scheduler{runtime};
   facade::pubsub_config settings;
   std::shared_ptr<observed_operations> source = std::make_shared<observed_operations>();
   std::unique_ptr<np::test::pubsub_service_fixture> owner;
   std::shared_ptr<p2p::node> native;
   std::shared_ptr<facade::pubsub_api> api;
   std::vector<std::shared_ptr<barrier>> barriers;

   fixture(bool partial_enabled = true, unsigned handlers = 4, unsigned topics = 1024) {
      settings.enabled = true;
      settings.partial_messages = partial_enabled;
      settings.max_handlers_per_topic = handlers;
      settings.max_topics = topics;
      owner = std::make_unique<np::test::pubsub_service_fixture>(runtime, source->operations(), settings);
      native = owner->native();
      source->native = native;
      run(native->async_start());
      api = owner->interface();
   }
   ~fixture() {
      for (const auto& hold : barriers) { hold->release(); }
      if (source->enabling) { source->enabling->release(); }
      if (source->disabling) { source->disabling->release(); }
      if (owner) {
         owner->request_stop();
         try { run(owner->shutdown()); }
         catch (...) { BOOST_ERROR("Partial fixture shutdown failure"); }
      }
      run(native->async_stop());
      run(scheduler.shutdown());
   }
   template<class T> std::future<T> start(boost::asio::awaitable<T> operation) {
      return boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
   }
   template<class T> T run(boost::asio::awaitable<T> operation) { auto value = start(std::move(operation)); return joined(value); }
   std::shared_ptr<barrier> hold() {
      auto result = std::make_shared<barrier>(); barriers.push_back(result); return result;
   }
   core::partial_topic enable(core::topic topic = {"partial"}, core::partial_options options = callbacks(),
                               facade::handler fallback = accepting()) {
      return run(api->enable_partial(std::move(topic), std::move(fallback), std::move(options)));
   }
   core::event event(core::topic topic = {"partial"}) {
      return {.source = source->local_peer(), .value = {.data = {1}, .subject = std::move(topic)}};
   }
   core::partial_event part(core::partial_topic token) {
      return {.registration = token, .source = source->local_peer(),
              .value = {.subject = token.subject(), .group_id = std::vector<std::uint8_t>{1},
                        .data = std::vector<std::uint8_t>{2}}};
   }
};

// Real authenticated transport coverage uses the official plugin and no private operation interception.
struct network_fixture {
   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 4}};
   forge::asio::task::scheduler scheduler{runtime};
   forge::api::core::registry apis;
   forge::app::signal_bus signals;
   forge::app::event_bus events{};
   forge::app::plugin_context context{scheduler, apis, signals, events};
   np::plugin node;
   std::shared_ptr<np::pubsub_api> api;

   explicit network_fixture(std::string name, std::optional<p2p::endpoint> bootstrap = {}) {
      auto config = forge::config::core::document{};
      apis.install<secrets::api>(std::make_shared<identity_source>(std::move(name)));
      config.set("plugins.net.p2p.node.allow-insecure-test-mode", true);
      config.set("plugins.net.p2p.node.identity.certificate-secret", "partial/cert");
      config.set("plugins.net.p2p.node.identity.private-key-secret", "partial/key");
      config.set("plugins.net.p2p.node.listen", forge::config::core::value::array_type{"/ip4/127.0.0.1/tcp/0"});
      config.set("plugins.net.p2p.node.topology.mode", "static-only");
      config.set("plugins.net.p2p.node.pubsub.enabled", true);
      config.set("plugins.net.p2p.node.pubsub.partial-messages", true);
      config.set("plugins.net.p2p.node.pubsub.max-handlers-per-topic", std::uint64_t{4});
      config.set("plugins.net.p2p.node.pubsub.max-topics", std::uint64_t{1024});
      if (bootstrap) {
         config.set("plugins.net.p2p.node.bootstrap", forge::config::core::value::array_type{bootstrap->to_string()});
         config.set("plugins.net.p2p.node.bootstrap-requirement", "require-connection");
      }
      run(node.configure({config, "plugins.net.p2p.node"}));
      auto provider = forge::api::core::installer{apis};
      run(node.provide(provider));
      run(node.initialize(context));
      run(node.after_initialize());
      run(node.startup());
      api = apis.get<np::pubsub_api>(np::pubsub_api::ref()).shared();
   }
   ~network_fixture() {
      node.request_stop();
      run(node.shutdown());
      run(scheduler.shutdown());
   }
   template<class T> std::future<T> start(boost::asio::awaitable<T> operation) {
      return boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
   }
   template<class T> T run(boost::asio::awaitable<T> operation) { auto value = start(std::move(operation)); return joined(value); }
   boost::asio::awaitable<bool> await_state(std::function<bool()> predicate) {
      const auto deadline = std::chrono::steady_clock::now() + 4s;
      auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
      while (!predicate()) {
         if (std::chrono::steady_clock::now() >= deadline) { co_return false; }
         timer.expires_at(std::min(deadline, std::chrono::steady_clock::now() + 5ms));
         co_await timer.async_wait(boost::asio::use_awaitable);
      }
      co_return true;
   }
   core::partial_topic enable(core::topic subject = {"partial"}, core::partial_options options = callbacks()) {
      return run(api->enable_partial(std::move(subject), accepting(), std::move(options)));
   }
};

} // namespace

BOOST_AUTO_TEST_CASE(p2p_pubsub_partial_contract_opt_in_and_required_handlers) {
   auto old = fixture{false};
   const auto old_options = np::detail::pubsub_options_for(old.settings);
   BOOST_CHECK(old_options.preferred == core::version::v1_1);
   BOOST_CHECK(!old_options.partial_messages);
   BOOST_CHECK_THROW(old.enable(), facade::exceptions::invalid_config);
   auto f = fixture{};
   const auto options = np::detail::pubsub_options_for(f.settings);
   BOOST_CHECK(options.preferred == core::version::v1_3);
   BOOST_CHECK(options.partial_messages);
   BOOST_CHECK_THROW(f.run(f.api->enable_partial({"partial"}, {}, callbacks())), facade::exceptions::handler_limit);
   BOOST_CHECK_THROW(f.run(f.api->enable_partial({"partial"}, accepting(), {})), facade::exceptions::handler_limit);
   const auto contract = np::pubsub_api::describe();
   BOOST_CHECK_EQUAL(contract.id.value, "forge.plugins.net.p2p.node.pubsub");
   BOOST_CHECK_EQUAL(contract.version.major, 1U);
   BOOST_CHECK_EQUAL(contract.version.revision, 0U);
   BOOST_CHECK(contract.supported_surfaces == forge::api::core::surface::local);
   BOOST_CHECK(contract.methods.empty());
   auto node = np::plugin{};
   auto apis = forge::api::core::registry{};
   auto provider = forge::api::core::installer{apis};
   f.run(node.provide(provider));
   BOOST_CHECK(apis.describe(np::pubsub_api::ref()) != nullptr);
   BOOST_CHECK(apis.describe({.id = {"forge.plugins.net.p2p.node.pubsub"}, .major = 2}) == nullptr);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_partial_independent_owners_aggregate_and_exact_tokens) {
   auto f = fixture{};
   const auto ordinary = f.run(f.api->subscribe({"partial"}, accepting()));
   auto rejected = std::atomic_uint{};
   const auto token = f.enable({"partial"}, callbacks(), [&](facade::message) -> boost::asio::awaitable<core::validation_result> {
      ++rejected; co_return core::validation_result::reject;
   });
   BOOST_CHECK_EQUAL(f.api->subscriptions().size(), 1U);
   BOOST_CHECK(f.run(f.source->full_handler({"partial"})(f.event())) == core::validation_result::reject);
   BOOST_CHECK_EQUAL(rejected.load(), 1U);
   f.run(f.api->unsubscribe(ordinary));
   BOOST_CHECK(f.api->subscriptions().empty());
   BOOST_CHECK_EQUAL(f.source->snapshot().topics, 1U);
   f.run(f.api->advertise_partial(token, {1, 2}));
   BOOST_CHECK_EQUAL(f.source->snapshot().partial_groups, 1U);
   f.run(f.api->forget_partial(token, {1, 2}));
   BOOST_CHECK_EQUAL(f.source->snapshot().partial_groups, 0U);
   BOOST_CHECK(f.run(f.api->partial_peers(token)).empty());
   const auto next_ordinary = f.run(f.api->subscribe({"partial"}, accepting()));
   f.run(f.api->disable_partial(token));
   BOOST_CHECK_EQUAL(f.api->subscriptions().size(), 1U);
   BOOST_CHECK(f.run(f.source->full_handler({"partial"})(f.event())) == core::validation_result::accept);
   BOOST_CHECK_THROW(f.run(f.api->disable_partial(token)), facade::exceptions::subscription_not_found);
   const auto replacement = f.enable();
   BOOST_CHECK(token != replacement);
   BOOST_CHECK_THROW(f.run(f.api->forget_partial(token, {1})), facade::exceptions::subscription_not_found);
   auto other = fixture{};
   const auto foreign = other.enable();
   BOOST_CHECK_THROW(f.run(f.api->partial_peers(foreign)), facade::exceptions::subscription_not_found);
   BOOST_CHECK_THROW(f.run(f.api->partial_peers({})), facade::exceptions::subscription_not_found);
   f.run(f.api->disable_partial(replacement));
   f.run(f.api->unsubscribe(next_ordinary));
   BOOST_CHECK_EQUAL(f.source->snapshot().topics, 0U);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_partial_pending_fallback_reserves_limit_and_stop_compensates) {
   auto f = fixture{true, 1};
   f.source->enabling = f.hold();
   auto enabled = f.start(f.api->enable_partial({"partial"}, accepting(), callbacks()));
   auto cleanup = boost::scope::scope_exit{[&] {
      f.source->enabling->release();
      if (enabled.valid()) { try { joined(enabled); } catch (...) {} }
   }};
   f.source->enabling->await_entered();
   BOOST_CHECK_THROW(f.run(f.api->subscribe({"partial"}, accepting())), facade::exceptions::handler_limit);
   BOOST_CHECK_THROW(f.enable(), facade::exceptions::handler_limit);
   f.owner->request_stop();
   auto stopping = f.start(f.owner->shutdown());
   auto join_stop = boost::scope::scope_exit{[&] {
      f.source->enabling->release();
      if (stopping.valid()) { try { joined(stopping); } catch (...) {} }
   }};
   BOOST_CHECK(stopping.wait_for(0ms) != std::future_status::ready);
   f.source->enabling->release();
   BOOST_CHECK_THROW(joined(enabled), p2p::exceptions::canceled);
   joined(stopping);
   BOOST_CHECK_EQUAL(f.source->snapshot().topics, 0U);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_partial_canceled_reservation_cannot_retain_last_ordinary_topic) {
   auto f = fixture{true, 2, 1};
   const auto ordinary = f.run(f.api->subscribe({"partial"}, accepting()));
   auto unsubscribe_context = boost::asio::io_context{};
   auto enable_context = boost::asio::io_context{};
   auto cancel = boost::asio::cancellation_signal{};
   auto leaving = std::future<void>{};
   auto enabling = std::future<core::partial_topic>{};
   auto cleanup = boost::scope::scope_exit{[&] {
      cancel.emit(boost::asio::cancellation_type::all);
      unsubscribe_context.restart(); unsubscribe_context.run_for(5s);
      enable_context.restart(); enable_context.run_for(5s);
      if (leaving.valid()) { try { joined(leaving); } catch (...) { BOOST_ERROR("unsubscribe cleanup failed"); } }
      if (enabling.valid()) {
         try { joined(enabling); }
         catch (const forge::asio::exceptions::canceled&) {}
         catch (...) { BOOST_ERROR("unexpected pending-enable cleanup error"); }
      }
   }};

   leaving = boost::asio::co_spawn(unsubscribe_context, f.api->unsubscribe(ordinary), boost::asio::use_future);
   // A synchronous co_spawn child posts its completion. One turn acquires the topic gate,
   // but leaves the parent's native-owner decision queued on this otherwise idle executor.
   BOOST_REQUIRE_EQUAL(unsubscribe_context.poll_one(), 1U);
   BOOST_REQUIRE(leaving.wait_for(0ms) != std::future_status::ready);
   BOOST_REQUIRE_EQUAL(f.source->leaves.load(), 0U);
   BOOST_CHECK_THROW(f.run(f.api->unsubscribe(ordinary)), facade::exceptions::subscription_not_found);

   enabling = boost::asio::co_spawn(enable_context,
       f.api->enable_partial({"partial"}, accepting(), callbacks()),
       boost::asio::bind_cancellation_slot(cancel.slot(), boost::asio::use_future));
   enable_context.poll(); // Drain reservation work only; the real gate remains owned by unsubscribe.
   BOOST_REQUIRE(enabling.wait_for(0ms) != std::future_status::ready);
   BOOST_REQUIRE_EQUAL(f.source->enable_calls.load(), 0U);
   // The one ordinary slot plus the actual pending fallback fill the configured quota.
   BOOST_CHECK_THROW(f.run(f.api->subscribe({"partial"}, accepting())), facade::exceptions::handler_limit);

   // Cancel while the gate waiter is queued, but keep its cleanup executor paused.
   // Unsubscribe must not treat that still-visible reservation as a native owner.
   cancel.emit(boost::asio::cancellation_type::all);
   unsubscribe_context.run_for(5s);
   joined(leaving);
   BOOST_CHECK_EQUAL(f.source->leaves.load(), 1U);
   BOOST_CHECK_EQUAL(f.source->snapshot().topics, 0U);
   BOOST_CHECK(f.api->subscriptions().empty());
   enable_context.restart(); enable_context.run_for(5s);
   BOOST_CHECK_THROW(joined(enabling), forge::asio::exceptions::canceled);
   BOOST_CHECK_EQUAL(f.source->enable_calls.load(), 0U);
   BOOST_REQUIRE_EQUAL(f.api->snapshot().topics, 0U);
   BOOST_CHECK_EQUAL(f.source->snapshot().topics, 0U);

   // max_topics=1 proves capacity is released without plugin/node shutdown or a cleanup retry.
   const auto replacement = f.run(f.api->subscribe({"after-cancel"}, accepting()));
   BOOST_CHECK_EQUAL(f.source->snapshot().topics, 1U);
   f.run(f.api->unsubscribe(replacement));
   BOOST_CHECK_EQUAL(f.source->snapshot().topics, 0U);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_partial_failed_enable_restores_full_dispatch_not_whole_leave) {
   auto f = fixture{};
   static_cast<void>(f.run(f.api->subscribe({"partial"}, accepting())));
   f.source->fail_enable = true;
   BOOST_CHECK_EXCEPTION(f.enable(), std::runtime_error, [](const std::runtime_error& error) {
      return std::string{error.what()} == "controlled error after native enable";
   });
   BOOST_CHECK_EQUAL(f.source->leaves.load(), 0U);
   BOOST_CHECK_EQUAL(f.source->snapshot().topics, 1U);
   BOOST_CHECK(f.run(f.source->full_handler({"partial"})(f.event())) == core::validation_result::accept);
   f.source->fail_enable = false;
   static_cast<void>(f.enable());
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_partial_failed_downgrade_never_resurrects_admission_and_can_repair) {
   auto f = fixture{};
   static_cast<void>(f.run(f.api->subscribe({"partial"}, accepting())));
   auto received = std::atomic_uint{};
   auto options = callbacks();
   options.receive = [&](core::partial_event, std::stop_token) -> boost::asio::awaitable<void> { ++received; co_return; };
   const auto token = f.enable({"partial"}, std::move(options));
   const auto old = f.source->observed({"partial"});
   f.source->fail_disable = true;
   BOOST_CHECK_EXCEPTION(f.run(f.api->disable_partial(token)), std::runtime_error, [](const std::runtime_error& error) {
      return std::string{error.what()} == "controlled error after native downgrade";
   });
   BOOST_CHECK_THROW(f.run(f.api->advertise_partial(token, {1})), facade::exceptions::subscription_not_found);
   f.run(old.receive(f.part(token), {}));
   BOOST_CHECK_EQUAL(received.load(), 0U);
   BOOST_CHECK_EQUAL(f.api->subscriptions().size(), 1U);
   f.source->fail_disable = false;
   f.run(f.api->disable_partial(token)); // Repair uses full resubscription; stale native token is not retried.
   BOOST_CHECK(f.run(f.source->full_handler({"partial"})(f.event())) == core::validation_result::accept);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_partial_failed_enable_and_restore_retain_cleanup_until_shutdown) {
   auto f = fixture{};
   static_cast<void>(f.run(f.api->subscribe({"partial"}, accepting())));
   f.source->fail_enable = true;
   f.source->fail_restore = true;
   BOOST_CHECK_EXCEPTION(f.enable(), std::runtime_error, [](const std::runtime_error& error) {
      return std::string{error.what()} == "controlled error after native enable";
   });
   BOOST_CHECK_EQUAL(f.api->subscriptions().size(), 1U);
   BOOST_CHECK_EQUAL(f.source->snapshot().topics, 1U);
   f.source->fail_enable = false;
   f.source->fail_restore = false;
   BOOST_CHECK_THROW(f.enable(), facade::exceptions::handler_limit);
   BOOST_CHECK_EXCEPTION(f.run(f.api->subscribe({"partial"}, accepting())), std::runtime_error,
       [](const std::runtime_error& error) { return std::string{error.what()} == "controlled restore failure"; });
   f.owner->request_stop();
   f.run(f.owner->shutdown());
   BOOST_CHECK_EQUAL(f.source->snapshot().topics, 0U);
   BOOST_CHECK_EQUAL(f.source->leaves.load(), 1U);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_partial_retired_callback_receives_all_stop_sources_and_shutdown_joins) {
   auto f = fixture{};
   auto hold = f.hold();
   auto record_stop = std::atomic_bool{};
   auto options = callbacks();
   options.receive = [hold, &record_stop](core::partial_event, std::stop_token stop) -> boost::asio::awaitable<void> {
      const auto observed = std::stop_callback{stop, [&] { record_stop = true; }};
      co_await hold->wait();
   };
   const auto token = f.enable({"partial"}, std::move(options));
   auto event = f.start(f.source->observed({"partial"}).receive(f.part(token), {}));
   auto cleanup = boost::scope::scope_exit{[&] { hold->release(); if (event.valid()) { try { joined(event); } catch (...) {} } }};
   hold->await_entered();
   f.run(f.api->disable_partial(token)); // No self/callback join here.
   BOOST_CHECK(record_stop.load());
   f.owner->request_stop();
   auto stopping = f.start(f.owner->shutdown());
   auto join_stop = boost::scope::scope_exit{[&] { hold->release(); if (stopping.valid()) { try { joined(stopping); } catch (...) {} } }};
   BOOST_CHECK(stopping.wait_for(0ms) != std::future_status::ready);
   hold->release();
   joined(event); joined(stopping);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_partial_native_callback_stop_does_not_retire_registration) {
   auto f = fixture{};
   auto native_stop = std::stop_source{};
   const auto hold = f.hold();
   auto notified = std::atomic_bool{};
   auto options = callbacks();
   options.receive = [hold, &notified](core::partial_event, std::stop_token stop) -> boost::asio::awaitable<void> {
      const auto callback = std::stop_callback{stop, [&] { notified = true; hold->release(); }};
      co_await hold->wait();
   };
   const auto token = f.enable({"partial"}, std::move(options));
   auto event = f.start(f.source->observed({"partial"}).receive(f.part(token), native_stop.get_token()));
   auto cleanup = boost::scope::scope_exit{[&] {
      hold->release();
      if (event.valid()) { try { joined(event); } catch (...) {} }
   }};
   hold->await_entered();
   native_stop.request_stop();
   joined(event);
   BOOST_CHECK(notified.load());
   f.run(f.api->advertise_partial(token, {1}));
   BOOST_CHECK_EQUAL(f.source->snapshot().partial_groups, 1U);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_partial_self_disable_and_delayed_old_callback_are_safe) {
   auto f = fixture{};
   auto calls = std::atomic_uint{};
   auto options = callbacks();
   options.receive = [api = f.api, &calls](core::partial_event event, std::stop_token) -> boost::asio::awaitable<void> {
      ++calls; co_await api->disable_partial(event.registration);
   };
   const auto token = f.enable({"partial"}, std::move(options));
   const auto old = f.source->observed({"partial"});
   auto deferred = old.receive(f.part(token), {});
   f.run(old.receive(f.part(token), {}));
   const auto next = f.enable();
   BOOST_CHECK(token != next);
   f.run(std::move(deferred));
   BOOST_CHECK_EQUAL(calls.load(), 1U);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_partial_service_stop_reaches_active_callback_before_native_stop) {
   auto f = fixture{};
   auto entered = f.hold();
   auto notified = std::atomic_bool{};
   auto options = callbacks();
   options.gossip = [entered, &notified](core::partial_gossip_event, std::stop_token stop) -> boost::asio::awaitable<void> {
      const auto callback = std::stop_callback{stop, [&] { notified = true; entered->release(); }};
      co_await entered->wait();
   };
   const auto token = f.enable({"partial"}, std::move(options));
   auto event = f.start(f.source->observed({"partial"}).gossip(core::partial_gossip_event{.registration = token}, {}));
   auto cleanup = boost::scope::scope_exit{[&] { entered->release(); if (event.valid()) { try { joined(event); } catch (...) {} } }};
   entered->await_entered();
   BOOST_REQUIRE(f.native->lifecycle_state().phase == p2p::lifecycle_phase::maintenance);
   f.owner->request_stop();
   auto stopping = f.start(f.owner->shutdown());
   joined(event); joined(stopping);
   BOOST_CHECK(notified.load());
   BOOST_CHECK(f.native->lifecycle_state().phase == p2p::lifecycle_phase::maintenance);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_partial_send_does_not_block_downgrade_or_independent_peer) {
   auto f = fixture{};
   const auto token = f.enable();
   const auto first = p2p::peer_id{"held-first"};
   const auto second = p2p::peer_id{"held-second"};
   const auto first_hold = f.hold();
   const auto second_hold = f.hold();
   {
      const auto lock = std::scoped_lock{f.source->mutex};
      f.source->sending.emplace(first.value, first_hold);
      f.source->sending.emplace(second.value, second_hold);
   }
   auto first_send = f.start(f.api->send_partial(token, first, {.group_id = std::vector<std::uint8_t>{1}}));
   auto second_send = f.start(f.api->send_partial(token, second, {.group_id = std::vector<std::uint8_t>{1}}));
   auto cleanup = boost::scope::scope_exit{[&] {
      first_hold->release(); second_hold->release();
      if (first_send.valid()) { try { joined(first_send); } catch (...) {} }
      if (second_send.valid()) { try { joined(second_send); } catch (...) {} }
   }};
   first_hold->await_entered(); second_hold->await_entered();
   BOOST_CHECK_EQUAL(f.source->send_calls.load(), 2U);
   f.run(f.api->disable_partial(token));
   BOOST_CHECK_EQUAL(f.source->send_stops.load(), 2U);
   BOOST_CHECK(first_send.wait_for(0ms) != std::future_status::ready);
   BOOST_CHECK(second_send.wait_for(0ms) != std::future_status::ready);
   f.owner->request_stop();
   auto stopping = f.start(f.owner->shutdown());
   auto finish_stop = boost::scope::scope_exit{[&] {
      first_hold->release(); second_hold->release();
      if (stopping.valid()) { try { joined(stopping); } catch (...) {} }
   }};
   BOOST_CHECK(stopping.wait_for(0ms) != std::future_status::ready);
   first_hold->release(); second_hold->release();
   BOOST_CHECK_THROW(joined(first_send), p2p::exceptions::closed);
   BOOST_CHECK_THROW(joined(second_send), p2p::exceptions::closed);
   joined(stopping);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_partial_send_caller_stop_is_bound_before_native_admission) {
   auto f = fixture{};
   const auto token = f.enable();
   auto caller = std::stop_source{};
   caller.request_stop();
   BOOST_CHECK_THROW(f.run(f.api->send_partial(token, {"held-peer"},
       {.group_id = std::vector<std::uint8_t>{1}}, caller.get_token())), p2p::exceptions::canceled);
   BOOST_CHECK_EQUAL(f.source->send_calls.load(), 0U);
   caller = std::stop_source{};
   const auto hold = f.hold();
   {
      const auto lock = std::scoped_lock{f.source->mutex};
      f.source->sending.emplace("held-peer", hold);
   }
   auto sending = f.start(f.api->send_partial(token, {"held-peer"},
       {.group_id = std::vector<std::uint8_t>{1}}, caller.get_token()));
   auto cleanup = boost::scope::scope_exit{[&] {
      hold->release();
      if (sending.valid()) { try { joined(sending); } catch (...) {} }
   }};
   hold->await_entered();
   caller.request_stop();
   BOOST_CHECK_EQUAL(f.source->send_stops.load(), 1U);
   BOOST_CHECK(sending.wait_for(0ms) != std::future_status::ready);
   hold->release();
   BOOST_CHECK_THROW(joined(sending), p2p::exceptions::canceled);
   f.run(f.api->advertise_partial(token, {1})); // Cancellation did not retire the registration.
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_partial_retired_capture_destruction_precedes_shutdown_completion) {
   auto f = fixture{};
   const auto destruction = std::make_shared<destruction_barrier>();
   auto capture = std::make_shared<retiring_capture>(destruction);
   auto release_capture = boost::scope::scope_exit{[destruction] { destruction->release(); }};
   const auto hold = f.hold();
   auto options = callbacks();
   options.receive = [capture, hold](core::partial_event, std::stop_token) -> boost::asio::awaitable<void> {
      co_await hold->wait();
   };
   const auto token = f.enable({"partial"}, std::move(options));
   capture.reset();
   auto event = f.start(f.source->observed({"partial"}).receive(f.part(token), {}));
   auto stopping_context = boost::asio::io_context{};
   auto stopping = std::future<void>{};
   auto cleanup = boost::scope::scope_exit{[&] {
      hold->release(); destruction->release();
      if (event.valid()) { try { joined(event); } catch (...) {} }
      stopping_context.restart(); stopping_context.run_for(5s);
      if (stopping.valid()) { try { joined(stopping); } catch (...) {} }
   }};
   hold->await_entered();
   f.run(f.api->disable_partial(token));
   hold->release();
   destruction->await_entry();
   BOOST_CHECK_EQUAL(f.api->snapshot().active_handlers, 1U);
   f.owner->request_stop();
   stopping = boost::asio::co_spawn(stopping_context, f.owner->shutdown(), boost::asio::use_future);
   stopping_context.poll();
   BOOST_CHECK(stopping.wait_for(0ms) != std::future_status::ready);
   BOOST_CHECK(event.wait_for(0ms) != std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(f.api->snapshot()), p2p::exceptions::canceled);
   destruction->release();
   joined(event);
   stopping_context.restart(); stopping_context.run_for(5s);
   joined(stopping);
   BOOST_CHECK_THROW(static_cast<void>(f.api->snapshot()), p2p::exceptions::canceled);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_partial_saved_api_awaitables_outlive_handles) {
   auto f = fixture{};
   const auto token = f.enable();
   auto operation = f.api->partial_peers(token);
   auto group_operation = f.api->advertise_partial(token, {1});
   f.owner->request_stop();
   f.run(f.owner->shutdown());
   f.api.reset(); f.owner.reset();
   BOOST_CHECK_THROW(f.run(std::move(operation)), p2p::exceptions::canceled);
   f.native->request_stop();
   BOOST_CHECK_THROW(f.run(std::move(group_operation)), p2p::exceptions::canceled);
}
BOOST_AUTO_TEST_CASE(p2p_pubsub_partial_native_tcp_full_delivery_survives_downgrade) {
   auto received = std::promise<core::partial_event>{};
   auto part = received.get_future();
   auto full_received = std::atomic_uint{};
   auto receiver = network_fixture{"plugin-partial-receiver"};
   const auto ordinary = receiver.run(receiver.api->subscribe({"partial"},
       [&](facade::message value) -> boost::asio::awaitable<core::validation_result> {
          if (value.data == std::vector<std::uint8_t>{7}) { ++full_received; }
          co_return core::validation_result::accept;
       }));
   auto options = callbacks();
   options.receive = [&](core::partial_event value, std::stop_token) -> boost::asio::awaitable<void> {
      received.set_value(std::move(value)); co_return;
   };
   const auto remote_token = receiver.enable({"partial"}, std::move(options));
   const auto remote_node = receiver.apis.get<np::api>(np::api::ref());
   const auto address = remote_node->local_endpoint();
   BOOST_REQUIRE(address);
   auto sender = network_fixture{"plugin-partial-sender", address};
   const auto token = sender.enable();
   const auto diagnostics = sender.apis.get<np::diagnostics_api>(np::diagnostics_api::ref());
   BOOST_REQUIRE(sender.run(sender.await_state([&] {
      const auto value = diagnostics->snapshot();
      return std::ranges::any_of(value.sessions, [&](const auto& session) {
         return session.remote_peer == remote_node->local_peer() && !session.closed &&
             session.authentication == p2p::peer_authentication::libp2p_tls &&
             session.muxer.value == "/yamux/1.0.0" &&
             session.identify_state == p2p::identify::state::identified;
      }) && sender.api->snapshot().core.mesh_edges != 0 && receiver.api->snapshot().core.mesh_edges != 0;
   })));
   // Native discovery itself checks the actual inbound v1.3 first-RPC and current per-topic capability.
   const auto peers = sender.run(sender.api->partial_peers(token));
   BOOST_REQUIRE(std::ranges::find(peers, remote_node->local_peer()) != peers.end());
   sender.run(sender.api->send_partial(token, remote_node->local_peer(),
       {.group_id = std::vector<std::uint8_t>{1}, .data = std::vector<std::uint8_t>{4, 5}}));
   const auto observed = joined(part);
   BOOST_CHECK(observed.registration == remote_token);
   BOOST_CHECK(observed.source == sender.apis.get<np::api>(np::api::ref())->local_peer());
   BOOST_REQUIRE(observed.value.data);
   BOOST_CHECK(*observed.value.data == (std::vector<std::uint8_t>{4, 5}));
   const auto mesh = receiver.api->snapshot().core.mesh_edges;
   receiver.run(receiver.api->disable_partial(remote_token));
   BOOST_CHECK_EQUAL(receiver.api->snapshot().core.mesh_edges, mesh);
   BOOST_CHECK_EQUAL(receiver.api->subscriptions().size(), 1U);
   // Only an observed updated peer capability authorizes the subsequent full path.
   BOOST_REQUIRE(sender.run([&]() -> boost::asio::awaitable<bool> {
      const auto deadline = std::chrono::steady_clock::now() + 4s;
      auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
      for (;;) {
         const auto current = co_await sender.api->partial_peers(token);
         if (std::ranges::find(current, remote_node->local_peer()) == current.end()) { co_return true; }
         if (std::chrono::steady_clock::now() >= deadline) { co_return false; }
         timer.expires_after(5ms); co_await timer.async_wait(boost::asio::use_awaitable);
      }
   }()));
   static_cast<void>(sender.run(sender.api->publish({"partial"}, std::vector<std::uint8_t>{7})));
   BOOST_REQUIRE(receiver.run(receiver.await_state([&] { return full_received.load() == 1; })));
   receiver.run(receiver.api->unsubscribe(ordinary));
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_partial_official_api_cold_awaitable_owns_state_after_plugin_destruction) {
   auto runtime = forge::asio::runtime{};
   auto pending = std::optional<boost::asio::awaitable<core::partial_topic>>{};
   {
      auto node = np::plugin{};
      auto apis = forge::api::core::registry{};
      auto provider = forge::api::core::installer{apis};
      auto provided = boost::asio::co_spawn(runtime.context(), node.provide(provider), boost::asio::use_future);
      joined(provided);
      auto api = apis.get<np::pubsub_api>(np::pubsub_api::ref()).shared();
      pending.emplace(api->enable_partial({"cold"}, accepting(), callbacks()));
      api.reset(); apis.clear();
   }
   auto result = boost::asio::co_spawn(runtime.context(), std::move(*pending), boost::asio::use_future);
   // This public API awaitable retains its service state; plugin destruction closes admission.
   BOOST_CHECK_THROW(joined(result), forge::net::p2p::exceptions::canceled);
}
