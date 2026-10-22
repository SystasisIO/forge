#include <wasm_export.h>
#include <wasm_exec_env.h>
#include <wasm_runtime.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
constexpr unsigned allocation_size = 17 * sizeof(std::uint64_t);
bool armed{};
unsigned rejected{}, host_calls{};
bool arguments_valid{true};
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
void* allocate(
#if WASM_MEM_ALLOC_WITH_USAGE != 0
   mem_alloc_usage_t,
#endif
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
   void*,
#endif
   unsigned size) {
   if (armed && size == allocation_size) {
      armed = false;
      ++rejected;
      return nullptr;
   }
   return std::malloc(size);
}
void* reallocate(
#if WASM_MEM_ALLOC_WITH_USAGE != 0
   mem_alloc_usage_t, bool,
#endif
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
   void*,
#endif
   void* ptr, unsigned size) { return std::realloc(ptr, size); }
void release(
#if WASM_MEM_ALLOC_WITH_USAGE != 0
   mem_alloc_usage_t,
#endif
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
   void*,
#endif
   void* ptr) { std::free(ptr); }
template<unsigned Count>
void raw(wasm_exec_env_t, std::uint64_t* arguments) {
   ++host_calls;
   for (unsigned i = 0; i < Count; ++i) arguments_valid &= arguments[i] == i + 1;
   arguments[0] = Count * (Count + 1) / 2;
}
struct runtime {
   runtime() {
      RuntimeInitArgs args{};
      args.mem_alloc_type = Alloc_With_Allocator;
      args.mem_alloc_option.allocator.malloc_func = reinterpret_cast<void*>(allocate);
      args.mem_alloc_option.allocator.realloc_func = reinterpret_cast<void*>(reallocate);
      args.mem_alloc_option.allocator.free_func = reinterpret_cast<void*>(release);
      require(wasm_runtime_full_init(&args), "Runtime initialization failed");
   }
   ~runtime() { wasm_runtime_destroy(); }
};
struct module {
   std::vector<std::uint8_t> bytes;
   wasm_module_t value{};
   explicit module(const char* path) {
      std::ifstream input(path, std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(input), {});
      require(!bytes.empty(), "Empty allocation fixture");
      std::array<char, 256> error{};
      value = wasm_runtime_load(bytes.data(), bytes.size(), error.data(), error.size());
      if (!value) throw std::runtime_error(error.data());
   }
   ~module() { wasm_runtime_unload(value); }
};
struct instance {
   wasm_module_inst_t value{};
   wasm_exec_env_t env{};
   explicit instance(wasm_module_t source) {
      std::array<char, 256> error{};
      value = wasm_runtime_instantiate(source, 128 * 1024, 0, error.data(), error.size());
      if (!value) throw std::runtime_error(error.data());
      env = wasm_runtime_create_exec_env(value, 128 * 1024);
      if (!env) { wasm_runtime_deinstantiate(value); throw std::runtime_error("Environment creation failed"); }
   }
   ~instance() { wasm_runtime_destroy_exec_env(env); wasm_runtime_deinstantiate(value); }
   bool call(const char* name, unsigned count = 0) {
      const auto function = wasm_runtime_lookup_function(value, name);
      require(function != nullptr, "Missing allocation fixture export");
      std::array<std::uint32_t, 17> arguments{};
      for (unsigned i = 0; i < count; ++i) arguments[i] = i + 1;
      const bool result = wasm_runtime_call_wasm(env, function, count, arguments.data());
      require(env->forge_call_depth == 0, "Allocation failure leaked call depth");
      if (result) require(arguments[0] == (count == 16 || std::string_view(name) == "guest16" ? 136 : 153),
         "Raw result changed");
      return result;
   }
};
void test(instance& inst, const char* control, const char* failing, bool guest) {
   armed = true;
   const auto before = host_calls;
   const auto failures = rejected;
   require(inst.call(control, guest ? 0 : 16), "16-argument raw control failed");
   require(armed && rejected == failures && host_calls == before + 1,
      "16-argument control unexpectedly allocated an argument buffer");
   require(!inst.call(failing, guest ? 0 : 17), "Injected allocation failure succeeded");
   require(!armed && rejected == failures + 1, "Expected raw argument allocation was not rejected");
   require(host_calls == before + 1, "Host was called after argument allocation failed");
   const auto info = wasm_runtime_get_exception_info(inst.value);
   std::cout << failing << ": rejected=" << rejected << " host_calls=" << host_calls
      << " status=" << info.status << " id=" << info.id << '\n';
   require(info.status == WASM_EXCEPTION_KNOWN && info.id == EXCE_OUT_OF_MEMORY,
      "Raw argument allocation failure lost its actual OOM ID");
   wasm_runtime_clear_exception(inst.value);
   require(wasm_runtime_get_exception_info(inst.value).status == WASM_EXCEPTION_NONE, "Clear retained OOM info");
   require(inst.call(failing, guest ? 0 : 17), "Healthy raw reuse failed after OOM");
   require(host_calls == before + 2 && arguments_valid, "Raw callback count or arguments changed");
}
}
int main(int argc, char** argv) {
   try {
      require(argc == 2, "Usage: allocation_failure_probe <wasm-or-aot>");
      runtime engine;
      NativeSymbol symbols[]{
         {"raw16", reinterpret_cast<void*>(raw<16>), "(iiiiiiiiiiiiiiii)i", nullptr},
         {"raw17", reinterpret_cast<void*>(raw<17>), "(iiiiiiiiiiiiiiiii)i", nullptr}};
      require(wasm_runtime_register_natives_raw("oom", symbols, 2), "Raw registration failed");
      module source(argv[1]);
      instance inst(source.value);
      test(inst, "raw16", "raw17", false);
      test(inst, "guest16", "guest17", true);
      require(rejected == 2 && host_calls == 4, "Unexpected final injection or callback count");
      std::cout << "PASS raw17 OOM: rejected=2 host_calls=4 (none on failed calls)\n";
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
}
