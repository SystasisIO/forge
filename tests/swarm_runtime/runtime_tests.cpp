#include <boost/asio/co_spawn.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/multiple_exceptions.hpp>
#include <boost/test/unit_test.hpp>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>
#include "../swarm_storage/support.hpp"

import forge.swarm.runtime.engine;
import forge.api.core.trusted_invocation;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.asio.notification;

namespace runtime_engine_test {
namespace swarm = forge::swarm::runtime;
namespace api = forge::api::core;
namespace core = forge::swarm::core;
using caller = forge::api::auth::authenticated_caller;
using peer = forge::swarm::api::peer;
using namespace std::chrono_literals;

struct upload_probe {
   using call_type = api::bidirectional_stream_call<forge::swarm::protocol::peer_message, forge::swarm::protocol::peer_message>;
   std::shared_ptr<call_type> call;
   std::atomic_bool unchoked{false};
   std::atomic_bool done{false};
   std::atomic_uint blocks{0};
};

boost::asio::awaitable<void> observe_upload(std::shared_ptr<upload_probe> probe,
   std::shared_ptr<forge::asio::notification> changed) {
   try {
      while (auto message = co_await probe->call->async_read()) {
         if (auto* choke = std::get_if<forge::swarm::protocol::choke>(&message->body)) probe->unchoked = !choke->choked;
         if (std::holds_alternative<forge::swarm::protocol::block>(message->body)) ++probe->blocks;
         changed->notify();
      }
   } catch (...) {}
   probe->done = true; changed->notify();
}

caller principal() {
   return {forge::api::auth::caller_source::p2p_peer, core::sha256::hash(std::string{"runtime-unit-principal"})};
}
caller trusted() {
   auto caller = runtime_engine_test::caller{};
   const auto context = api::trusted_invocation_builder{}.set(principal()).build();
   BOOST_REQUIRE(api::server_supplied<runtime_engine_test::caller>::apply(caller, context));
   return caller;
}

class selected_policy final : public swarm::admission {
 public:
   std::shared_ptr<swarm::quota> grant;
   bool metadata_only = false;
   bool wait_for_cancel = false;
   std::atomic_bool entered{false};
   std::atomic_bool left{false};
   forge::asio::notification changed;
   boost::asio::awaitable<std::shared_ptr<swarm::quota>> async_authorize(
      core::swarm_id, caller, swarm::access_kind kind, std::stop_token stop,
      std::chrono::steady_clock::time_point deadline) override {
      entered = true; changed.notify();
      if (wait_for_cancel) {
         try { co_await changed.async_wait_until(changed.epoch(), deadline, stop); } catch (...) {}
      }
      left = true; changed.notify();
      co_return metadata_only && kind != swarm::access_kind::metadata ? nullptr : grant;
   }
};

class stalled_peer final : public peer {
 public:
   std::atomic_uint entered{0};
   std::atomic_uint active{0};
   forge::asio::notification changed;
   boost::asio::awaitable<void> metadata(forge::swarm::protocol::metadata_request, caller,
      api::stream_writer<forge::swarm::protocol::metadata_chunk>) override { co_return; }
   boost::asio::awaitable<void> exchange(forge::swarm::protocol::exchange_request, caller,
      api::duplex_stream<forge::swarm::protocol::peer_message, forge::swarm::protocol::peer_message> stream) override {
      ++entered; ++active; changed.notify();
      try { while (co_await stream.async_read()) {} } catch (...) {}
      --active; changed.notify();
   }
};

class alias_connection final : public swarm::capabilities::connection {
 public:
   explicit alias_connection(std::shared_ptr<stalled_peer> value) : implementation{std::move(value)} {}
   api::handle<peer> remote() const override { return api::handle<peer>{implementation}; }
   caller remote_identity() const override { return principal(); }
   void cancel() noexcept override {}
   boost::asio::awaitable<void> async_close() override { co_return; }
 private:
   std::shared_ptr<stalled_peer> implementation;
};

class aliases final : public swarm::capabilities {
 public:
   std::shared_ptr<stalled_peer> implementation = std::make_shared<stalled_peer>();
   std::atomic_uint connected{0};
   forge::asio::notification changed;
   boost::asio::awaitable<std::vector<std::string>> async_lookup(core::swarm_id, std::string,
      std::size_t, std::stop_token, std::chrono::steady_clock::time_point) override { co_return std::vector<std::string>{"alias-one", "alias-two"}; }
   boost::asio::awaitable<std::unique_ptr<connection>> async_connect(std::string, std::stop_token stop,
      std::chrono::steady_clock::time_point deadline) override {
      ++connected; changed.notify();
      while (true) {
         const auto epoch = changed.epoch();
         if (connected.load() == 2) break;
         co_await changed.async_wait_until(epoch, deadline, stop);
      }
      co_return std::make_unique<alias_connection>(implementation);
   }
   boost::asio::awaitable<std::unique_ptr<registration>> async_register(core::swarm_id, std::string,
      std::stop_token, std::chrono::steady_clock::time_point) override { co_return nullptr; }
};

// A local binding fixture: the bridge, not request data, supplies trusted caller
// context. Production transport bindings remain coordinator-owned integration.
struct link_observation {
   std::atomic_uint connected{0};
   std::atomic_uint closed{0};
   std::atomic_uint canceled{0};
   std::atomic_uint cancel_requests{0};
   std::atomic_uint noop_cancel_requests{0};
   std::atomic_uint succeeded{0};
   std::atomic_uint failed{0};
   std::atomic_uint destroyed_open{0};
   forge::asio::notification changed;
   std::mutex mutex;
   std::exception_ptr first_failure;
   void record_failure(std::exception_ptr error) {
      auto lock = std::scoped_lock{mutex};
      if (!first_failure) first_failure = std::move(error);
   }
   std::exception_ptr failure() {
      auto lock = std::scoped_lock{mutex};
      return first_failure;
   }
};

std::string failure_text(std::exception_ptr error, unsigned depth = 0) {
   if (!error) return "none";
   if (depth == 4) return "nested exception depth limit";
   try { std::rethrow_exception(error); }
   catch (const boost::asio::multiple_exceptions& value) {
      return "Asio multiple_exceptions: " + failure_text(value.first_exception(), depth + 1);
   } catch (const forge::exceptions::base& value) {
      return std::string{value.code().category().name()} + ":" + std::to_string(value.code().value()) + " " + value.what();
   } catch (const std::system_error& value) {
      return std::string{value.code().category().name()} + ":" + std::to_string(value.code().value()) + " " + value.what();
   } catch (const std::exception& value) { return value.what(); }
   catch (...) { return "non-standard exception"; }
}

class trusted_peer_bridge final : public peer {
 public:
   trusted_peer_bridge(std::shared_ptr<peer> implementation, std::shared_ptr<link_observation> observation)
      : implementation{std::move(implementation)}, observation{std::move(observation)} {}
   boost::asio::awaitable<void> metadata(forge::swarm::protocol::metadata_request request, caller,
      api::stream_writer<forge::swarm::protocol::metadata_chunk> output) override {
      co_await implementation->metadata(std::move(request), trusted(), std::move(output));
   }
   boost::asio::awaitable<void> exchange(forge::swarm::protocol::exchange_request request, caller,
      api::duplex_stream<forge::swarm::protocol::peer_message, forge::swarm::protocol::peer_message> stream) override {
      ++active;
      try { co_await implementation->exchange(std::move(request), trusted(), std::move(stream)); ++observation->succeeded; }
      catch (...) {
         observation->record_failure(std::current_exception());
         ++observation->failed; --active; changed.notify(); throw;
      }
      --active; changed.notify();
   }
   std::atomic_uint active{0};
   forge::asio::notification changed;
 private:
   std::shared_ptr<peer> implementation;
   std::shared_ptr<link_observation> observation;
};

class local_connection final : public swarm::capabilities::connection {
 public:
   local_connection(std::shared_ptr<peer> peer, std::shared_ptr<link_observation> observation, bool fail_identity)
      : bridge{std::make_shared<trusted_peer_bridge>(std::move(peer), observation)},
        observation{std::move(observation)}, fail_identity{fail_identity} { ++this->observation->connected; }
   ~local_connection() override { if (!closed) ++observation->destroyed_open; }
   api::handle<peer> remote() const override { return api::handle<peer>{bridge}; }
   caller remote_identity() const override { if (fail_identity) throw std::bad_alloc{}; return principal(); }
   void cancel() noexcept override {
      ++observation->cancel_requests;
      if (bridge->active && !abort_requested.exchange(true)) ++observation->canceled;
      else ++observation->noop_cancel_requests;
   }
   boost::asio::awaitable<void> async_close() override {
      while (true) {
         const auto epoch = bridge->changed.epoch();
         if (!bridge->active) break;
         co_await bridge->changed.async_wait(epoch);
      }
      if (!closed) { closed = true; ++observation->closed; }
   }
 private:
   std::shared_ptr<trusted_peer_bridge> bridge;
   std::shared_ptr<link_observation> observation;
   bool fail_identity;
   bool closed = false;
   std::atomic_bool abort_requested{false};
};

class local_network final : public swarm::capabilities {
 public:
   explicit local_network(std::shared_ptr<peer> implementation) : implementation{std::move(implementation)} {}
   std::shared_ptr<link_observation> observation = std::make_shared<link_observation>();
   std::size_t routes = 1;
   bool fail_identity = false;
   boost::asio::awaitable<std::vector<std::string>> async_lookup(core::swarm_id, std::string,
      std::size_t limit, std::stop_token, std::chrono::steady_clock::time_point) override {
      auto result = std::vector<std::string>{};
      for (auto index = std::size_t{}; index < std::min(routes, limit); ++index) result.push_back(std::to_string(index));
      co_return result;
   }
   boost::asio::awaitable<std::unique_ptr<connection>> async_connect(std::string, std::stop_token,
      std::chrono::steady_clock::time_point) override {
      co_return std::make_unique<local_connection>(implementation, observation, fail_identity);
   }
   boost::asio::awaitable<std::unique_ptr<registration>> async_register(core::swarm_id, std::string,
      std::stop_token, std::chrono::steady_clock::time_point) override { co_return nullptr; }
 private:
   std::shared_ptr<peer> implementation;
};

class observed_store final : public core::store {
 public:
   class observed_writer final : public core::store::writer {
    public:
      observed_writer(observed_store& owner, std::unique_ptr<core::store::writer> writer)
         : owner{owner}, writer{std::move(writer)} { ++owner.writers; }
      ~observed_writer() override { --owner.writers; }
      core::piece_id piece() const noexcept override { return writer->piece(); }
      boost::asio::awaitable<void> async_write(std::uint32_t offset, std::vector<std::uint8_t> bytes, std::stop_token stop) override {
         ++owner.writes;
         if (++calls == 2 && owner.fail_second_write) {
            ++owner.write_failures;
            throw std::system_error{ENOSPC, std::generic_category(), "injected permanent second-block failure"};
         }
         co_await writer->async_write(offset, std::move(bytes), stop);
      }
      boost::asio::awaitable<void> async_commit(std::stop_token stop) override { co_await writer->async_commit(stop); }
      boost::asio::awaitable<void> async_discard() override { ++owner.discards; co_await writer->async_discard(); }
    private:
      observed_store& owner;
      std::unique_ptr<core::store::writer> writer;
      unsigned calls = 0;
   };

   explicit observed_store(std::shared_ptr<core::store> storage) : storage{std::move(storage)} {}
   core::availability snapshot() const override { return storage->snapshot(); }
   std::unique_ptr<core::store::writer> begin(core::piece_id piece) override {
      return std::make_unique<observed_writer>(*this, storage->begin(piece));
   }
   void discard(core::piece_id piece) override { storage->discard(piece); }
   core::store::lease acquire_lease() override { return storage->acquire_lease(); }
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read(
      core::piece_id piece, std::uint32_t offset, std::uint32_t length, std::stop_token stop) override {
      ++reads; changed.notify();
      if (hold_reads) co_await release_reads.async_wait(0, stop);
      co_return co_await storage->async_read(piece, offset, length, stop);
   }
   boost::asio::awaitable<core::availability> async_wait_change(std::uint64_t generation, std::stop_token stop) override {
      co_return co_await storage->async_wait_change(generation, stop);
   }
   boost::asio::awaitable<core::resume_state> async_checkpoint(std::stop_token stop) override {
      co_return co_await storage->async_checkpoint(stop);
   }
   boost::asio::awaitable<void> async_publish(std::stop_token stop) override { co_await storage->async_publish(stop); }
   void request_stop() noexcept override { ++stops; storage->request_stop(); }
   boost::asio::awaitable<void> async_close() override { co_await storage->async_close(); }
   std::atomic_bool hold_reads{false};
   std::atomic_bool fail_second_write{false};
   std::atomic_uint reads{0};
   std::atomic_uint writes{0};
   std::atomic_uint writers{0};
   std::atomic_uint write_failures{0};
   std::atomic_uint discards{0};
   std::atomic_uint stops{0};
   forge::asio::notification changed;
   forge::asio::notification release_reads;
 private:
   std::shared_ptr<core::store> storage;
};

class observed_registration final : public swarm::capabilities::registration {
 public:
   explicit observed_registration(std::shared_ptr<link_observation> observation) : observation{std::move(observation)} {
      ++this->observation->connected; this->observation->changed.notify();
   }
   ~observed_registration() override { if (!closed) ++observation->destroyed_open; }
   void cancel() noexcept override { ++observation->canceled; }
   boost::asio::awaitable<void> async_close() override {
      co_await boost::asio::post(co_await boost::asio::this_coro::executor, boost::asio::use_awaitable);
      if (!closed) { closed = true; ++observation->closed; observation->changed.notify(); }
   }
 private:
   std::shared_ptr<link_observation> observation;
   bool closed = false;
};

class registration_network final : public swarm::capabilities {
 public:
   std::shared_ptr<link_observation> observation = std::make_shared<link_observation>();
   std::function<void()> after_start;
   bool hold_registration = false;
   forge::asio::notification release_registration;
   boost::asio::awaitable<std::vector<std::string>> async_lookup(core::swarm_id, std::string,
      std::size_t, std::stop_token, std::chrono::steady_clock::time_point) override { co_return std::vector<std::string>{}; }
   boost::asio::awaitable<std::unique_ptr<connection>> async_connect(std::string, std::stop_token,
      std::chrono::steady_clock::time_point) override { co_return nullptr; }
   boost::asio::awaitable<std::unique_ptr<registration>> async_register(core::swarm_id, std::string,
      std::stop_token, std::chrono::steady_clock::time_point) override {
      auto resource = std::make_unique<observed_registration>(observation);
      if (after_start) after_start();
      if (hold_registration) co_await release_registration.async_wait(0);
      co_return std::move(resource);
   }
};

boost::asio::awaitable<bool> await_observation(std::shared_ptr<link_observation> observation,
   std::atomic_uint link_observation::* field, unsigned count, std::chrono::steady_clock::time_point deadline) {
   while (true) {
      const auto epoch = observation->changed.epoch();
      if ((observation.get()->*field).load() >= count) co_return true;
      if (std::chrono::steady_clock::now() >= deadline) co_return false;
      co_await observation->changed.async_wait_until(epoch, deadline);
   }
}

class selection_peer final : public peer {
 public:
   std::atomic_uint first{std::numeric_limits<unsigned>::max()};
   boost::asio::awaitable<void> metadata(forge::swarm::protocol::metadata_request, caller,
      api::stream_writer<forge::swarm::protocol::metadata_chunk>) override { co_return; }
   boost::asio::awaitable<void> exchange(forge::swarm::protocol::exchange_request request, caller,
      api::duplex_stream<forge::swarm::protocol::peer_message, forge::swarm::protocol::peer_message> stream) override {
      static_cast<void>(co_await stream.async_read());
      co_await stream.async_write({forge::swarm::protocol::handshake{1, request.swarm, request.realm, 0}});
      co_await stream.async_write({forge::swarm::protocol::inventory{{0xff, 0xff}}});
      co_await stream.async_write({forge::swarm::protocol::choke{false}});
      while (auto message = co_await stream.async_read()) {
         if (auto* block = std::get_if<forge::swarm::protocol::request>(&message->body)) {
            first = block->piece.index;
            co_await stream.async_close();
            co_return;
         }
      }
   }
};

class choked_peer final : public peer {
 public:
   std::atomic_bool ready{false};
   std::atomic_uint requests{0};
   forge::asio::notification changed;
   boost::asio::awaitable<void> metadata(forge::swarm::protocol::metadata_request, caller,
      api::stream_writer<forge::swarm::protocol::metadata_chunk>) override { co_return; }
   boost::asio::awaitable<void> exchange(forge::swarm::protocol::exchange_request request, caller,
      api::duplex_stream<forge::swarm::protocol::peer_message, forge::swarm::protocol::peer_message> stream) override {
      static_cast<void>(co_await stream.async_read());
      static_cast<void>(co_await stream.async_read());
      co_await stream.async_write({forge::swarm::protocol::handshake{1, request.swarm, request.realm, 0}});
      co_await stream.async_write({forge::swarm::protocol::inventory{{1}}});
      co_await stream.async_write({forge::swarm::protocol::choke{true}});
      ready = true; changed.notify();
      while (auto message = co_await stream.async_read()) {
         if (std::holds_alternative<forge::swarm::protocol::request>(message->body)) ++requests;
      }
   }
};

// One payload holder for cross-operation admission, or one winner plus a
// permanently choked sibling. Shallow extra inventory writes acknowledge that
// the client consumed choke before the test opens the payload gate.
class gated_peer final : public peer {
 public:
   gated_peer(core::descriptor manifest, std::vector<std::uint8_t> bytes, bool separate_operations)
      : manifest{std::move(manifest)}, bytes{std::move(bytes)}, separate_operations{separate_operations} {}
   std::array<std::atomic_bool, 3> ready{};
   std::array<std::atomic_uint, 3> requests{};
   std::array<forge::asio::notification, 3> release;
   std::atomic_uint input_eof{0};
   forge::asio::notification changed;
   boost::asio::awaitable<bool> async_ready(unsigned role, unsigned minimum_requests,
      std::chrono::steady_clock::time_point deadline) {
      while (true) {
         const auto epoch = changed.epoch();
         if (ready.at(role) && requests.at(role) >= minimum_requests) co_return true;
         if (std::chrono::steady_clock::now() >= deadline) co_return false;
         co_await changed.async_wait_until(epoch, deadline);
      }
   }
   boost::asio::awaitable<void> metadata(forge::swarm::protocol::metadata_request, caller,
      api::stream_writer<forge::swarm::protocol::metadata_chunk>) override { co_return; }
   boost::asio::awaitable<void> exchange(forge::swarm::protocol::exchange_request request, caller,
      api::duplex_stream<forge::swarm::protocol::peer_message, forge::swarm::protocol::peer_message> stream) override {
      const auto role = entered.fetch_add(1);
      if (role >= ready.size()) throw std::logic_error{"unexpected reconnect in gated peer"};
      static_cast<void>(co_await stream.async_read());
      static_cast<void>(co_await stream.async_read());
      const auto choked = !separate_operations && role == 1;
      co_await stream.async_write({forge::swarm::protocol::handshake{1, request.swarm, request.realm, 0}});
      co_await stream.async_write({forge::swarm::protocol::inventory{{1}}});
      co_await stream.async_write({forge::swarm::protocol::choke{choked}});
      if (separate_operations) {
         co_await stream.async_write({forge::swarm::protocol::inventory{{1}}});
         co_await stream.async_write({forge::swarm::protocol::inventory{{1}}});
      }
      ready.at(role) = true; changed.notify();
      while (auto message = co_await stream.async_read()) {
         forge::swarm::protocol::validate(*message, manifest);
         if (auto* block = std::get_if<forge::swarm::protocol::request>(&message->body)) {
            if (choked) throw std::logic_error{"request sent to choked sibling"};
            ++requests.at(role); changed.notify();
            if (separate_operations) co_await release.at(role).async_wait(0);
            else {
               while (true) {
                  const auto epoch = changed.epoch();
                  if (ready[0] && ready[1]) break;
                  co_await changed.async_wait(epoch);
               }
            }
            auto data = std::vector<std::uint8_t>{bytes.begin() + block->offset, bytes.begin() + block->offset + block->length};
            co_await stream.async_write({forge::swarm::protocol::block{block->id, block->piece, block->offset, std::move(data)}});
         }
      }
      ++input_eof; changed.notify();
      co_await stream.async_close();
   }
 private:
   core::descriptor manifest;
   std::vector<std::uint8_t> bytes;
   bool separate_operations;
   std::atomic_uint entered{0};
};

class admission_peer final : public peer {
 public:
   enum class scenario { rarity_change, choke_empty, reject_empty };
   admission_peer(core::descriptor manifest, std::vector<std::uint8_t> bytes, scenario mode)
      : manifest{std::move(manifest)}, bytes{std::move(bytes)}, mode{mode} {}
   std::atomic_uint entered{0};
   std::atomic_bool secondary_ready{false};
   std::atomic_uint empty_cancellations{0};
   std::atomic_uint continued_partial{0};
   forge::asio::notification changed;
   boost::asio::awaitable<void> metadata(forge::swarm::protocol::metadata_request, caller,
      api::stream_writer<forge::swarm::protocol::metadata_chunk>) override { co_return; }
   boost::asio::awaitable<void> exchange(forge::swarm::protocol::exchange_request request, caller,
      api::duplex_stream<forge::swarm::protocol::peer_message, forge::swarm::protocol::peer_message> stream) override {
      const auto role = entered.fetch_add(1);
      if (role >= (mode == scenario::rarity_change ? 2U : 1U)) throw std::logic_error{"unexpected admission reconnect"};
      static_cast<void>(co_await stream.async_read());
      static_cast<void>(co_await stream.async_read());
      co_await stream.async_write({forge::swarm::protocol::handshake{1, request.swarm, request.realm, 0}});
      co_await stream.async_write({forge::swarm::protocol::inventory{{1}}});
      co_await stream.async_write({forge::swarm::protocol::choke{role != 0}});
      if (role == 1) {
         co_await stream.async_write({forge::swarm::protocol::inventory{{1}}});
         co_await stream.async_write({forge::swarm::protocol::inventory{{1}}});
         secondary_ready = true; changed.notify();
      }
      bool first = true;
      bool restored = false;
      while (auto message = co_await stream.async_read()) {
         if (auto* block = std::get_if<forge::swarm::protocol::request>(&message->body)) {
            if (role != 0) throw std::logic_error{"request to choked admission observer"};
            if (first && mode == scenario::rarity_change) {
               while (true) {
                  const auto epoch = changed.epoch();
                  if (secondary_ready) break;
                  co_await changed.async_wait(epoch);
               }
               // P0 now has two sources and P1 only one. Unrestricted Core
               // selection prefers new P1 over the owned partial P0.
               co_await stream.async_write({forge::swarm::protocol::inventory{{3}}});
            }
            if (mode != scenario::rarity_change && block->piece.index == 0 && !restored) {
               if (mode == scenario::choke_empty) co_await stream.async_write({forge::swarm::protocol::choke{true}});
               else co_await stream.async_write({forge::swarm::protocol::reject{block->id}});
               co_await stream.async_write({forge::swarm::protocol::inventory{{2}}});
               if (mode == scenario::choke_empty) co_await stream.async_write({forge::swarm::protocol::choke{false}});
               ++empty_cancellations; first = false; continue;
            }
            if (block->piece.index == 0 && block->offset != 0) ++continued_partial;
            const auto start = block->piece.index * manifest.piece_size + block->offset;
            auto data = std::vector<std::uint8_t>{bytes.begin() + start, bytes.begin() + start + block->length};
            co_await stream.async_write({forge::swarm::protocol::block{block->id, block->piece, block->offset, std::move(data)}});
            if (mode != scenario::rarity_change && block->piece.index == 1 &&
                block->offset + block->length == manifest.piece_size) {
               restored = true;
               co_await stream.async_write({forge::swarm::protocol::inventory{{3}}});
            }
            first = false;
         }
      }
      co_await stream.async_close();
   }
 private:
   core::descriptor manifest;
   std::vector<std::uint8_t> bytes;
   scenario mode;
};

BOOST_AUTO_TEST_CASE(local_generated_calls_require_trusted_caller_and_explicit_policy) {
   auto fixture = swarm_test::fixture{};
   const auto manifest = swarm_test::describe({swarm_test::data(42)});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto denied = std::make_shared<swarm::engine>(tasks, fixture.cpu.get_executor(), nullptr);
   auto allowed = std::make_shared<swarm::engine>(tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>());
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto first = co_await denied->async_share(manifest, store);
      auto second = co_await allowed->async_share(manifest, store);
      const auto id = core::calculate_swarm_id(manifest);
      auto raw = api::handle<peer>{allowed};
      auto missing = co_await raw.async_open<&peer::metadata>(forge::swarm::protocol::metadata_request{id, {}}, principal());
      auto rejected = false;
      try { co_await missing.async_finish(); } catch (const api::exceptions::invocation_denied&) { rejected = true; }
      BOOST_CHECK(rejected);
      auto closed = api::handle<peer>{denied};
      auto no_policy = co_await closed.async_open<&peer::metadata>(forge::swarm::protocol::metadata_request{id, {}}, trusted());
      rejected = false;
      try { co_await no_policy.async_finish(); } catch (const api::exceptions::invocation_denied&) { rejected = true; }
      BOOST_CHECK(rejected);
      auto permitted = co_await raw.async_open<&peer::metadata>(forge::swarm::protocol::metadata_request{id, {}}, trusted());
      auto count = 0U;
      while (auto chunk = co_await permitted.async_read()) count += chunk->data.size();
      co_await permitted.async_finish();
      BOOST_CHECK_GT(count, 0U);
      co_await first.async_close(); co_await second.async_close();
      co_await denied->async_close(); co_await allowed->async_close();
      BOOST_CHECK(!store->snapshot().published);
      co_await store->async_close();
      co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(policy_callback_deadline_is_cancellable_and_joined) {
   auto fixture = swarm_test::fixture{};
   const auto manifest = swarm_test::describe({swarm_test::data(32)});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto policy = std::make_shared<selected_policy>(); policy->wait_for_cancel = true;
   auto config = swarm::engine::options{}; config.callback_timeout = 25ms;
   auto engine = std::make_shared<swarm::engine>(tasks, fixture.cpu.get_executor(), policy, nullptr, config);
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto share = co_await engine->async_share(manifest, store);
      auto handle = api::handle<peer>{engine};
      auto call = co_await handle.async_open<&peer::metadata>(forge::swarm::protocol::metadata_request{core::calculate_swarm_id(manifest), {}}, trusted());
      auto denied = false;
      try { co_await call.async_finish(); } catch (...) { denied = true; }
      BOOST_CHECK(denied);
      co_await share.async_close();
      BOOST_CHECK(policy->entered.load()); BOOST_CHECK(policy->left.load());
      BOOST_CHECK_EQUAL(share.inspect().active_calls, 0U);
      co_await engine->async_close(); co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(canceling_pending_download_does_not_wait_for_scheduler_slot) {
   auto fixture = swarm_test::fixture{};
   const auto manifest = swarm_test::describe({swarm_test::data(32)});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime, {.max_awaitable_tasks = 1, .max_pending_tasks = 8}};
   auto config = swarm::engine::options{}; config.global.items = 3; config.operations = 1;
   auto engine = swarm::engine{tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>(), nullptr, config};
   auto release = forge::asio::notification{};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto blocker = tasks.submit(forge::asio::task::awaitable{
         .name = "unrelated-shared-scheduler-worker", .work = [&](forge::asio::task::context&) -> boost::asio::awaitable<void> {
            co_await release.async_wait(0);
         }});
      auto download = co_await engine.async_download(manifest, store);
      auto wait_done = std::make_shared<std::atomic_bool>(false);
      boost::asio::co_spawn(co_await boost::asio::this_coro::executor, download.async_wait(),
         [wait_done](std::exception_ptr) { *wait_done = true; });
      co_await download.async_close();
      BOOST_CHECK(download.inspect().closed);
      BOOST_CHECK(!blocker.cancel_requested());
      // All three slots (metadata, picker, orchestration) must be reusable even
      // when the canceled scheduler callable is still retained internally.
      auto next = co_await engine.async_download(manifest, store);
      co_await next.async_close();
      release.notify(); co_await blocker.wait();
      co_await engine.async_close(); co_await store->async_close(); co_await tasks.shutdown();
      BOOST_CHECK(wait_done->load());
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(descriptor_store_mismatch_rejected_before_share_or_download) {
   auto fixture = swarm_test::fixture{};
   const auto actual = swarm_test::describe({swarm_test::data(32, 1)});
   const auto other = swarm_test::describe({swarm_test::data(32, 2)});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(actual)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto engine = swarm::engine{tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>()};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto failures = 0U;
      try { auto wrong = co_await engine.async_share(other, store); }
      catch (const core::exceptions::invalid_checkpoint&) { ++failures; }
      try { auto wrong = co_await engine.async_download(other, store); }
      catch (const core::exceptions::invalid_checkpoint&) { ++failures; }
      BOOST_CHECK_EQUAL(failures, 2U);
      BOOST_CHECK(!store->snapshot().published);
      auto correct = co_await engine.async_share(actual, store);
      co_await correct.async_close(); co_await engine.async_close();
      co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(shared_upload_choker_bounds_slots_and_redistributes_after_close) {
   auto fixture = swarm_test::fixture{};
   const auto bytes = swarm_test::data(32);
   const auto manifest = swarm_test::describe({bytes});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   swarm_test::fill(*store, manifest, {bytes});
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto config = swarm::engine::options{};
   config.upload.slots = 1; config.upload.optimistic = false; config.upload_round = 5ms;
   auto engine = std::make_shared<swarm::engine>(tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>(), nullptr, config);
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto share = co_await engine->async_share(manifest, store);
      auto handle = api::handle<peer>{engine};
      auto changed = std::make_shared<forge::asio::notification>();
      auto probes = std::vector<std::shared_ptr<upload_probe>>{};
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      const auto id = core::calculate_swarm_id(manifest);
      for (auto i = 0U; i < 3; ++i) {
         auto probe = std::make_shared<upload_probe>();
         auto call = co_await handle.async_open<&peer::exchange>(api::call_options{
            .max_item_bytes = 65536, .max_buffered_items = 1, .max_buffered_bytes = 65536, .deadline = deadline},
            forge::swarm::protocol::exchange_request{id, {}}, trusted());
         probe->call = std::make_shared<upload_probe::call_type>(std::move(call));
         boost::asio::co_spawn(co_await boost::asio::this_coro::executor, observe_upload(probe, changed), [](std::exception_ptr) {});
         probes.push_back(probe);
         co_await probe->call->async_write({forge::swarm::protocol::handshake{1, id, {}, 0}});
         co_await probe->call->async_write({forge::swarm::protocol::interest{true}});
      }
      auto winner = std::size_t{3};
      while (std::chrono::steady_clock::now() < deadline) {
         const auto epoch = changed->epoch();
         BOOST_CHECK_LE(share.inspect().unchoked_peers, 1U);
         if (share.inspect().upload_candidates == 3) {
            for (auto i = std::size_t{}; i < probes.size(); ++i) if (probes[i]->unchoked) winner = i;
            if (winner < probes.size()) break;
         }
         co_await changed->async_wait_until(epoch, deadline);
      }
      BOOST_CHECK_LT(winner, probes.size());
      if (winner < probes.size()) {
         co_await probes[winner]->call->async_write({forge::swarm::protocol::request{1, {0, 0}, 0, 32}});
         while (true) {
            const auto epoch = changed->epoch();
            if (probes[winner]->blocks || std::chrono::steady_clock::now() >= deadline) break;
            co_await changed->async_wait_until(epoch, deadline);
         }
         BOOST_CHECK_EQUAL(probes[winner]->blocks.load(), 1U);
         probes[winner]->call->cancel();
         auto replacement = std::size_t{3};
         while (std::chrono::steady_clock::now() < deadline) {
            const auto epoch = changed->epoch();
            BOOST_CHECK_LE(share.inspect().unchoked_peers, 1U);
            for (auto i = std::size_t{}; i < probes.size(); ++i) if (i != winner && probes[i]->unchoked) replacement = i;
            if (replacement < probes.size()) break;
            co_await changed->async_wait_until(epoch, deadline);
         }
         BOOST_CHECK_LT(replacement, probes.size());
         if (replacement < probes.size()) {
            co_await probes[replacement]->call->async_write({forge::swarm::protocol::request{2, {0, 0}, 0, 32}});
            while (true) {
               const auto epoch = changed->epoch();
               if (probes[replacement]->blocks || std::chrono::steady_clock::now() >= deadline) break;
               co_await changed->async_wait_until(epoch, deadline);
            }
            BOOST_CHECK_EQUAL(probes[replacement]->blocks.load(), 1U);
         }
      }
      for (auto& probe : probes) probe->call->cancel();
      for (auto& probe : probes) { try { co_await probe->call->async_finish(); } catch (...) {} }
      co_await share.async_close();
      for (auto& probe : probes) while (!probe->done) {
         const auto epoch = changed->epoch();
         if (probe->done) break;
         co_await changed->async_wait(epoch);
      }
      co_await engine->async_close(); co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(outbound_aliases_share_authenticated_peer_limit_before_exchange) {
   auto fixture = swarm_test::fixture{};
   const auto manifest = swarm_test::describe({swarm_test::data(32)});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto network = std::make_shared<aliases>();
   auto config = swarm::engine::options{};
   config.per_peer.calls = 1; config.peers = 2; config.attempts = 1; config.idle_timeout = 200ms;
   auto engine = swarm::engine{tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>(), network, config};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto download = co_await engine.async_download(manifest, store);
      auto failed = false;
      try { co_await download.async_wait(); } catch (...) { failed = true; }
      BOOST_CHECK(failed);
      BOOST_CHECK_EQUAL(network->connected.load(), 2U);
      BOOST_CHECK_EQUAL(network->implementation->entered.load(), 1U);
      co_await download.async_close();
      while (true) {
         const auto epoch = network->implementation->changed.epoch();
         if (!network->implementation->active) break;
         co_await network->implementation->changed.async_wait(epoch);
      }
      BOOST_CHECK(!store->snapshot().published);
      co_await engine.async_close(); co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(metadata_only_permission_denies_content_exchange) {
   auto fixture = swarm_test::fixture{};
   const auto manifest = swarm_test::describe({swarm_test::data(32)});
   const auto id = core::calculate_swarm_id(manifest);
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto policy = std::make_shared<selected_policy>();
   policy->metadata_only = true;
   policy->grant = std::make_shared<swarm::quota>(swarm::quota::options{.content = id, .requester = principal(), .bytes = 65536});
   auto engine = std::make_shared<swarm::engine>(tasks, fixture.cpu.get_executor(), policy);
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto share = co_await engine->async_share(manifest, store);
      auto handle = api::handle<peer>{engine};
      auto metadata = co_await handle.async_open<&peer::metadata>(forge::swarm::protocol::metadata_request{id, {}}, trusted());
      auto chunks = 0U;
      while (co_await metadata.async_read()) ++chunks;
      co_await metadata.async_finish(); BOOST_CHECK_GT(chunks, 0U);
      auto exchange = co_await handle.async_open<&peer::exchange>(forge::swarm::protocol::exchange_request{id, {}}, trusted());
      auto denied = false;
      try { co_await exchange.async_finish(); } catch (const api::exceptions::invocation_denied&) { denied = true; }
      BOOST_CHECK(denied);
      co_await share.async_close(); co_await engine->async_close(); co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(expiry_and_revocation_interrupt_accepted_metadata_and_finalize_quota) {
   auto fixture = swarm_test::fixture{};
   // A piece layer larger than one metadata chunk exercises queue backpressure.
   const auto manifest = swarm_test::describe({swarm_test::data(1100U * core::block_size)}, core::block_size);
   const auto id = core::calculate_swarm_id(manifest);
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto policy = std::make_shared<selected_policy>();
   auto engine = std::make_shared<swarm::engine>(tasks, fixture.cpu.get_executor(), policy);
   auto body = [&]() -> boost::asio::awaitable<void> {
      for (const auto revoke : {false, true}) {
         auto share = co_await engine->async_share(manifest, store);
         policy->grant = std::make_shared<swarm::quota>(swarm::quota::options{
            .content = id, .requester = principal(), .bytes = 65536,
            .expires = std::chrono::steady_clock::now() + (revoke ? 3s : 200ms)});
         auto handle = api::handle<peer>{engine};
         auto call = co_await handle.async_open<&peer::metadata>(api::call_options{
            .max_item_bytes = 65536, .max_buffered_items = 1, .max_buffered_bytes = 65536,
            .deadline = std::chrono::steady_clock::now() + 4s},
            forge::swarm::protocol::metadata_request{id, {}}, trusted());
         if (revoke) {
            auto first = co_await call.async_read(); BOOST_CHECK(first.has_value());
            policy->grant->revoke();
         }
         auto failed = false;
         try { co_await call.async_finish(); } catch (...) { failed = true; }
         BOOST_CHECK(failed);
         co_await share.async_close();
         const auto status = policy->grant->inspect();
         BOOST_CHECK_GT(status.traffic.queued, 0U);
         BOOST_CHECK_EQUAL(status.traffic.sent, 0U);
         BOOST_CHECK_EQUAL(status.held, 0U);
         BOOST_CHECK_GT(status.finalized, 0U);
      }
      co_await engine->async_close(); co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(lazy_awaitables_capture_state_before_facade_move_or_destruction) {
   auto fixture = swarm_test::fixture{};
   const auto manifest = swarm_test::describe({swarm_test::data(32)});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto engine = std::make_unique<swarm::engine>(tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>());
      auto share = co_await engine->async_share(manifest, store);
      auto pending_close = share.async_close();
      share = swarm::sharing{};
      co_await std::move(pending_close);
      auto download = co_await engine->async_download(manifest, store);
      auto pending_wait = download.async_wait();
      auto pending_download_close = download.async_close();
      download = swarm::download{};
      try { co_await std::move(pending_wait); } catch (...) {}
      co_await std::move(pending_download_close);
      auto pending_share = engine->async_share(manifest, store);
      auto pending_download = engine->async_download(manifest, store);
      auto pending_engine_close = engine->async_close();
      engine.reset();
      auto rejected = 0U;
      try { auto unused = co_await std::move(pending_share); } catch (...) { ++rejected; }
      try { auto unused = co_await std::move(pending_download); } catch (...) { ++rejected; }
      BOOST_CHECK_EQUAL(rejected, 2U);
      co_await std::move(pending_engine_close);
      co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(connected_peer_sees_only_verified_store_changes_without_reconnect) {
   auto fixture = swarm_test::fixture{};
   const auto bytes = swarm_test::data(32);
   const auto manifest = swarm_test::describe({bytes});
   const auto id = core::calculate_swarm_id(manifest);
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   // Interest coalesced in the current real millisecond must use upload_next,
   // not wait for a periodic round beyond this call's existing three seconds.
   auto config = swarm::engine::options{}; config.upload_round = 10s;
   auto engine = std::make_shared<swarm::engine>(tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>(), nullptr, config);
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto share = co_await engine->async_share(manifest, store);
      auto handle = api::handle<peer>{engine};
      auto call = co_await handle.async_open<&peer::exchange>(api::call_options{
         .max_item_bytes = 65536, .max_buffered_items = 1, .max_buffered_bytes = 65536,
         .deadline = std::chrono::steady_clock::now() + 3s},
         forge::swarm::protocol::exchange_request{id, {}}, trusted());
      co_await call.async_write({forge::swarm::protocol::handshake{1, id, {}, 0}});
      co_await call.async_write({forge::swarm::protocol::interest{true}});
      auto unchoked = false;
      while (auto message = co_await call.async_read()) {
         if (auto* inventory = std::get_if<forge::swarm::protocol::inventory>(&message->body)) {
            BOOST_CHECK(!core::bitmap_test(inventory->pieces, 0)); break;
         }
      }
      auto writer = store->begin({0, 0});
      co_await writer->async_write(0, bytes);
      BOOST_CHECK(!core::bitmap_test(store->snapshot().verified, 0));
      co_await writer->async_commit(); writer.reset();
      auto announced = false;
      while (auto message = co_await call.async_read()) {
         if (auto* choke = std::get_if<forge::swarm::protocol::choke>(&message->body)) unchoked = !choke->choked;
         if (auto* have = std::get_if<forge::swarm::protocol::have>(&message->body)) {
            BOOST_CHECK(have->piece == core::piece_id(0, 0)); announced = true;
         }
         if (announced && unchoked) break;
      }
      BOOST_CHECK(announced);
      BOOST_CHECK(unchoked);
      co_await call.async_write({forge::swarm::protocol::request{1, {0, 0}, 0, 32}});
      auto received = false;
      while (auto message = co_await call.async_read()) {
         if (auto* block = std::get_if<forge::swarm::protocol::block>(&message->body)) {
            BOOST_CHECK(block->data == bytes); received = true; break;
         }
      }
      BOOST_CHECK(received);
      co_await call.async_close();
      while (co_await call.async_read()) {}
      co_await call.async_finish();
      co_await share.async_close(); co_await engine->async_close(); co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(verified_resume_and_empty_content_publish_without_network) {
   for (const auto empty : {false, true}) {
      auto fixture = swarm_test::fixture{};
      const auto bytes = swarm_test::data(empty ? 0 : 32);
      const auto manifest = swarm_test::describe({bytes});
      auto store = std::shared_ptr<core::store>{swarm_test::run(
         forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
      swarm_test::fill(*store, manifest, {bytes});
      auto runtime = forge::asio::runtime{};
      auto tasks = forge::asio::task::scheduler{runtime};
      auto engine = swarm::engine{tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>()};
      auto body = [&]() -> boost::asio::awaitable<void> {
         auto download = co_await engine.async_download(manifest, store);
         auto failure = std::exception_ptr{};
         try { co_await download.async_wait(); } catch (...) { failure = std::current_exception(); }
         BOOST_CHECK(!failure);
         BOOST_CHECK(download.inspect().complete);
         BOOST_CHECK(store->snapshot().published);
         if (!failure) BOOST_CHECK(swarm_test::read_file(fixture.root / "result") == bytes);
         co_await download.async_close(); co_await engine.async_close();
         co_await store->async_close(); co_await tasks.shutdown();
      };
      forge::asio::blocking::run(runtime, body());
   }
}

BOOST_AUTO_TEST_CASE(rejected_scheduler_submission_rolls_back_operation_capacity) {
   auto fixture = swarm_test::fixture{};
   const auto manifest = swarm_test::describe({swarm_test::data(32)});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime, {.max_awaitable_tasks = 1, .max_pending_tasks = 1}};
   auto config = swarm::engine::options{}; config.global.items = 3; config.operations = 1;
   auto engine = swarm::engine{tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>(), nullptr, config};
   auto entered = false;
   auto changed = forge::asio::notification{};
   auto release = forge::asio::notification{};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto blocker = tasks.submit(forge::asio::task::awaitable{
         .name = "unrelated-running-worker", .work = [&](forge::asio::task::context&) -> boost::asio::awaitable<void> {
            entered = true; changed.notify(); co_await release.async_wait(0);
         }});
      while (true) {
         const auto epoch = changed.epoch();
         if (entered) break;
         co_await changed.async_wait(epoch);
      }
      auto pending = tasks.submit(forge::asio::task::awaitable{
         .name = "unrelated-pending-worker", .work = [](forge::asio::task::context&) -> boost::asio::awaitable<void> { co_return; }});
      BOOST_CHECK(pending.accepted());
      auto rejected = false;
      try { auto download = co_await engine.async_download(manifest, store); }
      catch (const api::exceptions::resource_exhausted&) { rejected = true; }
      BOOST_CHECK(rejected);
      pending.cancel(); try { co_await pending.wait(); } catch (...) {}
      auto download = co_await engine.async_download(manifest, store);
      co_await download.async_close();
      BOOST_CHECK(download.inspect().closed);
      BOOST_CHECK(!blocker.cancel_requested());
      release.notify(); co_await blocker.wait();
      co_await engine.async_close(); co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(published_success_stays_success_when_first_wait_is_after_total_deadline) {
   auto fixture = swarm_test::fixture{};
   const auto bytes = swarm_test::data(32);
   const auto manifest = swarm_test::describe({bytes});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   swarm_test::fill(*store, manifest, {bytes});
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto engine = swarm::engine{tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>()};
   auto body = [&]() -> boost::asio::awaitable<void> {
      const auto deadline = std::chrono::steady_clock::now() + 1s;
      auto download = co_await engine.async_download(manifest, store, {}, {}, deadline);
      while (true) {
         const auto state = store->snapshot();
         if (state.published) break;
         static_cast<void>(co_await store->async_wait_change(state.generation));
      }
      auto clock = forge::asio::notification{};
      co_await clock.async_wait_until(0, deadline + 10ms);
      auto failure = std::exception_ptr{};
      try { co_await download.async_wait(); } catch (...) { failure = std::current_exception(); }
      BOOST_CHECK(!failure);
      BOOST_CHECK(download.inspect().complete);
      co_await download.async_close();
      co_await download.async_wait();
      co_await engine.async_close(); co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(four_io_workers_share_picker_and_finish_streams_cleanly) {
   auto source = swarm_test::fixture{};
   auto target = swarm_test::fixture{};
   const auto bytes = swarm_test::data(128U * core::block_size + 31);
   const auto manifest = swarm_test::describe({bytes});
   auto seed = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(source.options(manifest)))};
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(target.options(manifest)))};
   swarm_test::fill(*seed, manifest, {bytes});
   auto runtime = forge::asio::runtime{{.worker_threads = 4}};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto config = swarm::engine::options{};
   config.peers = 2; config.per_peer.items = 64; config.per_operation.items = 64;
   config.upload.slots = 2; config.upload.optimistic = false; config.upload_round = 1ms;
   auto server = std::make_shared<swarm::engine>(tasks, source.cpu.get_executor(),
      std::make_shared<swarm::free_admission>(), nullptr, config);
   auto network = std::make_shared<local_network>(server); network->routes = 2;
   auto client = swarm::engine{tasks, target.cpu.get_executor(), std::make_shared<swarm::free_admission>(), network, config};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto share = co_await server->async_share(manifest, seed);
      auto download = co_await client.async_download(manifest, store, {}, {}, std::chrono::steady_clock::now() + 10s);
      auto failure = std::exception_ptr{};
      try { co_await download.async_wait(); } catch (...) { failure = std::current_exception(); }
      BOOST_CHECK_MESSAGE(!failure, failure_text(failure));
      BOOST_CHECK(download.inspect().complete);
      BOOST_CHECK_EQUAL(download.inspect().traffic.verified, bytes.size());
      BOOST_CHECK(store->snapshot().published);
      if (!failure) BOOST_CHECK(swarm_test::read_file(target.root / "result") == bytes);
      co_await download.async_close(); co_await share.async_close();
      BOOST_CHECK_EQUAL(network->observation->connected.load(), 2U);
      BOOST_CHECK_EQUAL(network->observation->closed.load(), 2U);
      BOOST_TEST_MESSAGE("four-I/O server failure: " << failure_text(network->observation->failure()));
      BOOST_TEST_CONTEXT("server failure=" << failure_text(network->observation->failure()) <<
         "; cancel requests=" << network->observation->cancel_requests.load() <<
         "; first active cancel requests=" << network->observation->canceled.load() <<
         "; duplicate/after-terminal cancel requests=" << network->observation->noop_cancel_requests.load()) {
         BOOST_CHECK_EQUAL(network->observation->succeeded.load(), 2U);
         BOOST_CHECK_EQUAL(network->observation->failed.load(), 0U);
         BOOST_CHECK_EQUAL(network->observation->canceled.load(), 0U);
         BOOST_CHECK_EQUAL(network->observation->cancel_requests.load(), 0U);
      }
      BOOST_CHECK_EQUAL(network->observation->destroyed_open.load(), 0U);
      co_await client.async_close(); co_await server->async_close();
      co_await store->async_close(); co_await seed->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(choked_download_waits_without_reserving_spare_data_capacity) {
   auto fixture = swarm_test::fixture{};
   const auto manifest = swarm_test::describe({swarm_test::data(32)});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto remote = std::make_shared<choked_peer>();
   auto network = std::make_shared<local_network>(remote);
   auto config = swarm::engine::options{};
   config.peers = 1; config.attempts = 1;
   // Exactly the two stream-scope items plus its retained control ticket. Data
   // cannot fit: merely waiting for an eligible pick must not abort this call.
   config.per_peer.items = 3;
   auto engine = swarm::engine{tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>(), network, config};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto download = co_await engine.async_download(manifest, store);
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      while (true) {
         const auto epoch = remote->changed.epoch();
         if (remote->ready || std::chrono::steady_clock::now() >= deadline) break;
         co_await remote->changed.async_wait_until(epoch, deadline);
      }
      BOOST_CHECK(remote->ready.load());
      BOOST_CHECK_MESSAGE(network->observation->failed.load() == 0U, failure_text(network->observation->failure()));
      BOOST_CHECK_EQUAL(network->observation->cancel_requests.load(), 0U);
      BOOST_CHECK_EQUAL(download.inspect().active_calls, 1U);
      BOOST_CHECK_EQUAL(remote->requests.load(), 0U);
      co_await download.async_close();
      BOOST_CHECK_EQUAL(network->observation->closed.load(), 1U);
      BOOST_CHECK_EQUAL(network->observation->destroyed_open.load(), 0U);
      co_await engine.async_close(); co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(owned_partial_continues_when_new_rarest_piece_cannot_be_admitted) {
   auto fixture = swarm_test::fixture{};
   const auto bytes = swarm_test::data(4 * core::block_size);
   const auto manifest = swarm_test::describe({bytes});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto remote = std::make_shared<admission_peer>(manifest, bytes, admission_peer::scenario::rarity_change);
   auto network = std::make_shared<local_network>(remote); network->routes = 2;
   auto config = swarm::engine::options{};
   config.peers = 2; config.attempts = 1; config.per_peer.pieces = 1;
   auto engine = swarm::engine{tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>(), network, config};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto operation = co_await engine.async_download(manifest, store, {}, {}, std::chrono::steady_clock::now() + 3s);
      std::exception_ptr failure;
      try { co_await operation.async_wait(); } catch (...) { failure = std::current_exception(); }
      BOOST_CHECK_MESSAGE(!failure, failure_text(failure));
      BOOST_CHECK(operation.inspect().complete);
      BOOST_CHECK_EQUAL(remote->continued_partial.load(), 1U);
      BOOST_CHECK_EQUAL(network->observation->connected.load(), 2U);
      BOOST_CHECK_EQUAL(network->observation->succeeded.load(), 2U);
      if (!failure) BOOST_CHECK(swarm_test::read_file(fixture.root / "result") == bytes);
      co_await operation.async_close(); co_await engine.async_close(); co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(choke_and_reject_retire_only_empty_piece_ownership) {
   for (const auto mode : {admission_peer::scenario::choke_empty, admission_peer::scenario::reject_empty}) {
      auto fixture = swarm_test::fixture{};
      const auto bytes = swarm_test::data(4 * core::block_size);
      const auto manifest = swarm_test::describe({bytes});
      auto store = std::shared_ptr<core::store>{swarm_test::run(
         forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
      auto runtime = forge::asio::runtime{};
      auto tasks = forge::asio::task::scheduler{runtime};
      auto remote = std::make_shared<admission_peer>(manifest, bytes, mode);
      auto network = std::make_shared<local_network>(remote);
      auto config = swarm::engine::options{};
      config.peers = 1; config.attempts = 1; config.per_peer.pieces = 1;
      auto engine = swarm::engine{tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>(), network, config};
      auto body = [&]() -> boost::asio::awaitable<void> {
         auto operation = co_await engine.async_download(manifest, store, {}, {}, std::chrono::steady_clock::now() + 3s);
         std::exception_ptr failure;
         try { co_await operation.async_wait(); } catch (...) { failure = std::current_exception(); }
         BOOST_CHECK_MESSAGE(!failure, failure_text(failure));
         BOOST_CHECK(operation.inspect().complete);
         BOOST_CHECK_GE(remote->empty_cancellations.load(), 1U);
         BOOST_CHECK_EQUAL(network->observation->connected.load(), 1U);
         if (!failure) BOOST_CHECK(swarm_test::read_file(fixture.root / "result") == bytes);
         co_await operation.async_close(); co_await engine.async_close(); co_await store->async_close(); co_await tasks.shutdown();
      };
      forge::asio::blocking::run(runtime, body());
   }
}

BOOST_AUTO_TEST_CASE(two_budget_waiters_do_not_wake_each_other_by_failed_request_probes) {
   auto first = swarm_test::fixture{};
   auto second = swarm_test::fixture{};
   auto third = swarm_test::fixture{};
   const auto bytes = swarm_test::data(32);
   const auto manifest = swarm_test::describe({bytes});
   auto a_store = std::shared_ptr<core::store>{swarm_test::run(forge::swarm::storage::file_store::async_open(first.options(manifest)))};
   auto b_store = std::shared_ptr<core::store>{swarm_test::run(forge::swarm::storage::file_store::async_open(second.options(manifest)))};
   auto c_store = std::shared_ptr<core::store>{swarm_test::run(forge::swarm::storage::file_store::async_open(third.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto remote = std::make_shared<gated_peer>(manifest, bytes, true);
   auto network = std::make_shared<local_network>(remote);
   auto config = swarm::engine::options{};
   config.peers = 1; config.attempts = 1; config.per_peer.pieces = 1;
   auto engine = swarm::engine{tasks, first.cpu.get_executor(), std::make_shared<swarm::free_admission>(), network, config};
   auto body = [&]() -> boost::asio::awaitable<void> {
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      auto a = co_await engine.async_download(manifest, a_store, {}, {}, deadline);
      const auto held = co_await remote->async_ready(0, 1, deadline); BOOST_CHECK(held);
      auto b = co_await engine.async_download(manifest, b_store, {}, {}, deadline);
      const auto b_ready = co_await remote->async_ready(1, 0, deadline); BOOST_CHECK(b_ready);
      auto c = co_await engine.async_download(manifest, c_store, {}, {}, deadline);
      const auto c_ready = co_await remote->async_ready(2, 0, deadline); BOOST_CHECK(c_ready);
      while ((!b.inspect().capacity_waiters || !c.inspect().capacity_waiters) && std::chrono::steady_clock::now() < deadline)
         co_await boost::asio::post(co_await boost::asio::this_coro::executor, boost::asio::use_awaitable);
      auto delay = forge::asio::notification{};
      co_await delay.async_wait_until(delay.epoch(), std::chrono::steady_clock::now() + 50ms);
      const auto b_waits = b.inspect().capacity_waits;
      const auto c_waits = c.inspect().capacity_waits;
      co_await delay.async_wait_until(delay.epoch(), std::chrono::steady_clock::now() + 50ms);
      BOOST_CHECK_EQUAL(b.inspect().capacity_waiters, 1U);
      BOOST_CHECK_EQUAL(c.inspect().capacity_waiters, 1U);
      BOOST_CHECK_EQUAL(b.inspect().capacity_waits, b_waits);
      BOOST_CHECK_EQUAL(c.inspect().capacity_waits, c_waits);
      BOOST_CHECK_EQUAL(remote->requests[1].load(), 0U);
      BOOST_CHECK_EQUAL(remote->requests[2].load(), 0U);
      co_await b.async_close();
      BOOST_CHECK_EQUAL(b.inspect().capacity_waiters, 0U);
      remote->release[2].notify(); remote->release[0].notify();
      std::exception_ptr failure;
      try { co_await a.async_wait(); co_await c.async_wait(); } catch (...) { failure = std::current_exception(); }
      BOOST_CHECK_MESSAGE(!failure, failure_text(failure));
      BOOST_CHECK(a.inspect().complete); BOOST_CHECK(c.inspect().complete);
      BOOST_CHECK_EQUAL(remote->requests[0].load(), 1U);
      BOOST_CHECK_EQUAL(remote->requests[2].load(), 1U);
      BOOST_CHECK_EQUAL(network->observation->connected.load(), 3U);
      co_await a.async_close(); co_await c.async_close(); co_await engine.async_close();
      co_await a_store->async_close(); co_await b_store->async_close(); co_await c_store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(shared_budget_wakes_other_operation_and_cancellation_joins_blocked_call) {
   auto first = swarm_test::fixture{};
   auto second = swarm_test::fixture{};
   const auto bytes = swarm_test::data(32);
   const auto manifest = swarm_test::describe({bytes});
   auto first_store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(first.options(manifest)))};
   auto second_store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(second.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto remote = std::make_shared<gated_peer>(manifest, bytes, true);
   auto network = std::make_shared<local_network>(remote);
   auto config = swarm::engine::options{};
   config.peers = 1; config.attempts = 1; config.per_peer.pieces = 1;
   auto engine = swarm::engine{tasks, first.cpu.get_executor(), std::make_shared<swarm::free_admission>(), network, config};
   auto body = [&]() -> boost::asio::awaitable<void> {
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      auto a = co_await engine.async_download(manifest, first_store, {}, {}, deadline);
      const auto held = co_await remote->async_ready(0, 1, deadline);
      BOOST_CHECK(held);
      auto b = co_await engine.async_download(manifest, second_store, {}, {}, deadline);
      const auto blocked = co_await remote->async_ready(1, 0, deadline);
      BOOST_CHECK(blocked);
      while (b.inspect().capacity_waiters == 0 && std::chrono::steady_clock::now() < deadline)
         co_await boost::asio::post(co_await boost::asio::this_coro::executor, boost::asio::use_awaitable);
      BOOST_CHECK_EQUAL(b.inspect().capacity_waiters, 1U);
      BOOST_CHECK_EQUAL(remote->requests[1].load(), 0U);
      BOOST_CHECK_EQUAL(b.inspect().active_calls, 1U);
      co_await b.async_close();
      BOOST_CHECK(b.inspect().closed);
      BOOST_CHECK_EQUAL(b.inspect().active_calls, 0U);
      BOOST_CHECK_EQUAL(b.inspect().capacity_waiters, 0U);
      BOOST_CHECK(!a.inspect().canceled);
      BOOST_CHECK(!first_store->snapshot().published);
      auto c = co_await engine.async_download(manifest, second_store, {}, {}, deadline);
      const auto waiting = co_await remote->async_ready(2, 0, deadline);
      BOOST_CHECK(waiting);
      while (c.inspect().capacity_waiters == 0 && std::chrono::steady_clock::now() < deadline)
         co_await boost::asio::post(co_await boost::asio::this_coro::executor, boost::asio::use_awaitable);
      BOOST_CHECK_EQUAL(c.inspect().capacity_waiters, 1U);
      BOOST_CHECK_EQUAL(remote->requests[2].load(), 0U);
      BOOST_CHECK_EQUAL(c.inspect().active_calls, 1U);
      remote->release[2].notify();
      remote->release[0].notify();
      auto failure = std::exception_ptr{};
      try { co_await a.async_wait(); co_await c.async_wait(); } catch (...) { failure = std::current_exception(); }
      BOOST_CHECK_MESSAGE(!failure, failure_text(failure));
      BOOST_CHECK(a.inspect().complete); BOOST_CHECK(c.inspect().complete);
      BOOST_CHECK_EQUAL(remote->requests[0].load(), 1U);
      BOOST_CHECK_EQUAL(remote->requests[1].load(), 0U);
      BOOST_CHECK_EQUAL(remote->requests[2].load(), 1U);
      BOOST_CHECK_EQUAL(remote->input_eof.load(), 2U);
      if (!failure) {
         BOOST_CHECK(swarm_test::read_file(first.root / "result") == bytes);
         BOOST_CHECK(swarm_test::read_file(second.root / "result") == bytes);
      }
      co_await a.async_close(); co_await c.async_close();
      BOOST_CHECK_EQUAL(c.inspect().capacity_waiters, 0U);
      BOOST_CHECK_EQUAL(network->observation->connected.load(), 3U);
      BOOST_CHECK_EQUAL(network->observation->closed.load(), 3U);
      BOOST_CHECK_EQUAL(network->observation->succeeded.load(), 2U);
      BOOST_CHECK_EQUAL(network->observation->failed.load(), 1U);
      BOOST_CHECK_EQUAL(network->observation->destroyed_open.load(), 0U);
      co_await engine.async_close(); co_await first_store->async_close(); co_await second_store->async_close();
      co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(global_completion_half_closes_and_joins_choked_sibling_successfully) {
   auto fixture = swarm_test::fixture{};
   const auto bytes = swarm_test::data(32);
   const auto manifest = swarm_test::describe({bytes});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{{.worker_threads = 4}};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto remote = std::make_shared<gated_peer>(manifest, bytes, false);
   auto network = std::make_shared<local_network>(remote); network->routes = 2;
   auto config = swarm::engine::options{}; config.peers = 2; config.attempts = 1;
   auto engine = swarm::engine{tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>(), network, config};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto download = co_await engine.async_download(manifest, store, {}, {}, std::chrono::steady_clock::now() + 3s);
      auto failure = std::exception_ptr{};
      try { co_await download.async_wait(); } catch (...) { failure = std::current_exception(); }
      BOOST_TEST_CONTEXT("server failure=" << failure_text(network->observation->failure())) {
         BOOST_CHECK_MESSAGE(!failure, failure_text(failure));
         BOOST_CHECK(download.inspect().complete);
         BOOST_CHECK_EQUAL(download.inspect().traffic.verified, bytes.size());
         BOOST_CHECK_EQUAL(remote->requests[0].load(), 1U);
         BOOST_CHECK_EQUAL(remote->requests[1].load(), 0U);
         BOOST_CHECK_EQUAL(remote->input_eof.load(), 2U);
         BOOST_CHECK_EQUAL(network->observation->succeeded.load(), 2U);
         BOOST_CHECK_EQUAL(network->observation->failed.load(), 0U);
         BOOST_CHECK_EQUAL(network->observation->cancel_requests.load(), 0U);
      }
      if (!failure) BOOST_CHECK(swarm_test::read_file(fixture.root / "result") == bytes);
      co_await download.async_close();
      BOOST_CHECK_EQUAL(network->observation->closed.load(), 2U);
      BOOST_CHECK_EQUAL(network->observation->destroyed_open.load(), 0U);
      co_await engine.async_close(); co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(input_half_close_drains_delayed_block_and_successful_terminal) {
   auto fixture = swarm_test::fixture{};
   const auto bytes = swarm_test::data(32);
   const auto manifest = swarm_test::describe({bytes});
   const auto id = core::calculate_swarm_id(manifest);
   auto inner = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   swarm_test::fill(*inner, manifest, {bytes});
   auto store = std::make_shared<observed_store>(inner); store->hold_reads = true;
   auto policy = std::make_shared<selected_policy>();
   policy->grant = std::make_shared<swarm::quota>(swarm::quota::options{
      .content = id, .requester = principal(), .bytes = bytes.size()});
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto config = swarm::engine::options{}; config.upload_round = 1ms;
   auto server = std::make_shared<swarm::engine>(tasks, fixture.cpu.get_executor(), policy, nullptr, config);
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto share = co_await server->async_share(manifest, store);
      auto handle = api::handle<peer>{server};
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      auto call = co_await handle.async_open<&peer::exchange>(api::call_options{
         .max_item_bytes = 65536, .max_buffered_items = 1, .max_buffered_bytes = 65536, .deadline = deadline},
         forge::swarm::protocol::exchange_request{id, {}}, trusted());
      auto failure = std::exception_ptr{};
      auto blocks = 0U;
      auto eof = false;
      auto terminal_waited = false;
      try {
         co_await call.async_write({forge::swarm::protocol::handshake{1, id, {}, 0}});
         co_await call.async_write({forge::swarm::protocol::interest{true}});
         while (auto message = co_await call.async_read()) {
            if (const auto* choke = std::get_if<forge::swarm::protocol::choke>(&message->body); choke && !choke->choked) break;
         }
         co_await call.async_write({forge::swarm::protocol::request{1, {0, 0}, 0, 32}});
         while (true) {
            const auto epoch = store->changed.epoch();
            if (store->reads) break;
            co_await store->changed.async_wait_until(epoch, deadline);
         }
         co_await call.async_close();
         BOOST_CHECK_EQUAL(policy->grant->inspect().held, bytes.size());
         store->release_reads.notify();
         while (auto message = co_await call.async_read()) {
            if (const auto* block = std::get_if<forge::swarm::protocol::block>(&message->body)) {
               ++blocks; BOOST_CHECK(block->data == bytes);
            }
         }
         eof = true;
         terminal_waited = true;
         co_await call.async_finish();
      } catch (...) { failure = std::current_exception(); call.cancel(); }
      store->release_reads.notify();
      if (!terminal_waited) {
         terminal_waited = true;
         try { co_await call.async_finish(); } catch (...) { if (!failure) failure = std::current_exception(); }
      }
      co_await share.async_close();
      BOOST_CHECK_MESSAGE(!failure, failure_text(failure)); BOOST_CHECK(eof); BOOST_CHECK_EQUAL(blocks, 1U);
      const auto accounting = policy->grant->inspect();
      BOOST_CHECK_EQUAL(accounting.traffic.queued, bytes.size());
      BOOST_CHECK_EQUAL(accounting.traffic.uncertain, 0U);
      BOOST_CHECK_EQUAL(accounting.traffic.sent, 0U);
      BOOST_CHECK_EQUAL(accounting.held, 0U);
      BOOST_CHECK_EQUAL(store->stops.load(), 0U);
      co_await server->async_close(); co_await inner->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(second_write_storage_failure_is_terminal_and_releases_resources) {
   auto source = swarm_test::fixture{};
   auto target = swarm_test::fixture{};
   const auto bytes = swarm_test::data(2U * core::block_size);
   const auto manifest = swarm_test::describe({bytes});
   auto seed = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(source.options(manifest)))};
   auto inner = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(target.options(manifest)))};
   swarm_test::fill(*seed, manifest, {bytes});
   auto store = std::make_shared<observed_store>(inner); store->fail_second_write = true;
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto config = swarm::engine::options{}; config.peers = 1; config.attempts = 3; config.operations = 1;
   config.upload_round = 1ms;
   auto server = std::make_shared<swarm::engine>(tasks, source.cpu.get_executor(),
      std::make_shared<swarm::free_admission>(), nullptr, config);
   auto network = std::make_shared<local_network>(server);
   auto client = swarm::engine{tasks, target.cpu.get_executor(), std::make_shared<swarm::free_admission>(), network, config};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto share = co_await server->async_share(manifest, seed);
      auto download = co_await client.async_download(manifest, store);
      auto enospc = false;
      try { co_await download.async_wait(); }
      catch (const std::system_error& error) { enospc = error.code() == std::error_code{ENOSPC, std::generic_category()}; }
      catch (...) {}
      BOOST_CHECK(enospc);
      co_await download.async_close();
      BOOST_CHECK_EQUAL(store->writes.load(), 2U);
      BOOST_CHECK_EQUAL(store->write_failures.load(), 1U);
      BOOST_CHECK_EQUAL(store->writers.load(), 0U);
      BOOST_CHECK_GE(store->discards.load(), 1U);
      BOOST_CHECK_EQUAL(network->observation->connected.load(), 1U);
      BOOST_CHECK_EQUAL(network->observation->closed.load(), 1U);
      BOOST_CHECK_EQUAL(download.inspect().active_calls, 0U);
      BOOST_CHECK(!inner->snapshot().published);
      BOOST_CHECK_EQUAL(store->stops.load(), 0U);
      store->fail_second_write = false;
      auto retry = co_await client.async_download(manifest, store, {}, {}, std::chrono::steady_clock::now() + 3s);
      auto failure = std::exception_ptr{};
      try { co_await retry.async_wait(); } catch (...) { failure = std::current_exception(); }
      BOOST_CHECK(!failure); BOOST_CHECK(retry.inspect().complete);
      if (!failure) BOOST_CHECK(swarm_test::read_file(target.root / "result") == bytes);
      co_await retry.async_close(); co_await share.async_close();
      BOOST_CHECK_EQUAL(network->observation->destroyed_open.load(), 0U);
      co_await client.async_close(); co_await server->async_close();
      co_await inner->async_close(); co_await seed->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(post_connect_failure_drains_unique_capability_before_destruction) {
   auto fixture = swarm_test::fixture{};
   const auto manifest = swarm_test::describe({swarm_test::data(32)});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto network = std::make_shared<local_network>(std::make_shared<stalled_peer>()); network->fail_identity = true;
   auto config = swarm::engine::options{}; config.attempts = 1;
   auto client = swarm::engine{tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>(), network, config};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto download = co_await client.async_download(manifest, store);
      auto failed = false;
      try { co_await download.async_wait(); } catch (...) { failed = true; }
      BOOST_CHECK(failed);
      co_await download.async_close();
      BOOST_CHECK_EQUAL(network->observation->connected.load(), 1U);
      BOOST_CHECK_EQUAL(network->observation->closed.load(), 1U);
      BOOST_CHECK_EQUAL(network->observation->destroyed_open.load(), 0U);
      co_await client.async_close(); co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(sharing_registration_tracks_verified_availability_without_any_rpc) {
   auto fixture = swarm_test::fixture{};
   const auto bytes = swarm_test::data(2 * core::block_size);
   const auto manifest = swarm_test::describe({bytes}, core::block_size);
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto network = std::make_shared<registration_network>();
   auto engine = swarm::engine{tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>(), network};
   auto body = [&]() -> boost::asio::awaitable<void> {
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      auto share = co_await engine.async_share(manifest, store);
      BOOST_CHECK_EQUAL(network->observation->connected.load(), 0U);
      auto first = store->begin({0, 0});
      co_await first->async_write(0, {bytes.begin(), bytes.begin() + core::block_size});
      BOOST_CHECK_EQUAL(network->observation->connected.load(), 0U);
      co_await first->async_commit(); first.reset();
      const auto partial_registered = co_await await_observation(network->observation, &link_observation::connected, 1, deadline);
      BOOST_CHECK(partial_registered);
      BOOST_CHECK(!store->snapshot().published);
      auto second = store->begin({0, 1});
      co_await second->async_write(0, {bytes.begin() + core::block_size, bytes.end()});
      store->discard({0, 0});
      const auto withdrawn = co_await await_observation(network->observation, &link_observation::closed, 1, deadline);
      BOOST_CHECK(withdrawn);
      BOOST_CHECK_EQUAL(network->observation->connected.load(), 1U);
      co_await second->async_commit(); second.reset();
      const auto registered_again = co_await await_observation(network->observation, &link_observation::connected, 2, deadline);
      BOOST_CHECK(registered_again);
      co_await share.async_close();
      BOOST_CHECK(share.inspect().closed);
      BOOST_CHECK_EQUAL(network->observation->closed.load(), 2U);
      BOOST_CHECK_EQUAL(network->observation->destroyed_open.load(), 0U);
      // No per-operation cancellation touched this shared store or scheduler.
      auto writer = store->begin({0, 0});
      co_await writer->async_write(0, {bytes.begin(), bytes.begin() + core::block_size});
      co_await writer->async_commit(); writer.reset();
      co_await engine.async_close(); co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(validated_empty_content_can_register_metadata_only_availability) {
   auto fixture = swarm_test::fixture{};
   const auto manifest = swarm_test::describe({{}});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto network = std::make_shared<registration_network>();
   auto engine = swarm::engine{tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>(), network};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto share = co_await engine.async_share(manifest, store);
      BOOST_CHECK_EQUAL(network->observation->connected.load(), 1U);
      co_await share.async_close();
      BOOST_CHECK_EQUAL(network->observation->closed.load(), 1U);
      co_await engine.async_close(); co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(cancel_joins_late_registration_before_releasing_store_lease) {
   auto fixture = swarm_test::fixture{};
   const auto bytes = swarm_test::data(32);
   const auto manifest = swarm_test::describe({bytes});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   swarm_test::fill(*store, manifest, {bytes});
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto network = std::make_shared<registration_network>(); network->hold_registration = true;
   auto engine = swarm::engine{tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>(), network};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto share_done = std::atomic_bool{false};
      auto close_done = std::atomic_bool{false};
      std::exception_ptr share_failure;
      std::exception_ptr close_failure;
      auto changed = forge::asio::notification{};
      const auto executor = co_await boost::asio::this_coro::executor;
      boost::asio::co_spawn(executor, engine.async_share(manifest, store), [&](std::exception_ptr failure, swarm::sharing) {
         share_failure = failure; share_done = true; changed.notify();
      });
      const auto entered = co_await await_observation(network->observation, &link_observation::connected, 1,
         std::chrono::steady_clock::now() + 3s);
      BOOST_CHECK(entered);
      engine.cancel();
      boost::asio::co_spawn(executor, engine.async_close(), [&](std::exception_ptr failure) {
         close_failure = failure; close_done = true; changed.notify();
      });
      for (auto turn = 0; turn != 10; ++turn)
         co_await boost::asio::post(executor, boost::asio::use_awaitable);
      BOOST_CHECK(!share_done); BOOST_CHECK(!close_done);
      BOOST_CHECK_EQUAL(network->observation->closed.load(), 0U);
      network->release_registration.notify();
      while (true) {
         const auto epoch = changed.epoch();
         if (share_done && close_done) break;
         co_await changed.async_wait(epoch);
      }
      BOOST_CHECK(share_failure != nullptr);
      BOOST_CHECK_MESSAGE(!close_failure, failure_text(close_failure));
      BOOST_CHECK_EQUAL(network->observation->closed.load(), 1U);
      BOOST_CHECK_EQUAL(network->observation->destroyed_open.load(), 0U);
      const auto actual = co_await store->async_read({0, 0}, 0, bytes.size());
      BOOST_CHECK(actual == bytes);
      co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(registration_cancellation_drains_unique_capability) {
   auto fixture = swarm_test::fixture{};
   const auto bytes = swarm_test::data(32);
   const auto manifest = swarm_test::describe({bytes});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   swarm_test::fill(*store, manifest, {bytes});
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto network = std::make_shared<registration_network>();
   auto engine = swarm::engine{tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>(), network};
   network->after_start = [&engine] { engine.cancel(); };
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto failed = false;
      try { auto share = co_await engine.async_share(manifest, store); } catch (...) { failed = true; }
      BOOST_CHECK(failed);
      co_await engine.async_close();
      BOOST_CHECK_EQUAL(network->observation->connected.load(), 1U);
      BOOST_CHECK_EQUAL(network->observation->closed.load(), 1U);
      BOOST_CHECK_GT(network->observation->canceled.load(), 0U);
      BOOST_CHECK_EQUAL(network->observation->destroyed_open.load(), 0U);
      co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(selection_seed_reproduces_ties_and_diversifies_fresh_engines) {
   const auto manifest = swarm_test::describe({swarm_test::data(16U * core::block_size)}, core::block_size);
   auto choices = std::vector<unsigned>{};
   for (const auto seed : {1U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U}) {
      auto fixture = swarm_test::fixture{};
      auto store = std::shared_ptr<core::store>{swarm_test::run(
         forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
      auto runtime = forge::asio::runtime{};
      auto tasks = forge::asio::task::scheduler{runtime};
      auto remote = std::make_shared<selection_peer>();
      auto network = std::make_shared<local_network>(remote);
      auto config = swarm::engine::options{}; config.selection_seed = seed; config.peers = 1; config.attempts = 1;
      auto engine = swarm::engine{tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>(), network, config};
      auto body = [&]() -> boost::asio::awaitable<void> {
         auto download = co_await engine.async_download(manifest, store, {}, {}, std::chrono::steady_clock::now() + 3s);
         try { co_await download.async_wait(); } catch (...) {}
         choices.push_back(remote->first.load());
         BOOST_CHECK_LT(choices.back(), 16U);
         co_await download.async_close(); co_await engine.async_close();
         co_await store->async_close(); co_await tasks.shutdown();
      };
      forge::asio::blocking::run(runtime, body());
      const auto all = std::array<std::uint8_t, 2>{0xff, 0xff};
      const auto common = std::array<std::uint8_t, 2>{0xfe, 0xff};
      const auto now = core::transfer_time{1ms};
      auto rare = core::transfer{manifest, {}, seed};
      const auto peer = rare.add_peer(all);
      static_cast<void>(rare.add_peer(common));
      static_cast<void>(rare.set_choked(peer, false, now));
      const auto rare_pick = rare.pick(peer, now, 1);
      BOOST_REQUIRE_EQUAL(rare_pick.size(), 1U);
      BOOST_CHECK_EQUAL(rare_pick.front().piece.index, 0U);
      auto prioritized = core::transfer{manifest, {}, seed};
      const auto selected = prioritized.add_peer(all);
      static_cast<void>(prioritized.add_peer(common));
      prioritized.set_priority({0, 1}, 2);
      static_cast<void>(prioritized.set_choked(selected, false, now));
      const auto priority_pick = prioritized.pick(selected, now, 1);
      BOOST_REQUIRE_EQUAL(priority_pick.size(), 1U);
      BOOST_CHECK_EQUAL(priority_pick.front().piece.index, 1U);
   }
   BOOST_CHECK_EQUAL(choices[0], choices[1]);
   BOOST_CHECK_GT((std::set<unsigned>{choices.begin(), choices.end()}.size()), 1U);
}

} // namespace runtime_engine_test
