module;

#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <limits>
#include <mutex>
#include <exception>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

export module forge.vm.wasm.interpret.prepared_module;

export import forge.vm.wasm.interpret.capacity_profile;
export import forge.vm.wasm.interpret.debug_info;
export import forge.vm.wasm.interpret.host_function;
export import forge.vm.wasm.interpret.options;

export namespace forge::vm::wasm::interpret {

enum class execution_mode { interpreter, jit, jit_profile };

struct host_binding {
   std::string module;
   std::string name;
   host_function signature;
   std::function<void(void*, execution_interface)> invoke;
};

class prepared_module {
 public:
   class context;
   class preparation;
   enum class native_origin { untrusted, trusted_compiler };
   enum class purpose { execution, validation_only, unlinked_execution };
   using function_type = module::jit_mod_t::jit_func_type;
   // Only Forge's initial projection is budgeted, not later caller mutations.
   // The bounded default-allocator projection is qualified for libc++ 22.1.8.
   std::vector<module::jit_mod_t::jit_import_entry> imports;
   prepared_module(std::span<const std::uint8_t>, const options&, std::vector<host_binding> = {},
                   purpose = purpose::execution);
   // Explicit-mode legacy path owns a bounded 1 GiB module arena;
   // transient LLVM compilation storage is outside that arena contract.
   prepared_module(std::span<const std::uint8_t>, const options&, std::vector<host_binding>, purpose, execution_mode);
   prepared_module(std::span<const std::uint8_t> source_wasm, std::span<const std::uint8_t> native_aot, const options&,
                   std::vector<host_binding> = {}, native_origin = native_origin::untrusted);
   // Bounded overloads move invoke and clear its source; names/signatures are borrowed.
   // Opaque callback-owned heap remains outside Forge-managed preparation storage.
   prepared_module(std::span<const std::uint8_t>, const options&, std::span<host_binding>, purpose, const preparation&);
   prepared_module(std::span<const std::uint8_t>, const options&, std::span<host_binding>, purpose, const preparation&,
                   execution_mode);
   prepared_module(std::span<const std::uint8_t> source_wasm, std::span<const std::uint8_t> native_aot, const options&,
                   std::span<host_binding>, native_origin, const preparation&);
   ~prepared_module();
   prepared_module(const prepared_module&) = delete;
   prepared_module& operator=(const prepared_module&) = delete;
   void validate() const;
   std::uint32_t get_exported_function(std::string_view) const;
   function_type get_function_type(std::uint32_t) const;
   struct signature_view {
      std::span<const value_type> params;
      std::span<const value_type> results;
   };
   // Views borrow immutable module storage for this module's lifetime.
   signature_view function_signature(std::uint32_t) const;
   std::uint32_t function_count() const noexcept;
   std::vector<std::string> function_exports() const;
   struct imported_function {
      std::string module;
      std::string name;
      host_function signature;
   };
   std::vector<imported_function> function_imports() const;
   std::size_t code_size() const noexcept;
   bool has_linear_memory() const noexcept;
   void check_limits(const options&) const;
   // Indexed by WASM function index; imported functions have size zero.
   std::vector<std::uint32_t> native_function_stack_bytes() const;
   std::uint32_t native_function_stack_bytes(std::uint32_t) const;
   std::span<const std::byte> native_code() const noexcept;
   // No interval copy or allocation. The observer keeps the native module alive;
   // unprofiled modules return an empty map. Stop sampling before its teardown.
   profile_instr_map get_debug() const noexcept;
   struct memory_accounting {
      std::uint64_t source_buffer_bytes = 0;
      std::uint64_t native_buffer_bytes = 0;
      std::uint64_t upstream_source_bytes = 0;
      std::uint64_t upstream_native_bytes = 0;
   };
   // Upstream structural accounting is not exact resident allocator accounting.
   memory_accounting accounted_module_memory() const;

 private:
   struct impl;
   void project_imports();
   std::shared_ptr<impl> _impl;
};

// Bounds requested Forge-owned preparation storage and its allocation headers,
// not allocator overhead or process resident memory. Caller-managed callback
// heaps, process-global runtime setup and transient LLVM compilation heap are excluded.
// Preparation is builder-exclusive. Complete all builder/allocator operations
// before sealing or publishing; sealing must not race with allocation, another
// seal, or reserve/release. Afterwards distinct owned blocks may be freed
// concurrently on other threads.
class prepared_module::preparation {
 public:
   struct limits {
      std::uint64_t resident_bytes;
      // Total tracked live storage, including retained data and temporary overlap.
      std::uint64_t workspace_bytes;
   };
   template <typename T> class allocator;
   explicit preparation(limits, std::stop_token = {});
   preparation(const preparation&) noexcept = default;
   preparation& operator=(const preparation&) noexcept = default;
   static std::uint64_t root_byte_size();
   static std::uint64_t minimum_workspace_bytes();
   static std::size_t allocation_overhead() noexcept;
   limits configured_limits() const noexcept;
   std::uint64_t current_bytes() const noexcept;
   std::uint64_t peak_bytes() const noexcept;
   void check() const;
   // With all builder/allocator operations finished, check the resident ceiling
   // and freeze at current storage. This is not a concurrent allocation barrier.
   std::uint64_t seal() const;
   void* allocate(std::size_t, std::size_t alignment = alignof(std::max_align_t)) const;
   static void deallocate(void*) noexcept;
   // For reviewed default-allocator projections only; reserve before allocation.
   void reserve(std::uint64_t) const;
   void release(std::uint64_t) const noexcept;
   // Attach once before native loading/publication, including failure cleanup.
   // External token must not own this preparation or use its allocator.
   void attach_resident_charge(std::shared_ptr<void>) const;
   // Retry failed physical cleanup. False requires retaining this owner/charge;
   // true does not clear a sticky failure or make preparation reusable.
   bool retry_cleanup() const noexcept;
   struct cleanup_result {
      std::size_t attempted;
      std::size_t remaining;
   };
   // Allocation-free process-wide maintenance, at most max_owners attempts.
   // Remaining is advisory, not evidence of ownership by a particular session.
   static cleanup_result retry_pending_cleanup(std::size_t max_owners) noexcept;

 private:
   friend struct prepared_module::impl;
   template <typename> friend class allocator;
   limits _limits;
   std::shared_ptr<void> _owner;
};

template <typename T> class prepared_module::preparation::allocator {
 public:
   using value_type = T;
   using is_always_equal = std::false_type;
   explicit allocator(const preparation& owner) noexcept : _owner(owner) {}
   template <typename U> allocator(const allocator<U>& other) noexcept : _owner(other._owner) {}
   T* allocate(std::size_t count) {
      if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
         throw exceptions::execution_unavailable{"Preparation allocation size overflow"};
      return static_cast<T*>(_owner.allocate(count * sizeof(T), alignof(T)));
   }
   void deallocate(T* pointer, std::size_t) noexcept {
      preparation::deallocate(pointer);
   }
   template <typename U> bool operator==(const allocator<U>& other) const noexcept {
      return _owner._owner == other._owner._owner;
   }

 private:
   template <typename> friend class allocator;
   preparation _owner;
};

class prepared_module::context {
 public:
   class activity;
   class failure_scope;
   // Defaults to ready JIT for bytecode prepared in that mode. AOT payloads
   // remain native regardless of this bytecode execution choice.
   context(prepared_module&, std::uint32_t max_call_depth);
   context(prepared_module&, std::uint32_t max_call_depth, execution_mode);
   ~context();
   context(const context&) = delete;
   context& operator=(const context&) = delete;
   bool matches(const prepared_module&, execution_mode) const noexcept;
   void set_wasm_allocator(wasm_allocator*);
   void set_max_call_depth(std::uint32_t);
   void set_max_pages(std::uint32_t);
   void initialize_globals();
   // Caller owns the physical stack and its guard for the entire invocation.
   void set_native_stack_boundary(std::byte*) noexcept;
   void initialize(void* host, std::stop_token = {});
   void reset();
   activity scoped_activity();
   std::optional<operand_stack_elem> call(void* host, std::string_view, std::span<const operand_stack_elem>,
                                          std::stop_token = {});
   std::optional<operand_stack_elem> call(void* host, std::uint32_t, std::span<const operand_stack_elem>,
                                          std::stop_token = {});
   std::optional<operand_stack_elem> call_indirect(void* host, std::uint32_t, std::span<const operand_stack_elem>,
                                                   std::stop_token = {});
   char* linear_memory() const;
   std::size_t memory_size() const;
   std::uint32_t current_linear_memory() const;
   bool has_linear_memory() const noexcept;
   bool initialized() const noexcept;
   std::exception_ptr host_failure() const noexcept;
   void clear_host_failure() noexcept;
   void exit(const std::error_code& = {});

 private:
   struct impl;
   std::shared_ptr<impl> _impl;
};

// Internal boundary owner: reset/initialize may retire an instance, but cannot
// erase the first host failure observed by an enclosing execution scope.
class prepared_module::context::failure_scope {
 public:
   explicit failure_scope(context&);
   ~failure_scope();
   failure_scope(const failure_scope&) = delete;
   failure_scope& operator=(const failure_scope&) = delete;
   std::exception_ptr failure() const noexcept;

 private:
   friend struct context::impl;
   std::shared_ptr<impl> _impl;
   failure_scope* _previous{};
   std::exception_ptr _failure;
};

class prepared_module::context::activity {
 public:
   explicit activity(context&);
   ~activity();
   activity(const activity&) = delete;
   activity& operator=(const activity&) = delete;

 private:
   activity(context&, bool initializing);
   friend class prepared_module::context;
   std::shared_ptr<impl> _impl;
   std::unique_lock<std::recursive_mutex> _lock;
   std::optional<wasm_allocator::borrow::activity> _memory;
};

using runtime_context = prepared_module::context;

} // namespace forge::vm::wasm::interpret
