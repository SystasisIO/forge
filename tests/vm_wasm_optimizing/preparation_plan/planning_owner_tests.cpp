#include "../../../libraries/vm/wasm/interpret/details/engine_load_plan.hxx"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <stop_token>
#include <string_view>

namespace {
using plan = forge::vm::wasm::interpret::detail::engine_load_plan;

void require(bool condition, const char* message) {
   if (!condition)
      throw std::runtime_error(message);
}

struct memory {
   struct allocation {
      void* address = nullptr;
      std::size_t bytes = 0;
   };
   std::array<allocation, 32> blocks{};
   std::size_t calls = 0;
   std::size_t fail_at = 0;
   std::size_t stop_at = 0;
   std::size_t bytes = 0;
   std::size_t peak = 0;
   std::size_t releases = 0;
   bool invalid_free = false;
   bool stop_on_release = false;
   std::stop_source stop;

   static void* allocate(void* data, std::size_t size) noexcept {
      auto& self = *static_cast<memory*>(data);
      ++self.calls;
      if (self.calls == self.stop_at)
         self.stop.request_stop();
      if (self.calls == self.fail_at)
         return nullptr;
      for (auto& block : self.blocks) {
         if (block.address)
            continue;
         auto* address = std::malloc(size);
         if (!address)
            return nullptr;
         block = {address, size};
         self.bytes += size;
         self.peak = std::max(self.peak, self.bytes);
         return address;
      }
      return nullptr;
   }

   static void release(void* data, void* address) noexcept {
      auto& self = *static_cast<memory*>(data);
      for (auto& block : self.blocks) {
         if (block.address != address)
            continue;
         self.bytes -= block.bytes;
         ++self.releases;
         std::free(address);
         block = {};
         if (self.stop_on_release)
            self.stop.request_stop();
         return;
      }
      self.invalid_free = true;
   }

   plan::allocator allocator() noexcept {
      return {this, &allocate, &release};
   }
   bool owns(void* address, std::size_t size) const noexcept {
      return std::any_of(blocks.begin(), blocks.end(),
                         [=](const auto& block) { return block.address == address && block.bytes == size; });
   }
   void clean() const {
      require(!bytes && !invalid_free, "Allocator ownership leaked or freed a foreign block");
   }
};

plan::limits limits(std::uint64_t extra, std::size_t events = 32, std::size_t blocks = 8, std::size_t page = 4096) {
   const auto base = plan::base_workspace_bytes(events, blocks);
   require(base.has_value() && extra <= UINT64_MAX - *base, "Invalid test workspace");
   return {*base + extra, events, blocks, page};
}

void empty(const plan& owner, std::uint64_t base) {
   const auto current = owner.observe();
   require(current.live_blocks == 0 && current.runtime_bytes == 0 && current.mapping_bytes == 0,
           "Rollback retained payload ownership");
   require(current.workspace_bytes == base, "Rollback released the wrong workspace charge");
}

void metadata_limit() {
   auto storage = memory{};
   auto configuration = limits(0);
   --configuration.workspace_bytes;
   {
      auto owner = plan{configuration, storage.allocator()};
      require(owner.error() == plan::failure::workspace_limit && !storage.calls,
              "Metadata was allocated before its workspace reservation");
      require(owner.observe().workspace_bytes == 0, "Rejected metadata retained a charge");
   }
   storage.clean();
}

void exact_limit() {
   auto storage = memory{};
   const auto configuration = limits(64);
   {
      auto owner = plan{configuration, storage.allocator()};
      auto* value = owner.allocate(64);
      require(value && owner.observe().workspace_bytes == configuration.workspace_bytes, "Exact workspace was refused");
      const auto calls = storage.calls;
      require(!owner.allocate(1) && owner.error() == plan::failure::workspace_limit,
              "One-byte workspace excess was admitted");
      require(storage.calls == calls && !owner.map(1, true) && storage.calls == calls,
              "Failed owner allowed an allocation fallback");
      owner.release(value);
      empty(owner, configuration.workspace_bytes - 64);
      require(owner.events().size() == 2 && owner.events()[1].kind == plan::operation::release,
              "Failure lost the reserved cleanup event");
   }
   storage.clean();
}

void ledger_limit() {
   auto storage = memory{};
   const auto configuration = limits(64, 2, 2);
   {
      auto owner = plan{configuration, storage.allocator()};
      require(owner.allocate(16), "First allocation failed");
      const auto calls = storage.calls;
      require(!owner.allocate(16) && owner.error() == plan::failure::ledger_limit && storage.calls == calls,
              "Journal exhaustion allocated unrecorded storage");
      owner.rollback();
      empty(owner, configuration.workspace_bytes - 64);
      require(owner.events().size() == 2 && owner.events()[0].id == owner.events()[1].id,
              "Full journal could not record complete rollback");
   }
   storage.clean();
}

void live_limit() {
   auto storage = memory{};
   {
      auto owner = plan{limits(64, 16, 1), storage.allocator()};
      require(owner.allocate(16), "First live slot failed");
      const auto calls = storage.calls;
      require(!owner.allocate(16) && owner.error() == plan::failure::ledger_limit && storage.calls == calls,
              "Live-block limit was not enforced before allocation");
   }
   storage.clean();
}

void shadow_maps() {
   for (const auto page : {4096U, 16384U}) {
      auto storage = memory{};
      const auto rounded = static_cast<std::uint64_t>(page) * 2;
      const auto backing = rounded + page - 1;
      const auto configuration = limits(backing, 8, 2, page);
      {
         auto owner = plan{configuration, storage.allocator()};
         auto* address = owner.map(page + 1, true);
         require(address && reinterpret_cast<std::uintptr_t>(address) % page == 0,
                 "Shadow mapping is not page aligned");
         const auto* bytes = static_cast<const unsigned char*>(address);
         require(std::all_of(bytes, bytes + rounded, [](auto value) { return value == 0; }),
                 "Shadow mapping is not zero filled");
         const auto& event = owner.events().front();
         require(event.kind == plan::operation::map && event.executable && event.retained_bytes == rounded &&
                     event.workspace_bytes == backing,
                 "Mapping ledger confused native size with shadow workspace");
         require(owner.observe().mapping_bytes == rounded && owner.observe().runtime_bytes == 0 &&
                     owner.observe().workspace_bytes == configuration.workspace_bytes,
                 "Mapping charge is incomplete");
         owner.unmap(address);
         require(owner.events().back().kind == plan::operation::unmap, "Shadow cleanup was not journaled");
         empty(owner, configuration.workspace_bytes - backing);
      }
      storage.clean();
   }
}

void realloc_peak() {
   auto storage = memory{};
   const auto configuration = limits(128);
   {
      auto owner = plan{configuration, storage.allocator()};
      auto* address = owner.allocate(64);
      require(address, "Initial realloc block failed");
      std::memset(address, 0xa5, 64);
      const auto calls = storage.calls;
      require(!owner.reallocate(address, 128) && owner.error() == plan::failure::workspace_limit &&
                  storage.calls == calls,
              "Realloc ignored simultaneous old and new allocation charges");
      require(static_cast<unsigned char*>(address)[63] == 0xa5, "Failed realloc invalidated the original block");
      owner.release(address);
      empty(owner, configuration.workspace_bytes - 128);
   }
   storage.clean();
}

void realloc_success() {
   auto storage = memory{};
   const auto configuration = limits(192);
   {
      auto owner = plan{configuration, storage.allocator()};
      auto* address = owner.allocate(64);
      require(address, "Initial realloc block failed");
      std::memset(address, 0x5a, 64);
      auto* grown = owner.reallocate(address, 128);
      require(grown && static_cast<unsigned char*>(grown)[63] == 0x5a, "Realloc did not preserve old bytes");
      require(owner.observe().workspace_bytes == configuration.workspace_bytes - 64 &&
                  owner.observe().peak_workspace_bytes == configuration.workspace_bytes,
              "Realloc peak/current workspace accounting differs");
      const auto log = owner.events();
      require(log.size() == 2 && log[0].id == log[1].id && log[1].kind == plan::operation::reallocate &&
                  log[1].previous_bytes == 64 && log[1].retained_bytes == 128,
              "Realloc identity or event sizes differ");
      owner.release(grown);
   }
   storage.clean();
}

void realloc_failure() {
   auto storage = memory{};
   storage.fail_at = 3;
   {
      auto owner = plan{limits(192), storage.allocator()};
      auto* address = owner.allocate(64);
      require(address, "Initial allocation failed");
      std::memset(address, 0x5a, 64);
      const auto before = owner.observe();
      require(!owner.reallocate(address, 128) && owner.error() == plan::failure::allocation,
              "Injected realloc OOM escaped");
      require(owner.observe().workspace_bytes == before.workspace_bytes && owner.events().size() == before.events &&
                  static_cast<unsigned char*>(address)[63] == 0x5a,
              "Realloc OOM lost original ownership");
      owner.release(address);
   }
   storage.clean();
}

void realloc_cancellation() {
   auto storage = memory{};
   storage.stop_at = 3;
   const auto configuration = limits(144);
   {
      auto owner = plan{configuration, storage.allocator(), storage.stop.get_token()};
      auto* original = owner.allocate(48);
      require(original, "Initial cancellation fixture allocation failed");
      std::memset(original, 0x5a, 48);
      const auto before = owner.observe();
      const auto allocated = storage.bytes;
      require(!owner.reallocate(original, 96) && owner.error() == plan::failure::cancelled,
              "Replacement allocation cancellation was not reported");
      require(storage.owns(original, 48) && owner.observe().runtime_bytes == 48 && owner.observe().live_blocks == 1,
              "Cancelled resize lost the original pointer or its 48-byte ownership");
      require(storage.bytes == allocated && owner.observe().workspace_bytes == before.workspace_bytes &&
                  owner.events().size() == before.events && storage.releases == 1,
              "Cancelled resize retained replacement storage, charge, or a committed resize event");
      const auto* bytes = static_cast<const unsigned char*>(original);
      require(std::all_of(bytes, bytes + 48, [](auto value) { return value == 0x5a; }),
              "Cancelled resize changed the original data");
      owner.rollback();
      empty(owner, configuration.workspace_bytes - 144);
      require(owner.events().back().kind == plan::operation::release && owner.events().back().retained_bytes == 48,
              "Cancelled resize rollback did not release the original block");
   }
   storage.clean();

   auto late = memory{};
   {
      auto owner = plan{configuration, late.allocator(), late.stop.get_token()};
      auto* original = owner.allocate(48);
      require(original, "Initial committed resize fixture allocation failed");
      std::memset(original, 0x5a, 48);
      late.stop_on_release = true;
      auto* replacement = owner.reallocate(original, 96);
      require(replacement && late.stop.stop_requested() && owner.error() == plan::failure::none,
              "Post-commit cancellation returned failure after the original was freed");
      require(late.owns(replacement, 96) && !late.owns(original, 48) && owner.observe().runtime_bytes == 96 &&
                  owner.observe().live_blocks == 1 && static_cast<unsigned char*>(replacement)[47] == 0x5a,
              "Committed resize did not return the new owned pointer and data");
      require(!owner.allocate(1) && owner.error() == plan::failure::cancelled,
              "Operation after committed resize ignored pending cancellation");
      owner.rollback();
      empty(owner, configuration.workspace_bytes - 144);
   }
   late.clean();
}

void scripted_load(plan& owner) {
   auto* metadata = owner.allocate(48);
   if (!metadata)
      return;
   auto* code = owner.map(65, true);
   if (!code)
      return;
   metadata = owner.reallocate(metadata, 96);
   if (!metadata)
      return;
   auto* scratch = owner.allocate(7);
   if (!scratch)
      return;
   owner.release(scratch);
   owner.unmap(code);
   owner.release(metadata);
}

void allocation_failures() {
   for (std::size_t fail = 1; fail <= 6; ++fail) {
      auto storage = memory{};
      storage.fail_at = fail;
      const auto configuration = limits(32768);
      {
         auto owner = plan{configuration, storage.allocator()};
         scripted_load(owner);
         require(owner.error() == (fail <= 5 ? plan::failure::allocation : plan::failure::none),
                 "Allocation failpoint was not observed");
         owner.rollback();
         empty(owner, fail == 1 ? 0 : configuration.workspace_bytes - 32768);
         const auto calls = storage.calls;
         if (fail <= 5)
            require(!owner.allocate(1) && storage.calls == calls, "OOM owner silently resumed allocation");
         require(owner.observe().peak_workspace_bytes <= configuration.workspace_bytes,
                 "OOM exceeded explicit workspace");
      }
      storage.clean();
   }
}

void cancellation() {
   auto pre_cancelled = memory{};
   pre_cancelled.stop.request_stop();
   {
      auto owner = plan{limits(32768), pre_cancelled.allocator(), pre_cancelled.stop.get_token()};
      require(owner.error() == plan::failure::cancelled && !pre_cancelled.calls,
              "Pre-cancelled planner allocated storage");
   }
   pre_cancelled.clean();
   for (std::size_t call = 1; call <= 5; ++call) {
      auto storage = memory{};
      storage.stop_at = call;
      const auto configuration = limits(32768);
      {
         auto owner = plan{configuration, storage.allocator(), storage.stop.get_token()};
         scripted_load(owner);
         require(owner.error() == plan::failure::cancelled, "Cancellation during allocation was lost");
         owner.rollback();
         empty(owner, configuration.workspace_bytes - 32768);
         require(owner.observe().peak_workspace_bytes <= configuration.workspace_bytes,
                 "Cancelled planner exceeded workspace");
      }
      storage.clean();
   }
}

void invalid_requests() {
   for (const auto page : {0U, 3U, 1U}) {
      auto storage = memory{};
      {
         auto owner = plan{limits(32768, 16, 4, page), storage.allocator()};
         require(owner.error() == plan::failure::invalid_request && !storage.calls,
                 "Invalid page geometry allocated metadata");
      }
      storage.clean();
   }
   auto storage = memory{};
   {
      auto owner = plan{limits(32768), storage.allocator()};
      require(!owner.map(0, true) && owner.error() == plan::failure::invalid_request && storage.calls == 1,
              "Zero mapping was not rejected before allocation");
   }
   storage.clean();
   {
      auto owner = plan{limits(32768), storage.allocator()};
      auto* address = owner.allocate(0);
      require(address && owner.events()[0].requested_bytes == 0 && owner.events()[0].retained_bytes == 1,
              "Runtime zero allocation was not normalized and accounted");
      owner.unmap(address);
      require(owner.error() == plan::failure::invalid_request && owner.observe().live_blocks == 1,
              "Mismatched unmap freed a runtime allocation");
   }
   storage.clean();
   {
      auto owner = plan{limits(32768), storage.allocator()};
      char foreign = 0;
      owner.release(&foreign);
      require(owner.error() == plan::failure::invalid_request && !storage.invalid_free,
              "Foreign pointer reached allocator free");
   }
   storage.clean();
}

void overflow() {
   require(!plan::base_workspace_bytes(std::numeric_limits<std::size_t>::max(), 1), "Event storage size overflowed");
   require(!plan::base_workspace_bytes(1, std::numeric_limits<std::size_t>::max()), "Block storage size overflowed");
   require(!plan::base_workspace_bytes(0, 1) && !plan::base_workspace_bytes(1, 0),
           "Zero journal geometry was accepted");
   auto storage = memory{};
   {
      auto owner = plan{limits(32768), storage.allocator()};
      require(!owner.map(UINT64_MAX, true) && owner.error() == plan::failure::overflow && storage.calls == 1,
              "Page rounding overflow reached the allocator");
   }
   storage.clean();
}

void isolation() {
   auto first = memory{}, second = memory{};
   {
      auto a = plan{limits(64), first.allocator()};
      auto b = plan{limits(64), second.allocator()};
      auto* x = a.allocate(32);
      auto* y = b.allocate(32);
      require(x && y, "Independent planner allocation failed");
      b.release(x);
      require(b.error() == plan::failure::invalid_request && a.error() == plan::failure::none &&
                  a.observe().live_blocks == 1 && b.observe().live_blocks == 1,
              "One planner released another owner's allocation");
      a.release(x);
      b.release(y);
   }
   first.clean();
   second.clean();
}

void rollback() {
   auto storage = memory{};
   const auto configuration = limits(32768);
   {
      auto owner = plan{configuration, storage.allocator()};
      require(owner.allocate(8) && owner.map(65, false), "Rollback fixture failed");
      owner.rollback();
      empty(owner, configuration.workspace_bytes - 32768);
      const auto count = owner.events().size();
      const auto calls = storage.calls;
      owner.rollback();
      require(owner.events().size() == count && storage.calls == calls, "Rollback was not idempotent/allocation-free");
      require(owner.events()[2].kind == plan::operation::unmap && owner.events()[3].kind == plan::operation::release,
              "Rollback cleanup events have the wrong ownership kind");
   }
   storage.clean();
   {
      auto owner = plan{configuration, storage.allocator()};
      require(owner.allocate(8) && owner.map(65, true), "Scope-exit fixture failed");
   }
   storage.clean();
}

} // namespace

int main(int argc, char** argv) {
   try {
      if (argc != 2)
         return 2;
      const auto name = std::string_view{argv[1]};
      struct test {
         std::string_view name;
         void (*run)();
      };
      const auto cases = std::array{test{"metadata_limit", &metadata_limit},
                                    test{"exact_limit", &exact_limit},
                                    test{"ledger_limit", &ledger_limit},
                                    test{"live_limit", &live_limit},
                                    test{"shadow_maps", &shadow_maps},
                                    test{"realloc_peak", &realloc_peak},
                                    test{"realloc_success", &realloc_success},
                                    test{"realloc_failure", &realloc_failure},
                                    test{"realloc_cancellation", &realloc_cancellation},
                                    test{"allocation_failures", &allocation_failures},
                                    test{"cancellation", &cancellation},
                                    test{"invalid_requests", &invalid_requests},
                                    test{"overflow", &overflow},
                                    test{"isolation", &isolation},
                                    test{"rollback", &rollback}};
      for (const auto& candidate : cases) {
         if (candidate.name != name)
            continue;
         candidate.run();
         std::cout << "PASS " << name << '\n';
         return 0;
      }
      return 2;
   } catch (const std::exception& error) {
      std::cerr << "FAIL " << error.what() << '\n';
      return 1;
   }
}
