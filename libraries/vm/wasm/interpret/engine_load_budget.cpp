#include "details/engine_load_budget.hxx"
#include "details/engine_runtime.hxx"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

namespace forge::vm::wasm::interpret::detail {

struct alignas(std::max_align_t) engine_load_budget::block {
   std::shared_ptr<engine_load_budget> owner;
   void* raw;
   std::size_t bytes;
   std::size_t charged;
   std::size_t alignment;
};

struct engine_load_budget::mapping {
   mapping* next;
   void* address;
   std::uint64_t retained;
   std::uint64_t peak;
   unmap_function release;
   bool failed;
};

struct engine_load_budget::root_deleter {
   void operator()(engine_load_budget*) const noexcept {}
};

template <typename T> struct engine_load_budget::root_allocator {
   using value_type = T;
   engine_load_budget* owner;
   explicit root_allocator(engine_load_budget* value) noexcept : owner(value) {}
   template <typename U> root_allocator(const root_allocator<U>& value) noexcept : owner(value.owner) {}
   T* allocate(std::size_t count) {
      if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
         throw error{failure::overflow};
      const auto bytes = count * sizeof(T);
      owner->check();
      const auto capacity = owner->_faults.root_control_limit
                                ? std::min(owner->_faults.root_control_limit, owner->_root_control.size())
                                : owner->_root_control.size();
      if (owner->_root_control_used || bytes > capacity || alignof(T) > alignof(std::max_align_t))
         throw error{failure::invalid};
      owner->_root_control_used = true;
      return reinterpret_cast<T*>(owner->_root_control.data());
   }
   // Keep root and residency token through weak control-block pins, too.
   void deallocate(T*, std::size_t) noexcept {
      if (owner->_root_adopted)
         destroy_root(owner);
      else
         owner->_root_control_used = false;
   }
   template <typename U> bool operator==(const root_allocator<U>& other) const noexcept {
      return owner == other.owner;
   }
};

thread_local std::shared_ptr<engine_load_budget> engine_load_budget::_current;
std::mutex engine_load_budget::_fault_mutex;
engine_load_budget::faults engine_load_budget::_default_faults;

engine_load_budget::fault_scope::fault_scope(faults value) noexcept {
   const auto lock = std::lock_guard{_fault_mutex};
   _previous = std::exchange(_default_faults, value);
}
engine_load_budget::fault_scope::~fault_scope() {
   const auto lock = std::lock_guard{_fault_mutex};
   _default_faults = _previous;
}

engine_load_budget::error::error(failure value) noexcept : reason(value) {}
const char* engine_load_budget::error::what() const noexcept {
   return "WAMR load resource refused";
}

engine_load_budget::engine_load_budget(std::uint64_t max_live_bytes, std::stop_token stop) noexcept
    : engine_load_budget(max_live_bytes, std::move(stop), faults{}) {}

engine_load_budget::engine_load_budget(std::uint64_t max_live_bytes, std::stop_token stop, faults injected) noexcept
    : _limit(max_live_bytes), _stop(std::move(stop)), _faults(injected) {
   if (!_limit)
      fail(failure::invalid);
}

std::size_t engine_load_budget::root_byte_size() {
   return sizeof(engine_load_budget);
}

std::shared_ptr<engine_load_budget> engine_load_budget::create(std::uint64_t limit, std::stop_token stop) {
   faults injected;
   {
      const auto lock = std::lock_guard{_fault_mutex};
      injected = _default_faults;
   }
   return create(limit, std::move(stop), injected);
}

std::shared_ptr<engine_load_budget> engine_load_budget::create(std::uint64_t limit, std::stop_token stop,
                                                               faults injected) {
   const auto root = root_byte_size();
   if (stop.stop_requested())
      throw error{failure::cancelled};
   if (root > limit)
      throw error{failure::limit};
   auto* raw = std::malloc(sizeof(engine_load_budget));
   if (!raw)
      throw error{failure::allocation};
   auto* owner = std::construct_at(static_cast<engine_load_budget*>(raw), limit, std::move(stop), injected);
   owner->_bytes = owner->_peak = owner->_root_bytes = sizeof(engine_load_budget);
   auto guard = std::unique_ptr<engine_load_budget, void (*)(engine_load_budget*)>{owner, &destroy_root};
   // A failed control allocation leaves the stack guard as the sole root owner.
   auto result = std::shared_ptr<engine_load_budget>{owner, root_deleter{}, root_allocator<std::byte>{owner}};
   owner->_root_adopted = true;
   guard.release();
   return result;
}

void engine_load_budget::destroy_root(engine_load_budget* owner) noexcept {
   auto charge = std::move(owner->_charge);
   std::destroy_at(owner);
   std::free(owner);
   charge.reset(); // Physical root/control storage is gone before refund.
}

void engine_load_budget::fail(failure reason) noexcept {
   auto expected = failure::none;
   _failure.compare_exchange_strong(expected, reason);
}

bool engine_load_budget::active() noexcept {
   if (_stop.stop_requested())
      fail(failure::cancelled);
   return status() == failure::none;
}

void engine_load_budget::check() {
   if (!active())
      throw error{status()};
}

bool engine_load_budget::reserve(std::uint64_t bytes) noexcept {
   if (!active())
      return false;
   auto current = _bytes.load();
   for (;;) {
      const auto limit = _limit.load();
      if (current > limit || bytes > limit - current) {
         fail(failure::limit);
         return false;
      }
      if (_bytes.compare_exchange_weak(current, current + bytes))
         break;
   }
   const auto total = current + bytes;
   auto peak = _peak.load();
   while (peak < total && !_peak.compare_exchange_weak(peak, total)) {
   }
   return true;
}

void engine_load_budget::release(std::uint64_t bytes) noexcept {
   auto current = _bytes.load();
   do {
      if (bytes > current) {
         fail(failure::invalid);
         return;
      }
   } while (!_bytes.compare_exchange_weak(current, current - bytes));
}

engine_load_budget::failure engine_load_budget::status() const noexcept {
   return _failure.load();
}
std::uint64_t engine_load_budget::current_bytes() const noexcept {
   return _bytes.load();
}
std::uint64_t engine_load_budget::peak_bytes() const noexcept {
   return _peak.load();
}
std::uint64_t engine_load_budget::limit_bytes() const noexcept {
   return _limit.load();
}
std::uint64_t engine_load_budget::root_bytes() const noexcept {
   return _root_bytes;
}
void engine_load_budget::pin_charge(std::shared_ptr<void> charge) {
   if (_charge || !charge)
      throw error{failure::invalid};
   _charge = std::move(charge); // Attach once, before publication/parallel use.
}
void engine_load_budget::seal(std::uint64_t resident_bytes) {
   check();
   const auto limit = std::min(resident_bytes, limit_bytes());
   const auto retained = current_bytes();
   if (retained > limit) {
      fail(failure::limit);
      check();
   }
   _limit = retained; // Refunded reservation must not become allocatable headroom.
}
void* engine_load_budget::begin_mapping(const std::shared_ptr<engine_load_budget>& owner, std::uint64_t retained,
                                        std::uint64_t peak, unmap_function release) noexcept {
   if (!owner || !release || !retained || retained > peak || peak > std::numeric_limits<std::size_t>::max()) {
      if (owner)
         owner->fail(failure::invalid);
      return nullptr;
   }
   if (owner->_faults.mapping && ++owner->_mappings == owner->_faults.mapping) {
      owner->fail(failure::allocation);
      return nullptr;
   }
   auto* storage = allocate_for(owner, sizeof(mapping), alignof(mapping));
   if (!storage)
      return nullptr;
   if (!owner->reserve(peak)) {
      free(storage);
      return nullptr;
   }
   return std::construct_at(static_cast<mapping*>(storage), mapping{nullptr, nullptr, retained, peak, release, false});
}

void engine_load_budget::complete_mapping(void* token, void* address) noexcept {
   auto* record = static_cast<mapping*>(token);
   if (!address) {
      release(record->peak);
      std::destroy_at(record);
      free(record);
      return;
   }
   record->address = address;
   release(record->peak - record->retained);
   const auto lock = std::lock_guard{_mapping_mutex};
   record->next = _mapping_head;
   _mapping_head = record;
}

bool engine_load_budget::unmap(void* address, std::uint64_t retained) noexcept {
   mapping* record = nullptr;
   {
      const auto lock = std::lock_guard{_mapping_mutex};
      auto** link = &_mapping_head;
      while (*link && (*link)->address != address)
         link = &(*link)->next;
      if (!*link || (*link)->retained != retained) {
         fail(failure::invalid);
         return false;
      }
      record = *link;
      if (!release_mapping(record)) {
         record->failed = true;
         fail(failure::cleanup);
         return false;
      }
      *link = record->next;
   }
   release(record->retained);
   std::destroy_at(record);
   free(record);
   return true;
}

bool engine_load_budget::release_mapping(mapping* record) noexcept {
   const auto attempt = ++_unmappings;
   if (_faults.unmapping_attempts)
      ++*_faults.unmapping_attempts;
   if ((_faults.hold_unmapping && _faults.hold_unmapping->load()) ||
       (_faults.unmapping && attempt == _faults.unmapping))
      return false;
   return record->release(record->address, record->retained) == 0;
}

bool engine_load_budget::cleanup_pending_locked() const noexcept {
   for (auto* record = _mapping_head; record; record = record->next)
      if (record->failed)
         return true;
   return false;
}

bool engine_load_budget::retry_cleanup() noexcept {
   auto success = true;
   mapping* released = nullptr;
   {
      const auto lock = std::lock_guard{_mapping_mutex};
      auto** link = &_mapping_head;
      while (*link) {
         auto* record = *link;
         if (!record->failed) {
            link = &record->next;
            continue;
         }
         if (!release_mapping(record)) {
            success = false;
            link = &record->next;
            continue;
         }
         *link = record->next;
         record->next = released;
         released = record;
      }
   }
   while (released) {
      auto* record = released;
      released = record->next;
      release(record->retained);
      std::destroy_at(record);
      free(record);
   }
   if (success) {
      std::shared_ptr<engine_runtime> runtime;
      {
         const auto lock = std::lock_guard{_mapping_mutex};
         runtime = _cleanup_runtime.lock();
      }
      if (runtime)
         runtime->retire_cleanup(this);
   }
   return success;
}
bool engine_load_budget::allow_protection() noexcept {
   if (_faults.protection && ++_protections == _faults.protection) {
      fail(failure::protection);
      return false;
   }
   return active();
}
std::shared_ptr<engine_load_budget> engine_load_budget::current() noexcept {
   return _current;
}
std::size_t engine_load_budget::allocation_overhead() noexcept {
   return sizeof(block);
}

engine_load_budget::scope::scope(std::shared_ptr<engine_load_budget> owner) noexcept
    : _previous(std::exchange(_current, std::move(owner))) {}
engine_load_budget::scope::~scope() {
   _current = std::move(_previous);
}

void* engine_load_budget::allocate_for(const std::shared_ptr<engine_load_budget>& owner, std::size_t bytes,
                                       std::size_t alignment) noexcept {
   bytes = std::max<std::size_t>(bytes, 1);
   if (!alignment || (alignment & (alignment - 1))) {
      if (owner)
         owner->fail(failure::invalid);
      return nullptr;
   }
   alignment = std::max(alignment, alignof(std::max_align_t));
   const auto slack = alignment > alignof(std::max_align_t) ? alignment - 1 : 0;
   if (bytes > std::numeric_limits<std::size_t>::max() - sizeof(block) ||
       slack > std::numeric_limits<std::size_t>::max() - sizeof(block) - bytes) {
      if (owner)
         owner->fail(failure::overflow);
      return nullptr;
   }
   const auto charged = sizeof(block) + bytes + slack;
   if (owner && !owner->reserve(charged))
      return nullptr;
   const auto attempt = owner ? ++owner->_allocations : 0;
   if (owner && owner->_faults.allocation && attempt == owner->_faults.allocation) {
      owner->release(charged);
      owner->fail(failure::allocation);
      return nullptr;
   }
   auto* raw = std::malloc(charged);
   if (!raw) {
      if (owner) {
         owner->release(charged);
         owner->fail(failure::allocation);
      }
      return nullptr;
   }
   auto* result = static_cast<void*>(static_cast<std::byte*>(raw) + sizeof(block));
   auto space = bytes + slack;
   std::align(alignment, bytes, result, space); // Charged alignment slack guarantees success.
   auto* prefix = reinterpret_cast<block*>(static_cast<std::byte*>(result) - sizeof(block));
   std::construct_at(prefix, block{owner, raw, bytes, charged, alignment});
   if (owner && owner->_faults.cancel_source && attempt == owner->_faults.cancel_allocation)
      owner->_faults.cancel_source->request_stop();
   if (owner && !owner->active()) {
      free(result);
      return nullptr;
   }
   return result;
}

void* engine_load_budget::allocate(std::size_t bytes) noexcept {
   return allocate_for(_current, bytes, alignof(std::max_align_t));
}
void* engine_load_budget::allocate(std::size_t bytes, std::size_t alignment) noexcept {
   return allocate_for(_current, bytes, alignment);
}

void* engine_load_budget::reallocate(void* address, std::size_t bytes) noexcept {
   if (!address)
      return allocate(bytes);
   auto* old = static_cast<block*>(address) - 1;
   auto* replacement = allocate_for(old->owner, bytes, old->alignment);
   if (!replacement)
      return nullptr;
   // Commit after allocate_for's cancellation check; the old pointer survives all failures above.
   std::memcpy(replacement, address, std::min(old->bytes, std::max<std::size_t>(bytes, 1)));
   free(address);
   return replacement;
}

void engine_load_budget::free(void* address) noexcept {
   if (!address)
      return;
   auto* prefix = static_cast<block*>(address) - 1;
   auto owner = std::move(prefix->owner);
   const auto charged = prefix->charged;
   auto* raw = prefix->raw;
   std::destroy_at(prefix);
   std::free(raw);
   if (owner)
      owner->release(charged);
}

} // namespace forge::vm::wasm::interpret::detail
