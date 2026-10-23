module;

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fcntl.h>
#include <forge/exceptions/macros.hpp>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <spawn.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/process/v2/process_handle.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/system/system_error.hpp>

module forge.expression.program;

import forge.expression.exceptions;
import forge.asio.blocking;
import forge.asio.child_signal_registration;
import forge.asio.gate;
import forge.asio.notification;

#include "details/runtime_impl.hxx"

namespace forge::expression {

runtime::runtime(options configuration) try : _impl(std::make_shared<impl>(std::move(configuration), nullptr)) {
   _impl->initialize_sync();
} catch (const std::bad_alloc&) {
   FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression runtime allocation failed");
}

runtime::runtime(forge::asio::runtime& executor, options configuration) try
    : _impl(std::make_shared<impl>(std::move(configuration), &executor)) {
   _impl->initialize_sync();
} catch (const std::bad_alloc&) {
   FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression runtime allocation failed");
}

runtime::~runtime() {
   if (_impl)
      _impl->shutdown_sync();
}
runtime::runtime(runtime&&) noexcept = default;
runtime& runtime::operator=(runtime&& other) noexcept {
   if (this != &other) {
      if (_impl)
         _impl->shutdown_sync();
      _impl = std::move(other._impl);
   }
   return *this;
}

worker_protocol::response runtime::exchange(const std::shared_ptr<impl>& owner, worker_protocol::request request,
                                            const forge::variant_object& values, std::uint64_t* accepted_program) {
   if (!owner)
      FORGE_THROW_EXCEPTION(exceptions::worker_unavailable, "expression runtime is unavailable");
   owner->require_host();
   return forge::asio::blocking::run(owner->executor,
                                     impl::submit(owner, std::move(request), values, accepted_program));
}

boost::asio::awaitable<worker_protocol::response> runtime::async_exchange(std::shared_ptr<impl> owner,
                                                                          worker_protocol::request request,
                                                                          forge::variant_object values,
                                                                          std::uint64_t* accepted_program) {
   co_return co_await impl::submit(std::move(owner), std::move(request), values, accepted_program);
}

void runtime::release(const std::shared_ptr<impl>& owner, std::uint64_t id) noexcept {
   if (owner && id != 0)
      owner->release(id);
}

memory_usage runtime::memory() const {
   worker_protocol::request request;
   request.op = worker_protocol::operation::hello;
   const auto reply = exchange(_impl, std::move(request));
   return {reply.heap_cap, reply.heap_live, reply.heap_peak, reply.heap_failures};
}

boost::asio::awaitable<memory_usage> runtime::read_memory(std::shared_ptr<impl> owner) {
   worker_protocol::request request;
   request.op = worker_protocol::operation::hello;
   const auto reply = co_await async_exchange(std::move(owner), std::move(request));
   co_return memory_usage{reply.heap_cap, reply.heap_live, reply.heap_peak, reply.heap_failures};
}

boost::asio::awaitable<memory_usage> runtime::memory_async() const {
   return read_memory(_impl);
}

boost::asio::awaitable<void> runtime::shutdown_owned(std::shared_ptr<impl> owner) {
   if (owner)
      co_await boost::asio::co_spawn(
          owner->strand, owner->shutdown(),
          boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
}

boost::asio::awaitable<void> runtime::shutdown_async() {
   return shutdown_owned(_impl);
}

} // namespace forge::expression
