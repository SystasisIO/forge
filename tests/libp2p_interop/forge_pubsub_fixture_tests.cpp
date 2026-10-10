#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

import forge.asio.runtime;
import forge.asio.notification;
import forge.codec.json;
import forge.codec.hex;
import forge.crypto.digest.sha256;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.pubsub;
import forge.variant.value;
import forge.variant.containers;

#include "forge_pubsub_fixture.hxx"
#include "forge_pubsub_partial.hxx"

namespace forge::test::libp2p_interop {

// Synthetic fixture units: no host, transport, router decision or interop evidence is created.
void forge_pubsub_fixture::self_test() {
   forge_pubsub_partial::self_test();
   auto check = [](bool okay, std::string_view reason) {
      if (!okay) { throw std::runtime_error{"PubSub fixture unit: " + std::string{reason}}; }
   };
   auto rejects = [&](auto work, std::string_view reason) {
      try { work(); }
      catch (const std::exception& error) {
         check(std::string_view{error.what()}.find(reason) != std::string_view::npos, "wrong rejection reason");
         return;
      }
      check(false, "missing rejection: " + std::string{reason});
   };
   namespace pubsub = forge::net::p2p::pubsub;
   check(protocol_version("1.0") == pubsub::version::v1_0 &&
         protocol_version("1.1") == pubsub::version::v1_1 &&
         protocol_version("1.2") == pubsub::version::v1_2 &&
         protocol_version("1.3") == pubsub::version::v1_3, "fixture mislabeled a native protocol version");
   for (const auto version : {"", "1.4", "1.30", "v1.3"}) {
      rejects([&] { static_cast<void>(protocol_version(version)); }, "protocol version");
   }
   for (const auto version : {pubsub::version::v1_0, pubsub::version::v1_1, pubsub::version::v1_2, pubsub::version::v1_3}) {
      validate_extension({}, version);
      for (const auto mode : {"idontwant", "partial", "advertisement", "unknown"}) {
         const auto supported = (std::string_view{mode} == "idontwant" && version == pubsub::version::v1_2) ||
             ((std::string_view{mode} == "partial" || std::string_view{mode} == "advertisement") && version == pubsub::version::v1_3);
         if (supported) { validate_extension(mode, version); }
         else { rejects([&] { validate_extension(mode, version); }, "extension mode/version"); }
      }
   }
   for (const auto transport : {"tcp", "tcp-pnet-noise", "quic"}) {
      auto options = forge::net::p2p::node::options{};
      options.stream_security = forge::net::p2p::node::stream_security::tls;
      configure_stream_security(options, transport);
      const auto expected = std::string_view{transport} == "quic" ? forge::net::p2p::node::stream_security::tls
                                                               : forge::net::p2p::node::stream_security::noise;
      check(options.stream_security == expected, "fixture stream security differs from its forced profile");
   }
   const auto local_peer = forge::net::p2p::make_peer_id({
       .type = forge::net::p2p::public_key::type::ed25519, .data = std::vector<std::uint8_t>(32, 1)});
   const auto other_peer = forge::net::p2p::make_peer_id({
       .type = forge::net::p2p::public_key::type::ed25519, .data = std::vector<std::uint8_t>(32, 2)});
   const auto prepare = forge::variant{forge::mutable_variant_object{}("sequence", 1u)("kind", "prepare_shutdown")
       ("actor", "victim")("case_token", std::string(32, 'a'))("local_peer_id", local_peer.to_string())};
#if defined(__unix__) || defined(__APPLE__)
   // Fork before creating any runtime workers in this single-threaded self-test.
   // These are process-boundary models, not native network/stop acceptance.
   struct child_owner {
      pid_t pid = -1;
      int status = 0;

      bool reap() {
         const auto result = ::waitpid(pid, &status, WNOHANG);
         if (result == pid) { pid = -1; return true; }
         if (result < 0 && errno != EINTR) {
            const auto error = errno;
            if (error == ECHILD) { pid = -1; }
            throw std::system_error{error, std::generic_category(), "reap PubSub Prepare child"};
         }
         return false;
      }
      ~child_owner() {
         if (pid <= 0) { return; }
         static_cast<void>(::kill(pid, SIGKILL));
         const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
         do {
            try { if (reap() || pid <= 0) { return; } } catch (...) {}
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
         } while (std::chrono::steady_clock::now() < deadline);
         std::fputs("FATAL: PubSub Prepare regression child did not reap\n", stderr);
         std::fflush(stderr);
         std::_Exit(86);
      }
   };
   for (const auto mode : {std::string_view{"fast"}, std::string_view{"stop_throws"},
                           std::string_view{"blocked_stop_join"}, std::string_view{"blocked_unsubscribe_join"}}) {
      auto log = std::unique_ptr<std::FILE, decltype(&std::fclose)>{std::tmpfile(), &std::fclose};
      check(static_cast<bool>(log), "cannot open subprocess diagnostic file");
      auto child = child_owner{};
      child.pid = ::fork();
      check(child.pid >= 0, "cannot fork Prepare process-boundary regression");
      if (child.pid == 0) {
         if (::dup2(::fileno(log.get()), STDERR_FILENO) < 0) { std::_Exit(90); }
         try {
            {
               auto fixture = forge_pubsub_fixture{std::string(32, 'a'), "victim"};
               fixture._extension = "partial";
               fixture._peer = local_peer.to_string();
               auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
               auto release_unsubscribe = forge::asio::notification{};
               auto release_stop = forge::asio::notification{};
               const auto unsubscribe_epoch = release_unsubscribe.epoch();
               const auto stop_epoch = release_stop.epoch();
               const auto unsubscribe = [&]() -> boost::asio::awaitable<void> {
                  std::fputs("synthetic unsubscribe entered\n", stderr);
                  co_await release_unsubscribe.async_wait(unsubscribe_epoch);
                  std::fputs("synthetic unsubscribe returned\n", stderr);
               };
               const auto stop = [&]() -> boost::asio::awaitable<void> {
                  std::fputs("synthetic stop entered\n", stderr);
                  if (mode == "stop_throws") {
                     throw std::runtime_error{"synthetic stop failed before releasing unsubscribe"};
                  }
                  if (mode == "blocked_stop_join") {
                     release_unsubscribe.notify();
                     co_await release_stop.async_wait(stop_epoch);
                  }
                  std::fputs("synthetic stop returned\n", stderr);
               };
               const auto draining = [&]() -> boost::asio::awaitable<void> {
                  const auto result = co_await fixture.drain_unsubscribe(unsubscribe(), stop(),
                      std::chrono::steady_clock::now() - std::chrono::milliseconds{1});
                  // The faulty-stop models must remain owned until the process
                  // boundary exits, never reach a normal returned future.
                  static_cast<void>(result);
                  throw std::runtime_error{"synthetic blocked drain unexpectedly returned"};
               };
               const auto started = std::chrono::steady_clock::now();
               if (mode == "fast") {
                  run_extension_prepare(runtime, fixture.prepare_extension(prepare, started + std::chrono::seconds{1}),
                                        started + std::chrono::seconds{1});
               } else {
                  run_extension_prepare(runtime, draining(), started + std::chrono::seconds{1});
               }
               fixture.prepare_shutdown(prepare, 1);
               const auto output = forge::codec::json::write_value(fixture.result(false, false, {}), {.max_bytes = 8191});
               check(output.ok(), "cannot encode synthetic child result");
               std::fprintf(stderr, "%s\n", output.text.c_str());
               runtime.stop();
            }
            std::fputs("synthetic Prepare future and runtime joined\n", stderr);
            std::fflush(stderr);
            std::_Exit(0);
         } catch (const std::exception& error) {
            std::fprintf(stderr, "unexpected Prepare child error: %.512s\n", error.what());
            std::fflush(stderr);
            std::_Exit(91);
         } catch (...) { std::_Exit(92); }
      }
      auto reaped = false;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
      do {
         if (child.reap()) { reaped = true; break; }
         std::this_thread::sleep_for(std::chrono::milliseconds{5});
      } while (std::chrono::steady_clock::now() < deadline);
      check(reaped, "Prepare child exceeded its independent bounded parent wait");
      check(std::fseek(log.get(), 0, SEEK_SET) == 0, "cannot rewind subprocess diagnostics");
      auto bytes = std::array<char, 8192>{};
      const auto count = std::fread(bytes.data(), 1, bytes.size(), log.get());
      check(count < bytes.size() && !std::ferror(log.get()), "subprocess diagnostics invalid or unbounded");
      const auto output = std::string_view{bytes.data(), count};
      check(WIFEXITED(child.status), "Prepare child did not exit normally through process boundary");
      if (mode == "fast") {
         check(WEXITSTATUS(child.status) == 0 && output.find("shutdown_prepared") != std::string_view::npos &&
             output.find("synthetic Prepare future and runtime joined") != std::string_view::npos &&
             output.find("FATAL:") == std::string_view::npos, "fast Prepare failed to join before actual fixture ACK");
      } else {
         check(WEXITSTATUS(child.status) == 86 &&
             output.find("FATAL: PubSub Prepare exceeded actor process deadline") != std::string_view::npos &&
             output.find("shutdown_prepared") == std::string_view::npos &&
             output.find("synthetic Prepare future and runtime joined") == std::string_view::npos &&
             output.find("synthetic unsubscribe entered") != std::string_view::npos &&
             output.find("synthetic stop entered") != std::string_view::npos,
             "blocked Prepare did not terminate nonzero with original diagnostics and no ACK/join claim");
         if (mode == "stop_throws") {
            check(output.find("synthetic stop failed before releasing unsubscribe") != std::string_view::npos &&
                output.find("synthetic unsubscribe returned") == std::string_view::npos,
                "failed-stop subprocess lost its cause or invented an unsubscribe join");
         } else if (mode == "blocked_stop_join") {
            check(output.find("synthetic unsubscribe returned") != std::string_view::npos &&
                output.find("synthetic stop returned") == std::string_view::npos,
                "blocked-stop subprocess did not preserve its actual unfinished branch");
         } else {
            check(output.find("synthetic stop returned") != std::string_view::npos &&
                output.find("synthetic unsubscribe returned") == std::string_view::npos,
                "blocked-unsubscribe subprocess did not preserve its actual unfinished branch");
         }
      }
   }
#endif
   // Synthetic application ownership only: no native node, stream or callback-join proof.
   struct owned_operation {
      boost::asio::io_context context;
      std::future<void> completion;
      std::function<void()> release;

      explicit owned_operation(std::function<void()> cleanup) : release{std::move(cleanup)} {}
      static void fail_closed() noexcept {
         std::fputs("FATAL: synthetic PubSub extension operation did not join\n", stderr);
         std::_Exit(86);
      }
      void join() {
         if (!completion.valid()) { return; }
         context.run_for(std::chrono::seconds{5});
         if (completion.wait_for(std::chrono::seconds{0}) != std::future_status::ready) { fail_closed(); }
      }
      ~owned_operation() {
         try {
            release();
            join();
            if (completion.valid()) {
               try { completion.get(); } catch (...) {}
            }
         } catch (...) { fail_closed(); }
      }
   };
   for (const auto mode : {"idontwant", "advertisement"}) {
      for (const auto expired : {false, true}) {
         auto fixture = forge_pubsub_fixture{std::string(32, 'a'), "victim"};
         fixture._peer = local_peer.to_string();
         fixture._extension = mode;
         check(fixture._extension_work == 0 && fixture._extension_validators == 0 &&
             !fixture._partial_registration, "empty drain model unexpectedly owns work/registration");
         const auto now = std::chrono::steady_clock::now();
         const auto deadline = expired ? now - std::chrono::milliseconds{1} : now + std::chrono::minutes{1};
         auto drain = owned_operation{[&] { fixture._extension_stop.request_stop(); }};
         drain.completion = boost::asio::co_spawn(drain.context, fixture.prepare_extension(prepare, deadline),
                                                 boost::asio::use_future);
         drain.join();
         if (expired) {
            rejects([&] { drain.completion.get(); }, "extension admitted work drain deadline exceeded");
            rejects([&] { fixture.prepare_shutdown(prepare, 1); }, "prepare_shutdown");
            check(fixture._extension_admission_closed && !fixture._extension_drained && !fixture._prepared &&
                fixture._events.empty() &&
                fixture._extension_drain_error == "extension admitted work drain deadline exceeded",
                "expired zero-work/no-registration drain emitted completion or Prepare ACK");
         } else {
            drain.completion.get();
            check(fixture._extension_drained && fixture._extension_drain_error.empty(),
                "normal empty drain spuriously exceeded its deadline");
            fixture.prepare_shutdown(prepare, 1);
            check(fixture._prepared, "normal empty drain did not permit Prepare ACK");
         }
      }
   }
   // Exercise the same owned drain composition with bounded coroutine models.
   // These do not claim a native blocked socket/write or router shutdown proof.
   for (const auto operation_fails : {false, true}) {
      auto fixture = forge_pubsub_fixture{std::string(32, 'a'), "victim"};
      auto unsubscribe_returned = false;
      auto stop_entered = false;
      auto result = std::optional<unsubscribe_result>{};
      const auto unsubscribe = [&]() -> boost::asio::awaitable<void> {
         unsubscribe_returned = true;
         if (operation_fails) { throw std::runtime_error{"synthetic unsubscribe failure"}; }
         co_return;
      };
      const auto stop = [&]() -> boost::asio::awaitable<void> {
         stop_entered = true;
         co_return;
      };
      const auto run = [&]() -> boost::asio::awaitable<void> {
         result = co_await fixture.drain_unsubscribe(unsubscribe(), stop(),
             std::chrono::steady_clock::now() + std::chrono::minutes{1});
      };
      auto drain = owned_operation{[&] { fixture._extension_stop.request_stop(); }};
      drain.completion = boost::asio::co_spawn(drain.context, run(), boost::asio::use_future);
      drain.join();
      drain.completion.get();
      check(result && unsubscribe_returned && !stop_entered && !result->interrupted &&
          !result->watchdog_error && !result->stop_error && fixture._extension_drain_error.empty(),
          "fast unsubscribe lost its notification or spuriously stopped the owner");
      if (operation_fails) {
         check(static_cast<bool>(result->operation_error), "actual unsubscribe error was discarded");
         rejects([&] { std::rethrow_exception(result->operation_error); }, "synthetic unsubscribe failure");
      } else {
         check(!result->operation_error, "successful unsubscribe became a synthetic failure");
      }
   }
   for (const auto unsubscribe_first : {false, true}) {
      auto fixture = forge_pubsub_fixture{std::string(32, 'a'), "victim"};
      fixture._extension = "partial";
      fixture._peer = local_peer.to_string();
      auto cancel_io = forge::asio::notification{};
      auto finish_unsubscribe = forge::asio::notification{};
      auto finish_stop = forge::asio::notification{};
      const auto cancel_epoch = cancel_io.epoch();
      const auto unsubscribe_epoch = finish_unsubscribe.epoch();
      const auto stop_epoch = finish_stop.epoch();
      auto unsubscribe_entered = false;
      auto unsubscribe_unblocked = false;
      auto unsubscribe_returned = false;
      auto stop_entered = false;
      auto stop_returned = false;
      auto result = std::optional<unsubscribe_result>{};
      const auto unsubscribe = [&]() -> boost::asio::awaitable<void> {
         unsubscribe_entered = true;
         co_await cancel_io.async_wait(cancel_epoch);
         unsubscribe_unblocked = true;
         co_await finish_unsubscribe.async_wait(unsubscribe_epoch);
         unsubscribe_returned = true;
      };
      const auto stop = [&]() -> boost::asio::awaitable<void> {
         check(!fixture._extension_drain_error.empty(), "stop preceded sticky timeout publication");
         stop_entered = true;
         cancel_io.notify();
         co_await finish_stop.async_wait(stop_epoch);
         stop_returned = true;
      };
      const auto run = [&]() -> boost::asio::awaitable<void> {
         result = co_await fixture.drain_unsubscribe(unsubscribe(), stop(),
             std::chrono::steady_clock::now() - std::chrono::milliseconds{1});
      };
      auto drain = owned_operation{[&] {
         fixture._extension_stop.request_stop();
         cancel_io.notify();
         finish_unsubscribe.notify();
         finish_stop.notify();
      }};
      drain.completion = boost::asio::co_spawn(drain.context, run(), boost::asio::use_future);
      drain.context.poll();
      check(unsubscribe_entered && unsubscribe_unblocked && stop_entered &&
          !unsubscribe_returned && !stop_returned && !result &&
          drain.completion.wait_for(std::chrono::seconds{0}) == std::future_status::timeout,
          "expired drain returned before its actual unsubscribe/stop coroutine joins");
      if (unsubscribe_first) { finish_unsubscribe.notify(); }
      else { finish_stop.notify(); }
      drain.context.poll();
      check(unsubscribe_returned == unsubscribe_first && stop_returned != unsubscribe_first && !result &&
          drain.completion.wait_for(std::chrono::seconds{0}) == std::future_status::timeout,
          "one completed branch released the other still-owned operation");
      if (unsubscribe_first) { finish_stop.notify(); }
      else { finish_unsubscribe.notify(); }
      drain.join();
      drain.completion.get();
      check(result && result->interrupted && !result->operation_error && !result->watchdog_error &&
          !result->stop_error && unsubscribe_returned && stop_returned &&
          fixture._extension_drain_error == "partial unsubscribe drain deadline exceeded" &&
          !fixture._extension_drained && !fixture._prepared && fixture._events.empty(),
          "stop-unblocked successful unsubscribe was mistaken for successful Prepare");
      rejects([&] { fixture.prepare_shutdown(prepare, 1); }, "prepare_shutdown");
   }
   {
      auto fixture = forge_pubsub_fixture{std::string(32, 'a'), "victim"};
      auto release = forge::asio::notification{};
      const auto epoch = release.epoch();
      auto unsubscribe_returned = false;
      auto stop_returned = false;
      auto result = std::optional<unsubscribe_result>{};
      const auto unsubscribe = [&]() -> boost::asio::awaitable<void> {
         co_await release.async_wait(epoch);
         unsubscribe_returned = true;
         throw std::runtime_error{"synthetic native unsubscribe error after stop"};
      };
      const auto stop = [&]() -> boost::asio::awaitable<void> {
         release.notify();
         stop_returned = true;
         throw std::runtime_error{"synthetic node stop error"};
         co_return;
      };
      const auto run = [&]() -> boost::asio::awaitable<void> {
         result = co_await fixture.drain_unsubscribe(unsubscribe(), stop(), std::chrono::steady_clock::now());
      };
      auto drain = owned_operation{[&] { release.notify(); fixture._extension_stop.request_stop(); }};
      drain.completion = boost::asio::co_spawn(drain.context, run(), boost::asio::use_future);
      drain.join();
      drain.completion.get();
      check(result && result->interrupted && unsubscribe_returned && stop_returned &&
          result->operation_error && result->stop_error && !fixture._extension_drain_error.empty(),
          "drain did not preserve both real operation failures through joined completion");
      rejects([&] { std::rethrow_exception(result->operation_error); }, "unsubscribe error after stop");
      rejects([&] { std::rethrow_exception(result->stop_error); }, "node stop error");
   }
   {
      auto fixture = forge_pubsub_fixture{std::string(32, 'a'), "victim"};
      fixture._peer = local_peer.to_string();
      fixture._extension = "partial";
      auto work = std::optional<partial_work>{};
      auto drain = owned_operation{[&] {
         work.reset();
         fixture._extension_stop.request_stop();
      }};
      {
         const auto lock = std::scoped_lock{fixture._mutex};
         check(fixture.admit_partial_locked(), "synthetic callback admission failed");
         work.emplace(&fixture);
      }
      drain.completion = boost::asio::co_spawn(drain.context,
          fixture.prepare_extension(prepare, std::chrono::steady_clock::now() + std::chrono::seconds{5}),
          boost::asio::use_future);
      drain.context.poll();
      check(fixture._extension_admission_closed && fixture._extension_work == 1 &&
          fixture._extension_inputs == 1 && !fixture._extension_drained && fixture._events.empty(),
          "drain did not wait for admitted application work");
      check(drain.completion.wait_for(std::chrono::seconds{0}) == std::future_status::timeout,
          "drain completed while admitted callback was still owned");
      fixture.partial_failure("synthetic callback failure before guard release");
      drain.context.poll();
      check(fixture._extension_work == 1 && !fixture._extension_drained && !fixture._prepared &&
          fixture._capture_error == "synthetic callback failure before guard release" && fixture._events.empty(),
          "callback failure was not retained before work release");
      check(drain.completion.wait_for(std::chrono::seconds{0}) == std::future_status::timeout,
          "failure publication alone released the callback ownership guard");
      work.reset();
      drain.join();
      rejects([&] { drain.completion.get(); }, "extension drain observed capture failure");
      rejects([&] { fixture.prepare_shutdown(prepare, 1); }, "prepare_shutdown");
      check(fixture._extension_work == 0 && !fixture._extension_drained && !fixture._prepared &&
          fixture._extension_drain_error == "extension drain observed capture failure" &&
          fixture._capture_error == "synthetic callback failure before guard release" && fixture._events.empty(),
          "failed callback produced a clean drain/Prepare acknowledgement");
   }
   {
      auto fixture = forge_pubsub_fixture{std::string(32, 'a'), "victim"};
      fixture._peer = local_peer.to_string();
      fixture._extension = "partial";
      auto callback = owned_operation{[&] { fixture._extension_stop.request_stop(); }};
      callback.completion = boost::asio::co_spawn(callback.context,
          fixture.gossip_partial(pubsub::partial_gossip_event{}, std::stop_token{}), boost::asio::use_future);
      callback.join();
      callback.completion.get();
      check(fixture._extension_inputs == 1 && fixture._extension_work == 0 &&
          fixture._capture_error == "gossip callback outside owned bounded partial group" && fixture._events.empty(),
          "actual gossip callback rejection lost its failure or leaked admitted work");
      rejects([&] { fixture.prepare_shutdown(prepare, 1); }, "prepare_shutdown");
      check(!fixture._prepared && !fixture._extension_drained,
          "failed gossip callback permitted a clean Prepare acknowledgement");
      check(fixture.result(true, false, {})["error"].get_string() ==
          "gossip callback outside owned bounded partial group", "gossip callback failure was not sticky");
   }
   {
      auto fixture = forge_pubsub_fixture{std::string(32, 'a'), "victim"};
      const auto legacy = fixture.pubsub_options();
      check(!legacy.partial_messages && legacy.limits.history_length == 5 &&
          legacy.limits.mesh_n == 2 && legacy.limits.mesh_n_low == 1 && legacy.limits.mesh_n_high == 4,
          "legacy public cache/mesh options changed");
      fixture._extension = "idontwant";
      const auto held = fixture.pubsub_options();
      check(!held.partial_messages && held.limits.history_length == 64 && held.limits.history_gossip == 3 &&
          held.limits.mesh_n == 3 && held.limits.mesh_n_low == 3 && held.limits.mesh_n_high == 4 &&
          held.limits.heartbeat_interval == std::chrono::milliseconds{250}, "IDW cache/mesh public option adapter");
      for (const auto mode : {"partial", "advertisement"}) {
         fixture._extension = mode;
         fixture._version = "1.3";
         const auto enabled = fixture.pubsub_options();
         check(enabled.partial_messages && enabled.limits.history_length == 5 && enabled.limits.mesh_n == 2 &&
             enabled.limits.mesh_n_low == 1 && enabled.limits.max_partial_groups == 1 &&
             enabled.limits.max_partial_group_bytes == 20 && enabled.limits.max_partial_metadata_size == 7 &&
             enabled.limits.max_partial_callbacks == 64 && enabled.limits.partial_group_ttl == 32,
             "bounded partial public options");
         const auto raw = fixture.result(false, false, {});
         check(raw["version"].get_string() == "1.3" && raw["extension"].get_string() == mode &&
             raw["requests_partial"].as_bool() == (std::string_view{mode} == "partial") &&
             !raw["partial_registration_active"].as_bool(), "mode schema claimed a subscription before its native return");
      }
   }
   {
      using format = forge_pubsub_partial;
      constexpr auto token = "00112233445566778899aabbccddeeff";
      auto consumer = forge_pubsub_fixture{token, "victim"};
      consumer._extension = "partial";
      auto message = pubsub::partial_message{.subject = pubsub::topic{"forge-pr11:" + std::string{token}},
          .group_id = format::group(token), .metadata = format::encode(format::metadata{1, 7, 0})};
      check(!consumer._partial.initialized && !consumer._partial.owned, "consumer was proactively seeded");
      check(!consumer.apply_partial_locked(other_peer, message, 1), "metadata manufactured a received part");
      check(consumer._partial.initialized && !consumer._partial.owned && consumer._partial.local_have == 0 &&
          consumer.received_have_locked() == 0 && consumer._partial.local == format::metadata{1, 0, 7},
          "unknown metadata did not create empty application-only state");
      auto request = consumer.plan_partial_locked(other_peer, 1, false);
      check(request.size() == 1 && request.front().peer == other_peer && !request.front().value.data &&
          request.front().value.metadata == format::encode(format::metadata{1, 0, 7}) &&
          !request.front().gossip_metadata_only && !consumer._partial.owned,
          "consumer did not request naturally from actual source metadata");
      check(!consumer.apply_partial_locked(other_peer, message, 1) &&
          consumer.plan_partial_locked(other_peer, 1, false).empty(), "equal metadata created a response loop");
      message.metadata = format::encode(format::metadata{2, 2, 5});
      consumer.apply_partial_locked(other_peer, message, 2);
      check(consumer._partial.peers.at(other_peer.to_string()).remote == format::metadata{2, 2, 5},
          "new metadata OR-accumulated remote availability");
      message.metadata = format::encode(format::metadata{1, 7, 0});
      message.data = format::encode(format::part{0, format::expected_part(token, 0)});
      rejects([&] { consumer.apply_partial_locked(other_peer, message, 3); }, "older or conflicting");
      message.metadata = format::encode(format::metadata{2, 1, 6});
      rejects([&] { consumer.apply_partial_locked(other_peer, message, 3); }, "older or conflicting");
      check(consumer.received_have_locked() == 0 &&
          consumer._partial.peers.at(other_peer.to_string()).remote == format::metadata{2, 2, 5},
          "rejected metadata changed actual receipt state");
      consumer._extension_admission_closed = true;
      const auto revision = consumer._partial.local.revision;
      check(!consumer.apply_partial_locked(other_peer, message, 3) &&
          consumer.plan_partial_locked(other_peer, 3, false).empty() && !consumer.admit_partial_locked() &&
          consumer._partial.local.revision == revision && consumer._events.empty(), "closed retained callback changed application state");
      consumer.record("synthetic_observer", "fixture_unit.only", forge::mutable_variant_object{});
      check(consumer._events.size() == 1, "application admission also closed evidence capture");
      auto empty = forge_pubsub_fixture{token, "victim"};
      empty._extension = "partial";
      message.data.reset();
      message.metadata = format::encode(format::metadata{1, 0, 7});
      empty.apply_partial_locked(other_peer, message, 1);
      check(empty.plan_partial_locked(other_peer, 1, false).empty(), "requested metadata without useful remote availability");
   }
   {
      using format = forge_pubsub_partial;
      constexpr auto token = "00112233445566778899aabbccddeeff";
      auto fixture = forge_pubsub_fixture{token, "victim"};
      fixture._extension = "partial";
      fixture.offer_partial_locked(1);
      fixture.reconstruct_partial_locked();
      check(fixture._partial.local_have == 1 && fixture.received_have_locked() == 0 &&
          !fixture._partial.reconstructed, "local offer became an actual received part");
      const auto part_hex = std::array<std::string_view, 3>{
          "01000032666f7267652d707231323a30303131323233333434353536363737383839396161626263636464656566663a706172742d30",
          "01010032666f7267652d707231323a30303131323233333434353536363737383839396161626263636464656566663a706172742d31",
          "01020032666f7267652d707231323a30303131323233333434353536363737383839396161626263636464656566663a706172742d32"};
      auto message = pubsub::partial_message{.subject = pubsub::topic{"forge-pr11:" + std::string{token}},
          .group_id = format::group(token), .metadata = forge::codec::hex::decode("01000000010700")};
      auto receive = [&](std::size_t index) {
         message.data = forge::codec::hex::decode(part_hex[index]);
         fixture.record("synthetic_input", "fixture_unit.only", forge::mutable_variant_object{}
             ("body_hex", std::string{part_hex[index]}));
         const auto reference = fixture._events.size();
         check(fixture.apply_partial_locked(other_peer, message, reference), "first golden part not applied");
         check(fixture._partial.received[index]->observation == reference &&
             fixture._partial.received[index]->peer == other_peer, "part lost received-input provenance");
         fixture.reconstruct_partial_locked();
      };
      receive(1);
      receive(2);
      check(fixture.received_have_locked() == 6 && !fixture._partial.reconstructed,
          "offer-have1 plus received1/2 falsely reconstructed");
      check(!fixture.apply_partial_locked(other_peer, message, fixture._events.size()),
          "equal part duplicate falsely reported new data/corruption");
      receive(0);
      check(fixture.received_have_locked() == 7 && fixture._partial.reconstructed, "actual part0 did not complete reconstruction");
      const auto& row = fixture._events.back();
      const auto expected = std::string{"forge-pr12:00112233445566778899aabbccddeeff:part-0"}
          + "forge-pr12:00112233445566778899aabbccddeeff:part-1"
          + "forge-pr12:00112233445566778899aabbccddeeff:part-2";
      const auto expected_bytes = std::vector<std::uint8_t>{expected.begin(), expected.end()};
      check(row["kind"].get_string() == "partial_reconstructed" && row["payload_bytes"].as_uint64() == 150 &&
          row["payload_hex"].get_string() == forge::codec::hex::encode(expected_bytes) &&
          row["payload_sha256"].get_string() == "064ef1cce9797038115fb0a843103954c771ea76dccfb25d9200e891b6de1fe4",
          "completion did not expose independently specified received bytes/hash");
      for (auto index = 0u; index < 3; ++index) {
         check(row["parts_hex"].get_array()[index].get_string() == part_hex[index] &&
             row["part_incoming_sequences"].get_array()[index].as_uint64() == fixture._partial.received[index]->observation &&
             row["part_peer_ids"].get_array()[index].get_string() == other_peer.to_string(), "reconstruction input mismatch");
      }
      const auto count = fixture._events.size();
      fixture.reconstruct_partial_locked();
      check(fixture._events.size() == count, "duplicate completion row");
   }
   {
      using format = forge_pubsub_partial;
      auto provider = forge_pubsub_fixture{std::string(32, 'a'), "victim"};
      provider._extension = "partial";
      provider.offer_partial_locked(7);
      check(provider.plan_partial_locked(other_peer, 1, true).size() == 1, "gossip failed to offer metadata");
      check(provider.plan_partial_locked(other_peer, 1, true).empty(), "unchanged gossip looped metadata");
      auto request = pubsub::partial_message{.subject = pubsub::topic{"forge-pr11:" + provider._token},
          .group_id = format::group(provider._token), .metadata = format::encode(format::metadata{1, 0, 7})};
      provider.apply_partial_locked(other_peer, request, 2);
      const auto parts = provider.plan_partial_locked(other_peer, 2, false);
      check(parts.size() == 3 && provider.received_have_locked() == 0, "provider generated parts became receipts");
      for (const auto& action : parts) {
         check(action.value.data && !action.gossip_metadata_only && action.observation == 2, "requested part action lost actual origin");
      }
      check(provider.plan_partial_locked(other_peer, 2, false).empty(), "same request repeated parts");
      provider._partial.peers.clear();
      for (auto index = std::uint8_t{1}; index <= 16; ++index) {
         const auto peer = forge::net::p2p::make_peer_id({.type = forge::net::p2p::public_key::type::ed25519,
             .data = std::vector<std::uint8_t>(32, index)});
         provider.plan_partial_locked(peer, 2, true);
      }
      const auto extra = forge::net::p2p::make_peer_id({.type = forge::net::p2p::public_key::type::ed25519,
          .data = std::vector<std::uint8_t>(32, 17)});
      rejects([&] { provider.plan_partial_locked(extra, 2, true); }, "peer state bound");
      check(provider._partial.peers.size() == 16, "peer limit mutated bounded state");
      for (auto index = 0u; index < 64; ++index) {
         check(provider.admit_partial_locked(), "bounded callback not admitted");
         provider.finish_partial();
      }
      rejects([&] { provider.admit_partial_locked(); }, "input/work bound");
      check(provider._extension_work == 0 && provider._extension_inputs == 64, "bounded callback leaked active work");
      provider.partial_failure("later native send error");
      check(provider._capture_error == "partial application input/work bound exceeded", "late error replaced original failure");
      check(provider.result(true, false, "later main failure")["extension_capture_error"].get_string() ==
          "partial application input/work bound exceeded", "final result masked prior capture error");
   }
   {
      auto fixture = forge_pubsub_fixture{std::string(32, 'a'), "victim"};
      fixture._peer = local_peer.to_string();
      fixture._extension = "partial";
      rejects([&] { fixture.prepare_shutdown(prepare, 1); }, "actual extension drain");
      fixture._extension_drained = true; // Synthetic state, not proof of a native unsubscribe or callback join.
      fixture._extension_work = 1;
      rejects([&] { fixture.prepare_shutdown(prepare, 1); }, "actual extension drain");
      fixture._extension_work = 0;
      fixture._extension_drain_error = "actual drain operation failed";
      rejects([&] { fixture.prepare_shutdown(prepare, 1); }, "prepare_shutdown");
      check(!fixture._prepared && fixture.result(true, false, "earlier native error")["extension_drain_error"].get_string() ==
          "actual drain operation failed", "final result masked actual drain error");
   }
   {
      auto fixture = forge_pubsub_fixture{std::string(32, 'a'), "victim"};
      fixture._extension = "idontwant";
      fixture._peer = local_peer.to_string();
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
      auto command = forge::mutable_variant_object{}("sequence", 1u)("kind", "validation_hold")("payload", "foreign");
      rejects([&] { fixture.command(forge::variant{command}, 1, runtime); }, "validation hold");
      command("payload", "accept:" + fixture._token + ":held");
      fixture.command(forge::variant{command}, 1, runtime);
      check(fixture._events.front()["payload_sha256"].get_string() ==
          "d11365aea10487c693993d641516d041de95f1f9615b9bbf0e2135a1f048d6ac",
          "held payload hash differs from actual raw bytes");
      rejects([&] { fixture.command(forge::variant{command}, 1, runtime); }, "validation hold");
      rejects([&] { fixture.prepare_shutdown(prepare, 1); }, "prepare_shutdown");
      auto release = forge::variant{forge::mutable_variant_object{}("sequence", 2u)("kind", "validation_release")};
      rejects([&] { fixture.command(release, 2, runtime); }, "without held observation");
      check(fixture._hold_observation == 0 && !fixture._hold_released, "command manufactured a received held message");
      runtime.stop();
   }
   for (const auto state : {"active_error", "overflow", "foreign_actor", "foreign_token", "foreign_identity"}) {
      auto fixture = forge_pubsub_fixture{std::string(32, 'a'), "victim"};
      fixture._peer = local_peer.to_string();
      auto input = forge::mutable_variant_object{prepare.get_object()};
      if (state == std::string_view{"active_error"}) { fixture._capture_error = "active native failure"; }
      if (state == std::string_view{"overflow"}) { fixture._overflow = true; }
      if (state == std::string_view{"foreign_actor"}) { input("actor", "sink"); }
      if (state == std::string_view{"foreign_token"}) { input("case_token", std::string(32, 'b')); }
      if (state == std::string_view{"foreign_identity"}) { input("local_peer_id", other_peer.to_string()); }
      rejects([&] { fixture.prepare_shutdown(forge::variant{input}, 1); }, "prepare_shutdown");
      check(!fixture._prepared && fixture._events.empty(), "failed prepare emitted acknowledgement");
      if (state == std::string_view{"active_error"}) {
         check(fixture._capture_error == "active native failure", "late prepare cleared active failure");
      }
   }
   auto prepared = forge_pubsub_fixture{std::string(32, 'a'), "victim"};
   prepared._peer = local_peer.to_string();
   prepared.prepare_shutdown(prepare, 1);
   check(prepared._prepared && prepared._events.size() == 1 &&
       prepared._events.front()["local_peer_id"].get_string() == local_peer.to_string() &&
       prepared._events.front()["admission_closed"].as_bool(), "prepare acknowledgement lost native identity/admission");
   rejects([&] { prepared.prepare_shutdown(prepare, 1); }, "prepare_shutdown");
   {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
      rejects([&] { prepared.command(prepare, 2, runtime); }, "admission closed");
      runtime.stop();
   }
   prepared._capture_error = "real teardown error";
   check(prepared.result(true, true, {})["error"].get_string() == "real teardown error", "prepare masked teardown failure");
   for (const auto text : {"/ip4/127.0.0.1/tcp/1234", "/ip4/127.0.0.1/udp/1234/quic-v1"}) {
      const auto native = forge::net::p2p::parse_endpoint(text);
      auto canonical = native;
      canonical.peer = local_peer;
      const auto address = ready_address(native, local_peer);
      check(!native.peer && address == canonical.to_string(), "ready address changed its typed native listener");
      const auto decoded = forge::net::p2p::parse_endpoint(address);
      check(decoded.peer == local_peer && !decoded.relayed, "ready address lost its native peer binding");
      check(ready_address(canonical, local_peer) == address, "canonical listener gained a duplicate peer suffix");
      canonical.peer = other_peer;
      rejects([&] { static_cast<void>(ready_address(canonical, local_peer)); }, "peer mismatch");
      canonical.peer = local_peer;
      canonical.relayed = forge::net::p2p::endpoint::circuit{.target = other_peer};
      rejects([&] { static_cast<void>(ready_address(canonical, local_peer)); }, "direct native listener");
   }
   struct scratch {
      std::filesystem::path path;
      ~scratch() { auto error = std::error_code{}; std::filesystem::remove_all(path, error); }
   };
   const auto root = std::filesystem::temp_directory_path() / ("forge-pubsub-fixture-unit-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
   check(std::filesystem::create_directory(root), "scratch directory is not fresh");
   const auto cleanup = scratch{root};
   auto write = [&](const auto& path, std::string_view bytes, bool append = false) {
      auto output = std::ofstream{path, std::ios::binary | (append ? std::ios::app : std::ios::trunc)};
      output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      output.close();
      check(static_cast<bool>(output), "cannot write scratch file");
   };
   auto line = [](std::uint64_t sequence) {
      return "{\"sequence\":" + std::to_string(sequence) + ",\"kind\":\"sample\",\"label\":\"before\"}\n";
   };
   auto calls = std::uint64_t{};
   auto apply = [&](const forge::variant& command, std::uint64_t sequence) {
      check(command["sequence"].as_uint64() == sequence && sequence == calls + 1, "noncontiguous application");
      ++calls;
   };
   const auto control = root / "control";
   auto reader = control_reader{control};
   reader.poll(control, apply);
   const auto one = line(1);
   write(control, std::string_view{one}.substr(0, one.size() - 1));
   reader.poll(control, apply);
   check(calls == 0, "partial line was applied");
   rejects([&] { reader.poll(control, apply, true); }, "incomplete");
   write(control, "\n", true);
   reader.poll(control, apply);
   reader.poll(control, apply);
   reader.poll(control, apply, true);
   check(calls == 1, "newline completion replayed or lost");
   auto rewritten = one;
   rewritten.replace(rewritten.find("before"), 6, "after!");
   write(control, rewritten + line(2));
   rejects([&] { reader.poll(control, apply); }, "prefix was rewritten");
   check(calls == 1, "command applied after prefix rewrite");
   write(control, one);
   write(control, line(2), true);
   rejects([&] { reader.poll(control, apply, true); }, "unprocessed");
   write(control, std::string_view{one}.substr(0, one.size() - 1));
   rejects([&] { reader.poll(control, apply); }, "truncated");
   std::filesystem::remove(control);

   const auto prepare_line = "{\"sequence\":1,\"kind\":\"prepare_shutdown\"}\n";
   for (const auto& tail : {line(2), std::string{"{"}}) {
      write(control, prepare_line + tail);
      auto queued = control_reader{control};
      rejects([&] { queued.poll(control, [&](const auto&, auto) { check(false, "prepare with pending command was applied"); }); }, "queued/incomplete");
   }
   write(control, prepare_line);
   auto closed_admission = control_reader{control};
   auto preparations = 0u;
   closed_admission.poll(control, [&](const auto&, auto) { ++preparations; });
   write(control, "{", true);
   rejects([&] { closed_admission.poll(control, [&](const auto&, auto) { ++preparations; }); }, "admission closed");
   check(preparations == 1, "control admitted commands after prepare");
   std::filesystem::remove(control);
   rejects([&] { reader.poll(control, apply); }, "disappeared");

   calls = 0;
   auto limits = control_reader{control};
   auto exact_line = one.substr(0, one.size() - 1);
   exact_line.resize(16 * 1024, ' ');
   write(control, exact_line);
   limits.poll(control, apply);
   check(calls == 0, "exact-bound partial line was applied");
   write(control, "\n", true);
   limits.poll(control, apply);
   for (auto sequence = 2u; sequence <= 64; ++sequence) { write(control, line(sequence), true); }
   limits.poll(control, apply);
   limits.poll(control, apply);
   check(calls == 64, "64 complete commands were not applied exactly once");
   write(control, line(65), true);
   rejects([&] { limits.poll(control, apply); }, "count bound");
   check(calls == 64, "65th command was applied");
   auto oversized = control_reader{control};
   write(control, std::string(16 * 1024 + 1, ' '));
   rejects([&] { oversized.poll(control, apply); }, "line bound");
   write(control, std::string(64 * (16 * 1024 + 1) + 1, ' '));
   rejects([&] { oversized.poll(control, apply); }, "over bound");
   std::filesystem::remove(control);

   auto paths = [&] {
      auto args = arguments{};
      for (const auto flag : {"ready-file", "result-file", "control-file", "stop-file", "store-dir"}) {
         args[flag] = (root / flag).string();
      }
      return args;
   };
   auto fresh = paths();
   canonical_paths(fresh, false);
   check(std::filesystem::path{fresh.at("ready-file")}.is_absolute(), "noncanonical output path");
   auto aliased = paths();
   aliased["control-file"] = (root / "." / "ready-file").string();
   rejects([&] { canonical_paths(aliased, false); }, "distinct");
   aliased = paths();
   aliased["control-file"] = aliased.at("ready-file") + ".tmp";
   rejects([&] { canonical_paths(aliased, false); }, "temporary paths");
   aliased = paths();
   aliased["store-dir"] = aliased.at("result-file");
   rejects([&] { canonical_paths(aliased, false); }, "distinct");
   for (const auto flag : {"ready-file", "result-file", "stop-file"}) {
      write(fresh.at(flag), "");
      rejects([&] { auto args = paths(); canonical_paths(args, false); }, "must be fresh");
      std::filesystem::remove(fresh.at(flag));
   }
   write(fresh.at("control-file"), "");
   canonical_paths(fresh, false);
   write(fresh.at("control-file"), one);
   rejects([&] { canonical_paths(fresh, false); }, "fresh empty regular file");
   std::filesystem::remove(fresh.at("control-file"));
   for (const auto flag : {"ready-file", "result-file"}) {
      const auto temporary = fresh.at(flag) + ".tmp";
      write(temporary, "");
      rejects([&] { auto args = paths(); canonical_paths(args, false); }, "temporary paths");
      rejects([&] { write_atomic(fresh.at(flag), forge::variant{}); }, "temporary file");
      std::filesystem::remove(temporary);
   }
   auto private_paths = paths();
   private_paths["pnet-key-file"] = (root / "key").string();
   write(private_paths.at("pnet-key-file"), std::string(1025, 'x'));
   rejects([&] { canonical_paths(private_paths, true); }, "bounded");
   private_paths["pnet-key-file"] = private_paths.at("stop-file");
   rejects([&] { canonical_paths(private_paths, true); }, "distinct");

   auto observer = forge_pubsub_fixture{std::string(32, 'a'), "victim"};
   const auto peer = forge::net::p2p::peer_id{.value = "synthetic-owner"};
   observer._connections.emplace(7, native_owner{.peer = peer, .remote_address = "synthetic-endpoint",
       .transport = "synthetic-transport", .security = "synthetic-security", .muxer = "synthetic-muxer"});
   check(!observer._node && observer.cached_owner_locked(7, peer), "cached owner requires active session");
   check(!observer.cached_owner_locked(8, peer), "unknown connection became authenticated");
   rejects([&] { static_cast<void>(observer.cached_owner_locked(7, {.value = "other-owner"})); }, "peer mismatch");
   check(observer._events.empty(), "synthetic owner emitted native proof");
   observer._peer.assign(512, '"');
   for (auto count = 0u; count < 300 && !observer._overflow; ++count) {
      observer.record("synthetic", "fixture.unit", forge::mutable_variant_object{}("payload", std::string(63 * 1024, 'x')));
   }
   check(observer._overflow && observer._events.size() < 2048, "trace byte overflow was not sticky");
   const auto events = observer._events.size();
   observer.record("synthetic", "fixture.unit", {});
   check(observer._events.size() == events, "overflow admitted another event");
   const auto final_path = root / "overflow.json";
   write_atomic(final_path, observer.result(true, true, std::string(1024, '"')));
   const auto size = std::filesystem::file_size(final_path);
   check(size <= 16 * 1024 * 1024 && size > 15 * 1024 * 1024, "final JSON bound/reserve not exercised");
   const auto decoded = forge::codec::json::load_value(final_path);
   check(decoded.ok() && decoded.value["finalized"].as_bool() && decoded.value["joined"].as_bool() &&
       decoded.value["overflow"].as_bool() && decoded.value["error"].get_string().size() == 512 &&
       decoded.value["events"].get_array().size() == events, "overflow final JSON lost terminal evidence");
   auto count_bound = forge_pubsub_fixture{std::string(32, 'b'), "victim"};
   for (auto count = 0u; count <= 2048; ++count) { count_bound.record("synthetic", "fixture.unit", {}); }
   check(count_bound._overflow && count_bound._events.size() == 2048, "event count bound failed");
   auto event_bound = forge_pubsub_fixture{std::string(32, 'c'), "victim"};
   event_bound.record("synthetic", "fixture.unit", forge::mutable_variant_object{}("payload", std::string(64 * 1024, 'x')));
   check(event_bound._overflow && !event_bound._capture_error.empty() && event_bound._events.empty(), "event byte bound failed");
}

} // namespace forge::test::libp2p_interop
