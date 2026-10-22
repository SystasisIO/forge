#pragma once

#include "engine_module.hxx"
#include "engine_exception.hxx"
#include "engine_memory.hxx"

#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>

namespace forge::vm::wasm::interpret::detail {

class engine_instance {
 public:
   using canceled = engine_bindings::canceled;
   using depth_exceeded = engine_bindings::depth_exceeded;
   struct fp_failure {};
   class unavailable : public std::runtime_error {
    public:
      unavailable() : std::runtime_error("WASM execution resource allocation failed") {}
   };
   class initialization_trap : public std::runtime_error {
    public:
      explicit initialization_trap(engine_exception::reason reason)
          : std::runtime_error("WASM guest initialization trapped"), reason(reason) {}
      engine_exception::reason reason;
   };
   engine_instance(std::shared_ptr<engine_module> source, std::uint32_t stack_bytes, std::stop_token stop = {},
                   void* custom_data = nullptr, std::uint32_t max_pages = UINT32_MAX / 65536,
                   std::uint32_t max_call_depth = UINT32_MAX, std::exception_ptr* host_failure = nullptr,
                   std::byte* native_stack_boundary = nullptr, engine_memory* memory = nullptr,
                   RunningMode mode = Mode_Interp, const std::exception_ptr* normal_exit = nullptr);
   ~engine_instance();
   engine_instance(const engine_instance&) = delete;
   engine_instance& operator=(const engine_instance&) = delete;
   wasm_module_inst_t get() const noexcept;
   wasm_exec_env_t env() const noexcept;
   bool invoke(const char* name, std::uint32_t argument_cells, std::span<std::uint32_t> cells,
               std::stop_token stop = {});
   bool invoke(std::uint32_t index, std::uint32_t argument_cells, std::span<std::uint32_t> cells,
               std::stop_token stop = {}, std::optional<void*> host = {});
   bool invoke_indirect(std::uint32_t index, std::uint32_t argument_cells, std::span<std::uint32_t> cells,
                        std::stop_token stop = {});
   void cancel() noexcept;
   std::exception_ptr host_failure() const noexcept;
   bool consume_normal_exit(const std::exception_ptr& expected) noexcept;

 private:
   bool is_normal_exit() const noexcept;
   bool invoke_function(wasm_function_inst_t, std::uint32_t, std::span<std::uint32_t>, std::stop_token,
                        std::optional<void*> host = {});
   std::shared_ptr<engine_module> _source;
   engine_bindings::state _host_state;
   const std::exception_ptr* _normal_exit;
   engine_memory _system_memory;
   engine_memory* _memory;
   wasm_module_inst_t _instance{};
   wasm_exec_env_t _env{};
   std::recursive_mutex _execution;
   std::size_t _nesting = 0;
};

} // namespace forge::vm::wasm::interpret::detail
