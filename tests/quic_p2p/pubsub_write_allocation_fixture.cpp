#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <future>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detail/thread_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include "libp2p_identity_fixture.hxx"

import forge.asio.blocking;
import forge.asio.runtime;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.identify;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.transport.stream;

#include "pubsub_write_allocation_fixture.hxx"

#if !defined(__unix__) && !defined(__APPLE__)
#error "This isolated allocation fixture requires POSIX allocation semantics."
#endif

// These replacements belong only to the isolated test executable. The probe is
// thread-local and one-shot. The native trace probe arms only after completed I/O.
void* operator new(std::size_t size) {
   using fixture = forge::tests::p2p::pubsub_write_allocation_fixture;
   if (fixture::reject_allocation(fixture::allocator::scalar_new, size)) {
      throw std::bad_alloc{};
   }
   for (;;) {
      if (auto* value = std::malloc(size == 0 ? 1 : size)) { return value; }
      const auto handler = std::get_new_handler();
      if (!handler) { throw std::bad_alloc{}; }
      handler();
   }
}

void operator delete(void* value) noexcept {
   std::free(value);
}

void operator delete(void* value, std::size_t) noexcept {
   std::free(value);
}

extern "C" void* aligned_alloc(std::size_t alignment, std::size_t size) noexcept {
   using fixture = forge::tests::p2p::pubsub_write_allocation_fixture;
   if (fixture::reject_allocation(fixture::allocator::aligned_alloc, size)) {
      errno = ENOMEM;
      return nullptr; // Asio's actual frame allocator constructs the original std::bad_alloc.
   }
   if (alignment == 0 || (alignment & (alignment - 1)) != 0 || size % alignment != 0) {
      errno = EINVAL;
      return nullptr;
   }
   auto* value = static_cast<void*>(nullptr);
   const auto native_alignment = alignment < sizeof(void*) ? sizeof(void*) : alignment;
   const auto error = ::posix_memalign(&value, native_alignment, size == 0 ? native_alignment : size);
   if (error != 0) { errno = error; return nullptr; }
   return value;
}

namespace forge::tests::p2p {

namespace transport = forge::net::transport;

thread_local bool pubsub_write_allocation_fixture::_armed = false;
thread_local std::size_t pubsub_write_allocation_fixture::_minimum_size = 0;
thread_local pubsub_write_allocation_fixture::allocator pubsub_write_allocation_fixture::_rejected_by = allocator::none;
thread_local pubsub_write_allocation_fixture::trace_result* pubsub_write_allocation_fixture::_trace_target = nullptr;

class pubsub_write_allocation_fixture::write_model final : public transport::detail::stream_concept {
 public:
   [[nodiscard]] bool valid() const noexcept override;
   [[nodiscard]] std::int64_t id() const noexcept override;
   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> bytes) override;
   boost::asio::awaitable<void> async_write_chunk(transport::chunk bytes) override;
   boost::asio::awaitable<void> async_write_frame(std::span<const std::uint8_t> bytes) override;
   boost::asio::awaitable<void> async_write_frame_chunk(transport::chunk bytes) override;
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read() override;
   boost::asio::awaitable<void> async_close() override;
   void cancel() override;

   std::size_t factories = 0;
   std::size_t bodies = 0;
   factory executed = factory::chunk;
   std::vector<std::uint8_t> delivered;

 private:
   [[gnu::noinline]] boost::asio::awaitable<void> write_owned(transport::chunk bytes, factory selected);
   bool _open = true;
};

pubsub_write_allocation_fixture::allocation_scope::allocation_scope(std::size_t minimum_size) noexcept {
   _rejected_by = allocator::none;
   _trace_target = nullptr;
   _minimum_size = minimum_size;
   _armed = true;
}

pubsub_write_allocation_fixture::allocation_scope::~allocation_scope() {
   _armed = false;
   _trace_target = nullptr;
   _minimum_size = 0;
}

pubsub_write_allocation_fixture::allocator
pubsub_write_allocation_fixture::allocation_scope::rejected_by() const noexcept {
   return _rejected_by;
}

void pubsub_write_allocation_fixture::arm_trace_allocation(trace_result& observed) noexcept {
   _rejected_by = allocator::none;
   _trace_target = &observed;
   _minimum_size = 0;
   _armed = true;
}

bool pubsub_write_allocation_fixture::reject_allocation(allocator source, std::size_t size) noexcept {
   if (!_armed || size < _minimum_size) { return false; }
   _armed = false;
   _rejected_by = source;
   if (_trace_target) {
      _trace_target->rejected_by = source;
      _trace_target = nullptr;
   }
   return true;
}

bool pubsub_write_allocation_fixture::write_model::valid() const noexcept { return _open; }
std::int64_t pubsub_write_allocation_fixture::write_model::id() const noexcept { return 7; }

boost::asio::awaitable<void>
pubsub_write_allocation_fixture::write_model::async_write(std::span<const std::uint8_t> bytes) {
   return async_write_chunk(transport::chunk{bytes});
}

boost::asio::awaitable<void>
pubsub_write_allocation_fixture::write_model::async_write_chunk(transport::chunk bytes) {
   ++factories;
   return write_owned(std::move(bytes), factory::chunk);
}

boost::asio::awaitable<void>
pubsub_write_allocation_fixture::write_model::async_write_frame(std::span<const std::uint8_t> bytes) {
   return async_write_frame_chunk(transport::chunk{bytes});
}

boost::asio::awaitable<void>
pubsub_write_allocation_fixture::write_model::async_write_frame_chunk(transport::chunk bytes) {
   ++factories;
   return write_owned(std::move(bytes), factory::frame_chunk);
}

boost::asio::awaitable<void>
pubsub_write_allocation_fixture::write_model::write_owned(transport::chunk bytes, factory selected) {
   ++bodies;
   executed = selected;
   delivered = bytes.to_vector();
   co_return;
}

boost::asio::awaitable<std::vector<std::uint8_t>> pubsub_write_allocation_fixture::write_model::async_read() {
   co_return std::vector<std::uint8_t>{};
}

boost::asio::awaitable<void> pubsub_write_allocation_fixture::write_model::async_close() {
   _open = false;
   co_return;
}

void pubsub_write_allocation_fixture::write_model::cancel() { _open = false; }

pubsub_write_allocation_fixture::result pubsub_write_allocation_fixture::observe(factory selected) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto model = std::make_shared<write_model>();
   auto stream = forge::net::p2p::stream{transport::detail::stream_access::make(model)};
   const auto payload = std::array<std::uint8_t, 4>{0, 1, 0, 255};
   auto input = transport::chunk{std::span<const std::uint8_t>{payload}};
   auto retry_input = input;
   auto observed = result{};
   auto unexpected = std::exception_ptr{};
   auto worker = std::jthread{[&] {
      try {
         observed.ordinary_thread = boost::asio::detail::thread_context::top_of_thread_call_stack() == nullptr;
         auto unexecuted = boost::asio::awaitable<void>{};
         try {
            const auto injection = allocation_scope{};
            // All facade/model/chunk inputs already exist. Chunk moves do not allocate.
            unexecuted = selected == factory::chunk ? stream.async_write(std::move(input))
                                                    : stream.async_write_frame(std::move(input));
         } catch (...) {
            observed.construction_error = std::current_exception();
         }
         observed.rejected_by = _rejected_by;
         observed.factories_after_failure = model->factories;
         observed.bodies_after_failure = model->bodies;
         unexecuted = {}; // A missed injection must not execute a body or contaminate recovery.
         auto recovery = selected == factory::chunk ? stream.async_write(std::move(retry_input))
                                                   : stream.async_write_frame(std::move(retry_input));
         observed.factories_before_recovery_execution = model->factories;
         observed.bodies_before_recovery_execution = model->bodies;
         forge::asio::blocking::run(runtime, std::move(recovery));
         observed.factories_after_recovery = model->factories;
         observed.bodies_after_recovery = model->bodies;
         observed.payload_preserved = std::ranges::equal(model->delivered, payload);
         observed.selected_factory_preserved = model->executed == selected;
      } catch (...) {
         unexpected = std::current_exception();
      }
   }};
   worker.join();
   if (unexpected) { std::rethrow_exception(unexpected); }
   return observed;
}

pubsub_write_allocation_fixture::trace_result
pubsub_write_allocation_fixture::observe_native_trace(std::uint64_t (*outbound_bytes)(const void*)) {
   namespace p2p = forge::net::p2p;
   using namespace std::chrono_literals;
   auto observed = trace_result{};
   auto sender_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto receiver_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto sender_identity = make_identity_fixture("trace-copy-sender");
   const auto receiver_identity = make_identity_fixture("trace-copy-receiver");
   const auto subject = p2p::pubsub::topic{"forge.pubsub.trace.copy"};
   const auto payload = std::vector<std::uint8_t>{'t', 'r', 'a', 'c', 'e'};
   auto announced = std::promise<void>{};
   auto announced_future = announced.get_future();
   auto frames = std::promise<void>{};
   auto frames_future = frames.get_future();
   auto delivered = std::promise<void>{};
   auto delivered_future = delivered.get_future();
   auto frame_count = std::uint64_t{};
   auto callback_count = std::uint64_t{};
   auto sender_session = std::uint64_t{};
   auto sender_stream = std::int64_t{-1};
   auto sender_generation = std::uint64_t{};
   auto recovery_session = std::uint64_t{};
   auto recovery_stream = std::int64_t{-1};
   auto recovery_generation = std::uint64_t{};
   auto receiver_session = std::uint64_t{};
   auto receiver_stream = std::int64_t{-1};
   auto receiver_generation = std::uint64_t{};
   auto committed_id = std::vector<std::uint8_t>{};
   auto committed_stream = std::int64_t{-1};
   auto committed_session = std::uint64_t{};
   auto committed_generation = std::uint64_t{};
   auto announced_once = false;
   auto delivered_once = false;
   auto arm = std::atomic_bool{false};
   auto receiver_peer = p2p::peer_id{};
   auto sender_options = p2p::node::options{.certificate_pem = sender_identity.certificate_pem,
      .private_key_pem = sender_identity.private_key_pem, .capabilities = {.bits = p2p::capabilities::pubsub}};
   auto receiver_options = p2p::node::options{.certificate_pem = receiver_identity.certificate_pem,
      .private_key_pem = receiver_identity.private_key_pem, .capabilities = {.bits = p2p::capabilities::pubsub}};
   for (auto* options : {&sender_options, &receiver_options}) {
      options->peer_state.persistence = p2p::peer_store::make_memory_persistence();
      options->dht_profiles.clear();
      options->limits.pubsub.limits.heartbeat_initial_delay = 60s;
      options->limits.pubsub.flood_publish = true;
   }
   sender_options.limits.pubsub.tracer = [&](const auto& event) {
      if (event.kind == p2p::pubsub::trace_kind::rpc_read && !announced_once) {
         const auto rpc = p2p::pubsub::codec::decode(event.framed_rpc);
         if (std::ranges::any_of(rpc.subscriptions, [&](const auto& value) {
                return value.subscribe && value.subject == subject;
             })) {
            announced_once = true;
            announced.set_value();
         }
      }
      if (event.kind != p2p::pubsub::trace_kind::rpc_write) { return; }
      ++callback_count;
      const auto rpc = p2p::pubsub::codec::decode(event.framed_rpc);
      if (!arm.exchange(false, std::memory_order_acq_rel)) {
         if (std::ranges::any_of(rpc.messages, [&](const auto& value) {
                return value.subject == subject && value.data == payload;
             })) {
            recovery_session = event.session_id;
            recovery_stream = event.stream_id;
            recovery_generation = event.generation;
         }
         return;
      }
      observed.snapshot_observed = event.peer == receiver_peer && event.session_id != 0 &&
          event.stream_id >= 0 && event.generation != 0 && rpc.messages.empty() &&
          std::ranges::any_of(rpc.subscriptions, [&](const auto& value) {
             return value.subscribe && value.subject == subject;
          });
      sender_session = event.session_id;
      sender_stream = event.stream_id;
      sender_generation = event.generation;
      // The next synchronous event starts with a copy of this real, non-SSO Peer ID.
      arm_trace_allocation(observed);
   };
   receiver_options.limits.pubsub.tracer = [&](const auto& event) {
      if (event.kind == p2p::pubsub::trace_kind::rpc_read) {
         const auto rpc = p2p::pubsub::codec::decode(event.framed_rpc);
         if (std::ranges::any_of(rpc.subscriptions, [&](const auto& value) {
                return value.subscribe && value.subject == subject;
             })) {
            if (frame_count == 0) {
               receiver_session = event.session_id;
               receiver_stream = event.stream_id;
               receiver_generation = event.generation;
            } else if (event.session_id != receiver_session || event.stream_id != receiver_stream ||
                       event.generation != receiver_generation) {
               throw std::runtime_error{"native SUBSCRIBE frames changed receiver stream"};
            }
            if (++frame_count == 2U) { frames.set_value(); }
         }
      }
      if (event.kind == p2p::pubsub::trace_kind::validation_committed && event.subject == subject &&
          event.result == p2p::pubsub::validation_result::accept && std::ranges::equal(event.data, payload)) {
         committed_id.assign(event.message_id.begin(), event.message_id.end());
         committed_stream = event.stream_id;
         committed_session = event.session_id;
         committed_generation = event.generation;
      }
      if (event.kind == p2p::pubsub::trace_kind::delivery && event.subject == subject &&
          std::ranges::equal(event.data, payload) && !delivered_once) {
         delivered_once = true;
         delivered.set_value();
      }
   };
   auto sender = p2p::node{sender_runtime, std::move(sender_options)};
   auto receiver = p2p::node{receiver_runtime, std::move(receiver_options)};
   receiver_peer = receiver.local_peer();
   const auto cleanup_owners = [&](void*) noexcept {
      try {
         auto sender_stop = boost::asio::co_spawn(sender_runtime.context(), sender.async_stop(), boost::asio::use_future);
         auto receiver_stop = boost::asio::co_spawn(receiver_runtime.context(), receiver.async_stop(), boost::asio::use_future);
         const auto deadline = std::chrono::steady_clock::now() + 5s;
         if (sender_stop.wait_until(deadline) != std::future_status::ready ||
             receiver_stop.wait_until(deadline) != std::future_status::ready) {
            throw std::runtime_error{"native trace owners did not join"};
         }
         sender_stop.get();
         receiver_stop.get();
      } catch (...) {
         std::fputs("FATAL: native trace allocation owners did not join\n", stderr);
         std::_Exit(86);
      }
   };
   auto cleanup = std::unique_ptr<void, decltype(cleanup_owners)>{&sender, cleanup_owners};
   const auto run = [](forge::asio::runtime& runtime, auto operation) {
      auto future = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
      if (future.wait_for(5s) != std::future_status::ready) {
         throw std::runtime_error{"native trace operation exceeded its budget"};
      }
      return future.get();
   };
   try {
      run(receiver_runtime, receiver.async_listen(p2p::parse_endpoint("/ip4/127.0.0.1/tcp/0")));
      static_cast<void>(run(receiver_runtime, receiver.async_subscribe(subject,
          [](auto) -> boost::asio::awaitable<p2p::pubsub::validation_result> {
             co_return p2p::pubsub::validation_result::accept;
          })));
      const auto connection = run(sender_runtime, sender.async_connect(receiver.local_endpoints().front(),
          p2p::node::connect_options{.expected_peer = receiver_peer, .allow_relay = false, .timeout = 3s,
                                    .allow_hole_punch = false}));
      observed.authenticated = connection.remote_peer == receiver_peer && std::ranges::any_of(
          sender.diagnostics().sessions, [&](const auto& session) {
             return session.id == connection.id && !session.closed && session.remote_peer == receiver_peer &&
                    session.authentication != p2p::peer_authentication::unverified;
          });
      observed.heap_peer_id = receiver_peer.value.size() > std::string{}.capacity();
      if (announced_future.wait_for(5s) != std::future_status::ready) {
         throw std::runtime_error{"native receiver SUBSCRIBE was not observed"};
      }
      announced_future.get();
      const auto receiver_sessions = receiver.diagnostics().sessions;
      const auto receiver_owner = std::ranges::find_if(receiver_sessions, [&](const auto& session) {
         return !session.closed && session.remote_peer == sender.local_peer();
      });
      if (receiver_owner == receiver_sessions.end()) {
         throw std::runtime_error{"native trace receiver session was not admitted"};
      }
      const auto receiver_id = receiver_owner->id;
      const auto quiescent_memory = [&]() -> boost::asio::awaitable<std::uint64_t> {
         const auto deadline = std::chrono::steady_clock::now() + 3s;
         auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
         for (;;) {
            const auto left = sender.diagnostics();
            const auto right = receiver.diagnostics();
            const auto left_owner = std::ranges::find_if(left.sessions,
                [&](const auto& session) { return session.id == connection.id; });
            const auto right_owner = std::ranges::find_if(right.sessions,
                [&](const auto& session) { return session.id == receiver_id; });
            if (left_owner == left.sessions.end() || right_owner == right.sessions.end() ||
                left_owner->closed || right_owner->closed || left_owner->remote_peer != receiver_peer ||
                right_owner->remote_peer != sender.local_peer() ||
                left_owner->authentication == p2p::peer_authentication::unverified ||
                right_owner->authentication == p2p::peer_authentication::unverified ||
                !left_owner->identify_error.empty() || !right_owner->identify_error.empty() ||
                left_owner->identify_state == p2p::identify::state::failed ||
                right_owner->identify_state == p2p::identify::state::failed) {
               throw std::runtime_error{"native trace authenticated Identify owner changed or failed"};
            }
            // This aggregate includes concurrent Identify decode reservations, not just PubSub writes.
            if (left_owner->identify_state == p2p::identify::state::identified &&
                right_owner->identify_state == p2p::identify::state::identified &&
                left.resources.streams.memory == 0U && right.resources.streams.memory == 0U) {
               co_return left.resources.streams.memory;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
               throw std::runtime_error{"native trace stream memory did not converge before stop"};
            }
            timer.expires_at(std::min(deadline, now + 1ms));
            co_await timer.async_wait(boost::asio::use_awaitable);
         }
      };
      const auto run_quiescence = [&] {
         auto joined = boost::asio::co_spawn(sender_runtime.context(), quiescent_memory(), boost::asio::use_future);
         if (joined.wait_for(5s) != std::future_status::ready) {
            // An unresolved fixture worker still borrows this scope; never unwind its captures.
            std::fputs("FATAL: native trace memory predicate did not join\n", stderr);
            std::_Exit(86);
         }
         return joined.get();
      };
      static_cast<void>(run_quiescence());
      const auto before = sender.metrics();
      arm.store(true, std::memory_order_release);
      static_cast<void>(run(sender_runtime, sender.async_subscribe(subject,
          [](auto) -> boost::asio::awaitable<p2p::pubsub::validation_result> {
             co_return p2p::pubsub::validation_result::accept;
          })));
      observed.subscription_succeeded = true;
      observed.write_callbacks = callback_count;
      observed.trace_failures = sender.pubsub_snapshot().trace_failures;
      observed.outbound_bytes_after_failure = outbound_bytes(&sender);
      if (frames_future.wait_for(5s) != std::future_status::ready) {
         throw std::runtime_error{"completed native snapshot and RPC were not both received"};
      }
      frames_future.get();
      observed.native_subscription_frames = frame_count;
      observed.stream_memory_after_failure = run_quiescence();
      const auto published = run(sender_runtime, sender.async_publish(subject, payload));
      observed.outbound_bytes_after_recovery = outbound_bytes(&sender);
      if (delivered_future.wait_for(5s) != std::future_status::ready) {
         throw std::runtime_error{"native trace recovery message was not delivered"};
      }
      delivered_future.get();
      observed.recovery_delivered = std::ranges::equal(committed_id, p2p::pubsub::codec::message_id(published)) &&
          committed_stream == receiver_stream && committed_session == receiver_session && committed_generation != 0 &&
          !published.signature.empty();
      observed.same_native_stream = sender_session == connection.id && sender_stream == receiver_stream &&
          sender_generation != 0 && receiver_session != 0 && receiver_generation != 0 &&
          recovery_session == sender_session && recovery_stream == sender_stream && recovery_generation == sender_generation &&
          sender.metrics().sessions_opened == before.sessions_opened;
      observed.trace_failures = sender.pubsub_snapshot().trace_failures;
      observed.stream_memory_after_recovery = run_quiescence();
      observed.messages_published = sender.metrics().pubsub_messages_published;
      observed.messages_delivered = receiver.metrics().pubsub_messages_delivered;
      const auto after = sender.metrics();
      observed.native_failures_unchanged = after.active_sessions == before.active_sessions &&
          after.protocol_rejections == before.protocol_rejections && after.direct_failures == before.direct_failures &&
          after.backpressure_rejections == before.backpressure_rejections &&
          after.pubsub_invalid_messages == before.pubsub_invalid_messages &&
          sender.diagnostics().resources.denied_memory == 0U;
   } catch (...) {
      observed.native_error = std::current_exception();
   }
   cleanup.reset();
   return observed;
}

} // namespace forge::tests::p2p
