#pragma once

#include <bit>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <wasm_export.h>

namespace forge::vm::wasm::interpret::detail {

// Private numeric ABI bridge. Registrations belong to a loaded module, never
// the process-global WAMR native-symbol registry.
class engine_bindings {
 public:
   class link_error : public std::runtime_error {
    public:
      using std::runtime_error::runtime_error;
   };
   class memory_error : public std::runtime_error {
    public:
      memory_error() : std::runtime_error("Guest memory range is invalid") {}
   };
   class canceled : public std::runtime_error {
    public:
      canceled() : std::runtime_error("WASM invocation canceled") {}
   };
   class depth_exceeded : public std::runtime_error {
    public:
      depth_exceeded() : std::runtime_error("WASM logical call depth exceeded") {}
   };

   struct state {
      void* host{};
      std::stop_token stop;
      std::exception_ptr failure;
   };

   template <typename T> static constexpr wasm_valkind_t kind() {
      using value = std::remove_cv_t<T>;
      if constexpr (std::is_pointer_v<value>)
         return WASM_I32;
      else if constexpr (std::is_same_v<value, float>)
         return WASM_F32;
      else if constexpr (std::is_same_v<value, double>)
         return WASM_F64;
      else {
         static_assert(std::is_integral_v<value> && sizeof(value) <= 8);
         if constexpr (sizeof(value) <= 4)
            return WASM_I32;
         else
            return WASM_I64;
      }
   }

   class call;
   struct function {
      explicit function(std::pmr::memory_resource* resource = std::pmr::get_default_resource());
      std::pmr::string module;
      std::pmr::string name;
      std::pmr::vector<wasm_valkind_t> parameters;
      std::optional<wasm_valkind_t> result;
      std::function<void(call&)> invoke;
      std::size_t index = 0;
      const engine_bindings* owner = nullptr;
   };

   class call {
    public:
      template <typename T> T argument(std::size_t index) const {
         static_assert(!std::is_pointer_v<T>, "Translate guest offsets with memory(), never pointer casts");
         if (index >= _function.parameters.size() || kind<T>() != _function.parameters[index])
            throw std::logic_error("Host argument does not match its registered WASM type");
         if constexpr (sizeof(T) < 4)
            return static_cast<T>(static_cast<std::uint32_t>(_arguments[index]));
         else if constexpr (sizeof(T) == 4)
            return std::bit_cast<T>(static_cast<std::uint32_t>(_arguments[index]));
         else
            return std::bit_cast<T>(_arguments[index]);
      }
      template <typename T> void result(T value) {
         static_assert(!std::is_pointer_v<T>);
         if (!_function.result || kind<T>() != *_function.result)
            throw std::logic_error("Host result does not match its registered WASM type");
         if constexpr (sizeof(T) < 4)
            _arguments[0] = static_cast<std::uint32_t>(value);
         else if constexpr (sizeof(T) == 4)
            _arguments[0] = std::bit_cast<std::uint32_t>(value);
         else
            _arguments[0] = std::bit_cast<std::uint64_t>(value);
         _returned = true;
      }
      // Views are callback-scoped. Reacquire them after any nested guest call
      // which can grow memory; do not retain them in a host object.
      std::span<char> memory(std::uint32_t offset, std::uint32_t size) const;
      std::span<char> memory() const;
      std::string_view string(std::uint32_t offset, std::uint32_t max_bytes) const;
      void* host() const noexcept;
      std::stop_token stop() const noexcept;
      void check_stop() const;
      std::size_t parameter_count() const noexcept;
      wasm_valkind_t parameter_kind(std::size_t) const;
      std::optional<wasm_valkind_t> result_kind() const noexcept;

    private:
      friend class engine_bindings;
      call(wasm_exec_env_t, state&, const function&, std::uint64_t*);
      wasm_exec_env_t _env;
      state& _state;
      const function& _function;
      std::uint64_t* _arguments;
      bool _returned = false;
   };

   template <typename R, typename... A>
   static function describe(std::string module, std::string name, R (*)(A...), void (*invoke)(call&)) {
      function value;
      value.module = module;
      value.name = name;
      value.parameters = {kind<A>()...};
      value.invoke = invoke;
      if constexpr (!std::is_void_v<R>)
         value.result = kind<R>();
      return value;
   }

   explicit engine_bindings(std::vector<function>);
   engine_bindings(std::pmr::vector<function>, const void*, void (*)(const void*, std::size_t, call&));
   engine_bindings(const engine_bindings&) = delete;
   engine_bindings& operator=(const engine_bindings&) = delete;
   // Called exactly once before publishing the module to any instance.
   void bind(wasm_module_t) const;

 private:
   static void dispatch(wasm_exec_env_t, std::uint64_t*) noexcept;
   std::pmr::vector<function> _functions;
   const void* _context = nullptr;
   void (*_invoke)(const void*, std::size_t, call&) = nullptr;
};

} // namespace forge::vm::wasm::interpret::detail
