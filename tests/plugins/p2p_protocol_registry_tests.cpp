module;

#include <boost/asio/awaitable.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.api.core.registry;
import forge.api.p2p.binding;
import forge.api.p2p.publication;
import forge.app.plugin_lifecycle;
import forge.asio.notification;
import forge.asio.runtime;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.net.p2p.stream;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.protocol_registration;

#include "../../../plugins/net/p2p/node/details/protocol_registry.hxx"
#include "../../../plugins/net/p2p/node/details/protocol_operations.hxx"
#include "details/protocol_registry_fixture.hxx"

namespace {
namespace np = forge::plugins::net::p2p::node;
using fixture = np::test::protocol_registry_fixture;
using point = fixture::point;
using forge::net::p2p::protocol_id;
using namespace std::chrono_literals;
}

// These are the production registry and slot, with only their fallible native
// install/remove boundary replaced. Real TCP admission is tested separately.
BOOST_AUTO_TEST_CASE(p2p_protocol_slot_retirement_before_attach_cannot_leave_an_untracked_native_route) {
   fixture f;
   const auto name = protocol_id{"/registry/retired-before-attach/1"};
   auto slot = std::make_shared<np::detail::protocol_slot>(name, f.lifecycle);
   slot->detach();
   slot->attach(f.operations);
   BOOST_TEST(f.installs() == 0U);
   BOOST_TEST(f.removals() == 0U);
   BOOST_TEST(!f.contains(name));
   f.run(f.owner->async_close());
   BOOST_TEST(!f.contains(name));
}

BOOST_AUTO_TEST_CASE(p2p_protocol_slot_retirement_keeps_remove_retry_without_reopening_attach) {
   for (const auto failure : {point::before, point::after}) {
      fixture f;
      const auto name = protocol_id{"/registry/retired-remove-retry/1"};
      auto slot = std::make_shared<np::detail::protocol_slot>(name, f.lifecycle);
      slot->attach(f.operations);
      BOOST_TEST(f.contains(name));
      f.fail_remove(failure);
      BOOST_CHECK_THROW(slot->detach(), fixture::remove_failure);
      slot->attach(f.operations);
      BOOST_TEST(f.installs() == 1U);
      BOOST_TEST(f.contains(name) == (failure == point::before));
      f.fail_remove(point::none);
      slot->detach();
      slot->attach(f.operations);
      BOOST_TEST(f.installs() == 1U);
      BOOST_TEST(f.removals() == 2U);
      BOOST_TEST(!f.contains(name));
   }
}

BOOST_AUTO_TEST_CASE(p2p_protocol_registry_prepared_routes_cannot_invoke_before_commit_or_after_common_stop) {
   for (const auto api : {false, true}) {
      fixture f;
      const auto name = protocol_id{"/registry/prepared-admission/1"};
      auto calls = std::atomic_uint{0};
      f.owner->attach(f.operations);
      f.hold_install(point::after);
      fixture::job preparation{f, [&] {
         if (api) { auto token = f.publish(name); }
         else {
            auto token = f.owner->register_raw(name, [&](auto) -> boost::asio::awaitable<void> {
               ++calls;
               co_return;
            });
         }
      }};
      f.wait_install();
      BOOST_REQUIRE(f.contains(name));
      BOOST_CHECK_NO_THROW(f.run(f.incoming(name)));
      BOOST_TEST(calls.load() == 0U);
      BOOST_REQUIRE(f.lifecycle.request_stop());
      auto complete_stop = boost::scope::scope_exit{[&] { f.lifecycle.complete_stop(); }};
      // Deliberately keep native registry cleanup unstarted while common admission is closed.
      BOOST_CHECK_NO_THROW(f.run(f.incoming(name)));
      BOOST_TEST(calls.load() == 0U);
      f.release_install();
      BOOST_CHECK_THROW(preparation.finish(), np::exceptions::route_conflict);
      BOOST_TEST(!f.contains(name));
      f.run(f.owner->async_close());
   }
}

BOOST_AUTO_TEST_CASE(p2p_protocol_registry_common_stop_gates_replacement_while_old_capture_retires_unlocked) {
   fixture f;
   const auto name = protocol_id{"/registry/replacement-admission/1"};
   f.owner->attach(f.operations);
   auto entered = std::make_shared<std::promise<void>>();
   auto retired = entered->get_future();
   auto release = std::make_shared<std::promise<void>>();
   auto released = release->get_future().share();
   auto armed = std::make_shared<std::atomic_bool>(false);
   auto capture = std::shared_ptr<int>{new int{0}, [entered, released, armed](int* value) {
      delete value;
      entered->set_value();
      if (armed->load() && released.wait_for(10s) != std::future_status::ready) { std::terminate(); }
   }};
   auto old = f.owner->register_raw(name, [capture](auto) -> boost::asio::awaitable<void> { co_return; });
   capture.reset();
   auto release_sent = false;
   const auto release_now = [&] {
      if (!std::exchange(release_sent, true)) { release->set_value(); }
   };
   auto release_capture = boost::scope::scope_exit{release_now};
   armed->store(true);
   auto calls = std::atomic_uint{0};
   auto replacement = np::protocol_registration{};
   fixture::job replace{f, [&] {
      replacement = f.owner->register_raw(name, [&](auto) -> boost::asio::awaitable<void> {
         ++calls;
         co_return;
      }, &old);
   }};
   auto release_before_join = boost::scope::scope_exit{release_now};
   BOOST_REQUIRE(retired.wait_for(5s) == std::future_status::ready);
   retired.get();
   BOOST_TEST(!old.active());
   BOOST_REQUIRE(f.lifecycle.request_stop());
   auto complete_stop = boost::scope::scope_exit{[&] { f.lifecycle.complete_stop(); }};
   BOOST_CHECK_NO_THROW(f.run(f.incoming(name)));
   BOOST_TEST(calls.load() == 0U);
   release_now();
   replace.finish();
   old.close();
   BOOST_TEST(replacement.active());
   replacement.close();
   BOOST_TEST(!f.contains(name));
}

BOOST_AUTO_TEST_CASE(p2p_protocol_registry_admitted_raw_stream_keeps_its_replacement_generation) {
   fixture f;
   const auto name = protocol_id{"/registry/admitted-generation/1"};
   f.owner->attach(f.operations);
   auto old_calls = std::atomic_uint{0};
   auto next_calls = std::atomic_uint{0};
   auto old = f.owner->register_raw(name, [&](auto) -> boost::asio::awaitable<void> {
      ++old_calls;
      co_return;
   });
   auto admitted = f.incoming(name);
   auto next = f.owner->register_raw(name, [&](auto) -> boost::asio::awaitable<void> {
      ++next_calls;
      co_return;
   }, &old);
   old.close();
   f.run(std::move(admitted));
   f.run(f.incoming(name));
   BOOST_TEST(old_calls.load() == 1U);
   BOOST_TEST(next_calls.load() == 1U);
   auto before_stop = f.incoming(name);
   BOOST_REQUIRE(f.lifecycle.request_stop());
   f.lifecycle.complete_stop();
   f.run(std::move(before_stop));
   f.run(f.incoming(name));
   BOOST_TEST(next_calls.load() == 2U);
   next.close();
}

BOOST_AUTO_TEST_CASE(p2p_protocol_registry_install_failure_keeps_original_error_and_uncertain_claim) {
   for (auto install : {point::before, point::after}) {
      for (auto remove : {point::before, point::after}) {
         for (auto api : {false, true}) {
            fixture f;
            const auto name = protocol_id{"/registry/install/1"};
            f.owner->attach(f.operations);
            f.fail_install(install);
            f.fail_remove(remove);
            if (api) { BOOST_CHECK_THROW((void)f.publish(name), fixture::install_failure); }
            else { BOOST_CHECK_THROW((void)f.owner->register_raw(name, fixture::handler()), fixture::install_failure); }
            BOOST_TEST(f.installs() == 1U);
            BOOST_TEST(f.removals() >= 1U);
            BOOST_TEST(f.contains(name) == (install == point::after && remove == point::before));
            BOOST_CHECK_THROW((void)f.owner->register_raw(name, fixture::handler()), fixture::remove_failure);
            BOOST_CHECK_THROW((void)f.publish(name), fixture::remove_failure);
            BOOST_CHECK_THROW((void)f.owner->register_raw({"/registry/other/1"}, fixture::handler()), np::exceptions::route_conflict);
            BOOST_TEST(f.installs() == 1U);
            f.fail_install(point::none);
            f.fail_remove(point::none);
            auto next = f.owner->register_raw(name, fixture::handler());
            BOOST_TEST(next.active());
            BOOST_TEST(f.contains(name));
            BOOST_REQUIRE(f.last_remove_result().has_value());
            BOOST_TEST(*f.last_remove_result() == (install == point::after && remove == point::before));
            next.close();
            BOOST_TEST(!f.contains(name));
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_protocol_registry_remove_failure_requires_successful_retry) {
   for (auto failure : {point::before, point::after}) {
      fixture f;
      const auto name = protocol_id{"/registry/remove/1"};
      f.owner->attach(f.operations);
      auto old = f.owner->register_raw(name, fixture::handler());
      f.fail_remove(failure);
      old.close();
      BOOST_TEST(!old.active());
      BOOST_TEST(f.contains(name) == (failure == point::before));
      BOOST_CHECK_THROW((void)f.publish(name), fixture::remove_failure);
      BOOST_CHECK_THROW((void)f.owner->register_raw(name, fixture::handler()), fixture::remove_failure);
      BOOST_CHECK_THROW((void)f.publish({"/registry/other/1"}), np::exceptions::route_conflict);
      f.fail_remove(point::none);
      old.close();
      BOOST_TEST(!f.contains(name));
      BOOST_REQUIRE(f.last_remove_result().has_value());
      BOOST_TEST(*f.last_remove_result() == (failure == point::before));
      auto next = f.publish(name);
      const auto removals = f.removals();
      old.close();
      BOOST_TEST(f.removals() == removals);
      BOOST_TEST(next.active());
      BOOST_TEST(f.contains(name));
      f.run(next.async_close());
      BOOST_TEST(!f.contains(name));
   }
}

BOOST_AUTO_TEST_CASE(p2p_protocol_registry_api_detach_failure_remains_retryable_after_generation_retirement) {
   fixture f;
   const auto name = protocol_id{"/registry/api-remove/1"};
   f.owner->attach(f.operations);
   auto old = f.publish(name);
   f.fail_remove(point::after);
   old.close();
   BOOST_TEST(!old.active());
   BOOST_TEST(!f.contains(name));
   BOOST_CHECK_THROW(f.run(old.async_close()), fixture::remove_failure);
   BOOST_CHECK_THROW((void)f.owner->register_raw(name, fixture::handler()), fixture::remove_failure);
   f.fail_remove(point::none);
   // A failed API drain remains sticky. Retrying the common name claim can
   // confirm absence, but must not rewrite the old publication's result.
   auto next = f.owner->register_raw(name, fixture::handler());
   BOOST_REQUIRE(f.last_remove_result().has_value());
   BOOST_TEST(!*f.last_remove_result());
   BOOST_CHECK_THROW(f.run(old.async_close()), fixture::remove_failure);
   old.close();
   BOOST_TEST(next.active());
   BOOST_TEST(f.contains(name));
}

BOOST_AUTO_TEST_CASE(p2p_protocol_registry_close_during_attach_before_and_after_mutation) {
   for (auto held : {point::before, point::after}) {
      fixture f;
      const auto name = protocol_id{"/registry/close-attach/1"};
      auto old = f.owner->register_raw(name, fixture::handler());
      f.hold_install(held);
      fixture::job attach{f, [&] { f.owner->attach(f.operations); }};
      f.wait_install();
      BOOST_TEST(f.contains(name) == (held == point::after));
      old.close();
      BOOST_TEST(!old.active());
      BOOST_CHECK_THROW((void)f.owner->register_raw(name, fixture::handler()), np::exceptions::route_conflict);
      BOOST_CHECK_THROW((void)f.publish(name), np::exceptions::route_conflict);
      BOOST_CHECK_THROW((void)f.owner->register_raw({"/registry/other/1"}, fixture::handler()), np::exceptions::route_conflict);
      f.release_install();
      attach.finish();
      BOOST_TEST(f.installs() == 1U);
      BOOST_TEST(f.removals() == 1U);
      BOOST_TEST(!f.contains(name));
      auto next = f.owner->register_raw(name, fixture::handler());
      old.close();
      BOOST_TEST(next.active());
      BOOST_TEST(f.contains(name));
   }
}

BOOST_AUTO_TEST_CASE(p2p_protocol_registry_stop_during_attach_before_and_after_mutation) {
   for (auto held : {point::before, point::after}) {
      fixture f;
      const auto name = protocol_id{"/registry/stop-attach/1"};
      auto old = f.owner->register_raw(name, fixture::handler());
      f.hold_install(held);
      fixture::job attach{f, [&] { f.owner->attach(f.operations); }};
      f.wait_install();
      BOOST_TEST(f.contains(name) == (held == point::after));
      auto admission_closed = std::atomic_bool{false};
      fixture::job release{f, [&] {
         const auto deadline = std::chrono::steady_clock::now() + 5s;
         while (old.active() && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
         admission_closed.store(!old.active());
         f.release_install();
      }};
      fixture::job stop{f, [&] { f.owner->request_close(); }};
      stop.finish();
      release.finish();
      attach.finish();
      f.run(f.owner->async_close());
      BOOST_TEST(admission_closed.load());
      BOOST_TEST(!old.active());
      BOOST_TEST(f.installs() == 1U);
      BOOST_TEST(f.removals() == 1U);
      BOOST_TEST(!f.contains(name));
      BOOST_CHECK_THROW((void)f.owner->register_raw(name, fixture::handler()), np::exceptions::route_conflict);
      BOOST_CHECK_THROW((void)f.publish(name), np::exceptions::route_conflict);
      old.close();
      BOOST_TEST(f.removals() == 1U);
   }
}

BOOST_AUTO_TEST_CASE(p2p_protocol_registry_stop_failure_is_reported_and_retried_not_silently_released) {
   for (auto failure : {point::before, point::after}) {
      fixture f;
      const auto name = protocol_id{"/registry/stop-failure/1"};
      f.owner->attach(f.operations);
      auto token = f.owner->register_raw(name, fixture::handler());
      f.fail_remove(failure);
      f.owner->request_close();
      BOOST_TEST(!token.active());
      BOOST_CHECK_THROW(f.run(f.owner->async_close()), fixture::remove_failure);
      BOOST_TEST(f.contains(name) == (failure == point::before));
      f.fail_remove(point::none);
      f.run(f.owner->async_close());
      BOOST_TEST(!f.contains(name));
      BOOST_REQUIRE(f.last_remove_result().has_value());
      BOOST_TEST(*f.last_remove_result() == (failure == point::before));
      BOOST_CHECK_THROW((void)f.owner->register_raw(name, fixture::handler()), np::exceptions::route_conflict);
   }
}
