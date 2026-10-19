module;

#include <boost/asio/awaitable.hpp>
#include <boost/compat/move_only_function.hpp>
#include <boost/process/v2/process_handle.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <mutex>
#include <span>
#include <stop_token>
#include <vector>
#include <forge/exceptions/macros.hpp>

module forge.net.tlsnotary.engine;

import forge.net.transport.stream;
import forge.asio.child_signal_registration;
import forge.asio.gate;
import forge.asio.notification;
#include "details/native_exchange.hxx"
#include "details/engine_impl.hxx"

namespace forge::net::tlsnotary {

namespace {

void validate(const source_constraints& source, const session_options& options) {
   if (source.server_name.empty() || source.server_name.size() > 253 || source.server_name.capacity() > 253 ||
       source.server_name.find('\0') != std::string::npos || source.trust_roots_der.empty() ||
       source.trust_roots_der.size() > 64 || source.trust_roots_der.capacity() > 64)
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "explicit server identity and trusted roots are required");
   auto total = std::size_t{};
   for (const auto& root : source.trust_roots_der) {
      if (root.empty() || root.capacity() > 65536 || total > 1048576 - root.capacity())
         FORGE_THROW_EXCEPTION(exceptions::limit_exceeded, "certificate roots exceed session limit");
      total += root.capacity();
   }

   if (options.id == 0 || options.timeout.count() <= 0 || options.timeout.count() > 600000 || options.max_sent == 0 ||
       options.max_sent > 65536 || options.max_received == 0 || options.max_received > 65536 ||
       options.max_sent_records == 0 || options.max_sent_records > 256 || options.max_received_records_online == 0 ||
       options.max_received_records_online > 256 || options.mpc_threads == 0 || options.mpc_threads > 8 ||
       options.max_peer_bytes == 0 || options.max_peer_bytes > 1073741824)
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "invalid session limits or identity");
   for (const auto* ranges : {&options.reveal_sent, &options.reveal_received}) {
      if (ranges->capacity() > 1024)
         FORGE_THROW_EXCEPTION(exceptions::limit_exceeded, "too many disclosure ranges");
      auto end = std::uint32_t{};
      for (const auto& range : *ranges) {
         if (range.begin >= range.end || range.begin < end || range.begin > 65536 ||
             (range.end != 0xffffffff && range.end > 65536))
            FORGE_THROW_EXCEPTION(exceptions::invalid_request, "disclosure ranges must be ordered and disjoint");
         end = range.end;
      }
   }
}
} // namespace
engine::engine(forge::asio::runtime& runtime, forge::asio::compute::executor compute, engine_options options)
    : _impl{std::make_shared<impl>(runtime, std::move(compute), std::move(options))} {}
engine::~engine() {
   request_stop();
}

boost::asio::awaitable<result> engine::prove(prover_request request) {
   validate(request.source, request.options);
   if (!request.peer.valid() || !request.server.valid() || request.request_bytes.empty() ||
       request.request_bytes.capacity() > request.options.max_sent)
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "prover requires valid raw streams and bounded request bytes");
   if (_impl->snapshot().stopped)
      FORGE_THROW_EXCEPTION(exceptions::stopped, "TLSNotary admission is stopped");
   auto options = request.options;
   const auto deadline = std::chrono::steady_clock::now() + options.timeout;
   auto make_exchange = [self = _impl, request = std::move(request), deadline]() mutable {
      return std::make_shared<detail::native_exchange>(self->runtime, self->compute, self->options.worker_path,
                                                       std::move(request), deadline);
   };
   return impl::execute(_impl, std::move(make_exchange), std::move(options), deadline);
}

boost::asio::awaitable<result> engine::verify(verifier_request request) {
   validate(request.source, request.options);
   if (!request.peer.valid())
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "verifier requires a peer stream");
   if (_impl->snapshot().stopped)
      FORGE_THROW_EXCEPTION(exceptions::stopped, "TLSNotary admission is stopped");
   auto options = request.options;
   const auto deadline = std::chrono::steady_clock::now() + options.timeout;
   auto make_exchange = [self = _impl, request = std::move(request), deadline]() mutable {
      return std::make_shared<detail::native_exchange>(self->runtime, self->compute, self->options.worker_path,
                                                       std::move(request), deadline);
   };
   return impl::execute(_impl, std::move(make_exchange), std::move(options), deadline);
}

bool engine::cancel(std::uint64_t id) noexcept {
   return _impl->cancel(id);
}

void engine::request_stop() noexcept {
   _impl->request_stop();
}

boost::asio::awaitable<void> engine::shutdown() {
   return impl::drain(_impl);
}

metrics engine::snapshot() const {
   return _impl->snapshot();
}
} // namespace forge::net::tlsnotary
