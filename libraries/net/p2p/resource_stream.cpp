module;

#include <atomic>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

module forge.net.p2p.node;

import forge.net.p2p.exceptions;
import forge.net.p2p.resource_manager;
import forge.net.transport.frame;
import forge.net.transport.stream;

#include "details/resource_stream.hxx"
#include "details/resource_service.hxx"

namespace forge::net::p2p::detail {
namespace {

[[nodiscard]] std::shared_ptr<void> memory_lifetime(resource_manager::memory_reservation reservation) {
   return std::make_shared<resource_manager::memory_reservation>(std::move(reservation));
}

[[nodiscard]] std::uint64_t framed_size(std::size_t payload_size) noexcept {
   constexpr auto frame_header_size = std::uint64_t{4};
   if (payload_size > (std::numeric_limits<std::uint64_t>::max)() - frame_header_size) {
      return (std::numeric_limits<std::uint64_t>::max)();
   }
   return static_cast<std::uint64_t>(payload_size) + frame_header_size;
}

} // namespace

stream_admission_handler::stream_admission_handler(admitted_callback admitted, callback commit)
    : admitted_(std::move(admitted)), commit_(std::move(commit)) {}

stream_admission_handler::operator bool() const noexcept {
   return static_cast<bool>(admitted_);
}

void stream_admission_handler::operator()(const std::shared_ptr<resource_stream>& resource) const {
   if (admitted_) {
      admitted_(resource);
   }
}

void stream_admission_handler::commit() const {
   if (commit_) {
      commit_();
   }
}

resource_stream::resource_stream(resource_manager::stream_reservation reservation)
    : reservation_(std::move(reservation)) {}

resource_stream::~resource_stream() noexcept {
   if (claim_terminal_owner()) {
      stream_.request_cancel();
      release_terminal_owner();
   }
}

void resource_stream::attach(forge::net::transport::stream stream) noexcept {
   stream_ = std::move(stream);
}

bool resource_stream::valid() const noexcept {
   return stream_.valid() && reservation_.active();
}

std::int64_t resource_stream::id() const noexcept {
   return stream_.id();
}

bool resource_stream::is_memory_rejection(const std::exception& error) noexcept {
   return dynamic_cast<const memory_rejected*>(&error) != nullptr;
}

resource_stream::preparation_failed::preparation_failed(std::exception_ptr failure) noexcept
    : failure_(std::move(failure)) {}

std::exception_ptr resource_stream::preparation_failed::failure() const noexcept {
   return failure_;
}

std::exception_ptr resource_stream::preparation_failure(const std::exception& error) noexcept {
   const auto prepared = dynamic_cast<const preparation_failed*>(&error);
   return prepared ? prepared->failure() : std::exception_ptr{};
}

resource_manager::stream_reservation::bind_result resource_stream::bind_protocol(const protocol_id& value) noexcept {
   return reservation_.bind_protocol(value);
}

resource_manager::stream_reservation::bind_result
resource_stream::bind_service_for_protocol(const protocol_id& value, bool dht_profile) noexcept {
   try {
      const auto service = resource_service_id(value, dht_profile);
      return reservation_.bind_service(service);
   } catch (...) {
      return resource_manager::stream_reservation::bind_result::runtime_failure;
   }
}

std::optional<resource_manager::memory_reservation>
resource_stream::reserve_memory(std::uint64_t bytes, resource_manager::memory_priority priority) noexcept {
   return reservation_.reserve_memory(bytes, priority);
}

boost::asio::awaitable<void> resource_stream::async_write(std::span<const std::uint8_t> bytes) {
   auto owned = forge::net::transport::chunk{};
   // Awaitable creation can allocate too; only execution of the lower body may have native effects.
   auto write = boost::asio::awaitable<void>{};
   try {
      auto admitted = reservation_.reserve_memory(bytes.size(), resource_manager::memory_priority::high);
      if (!admitted) {
         FORGE_THROW_EXCEPTION(memory_rejected, "P2P node queued-byte budget exhausted");
      }
      owned = forge::net::transport::chunk{bytes};
      forge::net::transport::detail::chunk_access::attach_lifetime(owned, memory_lifetime(std::move(*admitted)));
      write = stream_.async_write(std::move(owned));
   } catch (const std::bad_alloc&) {
      throw preparation_failed{std::current_exception()};
   }
   co_await std::move(write);
}

boost::asio::awaitable<void> resource_stream::async_write_chunk(forge::net::transport::chunk bytes) {
   auto write = boost::asio::awaitable<void>{};
   try {
      auto admitted = reservation_.reserve_memory(bytes.size(), resource_manager::memory_priority::high);
      if (!admitted) {
         FORGE_THROW_EXCEPTION(memory_rejected, "P2P node queued-byte budget exhausted");
      }
      forge::net::transport::detail::chunk_access::attach_lifetime(bytes, memory_lifetime(std::move(*admitted)));
      write = stream_.async_write(std::move(bytes));
   } catch (const std::bad_alloc&) {
      throw preparation_failed{std::current_exception()};
   }
   co_await std::move(write);
}

boost::asio::awaitable<void> resource_stream::async_write_frame(std::span<const std::uint8_t> bytes) {
   auto encoded = forge::net::transport::chunk{};
   auto write = boost::asio::awaitable<void>{};
   try {
      auto admitted = reservation_.reserve_memory(framed_size(bytes.size()), resource_manager::memory_priority::always);
      if (!admitted) {
         FORGE_THROW_EXCEPTION(memory_rejected, "P2P node queued-byte budget exhausted");
      }
      encoded = forge::net::transport::chunk{forge::net::transport::encode_frame(bytes)};
      forge::net::transport::detail::chunk_access::attach_lifetime(encoded, memory_lifetime(std::move(*admitted)));
      write = stream_.async_write(std::move(encoded));
   } catch (const std::bad_alloc&) {
      throw preparation_failed{std::current_exception()};
   }
   co_await std::move(write);
}

boost::asio::awaitable<void> resource_stream::async_write_frame_chunk(forge::net::transport::chunk bytes) {
   auto encoded = forge::net::transport::chunk{};
   auto write = boost::asio::awaitable<void>{};
   try {
      auto admitted = reservation_.reserve_memory(framed_size(bytes.size()), resource_manager::memory_priority::always);
      if (!admitted) {
         FORGE_THROW_EXCEPTION(memory_rejected, "P2P node queued-byte budget exhausted");
      }
      auto [payload, source_lifetime] = forge::net::transport::detail::chunk_access::consume(std::move(bytes));
      encoded = forge::net::transport::chunk{forge::net::transport::encode_frame(payload)};
      forge::net::transport::detail::chunk_access::attach_lifetime(encoded, std::move(source_lifetime));
      forge::net::transport::detail::chunk_access::attach_lifetime(encoded, memory_lifetime(std::move(*admitted)));
      write = stream_.async_write(std::move(encoded));
   } catch (const std::bad_alloc&) {
      throw preparation_failed{std::current_exception()};
   }
   co_await std::move(write);
}

boost::asio::awaitable<std::vector<std::uint8_t>> resource_stream::async_read() {
   co_return co_await stream_.async_read();
}

boost::asio::awaitable<forge::net::transport::chunk> resource_stream::async_read_chunk() {
   co_return co_await stream_.async_read_chunk();
}

boost::asio::awaitable<void> resource_stream::async_close() {
   if (!claim_terminal_owner()) {
      co_return;
   }
   auto release = std::unique_ptr<resource_stream, void (*)(resource_stream*)>{
       this, [](resource_stream* value) noexcept { value->release_terminal_owner(); }};
   try {
      co_await stream_.async_close();
   } catch (...) {
      // A failed graceful close still owns terminal cleanup. request_cancel()
      // is noexcept and cannot replace the original transport failure.
      stream_.request_cancel();
      throw;
   }
}

void resource_stream::cancel() {
   if (!claim_terminal_owner()) {
      return;
   }
   auto release = std::unique_ptr<resource_stream, void (*)(resource_stream*)>{
       this, [](resource_stream* value) noexcept { value->release_terminal_owner(); }};
   stream_.cancel();
}

void resource_stream::request_cancel() noexcept {
   auto observed = terminal_.load(std::memory_order_acquire);
   while (observed == terminal_state::active || observed == terminal_state::owner) {
      const auto requested = observed == terminal_state::active ? terminal_state::cancel_requested
                                                                : terminal_state::owner_cancel_requested;
      if (terminal_.compare_exchange_weak(observed, requested, std::memory_order_acq_rel, std::memory_order_acquire)) {
         stream_.request_cancel();
         return;
      }
   }
}

bool resource_stream::claim_terminal_owner() noexcept {
   auto observed = terminal_.load(std::memory_order_acquire);
   while (observed == terminal_state::active || observed == terminal_state::cancel_requested) {
      const auto owner =
          observed == terminal_state::active ? terminal_state::owner : terminal_state::owner_cancel_requested;
      if (terminal_.compare_exchange_weak(observed, owner, std::memory_order_acq_rel, std::memory_order_acquire)) {
         return true;
      }
   }
   return false;
}

void resource_stream::release_terminal_owner() noexcept {
   reservation_.release();
   terminal_.store(terminal_state::released, std::memory_order_release);
}

std::pair<forge::net::transport::stream, std::shared_ptr<resource_stream>>
prepare_resource_stream(resource_manager::stream_reservation reservation) {
   auto resource = std::make_shared<resource_stream>(std::move(reservation));
   auto weak = std::weak_ptr<resource_stream>{resource};
   auto stream = forge::net::transport::detail::stream_access::make_cancelable(
       resource,
       [weak = std::move(weak)]() noexcept {
          if (auto value = weak.lock()) {
             value->request_cancel();
          }
       },
       []() noexcept {
          // The dispatcher still owns resource and performs graceful close.
          // An escaped facade remains protected by resource_stream's destructor.
       });
   return {std::move(stream), std::move(resource)};
}

boost::asio::awaitable<void> async_close_unescaped(const std::shared_ptr<resource_stream>& resource) {
   // The dispatcher keeps one owner so normal handler completion can send a
   // graceful close. A second owner means the facade escaped the handler.
   if (resource && resource.use_count() == 1) {
      co_await resource->async_close();
   }
}

} // namespace forge::net::p2p::detail
