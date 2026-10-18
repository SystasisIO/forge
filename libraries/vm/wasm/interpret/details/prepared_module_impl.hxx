#pragma once

#include "engine_module.hxx"
#include "engine_limits.hxx"
#include "engine_load_budget.hxx"
#include <memory_resource>

namespace forge::vm::wasm::interpret {

struct prepared_module::impl {
   class resource final : public std::pmr::memory_resource {
    public:
      explicit resource(const std::optional<preparation>&);

    private:
      void* do_allocate(std::size_t, std::size_t) override;
      void do_deallocate(void*, std::size_t, std::size_t) override;
      bool do_is_equal(const std::pmr::memory_resource&) const noexcept override;
      std::optional<preparation> _preparation;
   };
   impl(std::span<const std::uint8_t>, const options&, std::span<host_binding>, purpose = purpose::execution,
        std::optional<preparation> = {});
   impl(std::span<const std::uint8_t>, std::span<const std::uint8_t>, const options&, std::span<host_binding>,
        std::optional<preparation> = {});
   ~impl();
   static void invoke(const void*, std::size_t, detail::engine_bindings::call&);
   static std::shared_ptr<detail::engine_load_budget> load_budget(const preparation&);
   static void check_budget(const std::shared_ptr<detail::engine_load_budget>&);
   template <typename T, typename... Args>
   static std::shared_ptr<T> make(const std::optional<preparation>& budget, Args&&... args) {
      if (budget)
         return std::allocate_shared<T>(preparation::allocator<T>{*budget}, std::forward<Args>(args)...);
      return std::make_shared<T>(std::forward<Args>(args)...);
   }
   std::optional<preparation> allocation;
   resource storage;
   std::pmr::vector<decltype(host_binding::invoke)> callbacks;
   std::shared_ptr<detail::engine_module> source;
   std::shared_ptr<detail::engine_module> engine;
   std::shared_ptr<const detail::engine_bindings> bindings;
   std::shared_ptr<detail::engine_limits> limits;
   std::uint64_t projection_bytes = 0;
   std::size_t bytes = 0;
   purpose use = purpose::execution;
};

} // namespace forge::vm::wasm::interpret
