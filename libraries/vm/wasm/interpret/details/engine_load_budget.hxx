#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stop_token>

namespace forge::vm::wasm::interpret::detail {

class engine_runtime;

// A caller-supplied bound on live load storage, including temporary overlap.
// Bridge objects use this same owner; process-global initialization is separate.
class engine_load_budget {
 public:
   enum class failure { none, limit, allocation, cancelled, invalid, overflow, protection, cleanup };
   class error : public std::exception {
    public:
      explicit error(failure) noexcept;
      const char* what() const noexcept override;
      failure reason;
   };
   class scope;
   struct faults {
      std::uint64_t allocation = 0;
      std::uint64_t mapping = 0;
      std::uint64_t protection = 0;
      std::uint64_t unmapping = 0;
      std::uint64_t cancel_allocation = 0;
      std::stop_source* cancel_source = nullptr;
      std::size_t root_control_limit = 0;
      const std::atomic<bool>* hold_unmapping = nullptr;
      std::atomic<std::uint64_t>* unmapping_attempts = nullptr;
   };
   class fault_scope;

   explicit engine_load_budget(std::uint64_t max_live_bytes, std::stop_token = {}) noexcept;
   engine_load_budget(std::uint64_t max_live_bytes, std::stop_token, faults) noexcept;
   static std::shared_ptr<engine_load_budget> create(std::uint64_t, std::stop_token = {});
   static std::shared_ptr<engine_load_budget> create(std::uint64_t, std::stop_token, faults);
   static std::size_t root_byte_size();
   engine_load_budget(const engine_load_budget&) = delete;
   engine_load_budget& operator=(const engine_load_budget&) = delete;
   bool reserve(std::uint64_t) noexcept;
   void release(std::uint64_t) noexcept;
   void fail(failure) noexcept;
   bool active() noexcept;
   void check();
   failure status() const noexcept;
   std::uint64_t current_bytes() const noexcept;
   std::uint64_t peak_bytes() const noexcept;
   std::uint64_t limit_bytes() const noexcept;
   std::uint64_t root_bytes() const noexcept;
   void pin_charge(std::shared_ptr<void>);
   // Exclusive prepublication transition; later allocations obey the smaller cap.
   void seal(std::uint64_t resident_bytes);
   using unmap_function = int (*)(void*, std::size_t);
   static void* begin_mapping(const std::shared_ptr<engine_load_budget>&, std::uint64_t retained, std::uint64_t peak,
                              unmap_function) noexcept;
   void complete_mapping(void* token, void* address) noexcept;
   bool unmap(void* address, std::uint64_t retained) noexcept;
   bool retry_cleanup() noexcept;
   bool allow_protection() noexcept;
   static std::shared_ptr<engine_load_budget> current() noexcept;

   // All Alloc_For_Runtime blocks use the same prefix, including unowned ones.
   // Prefixes pin their owner, so free/realloc never depend on current TLS.
   static void* allocate(std::size_t) noexcept;
   static void* allocate(std::size_t bytes, std::size_t alignment) noexcept;
   static void* reallocate(void*, std::size_t) noexcept;
   static void free(void*) noexcept;
   static std::size_t allocation_overhead() noexcept;

 private:
   friend class engine_runtime;
   struct block;
   struct mapping;
   struct root_deleter;
   template <typename T> struct root_allocator;
   static void destroy_root(engine_load_budget*) noexcept;
   static void* allocate_for(const std::shared_ptr<engine_load_budget>&, std::size_t, std::size_t) noexcept;
   bool release_mapping(mapping*) noexcept;
   bool cleanup_pending_locked() const noexcept;
   static thread_local std::shared_ptr<engine_load_budget> _current;
   static std::mutex _fault_mutex;
   static faults _default_faults;
   std::atomic<std::uint64_t> _limit;
   std::stop_token _stop;
   std::atomic<std::uint64_t> _bytes{0};
   std::atomic<std::uint64_t> _peak{0};
   std::atomic<failure> _failure{failure::none};
   std::uint64_t _root_bytes = 0;
   faults _faults;
   std::atomic<std::uint64_t> _allocations{0};
   std::atomic<std::uint64_t> _mappings{0};
   std::atomic<std::uint64_t> _protections{0};
   std::atomic<std::uint64_t> _unmappings{0};
   std::mutex _mapping_mutex;
   mapping* _mapping_head = nullptr;
   engine_load_budget* _cleanup_next = nullptr;
   std::shared_ptr<engine_load_budget> _cleanup_owner;
   bool _cleanup_registered = false;
   bool _cleanup_again = false;
   bool _cleanup_running = false;
   // Guarded by _mapping_mutex; cleanup must never borrow a dying runtime.
   std::weak_ptr<engine_runtime> _cleanup_runtime;
   std::shared_ptr<void> _charge;
   // Physical root storage, not an estimate of an STL control-block size.
   alignas(std::max_align_t) std::array<std::byte, 256> _root_control{};
   bool _root_control_used = false;
   bool _root_adopted = false;
};

// Private exclusive-subprocess fault control. Future owners copy a fixed
// snapshot, including owners created by the existing session worker thread.
class engine_load_budget::fault_scope {
 public:
   explicit fault_scope(faults) noexcept;
   ~fault_scope();
   fault_scope(const fault_scope&) = delete;
   fault_scope& operator=(const fault_scope&) = delete;

 private:
   faults _previous;
};

class engine_load_budget::scope {
 public:
   explicit scope(std::shared_ptr<engine_load_budget>) noexcept;
   ~scope();
   scope(const scope&) = delete;
   scope& operator=(const scope&) = delete;

 private:
   std::shared_ptr<engine_load_budget> _previous;
};

} // namespace forge::vm::wasm::interpret::detail
