module;

#include <boost/asio/awaitable.hpp>
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
