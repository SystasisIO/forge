module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <forge/exceptions/macros.hpp>
#include <tuple>
#include <string_view>
#include <utility>

module forge.vm.wasm.optimizing.compile_request;

import forge.raw.raw;
import forge.vm.wasm.optimizing.exceptions;
import forge.vm.wasm.optimizing.invocation_abi;

namespace forge::vm::wasm::optimizing {
namespace {
enum class target_kind { unsupported, linux_x86_64, darwin_arm64 };
constexpr auto linux_definition = std::array<std::string_view, 3>{"x86_64-unknown-linux-gnu", "x86-64", ""};
constexpr auto darwin_definition =
    std::array<std::string_view, 3>{"aarch64-apple-macosx11.0.0-elf", "generic", "+fp-armv8,+neon"};

constexpr target_kind host_kind() noexcept {
#if defined(__linux__) && defined(__x86_64__)
   return target_kind::linux_x86_64;
#elif defined(__APPLE__) && defined(__aarch64__)
   return target_kind::darwin_arm64;
#else
   return target_kind::unsupported;
#endif
}

bool matches(const target_profile& target, const std::array<std::string_view, 3>& definition) noexcept {
   return target.triple == definition[0] && target.cpu == definition[1] && target.features == definition[2];
}

target_kind recognize(const target_profile& target) noexcept {
   if (target.llvm_version != "22.1.8" || target.codegen_version != 3 ||
       target.host_abi_version != invocation_abi::current_version)
      return target_kind::unsupported;
   if (matches(target, linux_definition))
      return target_kind::linux_x86_64;
   if (matches(target, darwin_definition))
      return target_kind::darwin_arm64;
   return target_kind::unsupported;
}
} // namespace

bool target_profile::matches_host() const noexcept {
   return host_kind() != target_kind::unsupported && recognize(*this) == host_kind();
}

target_profile target_profile::native() {
   const auto kind = host_kind();
   if (kind == target_kind::unsupported)
      FORGE_THROW_EXCEPTION(exceptions::unsupported_target, "native compiler profile is unsupported on this host");
   const auto& definition = kind == target_kind::linux_x86_64 ? linux_definition : darwin_definition;
   auto result = target_profile{};
   result.triple = definition[0];
   result.cpu = definition[1];
   result.features = definition[2];
   return result;
}

compile_configuration canonicalize(compile_configuration configuration) {
   if (configuration.version != compile_configuration::current_version)
      FORGE_THROW_EXCEPTION(exceptions::invalid_module, "unsupported compiler configuration version");
   if (recognize(configuration.target) == target_kind::unsupported)
      FORGE_THROW_EXCEPTION(exceptions::unsupported_target, "unsupported canonical compiler target profile");
   const auto numeric = [](interpret::value_type type) {
      return type == interpret::i32 || type == interpret::i64 || type == interpret::f32 || type == interpret::f64;
   };
   for (const auto& import : configuration.imports) {
      if (import.signature.ret.size() > 1 ||
          !std::all_of(import.signature.params.begin(), import.signature.params.end(), numeric) ||
          !std::all_of(import.signature.ret.begin(), import.signature.ret.end(), numeric))
         FORGE_THROW_EXCEPTION(exceptions::unsupported_import, "unsupported compiler import signature");
   }
   std::sort(configuration.imports.begin(), configuration.imports.end(), [](const auto& left, const auto& right) {
      return std::tie(left.module, left.name) < std::tie(right.module, right.name);
   });
   for (std::size_t i = 1; i < configuration.imports.size(); ++i) {
      const auto& left = configuration.imports[i - 1];
      const auto& right = configuration.imports[i];
      if (left.module == right.module && left.name == right.name)
         FORGE_THROW_EXCEPTION(exceptions::unsupported_import, "duplicate compiler import binding");
   }
   return configuration;
}

crypto::digest::sha256 identify(const crypto::digest::sha256& wasm, const compile_configuration& configuration) {
   return crypto::digest::sha256::packhash(wasm, canonicalize(configuration));
}

crypto::digest::sha256 identify(const compile_request& request) {
   return identify(crypto::digest::sha256::hash(std::span{request.wasm}), request.configuration);
}

std::vector<std::uint8_t> compile_request::encode() const {
   auto canonical = *this;
   canonical.configuration = canonicalize(std::move(canonical.configuration));
   return raw::pack(canonical);
}

compile_request compile_request::decode(std::span<const std::uint8_t> bytes, std::uint64_t max_encoded_bytes) {
   if (bytes.size() > max_encoded_bytes || bytes.size() > raw::max_byte_array_size)
      FORGE_THROW_EXCEPTION(exceptions::compile_limit, "compiler request exceeds the encoded byte budget");
   auto limits = raw::unpack_limits{};
   limits.max_bytes = static_cast<std::uint32_t>(bytes.size());
   auto result = raw::unpack_exact<compile_request>(bytes, limits);
   result.configuration = canonicalize(std::move(result.configuration));
   return result;
}

} // namespace forge::vm::wasm::optimizing
