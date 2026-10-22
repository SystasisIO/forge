#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>

namespace forge::vm::wasm::interpret::detail {

// Experimental allocation owner only. Not connected to the WAMR loaders.
class engine_load_plan {
 public:
   enum class operation { allocate, reallocate, release, map, unmap };
   enum class failure { none, invalid_request, overflow, workspace_limit, ledger_limit, allocation, cancelled };
   struct limits {
      std::uint64_t workspace_bytes;
      std::size_t max_events;
      std::size_t max_live_blocks;
      std::size_t page_bytes;
   };
   struct allocator {
      // Callbacks return max_align_t-aligned storage, do not throw or reenter.
      void* data;
      void* (*allocate)(void*, std::size_t) noexcept;
      void (*release)(void*, void*) noexcept;
   };
   struct event {
      operation kind;
      std::uint64_t id;
      std::uint64_t requested_bytes;
      std::uint64_t retained_bytes;
      std::uint64_t workspace_bytes;
      std::uint64_t previous_bytes;
      std::size_t alignment;
      bool executable;
   };
   struct observation {
      std::uint64_t workspace_bytes;
      std::uint64_t peak_workspace_bytes;
      std::uint64_t runtime_bytes;
      std::uint64_t mapping_bytes;
      std::size_t live_blocks;
      std::size_t events;
   };

   static allocator system_allocator() noexcept;
   static std::optional<std::uint64_t> base_workspace_bytes(std::size_t max_events,
                                                            std::size_t max_live_blocks) noexcept;
   engine_load_plan(limits, allocator, std::stop_token = {}) noexcept;
   ~engine_load_plan();
   engine_load_plan(const engine_load_plan&) = delete;
   engine_load_plan& operator=(const engine_load_plan&) = delete;

   void* allocate(std::uint64_t bytes) noexcept;
   void* reallocate(void*, std::uint64_t bytes) noexcept;
   void release(void*) noexcept;
   void* map(std::uint64_t bytes, bool executable) noexcept;
   void unmap(void*) noexcept;
   // Failure is sticky. Cleanup remains valid after failure or cancellation.
   void rollback() noexcept;
   failure error() const noexcept;
   observation observe() const noexcept;
   std::span<const event> events() const noexcept;

 private:
   struct block {
      void* raw = nullptr;
      void* address = nullptr;
      std::uint64_t id = 0;
      std::uint64_t requested_bytes = 0;
      std::uint64_t retained_bytes = 0;
      std::uint64_t workspace_bytes = 0;
      std::size_t alignment = 0;
      bool mapped = false;
      bool executable = false;
   };
   bool active() noexcept;
   bool room(bool new_block) noexcept;
   bool charge(std::uint64_t) noexcept;
   void fail(failure) noexcept;
   block* find(void*) noexcept;
   void* create(std::uint64_t, bool mapped, bool executable) noexcept;
   void destroy(block&) noexcept;
   void dispose(void*, bool mapped) noexcept;

   limits _limits;
   allocator _allocator;
   std::stop_token _stop;
   failure _error = failure::none;
   void* _metadata = nullptr;
   event* _events = nullptr;
   block* _blocks = nullptr;
   observation _observation{};
   std::uint64_t _next_id = 1;
};

} // namespace forge::vm::wasm::interpret::detail
