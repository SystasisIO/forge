module;

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/describe.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>
#include <forge/exceptions/macros.hpp>

module forge.vm.wasm.optimizing.artifact;

import forge.raw.raw;
import forge.raw.exceptions;
import forge.vm.wasm.optimizing.exceptions;

#include "details/artifact_impl.hxx"

namespace forge::vm::wasm::optimizing {
namespace {
constexpr std::uint64_t artifact_magic = 0x315452414d415746ULL;
struct artifact_image {
   std::uint64_t magic = artifact_magic;
   std::uint32_t version = artifact::current_version;
   crypto::digest::sha256 checksum;
   artifact_payload payload;
};
BOOST_DESCRIBE_STRUCT(artifact_image, (), (magic, version, checksum, payload))

void validate_payload(const artifact_payload& value) {
   constexpr std::array<std::uint8_t, 8> wasm_header{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   constexpr std::array<std::uint8_t, 4> aot_header{0, 0x61, 0x6f, 0x74};
   if (!value.configuration.limits.max_generated_bytes || !value.configuration.limits.max_function_stack_bytes)
      FORGE_THROW_EXCEPTION(exceptions::compile_limit, "AOT artifact requires explicit compiler budgets");
   if (!value.sections.empty() || !value.relocations.empty() || !value.globals.empty() || !value.data.empty() ||
       !value.table.empty())
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "Legacy OC payload is not a WAMR artifact");
   if (value.source.size() < wasm_header.size() ||
       value.source.size() > interpret::capacity_profile::max_module_bytes ||
       !std::equal(wasm_header.begin(), wasm_header.end(), value.source.begin()) ||
       crypto::digest::sha256::hash(std::span{value.source}) != value.wasm || value.aot.size() < 8 ||
       value.aot.size() > value.configuration.limits.max_generated_bytes ||
       !std::equal(aot_header.begin(), aot_header.end(), value.aot.begin()))
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "AOT artifact has invalid original source or native payload");
   const auto numeric = [](interpret::value_type type) {
      return type == interpret::i32 || type == interpret::i64 || type == interpret::f32 || type == interpret::f64;
   };
   for (const auto& type : value.types)
      if (type.ret.size() > 1 || !std::all_of(type.params.begin(), type.params.end(), numeric) ||
          !std::all_of(type.ret.begin(), type.ret.end(), numeric))
         FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "AOT artifact has a nonnumeric function signature");
   for (const auto& function : value.functions)
      if (function.type_index >= value.types.size() || function.stack_bytes != 0 ||
          (function.import_index != UINT32_MAX &&
           (function.import_index >= value.configuration.imports.size() ||
            value.configuration.imports[function.import_index].signature != value.types[function.type_index])))
         FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "AOT artifact function metadata is inconsistent");
   auto names = std::set<std::string>{};
   for (const auto& exported : value.exports)
      if (exported.function_index >= value.functions.size() || !names.insert(exported.name).second)
         FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "AOT artifact export metadata is inconsistent");
   if (value.start != UINT32_MAX && value.start >= value.functions.size())
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "AOT artifact start function is outside its source");
}

std::uint64_t signature_bytes(const interpret::host_function& signature) noexcept {
   return (signature.params.capacity() + signature.ret.capacity()) * sizeof(interpret::value_type);
}
std::uint64_t payload_bytes(const artifact_payload& value) noexcept {
   auto bytes =
       value.source.capacity() + value.aot.capacity() + value.functions.capacity() * sizeof(compiled_function) +
       value.exports.capacity() * sizeof(compiled_export) + value.types.capacity() * sizeof(interpret::host_function) +
       value.configuration.imports.capacity() * sizeof(import_signature);
   for (const auto& type : value.types)
      bytes += signature_bytes(type);
   for (const auto& exported : value.exports)
      bytes += exported.name.capacity() + 1;
   for (const auto& imported : value.configuration.imports)
      bytes += imported.module.capacity() + 1 + imported.name.capacity() + 1 + signature_bytes(imported.signature);
   const auto& target = value.configuration.target;
   return bytes + target.triple.capacity() + 1 + target.cpu.capacity() + 1 + target.features.capacity() + 1 +
          target.llvm_version.capacity() + 1 + target.engine.capacity() + 1;
}
} // namespace

artifact::artifact(std::shared_ptr<const impl> value) : _impl(std::move(value)) {}
artifact artifact::from_payload(artifact_payload payload, origin authority) {
   if (payload.configuration != canonicalize(payload.configuration))
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "AOT artifact configuration is not canonical");
   validate_payload(payload);
   auto image = artifact_image{};
   image.payload = std::move(payload);
   image.checksum = crypto::digest::sha256::hash(raw::pack(image.payload));
   auto bytes = raw::pack(image);
   if (bytes.size() > raw::max_byte_array_size)
      FORGE_THROW_EXCEPTION(exceptions::compile_limit, "AOT artifact exceeds the encoded byte limit");
   const auto owned_bytes = bytes.capacity() + payload_bytes(image.payload);
   // The allocation's owner survives impl destruction while weak module pins
   // retain its control block. Its token is released after physical deallocation.
   auto* retained = new impl::retention{};
   return artifact{std::allocate_shared<impl>(impl::allocator<impl>{retained}, std::move(image.payload),
                                              std::move(bytes), owned_bytes, authority, retained)};
}

artifact artifact::decode(std::span<const std::uint8_t> bytes, const crypto::digest::sha256& expected,
                          std::uint64_t max_bytes, origin authority) {
   if (!max_bytes || bytes.size() > max_bytes || bytes.size() > raw::max_byte_array_size)
      FORGE_THROW_EXCEPTION(exceptions::compile_limit, "AOT artifact exceeds the encoded byte limit");
   auto limits = raw::unpack_limits{};
   limits.max_bytes = static_cast<std::uint32_t>(bytes.size());
   auto image = artifact_image{};
   try {
      image = raw::unpack_exact<artifact_image>(bytes, limits);
   } catch (const raw::exceptions::codec_error&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "Invalid AOT artifact encoding");
   } catch (const raw::exceptions::allocation_limit&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "Invalid AOT artifact allocation frame");
   } catch (const raw::exceptions::range_error&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "Truncated AOT artifact encoding");
   }
   if (image.magic != artifact_magic || image.version != current_version ||
       image.checksum != crypto::digest::sha256::hash(raw::pack(image.payload)))
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "Foreign, legacy or corrupted AOT artifact");
   auto result = from_payload(std::move(image.payload), authority);
   if (result.identity() != expected)
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "AOT artifact identity mismatch");
   return result;
}

std::vector<std::uint8_t> artifact::encode() const {
   return _impl->encoded;
}
const crypto::digest::sha256& artifact::identity() const noexcept {
   return _impl->identity;
}
const artifact_payload& artifact::image() const noexcept {
   return _impl->image;
}
std::span<const compiled_export> artifact::exports() const noexcept {
   return _impl->image.exports;
}
bool artifact::is_unique() const noexcept {
   return _impl.use_count() == 1;
}
} // namespace forge::vm::wasm::optimizing
