#include "../../libraries/vm/wasm/interpret/details/engine_module.hxx"
#include "../../libraries/vm/wasm/interpret/details/engine_instance.hxx"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <thread>
#include <vector>
#include <platform_api_vmcore.h>
#include <aot_runtime.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

namespace {
namespace engine = forge::vm::wasm::interpret::detail;
using budget = engine::engine_load_budget;

void require(bool value, const char* message) {
   if (!value)
      throw std::runtime_error(message);
}

struct late_cleanup_probe {
   std::atomic<bool> deny_unmap{false};
   std::shared_ptr<budget> owner;
   std::weak_ptr<engine::engine_runtime> runtime;
   ~late_cleanup_probe() {
      if (!owner)
         return;
      if (!runtime.expired() || !owner->retry_cleanup() || owner->current_bytes() != owner->root_bytes())
         std::abort();
      std::fputs("PASS cleanup owner retry after runtime destruction\n", stdout);
   }
};

late_cleanup_probe& late_cleanup() {
   // Register before the runtime singleton, so this destructor runs afterwards.
   static late_cleanup_probe probe;
   return probe;
}

struct permissions {
   bool read, write, execute;
};
permissions query_permissions(const void* pointer) {
   const auto address = reinterpret_cast<std::uintptr_t>(pointer);
#if defined(__APPLE__)
   auto start = static_cast<mach_vm_address_t>(address);
   mach_vm_size_t size = 0;
   vm_region_basic_info_data_64_t info{};
   auto count = mach_msg_type_number_t{VM_REGION_BASIC_INFO_COUNT_64};
   mach_port_t object = MACH_PORT_NULL;
   const auto result = mach_vm_region(mach_task_self(), &start, &size, VM_REGION_BASIC_INFO_64,
                                      reinterpret_cast<vm_region_info_t>(&info), &count, &object);
   if (object != MACH_PORT_NULL)
      mach_port_deallocate(mach_task_self(), object);
   require(result == KERN_SUCCESS && start <= address && address - start < size,
           "Cannot query actual native mapping permissions");
   return {bool(info.protection & VM_PROT_READ), bool(info.protection & VM_PROT_WRITE),
           bool(info.protection & VM_PROT_EXECUTE)};
#elif defined(__linux__)
   auto maps = std::ifstream{"/proc/self/maps"};
   for (std::string line; std::getline(maps, line);) {
      unsigned long long begin = 0, end = 0;
      char flags[5]{};
      if (std::sscanf(line.c_str(), "%llx-%llx %4s", &begin, &end, flags) == 3 && address >= begin && address < end)
         return {flags[0] == 'r', flags[1] == 'w', flags[2] == 'x'};
   }
   throw std::runtime_error("Cannot find actual native mapping permissions");
#else
   throw std::runtime_error("Native mapping permission query is unqualified");
#endif
}

void verify_data_pages(const void* data, std::size_t bytes) {
   const auto page = static_cast<std::size_t>(os_getpagesize());
   for (std::size_t offset = 0; offset < bytes; offset += page) {
      const auto flags = query_permissions(static_cast<const std::byte*>(data) + offset);
      require(flags.read && !flags.execute, "Native data/default section/padding is executable");
   }
}

void verify_module_protections(const engine::engine_module& loaded) {
   const auto& module = *reinterpret_cast<const AOTModule*>(loaded.get());
   const auto policy = wasm_runtime_get_native_code_policy(loaded.get());
   require(policy == (module.func_count == 0 ? WASM_NATIVE_CODE_NONE
                      : module.load_jit_code ? WASM_NATIVE_CODE_MAP_JIT
                                             : WASM_NATIVE_CODE_RX),
           "AOT module policy differs from its committed native mapping mechanism");
   if (module.code_size) {
      const auto flags = query_permissions(module.code);
      require(flags.read && flags.execute, "AOT text is not executable");
#if defined(__APPLE__) && defined(__aarch64__)
      if (module.load_jit_code)
         require(os_thread_jit_write_protect_supported() && os_thread_jit_write_protect_state(),
                 "Darwin code did not retain its supported MAP_JIT per-thread W^X mechanism");
      else
         require(!flags.write, "Ordinary Darwin AOT text retained write permission");
#else
      require(!flags.write, "AOT text retained write permission");
#endif
   }
   if (module.merged_data_sections)
      verify_data_pages(module.merged_data_sections, module.merged_data_sections_size);
   if (module.merged_data_text_sections) {
      const auto page = static_cast<std::size_t>(os_getpagesize());
      const auto text = (sizeof(std::uint32_t) + module.literal_size + module.code_size + page - 1) & ~(page - 1);
      require(text <= module.merged_data_text_sections_size, "Merged text extent is invalid");
      verify_data_pages(module.merged_data_text_sections + text, module.merged_data_text_sections_size - text);
   }
   for (std::uint32_t i = 0; i < module.data_section_count; ++i) {
      const auto& data = module.data_sections[i];
      verify_data_pages(data.data, data.size);
      const auto name = std::string_view{data.name};
      const auto readonly =
          name == ".rdata" || name == ".rodata" || name.starts_with(".rodata.cst") || name.starts_with(".rodata.str");
      for (std::size_t offset = 0; offset < data.size; offset += static_cast<std::size_t>(os_getpagesize()))
         require(query_permissions(data.data + offset).write != readonly,
                 "Native data/default/metadata section did not preserve its requested RW or R policy");
   }
}
std::vector<std::uint8_t> read(const char* path) {
   auto input = std::ifstream(path, std::ios::binary);
   require(bool(input), "Cannot read supplied real loader fixture");
   return {std::istreambuf_iterator<char>{input}, {}};
}

void bootstrap_and_alias_charge() {
   const auto untouched = engine::engine_runtime::retry_pending_cleanup(1);
   require(untouched.attempted == 0 && untouched.remaining == 0, "Cleanup initialized a fresh runtime");
   const auto root = budget::root_byte_size();
   auto rejected = false;
   try {
      static_cast<void>(budget::create(root - 1));
   } catch (const budget::error& error) {
      rejected = error.reason == budget::failure::limit;
   }
   require(rejected, "Root/control allocation ignored caller ceiling");
   auto insufficient = budget::faults{};
   insufficient.root_control_limit = 1;
   rejected = false;
   try {
      static_cast<void>(budget::create(root, {}, insufficient));
   } catch (const budget::error& error) {
      rejected = error.reason == budget::failure::invalid;
   }
   require(rejected, "Undersized physical control slab was not a typed refusal");
   auto owner = budget::create(root);
   require(owner->root_bytes() == root && owner->current_bytes() == root && owner->peak_bytes() == root,
           "Physical root/control-slab requested-storage contract differs");
   auto token = std::make_shared<int>(0);
   auto token_view = std::weak_ptr<int>{token};
   owner->pin_charge(token);
   token.reset();
   auto weak_root = std::weak_ptr<budget>{owner};
   owner.reset();
   require(!token_view.expired(), "Charge dropped before weak-pinned physical control storage");
   weak_root.reset();
   require(token_view.expired(), "Charge did not drop after physical root/control storage");
   auto sealed = budget::create(root + 1024);
   sealed->seal(root + 512);
   require(sealed->limit_bytes() == root, "Seal left refunded headroom available");
   require(!sealed->reserve(1) && sealed->status() == budget::failure::limit,
           "Sealed owner allocated past its actual retained charge");
}

std::atomic<bool> refuse_unmap{true};
int controlled_unmap(void*, std::size_t) {
   return refuse_unmap ? -1 : 0;
}

void failed_mapping_keeps_physical_owner() {
   auto owner = budget::create(1024 * 1024);
   auto token = std::make_shared<int>(0);
   auto token_view = std::weak_ptr<int>{token};
   owner->pin_charge(token);
   token.reset();
   auto weak = std::weak_ptr<budget>{owner};
   auto* record = budget::begin_mapping(owner, 4096, 8192, &controlled_unmap);
   require(record, "Could not precharge mapping record and temporary peak");
   auto* address = reinterpret_cast<void*>(std::uintptr_t{4096});
   owner->complete_mapping(record, address);
   const auto retained = owner->current_bytes();
   require(!owner->unmap(address, 4096) && owner->status() == budget::failure::cleanup,
           "Failed unmap was not a typed cleanup refusal");
   require(owner->current_bytes() == retained && !owner->retry_cleanup(), "Failed unmap refunded live mapping storage");
   owner.reset();
   require(!weak.expired() && !token_view.expired(), "Failed native unmap dropped owner or admission token");
   refuse_unmap = false;
   auto cleanup_owner = weak.lock();
   std::thread cleanup{
       [cleanup_owner] { require(cleanup_owner->retry_cleanup(), "Cross-thread failed-map retry did not succeed"); }};
   cleanup.join();
   require(cleanup_owner->current_bytes() == cleanup_owner->root_bytes(),
           "Retry retained mapping record/storage charge");
   cleanup_owner.reset();
   require(weak.expired() && !token_view.expired(), "Final weak root did not retain physical bootstrap charge");
   weak.reset();
   require(token_view.expired(), "Cleanup completed but admission token remained pinned");
}

void resize_and_thread_ownership() {
   const auto root = budget::root_byte_size();
   const auto header = budget::allocation_overhead();
   auto owner = budget::create(root + header + 96);
   void* original;
   {
      const budget::scope scope{owner};
      original = budget::allocate(48);
      require(original, "Initial real runtime allocation failed");
      std::memset(original, 0x5a, 48);
      require(!budget::reallocate(original, 96) && owner->status() == budget::failure::limit,
              "Resize ignored old+new prefix/storage overlap");
      require(owner->current_bytes() == root + header + 48 && static_cast<unsigned char*>(original)[47] == 0x5a,
              "Refused runtime resize destroyed the old allocation");
   }
   std::thread release{[original] { budget::free(original); }};
   release.join();
   require(owner->current_bytes() == root, "Cross-thread free lost the original owner");

   auto stop = std::stop_source{};
   auto faults = budget::faults{};
   faults.cancel_allocation = 2;
   faults.cancel_source = &stop;
   auto cancelled = budget::create(root + 2 * header + 144, stop.get_token(), faults);
   {
      const budget::scope scope{cancelled};
      auto* old = budget::allocate(48);
      require(old, "Initial cancellation allocation failed");
      std::memset(old, 0xa5, 48);
      require(!budget::reallocate(old, 96) && cancelled->status() == budget::failure::cancelled,
              "Allocation cancellation did not refuse runtime resize");
      require(cancelled->current_bytes() == root + header + 48 && static_cast<unsigned char*>(old)[47] == 0xa5,
              "Cancelled real runtime resize invalidated the old pointer/data/charge");
      budget::free(old);
   }
   require(cancelled->current_bytes() == root, "Cancelled runtime resize leaked replacement storage");
   auto aligned = budget::create(root + header + 32 + 255);
   {
      const budget::scope scope{aligned};
      auto* value = budget::allocate(32, 256);
      require(value && reinterpret_cast<std::uintptr_t>(value) % 256 == 0,
              "Bounded C++ storage did not preserve requested alignment");
      require(aligned->current_bytes() == aligned->limit_bytes(), "Alignment slack/header was not precharged");
      budget::free(value);
   }
   require(aligned->current_bytes() == root, "Aligned storage released the wrong backing charge");

   auto early_stop = std::stop_source{};
   auto early_fault = budget::faults{};
   early_fault.cancel_allocation = 1;
   early_fault.cancel_source = &early_stop;
   auto early = budget::create(root + header + 48, early_stop.get_token(), early_fault);
   auto weak_early = std::weak_ptr<budget>{early};
   {
      const budget::scope scope{early};
      require(!budget::allocate(48) && early->status() == budget::failure::cancelled && early->current_bytes() == root,
              "Cancelled allocation retained a provisional block/prefix charge");
   }
   early.reset();
   require(weak_early.expired(), "Cancelled backing allocation orphaned a shared owner self-pin");
   weak_early.reset();
}

std::uint64_t load(const std::shared_ptr<engine::engine_runtime>& runtime, const std::vector<std::uint8_t>& source,
                   const std::vector<std::uint8_t>& native, const std::shared_ptr<budget>& owner,
                   bool threaded = false) {
   auto original = std::make_unique<engine::engine_module>(runtime, source, nullptr, true, false, owner);
   auto compiled = std::make_unique<engine::engine_module>(runtime, native, nullptr, true, false, owner);
   require(original->load_budget() == owner && compiled->load_budget() == owner, "Source/AOT have separate counters");
   require(owner->current_bytes() <= owner->limit_bytes() && owner->peak_bytes() <= owner->limit_bytes(),
           "Actual loader storage exceeded the supplied ceiling");
   const auto peak = owner->peak_bytes();
   if (threaded) {
      std::thread unload{[original = std::move(original), compiled = std::move(compiled)]() mutable {
         compiled.reset();
         original.reset();
      }};
      unload.join();
   } else {
      compiled.reset();
      original.reset();
   }
   require(owner->current_bytes() == owner->root_bytes(), "Actual WASM/AOT unload retained heap/mapping storage");
   owner->check();
   return peak;
}

void sweep(const std::shared_ptr<engine::engine_runtime>& runtime, const std::vector<std::uint8_t>& source,
           const std::vector<std::uint8_t>& native, unsigned kind) {
   bool finished = false;
   std::size_t refusals = 0;
   for (std::uint64_t step = 1; step <= 512; ++step) {
      auto stop = std::stop_source{};
      auto fault = budget::faults{};
      if (kind == 0)
         fault.allocation = step;
      if (kind == 1)
         fault.mapping = step;
      if (kind == 2)
         fault.protection = step;
      if (kind == 3) {
         fault.cancel_allocation = step;
         fault.cancel_source = &stop;
      }
      if (kind == 4)
         fault.unmapping = step;
      // An explicit test caller ceiling, not a production estimate or default.
      auto owner = budget::create(64ULL * 1024 * 1024, stop.get_token(), fault);
      const auto protection = step % 2 == 0;
      os_thread_jit_write_protect_np(protection);
      try {
         load(runtime, source, native, owner);
         finished = true;
      } catch (const budget::error& error) {
         const auto expected = kind == 2   ? budget::failure::protection
                               : kind == 3 ? budget::failure::cancelled
                               : kind == 4 ? budget::failure::cleanup
                                           : budget::failure::allocation;
         require(error.reason == expected, "Loader fault lost its typed owner reason");
         ++refusals;
      }
      require(os_thread_jit_write_protect_state() == protection, "AOT load/failure changed outer WAMR JIT protection");
      os_thread_jit_write_protect_np(true);
      require(owner->current_bytes() == owner->root_bytes(), "Loader fault cleanup leaked charged heap/maps");
      require(owner->peak_bytes() <= owner->limit_bytes(), "Loader fault escaped its precharged ceiling");
      if (finished)
         break;
   }
   require(finished && refusals > 0, "Actual loader fault sweep did not reach success after all failpoints");
   std::cout << "PASS actual loader failpoints kind=" << kind << " refusals=" << refusals << '\n';
}

void module_lifetime(const std::shared_ptr<engine::engine_runtime>& runtime, const std::vector<std::uint8_t>& source,
                     const std::vector<std::uint8_t>& native) {
   auto owner = budget::create(64ULL * 1024 * 1024);
   auto token = std::make_shared<int>(0);
   auto token_view = std::weak_ptr<int>{token};
   owner->pin_charge(token);
   token.reset();
   auto weak = std::weak_ptr<budget>{owner};
   auto original = std::make_unique<engine::engine_module>(runtime, source, nullptr, true, false, owner);
   auto compiled = std::make_unique<engine::engine_module>(runtime, native, nullptr, true, false, owner);
   verify_module_protections(*compiled);
   owner.reset();
   require(!weak.expired() && !token_view.expired(), "Module did not retain the physical accounting owner");
   std::thread unload{[original = std::move(original), compiled = std::move(compiled)]() mutable {
      compiled.reset();
      original.reset();
   }};
   unload.join();
   require(weak.expired() && !token_view.expired(), "Unload lost storage charge before final weak control pin");
   weak.reset();
   require(token_view.expired(), "Cross-thread unload retained an orphaned physical owner");

   engine::engine_module legacy_source{runtime, source, nullptr, true};
   engine::engine_module legacy_native{runtime, native, nullptr, true};
   require(!legacy_source.load_budget() && !legacy_native.load_budget(),
           "Legacy constructors changed their owner policy");
   verify_module_protections(legacy_native);
}

void aot_policy_lifecycle(const std::shared_ptr<engine::engine_runtime>& runtime,
                          const std::vector<std::uint8_t>& native) {
   auto owner = budget::create(64ULL * 1024 * 1024);
   auto compiled = std::make_shared<engine::engine_module>(runtime, native, nullptr, true, false, owner);
   auto& module = *reinterpret_cast<AOTModule*>(compiled->get());
   require(module.func_count != 0, "AOT policy fixture must contain real native guest bodies");
   const auto policy = wasm_runtime_get_native_code_policy(compiled->get());
   require(policy == WASM_NATIVE_CODE_RX || policy == WASM_NATIVE_CODE_MAP_JIT,
           "Loaded AOT bodies have no committed protection policy");
   for (const auto invalid :
        {WASM_NATIVE_CODE_UNCOMMITTED, WASM_NATIVE_CODE_NONE, static_cast<WASMNativeCodePolicy>(255)}) {
      module.native_code_policy = invalid; // Test-only corruption, never a capability override.
      bool refused = false;
      try {
         engine::engine_instance rejected(compiled, 64 * 1024);
      } catch (const engine::engine_instance::unavailable&) {
         refused = true;
      }
      module.native_code_policy = policy;
      require(refused, "Invalid AOT policy reached guest initialization");
   }
   const auto initial = os_thread_jit_write_protect_state();
   struct restore_state {
      bool value;
      ~restore_state() {
         os_thread_jit_write_protect_np(value);
      }
   } restore{initial};
   for (const bool previous : {false, true}) {
      os_thread_jit_write_protect_np(previous);
      {
         engine::engine_instance initialized(compiled, 64 * 1024);
      }
      require(os_thread_jit_write_protect_state() == previous,
              "AOT initialization changed its caller protection state");
      std::stop_source stop;
      stop.request_stop();
      bool cancelled = false;
      try {
         engine::engine_instance rejected(compiled, 64 * 1024, stop.get_token());
      } catch (const engine::engine_instance::canceled&) {
         cancelled = true;
      }
      require(cancelled && os_thread_jit_write_protect_state() == previous,
              "AOT cancellation changed caller protection or was ignored");
      {
         engine::engine_instance fresh(compiled, 64 * 1024);
      }
      require(wasm_runtime_get_native_code_policy(compiled->get()) == policy &&
                  os_thread_jit_write_protect_state() == previous,
              "Fresh AOT initialization changed policy or caller protection");
   }
   std::thread unload{[compiled = std::move(compiled)]() mutable { compiled.reset(); }};
   unload.join();
   require(owner->current_bytes() == owner->root_bytes(), "AOT lifecycle retained charged module storage");
   std::cout << "PASS AOT policy refusal, initialization, cancellation and caller-state restoration\n";
}

void large_native_data_mapping() {
   constexpr auto requested = std::size_t{2 * 1024 * 1024 + 1};
   std::size_t retained = 0, peak = 0;
   require(os_mmap_sizes(requested, MMAP_MAP_NO_HUGE, &retained, &peak) && retained == peak,
           "Bounded large map secretly needs huge-page trim overlap");
   auto owner = budget::create(4ULL * 1024 * 1024);
   auto* record = budget::begin_mapping(owner, retained, peak, &os_munmap_checked);
   require(record, "Large native mapping precharge failed");
   auto* address = os_mmap(nullptr, requested, MMAP_PROT_READ | MMAP_PROT_WRITE, MMAP_MAP_NO_HUGE | MMAP_MAP_NO_JIT,
                           os_get_invalid_handle());
   owner->complete_mapping(record, address);
   require(address, "Large native data mapping failed");
   verify_data_pages(address, retained);
   require(owner->unmap(address, retained) && owner->current_bytes() == owner->root_bytes(),
           "Large native data mapping rollback failed");
   std::cout << "PASS actual nonexecuting native data pages >=2MiB, retained=" << retained << '\n';
}

void repeated_real_module_cleanup(const std::shared_ptr<engine::engine_runtime>& runtime,
                                  const std::vector<std::uint8_t>& native) {
   require(runtime->pending_cleanup() == 0, "Previous test left runtime cleanup pending");
   auto gate = std::atomic<bool>{false};
   auto attempts = std::atomic<std::uint64_t>{0};
   auto fault = budget::faults{};
   fault.hold_unmapping = &gate;
   fault.unmapping_attempts = &attempts;
   const budget::fault_scope injected{fault};
   std::shared_ptr<budget> owner;
   std::thread create{[&] { owner = budget::create(64ULL * 1024 * 1024); }};
   create.join();
   auto token = std::make_shared<int>(0);
   auto token_view = std::weak_ptr<int>{token};
   owner->pin_charge(token);
   token.reset();
   auto compiled = std::make_unique<engine::engine_module>(runtime, native, nullptr, true, false, owner);
   verify_module_protections(*compiled);
   owner.reset();
   attempts = 0;
   gate = true;
   std::thread unload{[compiled = std::move(compiled)]() mutable { compiled.reset(); }};
   unload.join();
   require(attempts >= 2 && runtime->pending_cleanup() == 1 && !token_view.expired(),
           "Repeated real module unmap failure lost its reachable physical cleanup owner/token");
   const auto idle = engine::engine_runtime::retry_pending_cleanup(0);
   require(idle.attempted == 0 && idle.remaining == 1, "Zero cleanup work bound was not respected");
   const auto refused = engine::engine_runtime::retry_pending_cleanup(1);
   require(refused.attempted == 1 && refused.remaining == 1 && !token_view.expired(),
           "Runtime retry lost a still-refused physical cleanup root");
   gate = false;
   // No preparation, module, artifact or weak-root reference remains here.
   const auto recovered = engine::engine_runtime::retry_pending_cleanup(1);
   require(recovered.attempted == 1 && recovered.remaining == 0 && token_view.expired(),
           "Runtime could not recover real module cleanup after every caller reference was dropped");
   require(runtime->retry_cleanup(1).remaining == 0, "Completed runtime cleanup is not idempotent");
   std::cout << "PASS reachable repeated real module unload cleanup, attempts=" << attempts << '\n';
}

void arm_late_cleanup(const std::shared_ptr<engine::engine_runtime>& runtime, const std::vector<std::uint8_t>& native) {
   auto& late = late_cleanup();
   auto faults = budget::faults{};
   faults.hold_unmapping = &late.deny_unmap;
   late.owner = budget::create(64ULL * 1024 * 1024, {}, faults);
   late.runtime = runtime;
   auto module = std::make_unique<engine::engine_module>(runtime, native, nullptr, true, false, late.owner);
   late.deny_unmap = true;
   module.reset();
   require(runtime->pending_cleanup() == 1, "Late retry fixture never registered failed module cleanup");
   late.deny_unmap = false;
   require(late.owner->retry_cleanup() && runtime->pending_cleanup() == 0,
           "Late retry fixture did not retire its runtime registry entry");
   require(late.owner->current_bytes() == late.owner->root_bytes(), "Late retry fixture still owns native storage");
}

} // namespace

int main(int argc, char** argv) {
   static_cast<void>(late_cleanup());
   try {
      if (argc != 3)
         return 2;
      bootstrap_and_alias_charge();
      std::cout << "PASS physical root/control-slab bootstrap and final weak pin\n";
      resize_and_thread_ownership();
      std::cout << "PASS realloc/cancel/alignment and cross-thread ownership\n";
      failed_mapping_keeps_physical_owner();
      std::cout << "PASS failed native unmap retains physical owner and charge\n";
      auto runtime = engine::engine_runtime::acquire(); // Global setup is outside per-module storage.
      const auto source = read(argv[1]);
      const auto native = read(argv[2]);
      module_lifetime(runtime, source, native);
      aot_policy_lifecycle(runtime, native);
      large_native_data_mapping();
      repeated_real_module_cleanup(runtime, native);
      std::cout << "PASS cross-thread module/weak/token lifetime and legacy loading\n";
      auto baseline = budget::create(64ULL * 1024 * 1024);
      const auto peak = load(runtime, source, native, baseline, true);
      std::cout << "PASS actual source/AOT baseline, peak=" << peak << '\n';
      require(peak > baseline->root_bytes(), "Fixture did not exercise actual module loading");
      auto exact = budget::create(peak);
      load(runtime, source, native, exact);
      auto shortfall = budget::create(peak - 1);
      auto refused = false;
      try {
         load(runtime, source, native, shortfall);
      } catch (const budget::error& error) {
         refused = error.reason == budget::failure::limit;
      }
      require(refused && shortfall->current_bytes() == shortfall->root_bytes(),
              "One-byte loader peak excess was admitted/leaked");
      for (unsigned kind = 0; kind < 5; ++kind)
         sweep(runtime, source, native, kind);
      // A valid load after every protect failure also exercises the restored thread JIT state.
      load(runtime, source, native, budget::create(64ULL * 1024 * 1024));
      arm_late_cleanup(runtime, native);
      std::cout << "PASS shared bounded WASM+AOT loader; peak=" << peak << '\n';
      return 0;
   } catch (const budget::error& error) {
      std::cerr << "FAIL typed budget reason=" << static_cast<unsigned>(error.reason) << " errno=" << errno << '\n';
      return 1;
   } catch (const std::exception& error) {
      std::cerr << "FAIL " << error.what() << '\n';
      return 1;
   }
}
