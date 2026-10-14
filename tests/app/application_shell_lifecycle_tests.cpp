#include <boost/test/unit_test.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

import forge.app.application_builder;
import forge.app.application_shell;
import forge.app.exceptions;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.app.plugin_lifecycle;
import forge.asio.notification;
import forge.asio.runtime;

namespace {

struct paused_plugin_state {
   forge::asio::notification entered;
   forge::asio::notification release;
   std::atomic_bool published = false;
   std::atomic_uint shutdown_calls = 0;
   std::atomic_uint stop_calls = 0;
};

class paused_plugin final : public forge::app::plugin {
 public:
   explicit paused_plugin(std::shared_ptr<paused_plugin_state> state) : state_{std::move(state)} {}
   forge::app::plugin_id id() const override { return {"paused"}; }
   std::string version() const override { return "1"; }

 protected:
   boost::asio::awaitable<void> on_initialize(forge::app::plugin_context&) override { co_return; }

   boost::asio::awaitable<void> on_startup() override {
      const auto lifecycle = lifecycle_handle();
      const auto generation = lifecycle.snapshot().generation;
      const auto epoch = state_->release.epoch();
      state_->entered.notify();
      co_await state_->release.async_wait_until(epoch, std::chrono::steady_clock::now() + std::chrono::seconds{5});
      static_cast<void>(lifecycle.try_admit(generation, [&] { state_->published.store(true); }));
   }

   void on_request_stop() noexcept override { ++state_->stop_calls; }
   boost::asio::awaitable<void> on_shutdown() override {
      ++state_->shutdown_calls;
      state_->published.store(false);
      co_return;
   }

 private:
   std::shared_ptr<paused_plugin_state> state_;
};

} // namespace

BOOST_AUTO_TEST_CASE(application_shell_stop_joins_setup_and_rejects_stale_publication) {
   auto state = std::make_shared<paused_plugin_state>();
   auto builder = forge::app::application_builder{};
   builder.runtime({.worker_threads = 4});
   builder.plugin({.id = {"paused"}, .factory = [state] { return std::make_unique<paused_plugin>(state); }});
   auto app = std::move(builder).build();
   auto& io = app->runtime().context();
   auto release = boost::scope::scope_exit{[&] { state->release.notify(); }};

   const auto before = state->entered.epoch();
   auto entered = boost::asio::co_spawn(io, state->entered.async_wait_until(
      before, std::chrono::steady_clock::now() + std::chrono::seconds{5}), boost::asio::use_future);
   auto starting = boost::asio::co_spawn(io, app->startup(), boost::asio::use_future);
   BOOST_REQUIRE(entered.wait_for(std::chrono::seconds{6}) == std::future_status::ready);
   BOOST_REQUIRE(entered.get() != before);

   app->request_stop();
   app->request_stop();
   auto stopping = boost::asio::co_spawn(io, app->shutdown(), boost::asio::use_future);
   BOOST_CHECK(stopping.wait_for(std::chrono::milliseconds{25}) == std::future_status::timeout);
   BOOST_CHECK_EQUAL(state->stop_calls.load(), 1U);
   BOOST_CHECK_EQUAL(state->shutdown_calls.load(), 0U);
   BOOST_CHECK(!state->published.load());
   state->release.notify();

   BOOST_REQUIRE(starting.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   BOOST_CHECK_THROW(starting.get(), forge::app::exceptions::invalid_state);
   BOOST_REQUIRE(stopping.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(stopping.get());
   BOOST_CHECK_EQUAL(state->shutdown_calls.load(), 1U);
   BOOST_CHECK(!state->published.load());
   BOOST_CHECK(app->state() == forge::app::lifecycle_phase::stopped);
}
