#include <atomic>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

import forge.vm.wasm.optimizing.artifact;
import forge.vm.wasm.optimizing.exceptions;

namespace {
namespace oc = forge::vm::wasm::optimizing;
void require(bool condition, const char* text) {
   if (!condition)
      throw std::runtime_error(text);
}
std::vector<std::uint8_t> read(const char* path) {
   std::ifstream input(path, std::ios::binary);
   require(bool(input), "Cannot read preparation fixture");
   return {std::istreambuf_iterator<char>{input}, {}};
}
template <typename Error, typename Work> void rejects(Work work) {
   auto rejected = false;
   try {
      work();
   } catch (const Error&) {
      rejected = true;
   }
   require(rejected, "Expected typed preparation rejection");
}
} // namespace

int main(int argc, char** argv) {
   try {
      if (argc != 3)
         return 2;
      const auto request = oc::compile_request::decode(read(argv[2]), 4 * 1024 * 1024);
      const auto bytes = read(argv[1]);
      const auto decode = [&] {
         return oc::artifact::decode(bytes, oc::identify(request), 16 * 1024 * 1024,
                                     oc::artifact::origin::isolated_compiler);
      };
      const auto policy = oc::artifact::standalone_preparation_policy;
      const auto root = forge::vm::wasm::interpret::prepared_module::preparation::root_byte_size();
      auto code = decode();
      const auto base = code.byte_size();
      const auto upper = code.prepared_byte_size(policy);
      require(!code.is_prepared() && upper == base + policy.resident_bytes,
              "Reservation mutated artifact or promised an exact size");
      rejects<oc::exceptions::compile_limit>([&] { code.prepare({root - 1, policy.workspace_bytes}); });
      rejects<oc::exceptions::compile_limit>([&] { code.prepare({policy.resident_bytes, root - 1}); });
      require(!code.is_prepared(), "Failed root admission published code");
      std::stop_source stopped;
      stopped.request_stop();
      rejects<oc::exceptions::cancelled>([&] { code.prepare(policy, stopped.get_token()); });
      require(!code.is_prepared(), "Cancelled preparation published code");
      code.prepare(policy);
      require(code.byte_size() < upper && !code.native_code().empty(), "Preparation did not fit conservative cap");
      const auto actual = code.byte_size() - base;
      auto exact_test = decode();
      exact_test.prepare({actual, policy.workspace_bytes});
      auto one_short = decode();
      rejects<oc::exceptions::compile_limit>([&] { one_short.prepare({actual - 1, policy.workspace_bytes}); });
      require(!one_short.is_prepared(), "One-byte-short resident cap published code");
      one_short.prepare(policy);
      require(one_short.is_prepared(), "Failed preparation prevented explicit retry");
      rejects<oc::exceptions::compile_limit>([&] { code.prepare({actual - 1, policy.workspace_bytes}); });
      rejects<oc::exceptions::cancelled>([&] { code.prepare(policy, stopped.get_token()); });
      auto unrelated_charge = std::make_shared<int>(1);
      rejects<oc::exceptions::invalid_artifact>([&] { code.prepare(policy, {}, unrelated_charge); });

      std::atomic_bool refunded = false;
      auto charge = std::shared_ptr<void>{new int{}, [&](void* value) {
                                             delete static_cast<int*>(value);
                                             refunded.store(true);
                                          }};
      auto pinned = std::optional<oc::artifact>{decode()};
      std::exception_ptr failures[2];
      {
         std::jthread first([&] {
            try {
               pinned->prepare(policy, {}, charge);
            } catch (...) {
               failures[0] = std::current_exception();
            }
         });
         std::jthread second([&] {
            try {
               pinned->prepare(policy, {}, charge);
            } catch (...) {
               failures[1] = std::current_exception();
            }
         });
      }
      for (const auto& error : failures)
         if (error)
            std::rethrow_exception(error);
      require(pinned->is_prepared(), "Concurrent same-owner preparation did not publish");
      pinned->prepare(policy, {}, charge);
      rejects<oc::exceptions::invalid_artifact>([&] { pinned->prepare(policy, {}, unrelated_charge); });
      auto module = pinned->runtime_module();
      auto weak = std::weak_ptr{module};
      pinned.reset();
      charge.reset();
      require(!refunded && !weak.expired() && !module->native_code().empty(), "Module pin lost storage/charge");
      module.reset();
      require(weak.expired() && !refunded, "Charge refunded before weak control-block storage was released");
      weak.reset();
      require(refunded, "Final physical owner did not release charge");

      auto raced = decode();
      auto owner_a = std::make_shared<int>(1);
      auto owner_b = std::make_shared<int>(2);
      std::atomic_uint successes = 0;
      std::atomic_uint conflicts = 0;
      failures[0] = failures[1] = {};
      const auto contender = [&](std::shared_ptr<void> owner, unsigned index) {
         try {
            raced.prepare(policy, {}, std::move(owner));
            ++successes;
         } catch (const oc::exceptions::invalid_artifact&) {
            ++conflicts;
         } catch (...) {
            failures[index] = std::current_exception();
         }
      };
      {
         std::jthread a([&] { contender(owner_a, 0); });
         std::jthread b([&] { contender(owner_b, 1); });
      }
      for (const auto& error : failures)
         if (error)
            std::rethrow_exception(error);
      require(successes == 1 && conflicts == 1, "Concurrent preparation silently replaced a resident token");
      rejects<oc::exceptions::compile_limit>([&] { raced.prepare({actual - 1, policy.workspace_bytes}); });
      rejects<oc::exceptions::cancelled>([&] { raced.prepare(policy, stopped.get_token(), owner_a); });
      std::cout << "PASS bounded preparation, exact TEST cap=" << actual
                << ", one-byte-short/retry, stop, repeated/concurrent owner and weak-pin lifetime\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << "FAIL " << error.what() << '\n';
      return 1;
   } catch (...) {
      std::cerr << "FAIL unexpected typed error\n";
      return 1;
   }
}
