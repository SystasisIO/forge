module;

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>

export module forge.vm.wasm.optimizing.executor;

export import forge.asio.task;
export import forge.vm.wasm.optimizing.artifact;
export import forge.vm.wasm.interpret.allocator;
export import forge.vm.wasm.interpret.host_function;

export namespace forge::vm::wasm::optimizing {

struct invocation_result {
   trap_reason trap = trap_reason::none;
   std::optional<interpret::operand_stack_elem> value;
   std::exception_ptr host_error;
};

// Runtime-only owner. No compiler, parser, artifact preparation or second code
// cache is constructed here. Supplied runtime outlives mandatory shutdown;
// worker leases outlive every invoke and are released before their thread exits.
class executor {
 public:
   struct options {
      // Physical usable cap (rounded down to OS pages). Includes the fixed
      // donor-derived 4MiB guest fallback reserve; guard mappings are additional.
      std::uint64_t max_guest_stack_bytes;
      std::uint64_t min_host_stack_headroom;
   };
   template <class Registered> class binding;

   explicit executor(asio::task::scheduler& scheduler, options limits);
   ~executor();
   executor(const executor&) = delete;
   executor& operator=(const executor&) = delete;
   executor(executor&&) = delete;
   executor& operator=(executor&&) = delete;

   // Store this owner before start. If mandatory cleanup cannot prove drain,
   // preserve it and its supplied runtime, then retry shutdown; do not destroy.
   boost::asio::awaitable<void> start(std::chrono::steady_clock::time_point deadline, std::stop_token stop = {});
   boost::asio::awaitable<void> shutdown();
   void prepare_thread();
   void release_thread();

   template <class Registered> static binding<Registered> bind(artifact prepared_code);
   // Ordinary C++ host entry, including nested calls from registered callbacks
   // whose complete lifetime is whole-NULL. Never call from a private legacy
   // destructor-free guest visitor with active recovery/automatic C++ objects.
   template <class Registered>
   invocation_result invoke(const binding<Registered>& code, typename Registered::host_type_t* host,
                            std::uint32_t function_index, std::span<const interpret::operand_stack_elem> arguments,
                            interpret::wasm_allocator& memory, std::chrono::steady_clock::time_point deadline,
                            std::stop_token caller_stop = {}, std::stop_token compute_stop = {});

 private:
   struct impl;
   using host_thunk = void (*)(void*, interpret::execution_interface, std::uint32_t);
   static std::uint64_t checked_frame_bytes(const artifact& code);
   static std::uint64_t checked_initialization_frame_bytes(const artifact& code);
   invocation_result invoke_erased(const artifact& code, std::uint64_t max_frame_bytes,
                                   std::uint64_t initialization_frame_bytes, std::span<const std::uint32_t> imports,
                                   host_thunk thunk, void* host, bool requires_host, std::uint32_t function_index,
                                   std::span<const interpret::operand_stack_elem> arguments,
                                   interpret::wasm_allocator& memory, std::chrono::steady_clock::time_point deadline,
                                   std::stop_token caller_stop, std::stop_token compute_stop);
   std::shared_ptr<impl> _impl;
};

template <class Registered> class executor::binding {
 public:
   // Copies pin the SAME checked/prepared artifact mapping and resolved registry
   // ordinals. Registry registration must be complete and immutable thereafter.
   binding(const binding&) = default;
   binding(binding&&) noexcept = default;
   binding& operator=(const binding&) = default;
   binding& operator=(binding&&) noexcept = default;

 private:
   friend class executor;
   explicit binding(artifact code) : _code(std::move(code)) {
      // The checked accessor rejects unprepared state; it never maps/decode/hash.
      _max_frame_bytes = executor::checked_frame_bytes(_code);
      _initialization_frame_bytes = executor::checked_initialization_frame_bytes(_code);
      _imports.reserve(_code.image().configuration.imports.size());
      for (const auto& imported : _code.image().configuration.imports)
         _imports.push_back(Registered::resolve_import(imported.module, imported.name, imported.signature));
   }
   artifact _code;
   std::uint64_t _max_frame_bytes = 0;
   std::uint64_t _initialization_frame_bytes = 0;
   std::vector<std::uint32_t> _imports;
};

template <class Registered> executor::binding<Registered> executor::bind(artifact code) {
   return binding<Registered>{std::move(code)};
}

template <class Registered>
invocation_result executor::invoke(const binding<Registered>& code, typename Registered::host_type_t* host,
                                   std::uint32_t function_index,
                                   std::span<const interpret::operand_stack_elem> arguments,
                                   interpret::wasm_allocator& memory, std::chrono::steady_clock::time_point deadline,
                                   std::stop_token caller_stop, std::stop_token compute_stop) {
   // Only this SAME Registered instantiation can choose its private erased
   // thunk. A binding for another host type cannot authorize a void* cast.
   const auto thunk = +[](void* erased, interpret::execution_interface interface, std::uint32_t index) {
      Registered{}(static_cast<typename Registered::host_type_t*>(erased), std::move(interface), index);
   };
   return invoke_erased(code._code, code._max_frame_bytes, code._initialization_frame_bytes, code._imports, thunk, host,
                        !std::is_same_v<typename Registered::host_type_t, interpret::standalone_function_t>,
                        function_index, arguments, memory, deadline, caller_stop, compute_stop);
}

} // namespace forge::vm::wasm::optimizing
