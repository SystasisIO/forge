module;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

module forge.vm.wasm.optimizing.executor;

import forge.asio.notification;
import forge.vm.wasm.interpret.backend;
import forge.vm.wasm.interpret.exceptions;

#include "details/executor_epoch.hxx"
#include "details/executor_impl.hxx"
#include "details/native_stack.hxx"

namespace forge::vm::wasm::optimizing {

executor::executor(asio::task::scheduler& scheduler, options limits)
    : _impl(std::make_shared<impl>(scheduler, limits)) {}
executor::~executor() = default;

boost::asio::awaitable<void> executor::start(std::chrono::steady_clock::time_point deadline, std::stop_token stop) {
   // Ordinary wrapper pins BEFORE returning the coroutine, not after its first
   // resume. Startup failure preserves this stored owner for mandatory retry.
   return impl::start(_impl, deadline, stop);
}

boost::asio::awaitable<void> executor::shutdown() {
   return impl::shutdown(_impl);
}

void executor::prepare_thread() {
   impl::prepare_thread(_impl);
}

void executor::release_thread() {
   const auto owner = _impl;
   owner->release_thread();
}

std::uint64_t executor::checked_frame_bytes(const artifact& code) {
   auto largest = std::uint64_t{0};
   // Cold binding reads the SAME prepared descriptors once, without copying
   // metadata or scanning them on the warm invocation/call path.
   for (const auto& function : code.functions()) {
      if (function.stack_bytes > native_stack::max_frame_bytes)
         FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable,
                               "native frame exceeds the local mapped guard bound");
      largest = std::max(largest, static_cast<std::uint64_t>(function.stack_bytes));
   }
   return largest;
}

invocation_result executor::invoke_erased(const artifact& code, std::uint64_t max_frame_bytes,
                                          std::span<const std::uint32_t> imports, host_thunk thunk, void* host,
                                          bool requires_host, std::uint32_t function_index,
                                          std::span<const interpret::operand_stack_elem> arguments,
                                          interpret::wasm_allocator& memory,
                                          std::chrono::steady_clock::time_point deadline, std::stop_token caller_stop,
                                          std::stop_token compute_stop) {
   // Pin spans ALL run state, activation destruction/retirement and the final
   // result/exception cleanup. The completion gate proves no runtime-notify tail
   // survives shutdown; this pin additionally protects the implementation itself.
   const auto owner = _impl;
   return owner->invoke(code, max_frame_bytes, imports, thunk, host, requires_host, function_index, arguments, memory,
                        deadline, caller_stop, compute_stop);
}

} // namespace forge::vm::wasm::optimizing
