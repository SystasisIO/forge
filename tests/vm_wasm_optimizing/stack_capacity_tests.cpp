#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

import forge.asio.blocking;
import forge.asio.runtime;
import forge.vm.wasm.optimizing.executor;

namespace {
namespace oc = forge::vm::wasm::optimizing;
namespace vm = forge::vm::wasm::interpret;
using namespace std::chrono_literals;
constexpr auto reserve = std::uint64_t{4 * 1024 * 1024};
constexpr auto anchor = std::uint64_t{32};
constexpr auto small = reserve + 16 * 1024;
constexpr auto intermediate = reserve + 48 * 1024;
constexpr auto adequate = std::uint64_t{8 * 1024 * 1024};
constexpr auto locals = (32768U - 16U) / 8U;

void require(bool value, const char* message) {
   if (!value)
      throw std::runtime_error(message);
}

std::vector<std::uint8_t> read(const char* path) {
   auto input = std::ifstream(path, std::ios::binary);
   require(bool(input), "Cannot read compiled native-stack fixture");
   return {std::istreambuf_iterator<char>(input), {}};
}

oc::artifact load(const char* artifact_path, const char* request_path) {
   const auto request = oc::compile_request::decode(read(request_path), 16 * 1024 * 1024);
   const auto bytes = read(artifact_path);
   auto code =
       oc::artifact::decode(bytes, oc::identify(request), 32 * 1024 * 1024, oc::artifact::origin::isolated_compiler);
   require(code.image().source == request.wasm && code.encode() == bytes,
           "Compiled fixture lost its original source or encoded identity");
   code.prepare();
   return code;
}

struct host {
   std::uint64_t issued = 0, consumed = 0, consumed_sum = 0;
   std::uint32_t marks = 0, last_mark = 0;
   std::uint64_t next() {
      return ++issued;
   }
   void consume(std::uint64_t value) {
      ++consumed;
      consumed_sum += value;
   }
   void mark(std::uint32_t value) {
      ++marks;
      last_mark = value;
   }
   bool untouched() const {
      return issued == 0 && consumed == 0 && consumed_sum == 0 && marks == 0 && last_mark == 0;
   }
};
using registered = vm::registered_host_functions<host>;

std::uint32_t exported(const oc::artifact& code, const char* name) {
   const auto found = std::ranges::find(code.exports(), name, &oc::compiled_export::name);
   require(found != code.exports().end(), "Compiled fixture is missing its required export");
   return found->function_index;
}

std::uint64_t largest_frame(const oc::artifact& code) {
   auto frame = std::uint64_t{};
   for (const auto& function : code.functions()) {
      require(function.stack_bytes <= 64 * 1024, "Compiler fixture exceeds the unchanged physical frame bound");
      frame = std::max(frame, function.stack_bytes);
   }
   return frame;
}

void run(const std::string& mode, const oc::artifact& code, const oc::artifact& healthy) {
   const auto page = ::sysconf(_SC_PAGESIZE);
   require(page > 0 && (16 * 1024) % page == 0 && (48 * 1024) % page == 0,
           "Native-stack fixture requires the supported page-rounded capacities");
   require(code.image().memory.defined && code.image().memory.initial_pages == 1,
           "Refusal fixture must actually initialize memory on successful entry");
   const auto entry = exported(code, "entry");
   const auto shallow = exported(code, "shallow");
   const auto healthy_entry = exported(healthy, "entry");
   const auto frame = largest_frame(code);
   const auto healthy_frame = largest_frame(healthy);
   require(frame > small - reserve, "Real compiled frame no longer exercises the unchanged small physical cap");
   require(frame + anchor < intermediate - reserve &&
               code.functions()[3].stack_bytes > intermediate - reserve - frame - anchor,
           "Real frame no longer distinguishes reserve admission from first/start-frame admission");
   require(code.functions()[shallow].stack_bytes <= intermediate - reserve - frame - anchor,
           "Shallow same-artifact control does not fit the intermediate physical window");
   require(healthy_frame + healthy.functions()[healthy_entry].stack_bytes + anchor < small - reserve,
           "Healthy control does not fit the unchanged small physical cap");
   require(code.image().start == (mode == "start" ? 3U : UINT32_MAX),
           "Compiler-generated start metadata differs from the original fixture");
   require(entry == (mode == "entry" ? 3U : 4U), "Wrong public entry selected");
   if (mode == "post_instantiate")
      require(exported(code, "__post_instantiate") == 3U, "Missing real WAMR post-instantiation entry");

   const auto encoded = code.encode();
   const auto healthy_encoded = healthy.encode();
   const auto binding = oc::executor::bind<registered>(code);
   const auto healthy_binding = oc::executor::bind<registered>(healthy);
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto failure = std::exception_ptr{};
   try {
      for (const auto limits :
           {oc::executor::options{reserve, 64 * 1024}, oc::executor::options{reserve + anchor, 64 * 1024},
            oc::executor::options{small, 4095}, oc::executor::options{small, UINTPTR_MAX}}) {
         auto rejected = false;
         try {
            auto invalid = oc::executor{scheduler, limits};
         } catch (const vm::exceptions::execution_unavailable&) {
            rejected = true;
         }
         require(rejected, "Constructor weakened reserve/anchor or host-headroom admission");
      }
      for (const auto physical : {small, intermediate, adequate}) {
         auto executor = oc::executor{scheduler, {physical, 64 * 1024}};
         forge::asio::blocking::run(runtime, executor.start(std::chrono::steady_clock::now() + 5s));
         auto invocation_failure = std::exception_ptr{};
         try {
            executor.prepare_thread();
            auto state = host{};
            auto memory = vm::wasm_allocator{};
            const auto invoke = [&](const auto& selected, std::uint32_t index) {
               return executor.invoke(selected, &state, index, {}, memory, std::chrono::steady_clock::now() + 5s);
            };
            const auto check_healthy = [&](const auto& selected, std::uint32_t index) {
               state = host{};
               const auto result = invoke(selected, index);
               require(result.trap == oc::trap_reason::none && !result.host_error && !result.value,
                       "Healthy public invocation failed");
               require(state.issued == 0 && state.consumed == 0 && state.marks == 1 && state.last_mark == 9,
                       "Healthy host-effect control changed");
               require(memory.get_current_page() == 1 && *memory.get_base_ptr<std::uint8_t>() == 43,
                       "Healthy memory-effect control changed");
            };
            check_healthy(healthy_binding, healthy_entry); // SAME executor/worker before any expected refusal.
            state = host{};
            memory.reset(0);
            if (physical != adequate) {
               const auto check_refusal = [&] {
                  auto unavailable = false;
                  try {
                     static_cast<void>(invoke(binding, entry));
                  } catch (const vm::exceptions::execution_unavailable&) {
                     unavailable = true;
                  }
                  require(unavailable, "First/start-frame shortfall did not remain typed execution_unavailable");
                  require(state.untouched(), "Native capacity refusal happened after a host effect");
               };
               check_refusal();
               require(memory.get_current_page() == 0, "Refusal initialized previously fresh guest memory");
               memory.reset(1);
               *memory.get_base_ptr<std::uint8_t>() = 0xa5;
               check_refusal();
               require(memory.get_current_page() == 1 && *memory.get_base_ptr<std::uint8_t>() == 0xa5,
                       "Refusal reset or changed preexisting guest memory");
               if (physical == intermediate && mode == "entry")
                  check_healthy(binding, shallow); // SAME artifact and worker after refusal, no large initializer.
               else
                  check_healthy(healthy_binding, healthy_entry);
               check_healthy(healthy_binding, healthy_entry); // Owner remains reusable after both refusals.
            } else {
               for (auto invocation = 1U; invocation <= 2U; ++invocation) {
                  const auto result = invoke(binding, entry);
                  require(result.trap == oc::trap_reason::none && !result.host_error && !result.value,
                          "Adequate physical capacity rejected the SAME compiled artifact");
                  const auto expected_calls = std::uint64_t{locals} * invocation;
                  require(state.issued == expected_calls && state.consumed == expected_calls &&
                              state.consumed_sum == expected_calls * (expected_calls + 1) / 2,
                          "Live frame values changed across real initialization/entry");
                  require(state.marks == invocation * (mode == "entry" ? 1U : 2U) &&
                              state.last_mark == (mode == "entry" ? 7U : 9U),
                          "Real start/post-instantiate/entry effects were missing or duplicated");
                  require(memory.get_current_page() == 1 &&
                              *memory.get_base_ptr<std::uint8_t>() == (mode == "entry" ? 42 : 43),
                          "Adequate invocation lost its real memory effects");
               }
            }
            require(code.encode() == encoded && healthy.encode() == healthy_encoded,
                    "Refusal/reuse changed the genuine compiled artifact bytes");
         } catch (...) {
            invocation_failure = std::current_exception();
         }
         executor.release_thread();
         forge::asio::blocking::run(runtime, executor.shutdown());
         if (invocation_failure)
            std::rethrow_exception(invocation_failure);
      }
   } catch (...) {
      failure = std::current_exception();
   }
   forge::asio::blocking::run(runtime, scheduler.shutdown());
   if (failure)
      std::rethrow_exception(failure);
   std::cout << "PASS native-stack " << mode << " frame=" << frame
             << " small=4210688 intermediate=4243456 adequate=8388608 before_memory_and_effects reuse=same_owner\n";
}
} // namespace

int main(int argc, char** argv) {
   try {
      if (argc != 6)
         return 2;
      const auto mode = std::string{argv[1]};
      require(mode == "entry" || mode == "start" || mode == "post_instantiate", "Unknown native-stack case");
      registered::add<&host::next>("env", "next");
      registered::add<&host::consume>("env", "consume");
      registered::add<&host::mark>("env", "mark");
      const auto code = load(argv[2], argv[3]);
      const auto healthy = load(argv[4], argv[5]);
      run(mode, code, healthy);
      return 0;
   } catch (const std::exception& error) {
      std::cerr << "FAIL " << error.what() << '\n';
      return 1;
   } catch (...) {
      std::cerr << "FAIL unexpected typed error\n";
      return 1;
   }
}
