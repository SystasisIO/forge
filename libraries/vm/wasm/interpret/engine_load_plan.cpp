#include "details/engine_load_plan.hxx"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace forge::vm::wasm::interpret::detail {
namespace {

bool add(std::uint64_t& value, std::uint64_t extra) noexcept {
   if (extra > UINT64_MAX - value)
      return false;
   value += extra;
   return true;
}

bool multiply(std::size_t count, std::size_t bytes, std::uint64_t& value) noexcept {
   if (count > UINT64_MAX / bytes)
      return false;
   value = static_cast<std::uint64_t>(count) * bytes;
   return true;
}

void* system_allocate(void*, std::size_t bytes) noexcept {
   return std::malloc(bytes);
}
void system_release(void*, void* address) noexcept {
   std::free(address);
}

} // namespace

engine_load_plan::allocator engine_load_plan::system_allocator() noexcept {
   return {nullptr, &system_allocate, &system_release};
}

std::optional<std::uint64_t> engine_load_plan::base_workspace_bytes(std::size_t max_events,
                                                                    std::size_t max_live_blocks) noexcept {
   if (!max_events || !max_live_blocks)
      return {};
   std::uint64_t events = 0, blocks = 0;
   if (!multiply(max_events, sizeof(event), events) || !multiply(max_live_blocks, sizeof(block), blocks))
      return {};
   const auto padding = (alignof(block) - events % alignof(block)) % alignof(block);
   if (!add(events, padding) || !add(events, blocks) || !add(events, sizeof(engine_load_plan)) ||
       events > std::numeric_limits<std::size_t>::max())
      return {};
   return events;
}

engine_load_plan::engine_load_plan(limits configuration, allocator memory, std::stop_token stop) noexcept
    : _limits(configuration), _allocator(memory), _stop(std::move(stop)) {
   if (!memory.allocate || !memory.release || configuration.page_bytes < alignof(std::max_align_t) ||
       (configuration.page_bytes & (configuration.page_bytes - 1)) != 0) {
      fail(failure::invalid_request);
      return;
   }
   const auto base = base_workspace_bytes(configuration.max_events, configuration.max_live_blocks);
   if (!base) {
      fail(configuration.max_events && configuration.max_live_blocks ? failure::overflow : failure::invalid_request);
      return;
   }
   if (!active() || !charge(*base))
      return;
   const auto metadata_bytes = static_cast<std::size_t>(*base - sizeof(engine_load_plan));
   _metadata = memory.allocate(memory.data, metadata_bytes);
   if (!_metadata) {
      _observation.workspace_bytes = 0;
      fail(failure::allocation);
      return;
   }
   _events = static_cast<event*>(_metadata);
   const auto event_bytes = configuration.max_events * sizeof(event);
   const auto padding = (alignof(block) - event_bytes % alignof(block)) % alignof(block);
   _blocks = reinterpret_cast<block*>(static_cast<std::byte*>(_metadata) + event_bytes + padding);
   for (std::size_t i = 0; i < configuration.max_live_blocks; ++i)
      std::construct_at(_blocks + i);
   active();
}

engine_load_plan::~engine_load_plan() {
   rollback();
   if (_metadata)
      _allocator.release(_allocator.data, _metadata);
}

void engine_load_plan::fail(failure reason) noexcept {
   if (_error == failure::none)
      _error = reason;
}

bool engine_load_plan::active() noexcept {
   if (_stop.stop_requested())
      fail(failure::cancelled);
   return _error == failure::none;
}

bool engine_load_plan::charge(std::uint64_t bytes) noexcept {
   if (bytes > _limits.workspace_bytes - _observation.workspace_bytes) {
      fail(failure::workspace_limit);
      return false;
   }
   _observation.workspace_bytes += bytes;
   _observation.peak_workspace_bytes = std::max(_observation.peak_workspace_bytes, _observation.workspace_bytes);
   return true;
}

bool engine_load_plan::room(bool new_block) noexcept {
   // Every live block owns a future cleanup event, even after the journal fills.
   const auto available = _limits.max_events - _observation.events - _observation.live_blocks;
   if (available < (new_block ? 2U : 1U) || (new_block && _observation.live_blocks == _limits.max_live_blocks)) {
      fail(failure::ledger_limit);
      return false;
   }
   return true;
}

engine_load_plan::block* engine_load_plan::find(void* address) noexcept {
   for (std::size_t i = 0; i < _limits.max_live_blocks; ++i)
      if (_blocks[i].raw && _blocks[i].address == address)
         return _blocks + i;
   return nullptr;
}

void* engine_load_plan::allocate(std::uint64_t bytes) noexcept {
   return create(bytes, false, false);
}
void* engine_load_plan::map(std::uint64_t bytes, bool executable) noexcept {
   return create(bytes, true, executable);
}

void* engine_load_plan::create(std::uint64_t bytes, bool mapped, bool executable) noexcept {
   if (!active())
      return nullptr;
   if (mapped && !bytes) {
      fail(failure::invalid_request);
      return nullptr;
   }
   const auto alignment = mapped ? _limits.page_bytes : alignof(std::max_align_t);
   auto retained = std::max<std::uint64_t>(bytes, 1);
   if (mapped) {
      const auto padding = (alignment - retained % alignment) % alignment;
      if (!add(retained, padding)) {
         fail(failure::overflow);
         return nullptr;
      }
   }
   auto backing = retained;
   if ((mapped && !add(backing, alignment - 1)) || backing > std::numeric_limits<std::size_t>::max()) {
      fail(failure::overflow);
      return nullptr;
   }
   if (!room(true) || !charge(backing))
      return nullptr;
   auto* raw = _allocator.allocate(_allocator.data, static_cast<std::size_t>(backing));
   if (!raw) {
      _observation.workspace_bytes -= backing;
      fail(failure::allocation);
      return nullptr;
   }
   auto* address = raw;
   if (mapped) {
      auto space = static_cast<std::size_t>(backing);
      address = std::align(alignment, static_cast<std::size_t>(retained), address, space);
      if (!address) {
         _allocator.release(_allocator.data, raw);
         _observation.workspace_bytes -= backing;
         fail(failure::invalid_request);
         return nullptr;
      }
      std::memset(address, 0, static_cast<std::size_t>(retained));
   }
   auto* entry = _blocks;
   while (entry->raw)
      ++entry;
   *entry = {raw, address, _next_id++, bytes, retained, backing, alignment, mapped, executable};
   std::construct_at(_events + _observation.events++, event{mapped ? operation::map : operation::allocate, entry->id,
                                                            bytes, retained, backing, 0, alignment, executable});
   ++_observation.live_blocks;
   (mapped ? _observation.mapping_bytes : _observation.runtime_bytes) += retained;
   return active() ? address : nullptr;
}

void* engine_load_plan::reallocate(void* address, std::uint64_t bytes) noexcept {
   if (!address)
      return allocate(bytes);
   if (!active())
      return nullptr;
   auto* entry = find(address);
   if (!entry || entry->mapped) {
      fail(failure::invalid_request);
      return nullptr;
   }
   const auto backing = std::max<std::uint64_t>(bytes, 1);
   if (backing > std::numeric_limits<std::size_t>::max()) {
      fail(failure::overflow);
      return nullptr;
   }
   if (!room(false) || !charge(backing))
      return nullptr;
   auto* raw = _allocator.allocate(_allocator.data, static_cast<std::size_t>(backing));
   if (!raw) {
      _observation.workspace_bytes -= backing;
      fail(failure::allocation);
      return nullptr;
   }
   if (!active()) {
      _allocator.release(_allocator.data, raw);
      _observation.workspace_bytes -= backing;
      return nullptr;
   }
   // Commit point: the old block is unchanged above; no failure is returned below.
   std::memcpy(raw, entry->address, static_cast<std::size_t>(std::min(entry->retained_bytes, backing)));
   _allocator.release(_allocator.data, entry->raw);
   _observation.workspace_bytes -= entry->workspace_bytes;
   _observation.runtime_bytes -= entry->retained_bytes;
   _observation.runtime_bytes += backing;
   std::construct_at(_events + _observation.events++, event{operation::reallocate, entry->id, bytes, backing, backing,
                                                            entry->retained_bytes, entry->alignment, false});
   entry->raw = entry->address = raw;
   entry->requested_bytes = bytes;
   entry->retained_bytes = entry->workspace_bytes = backing;
   return raw;
}

void engine_load_plan::destroy(block& entry) noexcept {
   _allocator.release(_allocator.data, entry.raw);
   std::construct_at(_events + _observation.events++,
                     event{entry.mapped ? operation::unmap : operation::release, entry.id, entry.requested_bytes,
                           entry.retained_bytes, entry.workspace_bytes, 0, entry.alignment, entry.executable});
   _observation.workspace_bytes -= entry.workspace_bytes;
   (entry.mapped ? _observation.mapping_bytes : _observation.runtime_bytes) -= entry.retained_bytes;
   --_observation.live_blocks;
   entry = {};
}

void engine_load_plan::dispose(void* address, bool mapped) noexcept {
   if (!address)
      return;
   if (!_metadata) {
      fail(failure::invalid_request);
      return;
   }
   auto* entry = find(address);
   if (!entry || entry->mapped != mapped) {
      fail(failure::invalid_request);
      return;
   }
   destroy(*entry);
}

void engine_load_plan::release(void* address) noexcept {
   dispose(address, false);
}
void engine_load_plan::unmap(void* address) noexcept {
   dispose(address, true);
}

void engine_load_plan::rollback() noexcept {
   if (!_metadata)
      return;
   for (std::size_t i = _limits.max_live_blocks; i > 0; --i)
      if (_blocks[i - 1].raw)
         destroy(_blocks[i - 1]);
}

engine_load_plan::failure engine_load_plan::error() const noexcept {
   return _error;
}
engine_load_plan::observation engine_load_plan::observe() const noexcept {
   return _observation;
}
std::span<const engine_load_plan::event> engine_load_plan::events() const noexcept {
   return {_events, _observation.events};
}

} // namespace forge::vm::wasm::interpret::detail
