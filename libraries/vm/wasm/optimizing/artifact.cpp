module;

#include <algorithm>
#include <atomic>
#include <boost/describe.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <forge/exceptions/macros.hpp>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

module forge.vm.wasm.optimizing.artifact;

import forge.raw.raw;
import forge.raw.exceptions;
import forge.vm.wasm.optimizing.exceptions;

#include "details/native_image.hxx"

namespace forge::vm::wasm::optimizing {
namespace {
struct artifact_image {
   std::uint32_t version = artifact::current_version;
   crypto::digest::sha256 checksum;
   artifact_payload payload;
};
BOOST_DESCRIBE_STRUCT(artifact_image, (), (version, checksum, payload))

void validate_payload(const artifact_payload& value) {
   const auto& configuration = value.configuration;
   const auto arm64 = configuration.target.triple == "aarch64-apple-macosx11.0.0-elf";
   if (configuration.limits.max_generated_bytes == 0 || configuration.limits.max_function_stack_bytes == 0)
      FORGE_THROW_EXCEPTION(exceptions::compile_limit, "native artifact requires explicit compiler budgets");
   const auto layout = native_image::plan(value);
   if (arm64)
      for (const auto& section : value.sections)
         if (section.kind == section_kind::code && (section.alignment < 4 || section.bytes.size() % 4 != 0))
            FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "misaligned AArch64 executable section");
   const auto numeric = [](interpret::value_type type) {
      return type == interpret::i32 || type == interpret::i64 || type == interpret::f32 || type == interpret::f64;
   };
   auto signatures = std::set<std::pair<std::vector<interpret::value_type>, std::vector<interpret::value_type>>>{};
   for (const auto& type : value.types) {
      if (type.ret.size() > 1 || !std::all_of(type.params.begin(), type.params.end(), numeric) ||
          !std::all_of(type.ret.begin(), type.ret.end(), numeric) || !signatures.emplace(type.params, type.ret).second)
         FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "invalid canonical native signature table");
   }
   for (const auto& function : value.functions) {
      if (function.type_index >= value.types.size() || function.entry.section >= value.sections.size() ||
          value.sections[function.entry.section].kind != section_kind::code ||
          function.entry.offset >= value.sections[function.entry.section].bytes.size() ||
          (arm64 && (function.entry.offset % 4 != 0 || function.stack_bytes % 16 != 0)))
         FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "invalid native function entry or type");
      if (function.stack_bytes > configuration.limits.max_function_stack_bytes)
         FORGE_THROW_EXCEPTION(exceptions::compile_limit, "native function exceeds the stack byte budget");
      if (function.import_index != UINT32_MAX &&
          (function.import_index >= configuration.imports.size() ||
           configuration.imports[function.import_index].signature != value.types[function.type_index]))
         FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "invalid native import thunk binding");
   }
   auto export_names = std::set<std::string>{};
   for (const auto& export_value : value.exports) {
      if (export_value.function_index >= value.functions.size() || !export_names.insert(export_value.name).second)
         FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "invalid native export table");
   }
   for (const auto index : value.table) {
      if (index != UINT32_MAX && index >= value.functions.size())
         FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "invalid native indirect table entry");
   }
   if (value.start != UINT32_MAX &&
       (value.start >= value.functions.size() || !value.types[value.functions[value.start].type_index].params.empty() ||
        !value.types[value.functions[value.start].type_index].ret.empty()))
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "invalid native start function");
   for (const auto& global : value.globals) {
      if (!numeric(global.type) ||
          ((global.type == interpret::i32 || global.type == interpret::f32) && global.bits >> 32))
         FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "invalid native global initial value");
   }
   if ((!value.memory.defined && (value.memory.initial_pages != 0 || value.memory.max_pages != 0)) ||
       value.memory.initial_pages > value.memory.max_pages || value.memory.max_pages > interpret::max_pages ||
       value.memory.initial_pages > configuration.execution.max_pages)
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "invalid native memory initial limits");
   const auto initial_bytes = static_cast<std::uint64_t>(value.memory.initial_pages) * interpret::page_size;
   for (const auto& segment : value.data) {
      if (segment.offset > initial_bytes || segment.bytes.size() > initial_bytes - segment.offset)
         FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "native initial data segment outside memory");
   }
   auto patches = std::vector<std::tuple<std::uint32_t, std::uint64_t, std::uint64_t>>{};
   patches.reserve(value.relocations.size());
   for (const auto& relocation : value.relocations) {
      const auto width = relocation.kind == relocation_kind::absolute64 ? 8U : 4U;
      const auto instruction = relocation.kind >= relocation_kind::aarch64_call26 &&
                               relocation.kind <= relocation_kind::aarch64_load_store64;
      if ((relocation.kind != relocation_kind::absolute64 && relocation.kind != relocation_kind::relative32 &&
           !(arm64 && instruction)) ||
          relocation.source.section >= value.sections.size() || relocation.target.section >= value.sections.size())
         FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "unknown native relocation kind or section");
      const auto source_bytes = value.sections[relocation.source.section].bytes.size();
      if (relocation.source.offset > source_bytes || width > source_bytes - relocation.source.offset ||
          relocation.target.offset > value.sections[relocation.target.section].bytes.size() ||
          relocation.addend < std::numeric_limits<std::int32_t>::min() ||
          relocation.addend > std::numeric_limits<std::int32_t>::max())
         FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "native relocation outside the checked image");
      if (arm64 && relocation.source.offset % (instruction ? 4 : width) != 0)
         FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "misaligned AArch64 relocation source");
      const auto source = static_cast<std::int64_t>(layout.offsets[relocation.source.section] + relocation.source.offset);
      const auto target = static_cast<std::int64_t>(layout.offsets[relocation.target.section] + relocation.target.offset) +
                          relocation.addend;
      const auto relative = target - source;
      if (relocation.kind == relocation_kind::relative32 &&
          (relative < INT32_MIN || relative > INT32_MAX))
         FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "native relative relocation overflow");
      if (relocation.kind == relocation_kind::absolute64 &&
          (target < 0 || static_cast<std::uint64_t>(target) > layout.total_bytes))
         FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "native absolute relocation outside the image");
      if (instruction) {
         const auto& section = value.sections[relocation.source.section];
         const auto target_in_section = static_cast<std::int64_t>(relocation.target.offset) + relocation.addend;
         if (section.kind != section_kind::code || target < 0 ||
             static_cast<std::uint64_t>(target) >= layout.total_bytes || target_in_section < 0 ||
             static_cast<std::uint64_t>(target_in_section) >= value.sections[relocation.target.section].bytes.size())
            FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "invalid AArch64 instruction relocation extent");
         auto word = std::uint32_t{0};
         for (unsigned i = 0; i < 4; ++i)
            word |= std::uint32_t{section.bytes[relocation.source.offset + i]} << (i * 8);
         auto valid = false;
         if (relocation.kind == relocation_kind::aarch64_call26 ||
             relocation.kind == relocation_kind::aarch64_jump26) {
            const auto opcode = relocation.kind == relocation_kind::aarch64_call26 ? 0x94000000U : 0x14000000U;
            valid = (word & 0xfc000000U) == opcode && target % 4 == 0 &&
                    value.sections[relocation.target.section].kind == section_kind::code &&
                    relocation.target.offset < value.sections[relocation.target.section].bytes.size() &&
                    relative >= -(std::int64_t{1} << 27) && relative < (std::int64_t{1} << 27);
         } else if (relocation.kind == relocation_kind::aarch64_page21) {
            const auto pages = (target >> 12) - (source >> 12);
            valid = (word & 0x9f000000U) == 0x90000000U && pages >= -(std::int64_t{1} << 20) &&
                    pages < (std::int64_t{1} << 20);
         } else if (relocation.kind == relocation_kind::aarch64_add12) {
            valid = (word & 0xffc00000U) == 0x91000000U;
         } else {
            const auto scale = static_cast<unsigned>(relocation.kind) -
                               static_cast<unsigned>(relocation_kind::aarch64_load_store8);
            valid = (word & 0x3f000000U) == 0x39000000U && word >> 30 == scale &&
                    target % (std::int64_t{1} << scale) == 0;
         }
         if (!valid)
            FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "invalid AArch64 relocation opcode, alignment or range");
      }
      patches.emplace_back(relocation.source.section, relocation.source.offset, relocation.source.offset + width);
   }
   std::sort(patches.begin(), patches.end());
   for (std::size_t i = 1; i < patches.size(); ++i) {
      if (std::get<0>(patches[i - 1]) == std::get<0>(patches[i]) &&
          std::get<2>(patches[i - 1]) > std::get<1>(patches[i]))
         FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "overlapping native relocation patches");
   }
}

std::uint64_t signature_bytes(const interpret::host_function& signature) noexcept {
   return signature.params.capacity() * sizeof(interpret::value_type) +
          signature.ret.capacity() * sizeof(interpret::value_type);
}

std::uint64_t payload_bytes(const artifact_payload& value) noexcept {
   auto bytes =
       value.sections.capacity() * sizeof(image_section) + value.relocations.capacity() * sizeof(image_relocation) +
       value.types.capacity() * sizeof(interpret::host_function) +
       value.functions.capacity() * sizeof(compiled_function) + value.exports.capacity() * sizeof(compiled_export) +
       value.globals.capacity() * sizeof(global_initial_value) +
       value.data.capacity() * sizeof(memory_initial_segment) + value.table.capacity() * sizeof(std::uint32_t) +
       value.configuration.imports.capacity() * sizeof(import_signature);
   for (const auto& section : value.sections)
      bytes += section.bytes.capacity();
   for (const auto& type : value.types)
      bytes += signature_bytes(type);
   for (const auto& export_value : value.exports)
      bytes += export_value.name.capacity();
   for (const auto& segment : value.data)
      bytes += segment.bytes.capacity();
   for (const auto& import : value.configuration.imports)
      bytes += import.module.capacity() + import.name.capacity() + signature_bytes(import.signature);
   const auto& target = value.configuration.target;
   return bytes + target.triple.capacity() + target.cpu.capacity() + target.features.capacity() +
          target.llvm_version.capacity();
}
} // namespace

struct artifact::impl {
   impl(artifact_payload value, std::vector<std::uint8_t> encoded)
       : image(std::move(value)), encoded(std::move(encoded)), identity(identify(image.wasm, image.configuration)) {
      bytes = sizeof(impl) + this->encoded.capacity() + payload_bytes(image);
   }

   artifact_payload image;
   std::vector<std::uint8_t> encoded;
   crypto::digest::sha256 identity;
   std::uint64_t bytes = 0;
   mutable std::mutex preparation;
   mutable std::unique_ptr<const native_image> native;
   mutable std::atomic_bool prepared = false;
};

artifact::artifact(std::shared_ptr<const impl> value) : _impl(std::move(value)) {}

artifact artifact::from_payload(artifact_payload payload) {
   // Import ordinals are already canonical compiler ordinals. Do not silently
   // sort the payload here and invalidate its thunk bindings.
   if (payload.configuration != canonicalize(payload.configuration))
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "native payload imports are not canonical");
   validate_payload(payload);
   auto image = artifact_image{};
   image.payload = std::move(payload);
   image.checksum = crypto::digest::sha256::hash(raw::pack(image.payload));
   auto bytes = raw::pack(image);
   if (bytes.size() > raw::max_byte_array_size)
      FORGE_THROW_EXCEPTION(exceptions::compile_limit, "native artifact exceeds the encoded byte budget");
   return artifact{std::shared_ptr<const impl>{new impl(std::move(image.payload), std::move(bytes))}};
}

artifact artifact::decode(std::span<const std::uint8_t> bytes, const crypto::digest::sha256& expected_identity,
                          std::uint64_t max_encoded_bytes) {
   if (bytes.size() > max_encoded_bytes || bytes.size() > raw::max_byte_array_size)
      FORGE_THROW_EXCEPTION(exceptions::compile_limit, "native artifact exceeds the encoded byte budget");
   auto limits = raw::unpack_limits{};
   limits.max_bytes = static_cast<std::uint32_t>(bytes.size());
   auto image = artifact_image{};
   try {
      image = raw::unpack_exact<artifact_image>(bytes, limits);
   } catch (const raw::exceptions::codec_error&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "invalid native artifact encoding");
   } catch (const raw::exceptions::allocation_limit&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "invalid native artifact allocation frame");
   } catch (const raw::exceptions::range_error&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "truncated native artifact encoding");
   }
   if (image.version != current_version || image.checksum != crypto::digest::sha256::hash(raw::pack(image.payload)))
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "native artifact version or checksum mismatch");
   auto result = from_payload(std::move(image.payload));
   if (result.identity() != expected_identity)
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "native artifact identity mismatch");
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

std::uint64_t artifact::byte_size() const noexcept {
   const auto prepared = _impl->prepared.load(std::memory_order_acquire);
   return _impl->bytes + (prepared ? _impl->native->byte_size() : 0);
}

std::uint64_t artifact::prepared_byte_size() const {
   const auto native_bytes = native_image::plan(_impl->image).allocation_bytes;
   if (_impl->bytes > UINT64_MAX - native_bytes)
      FORGE_THROW_EXCEPTION(exceptions::compile_limit, "prepared artifact accounting size overflow");
   return _impl->bytes + native_bytes;
}

bool artifact::is_unique() const noexcept {
   return _impl.use_count() == 1;
}

void artifact::prepare() const {
   if (_impl->prepared.load(std::memory_order_acquire))
      return;
   const auto lock = std::scoped_lock{_impl->preparation};
   if (_impl->prepared.load(std::memory_order_acquire))
      return;
   // Accounting overflow is rejected before the sole mapping allocation even
   // when a caller did not query the prospective size first.
   static_cast<void>(prepared_byte_size());
   auto native = std::make_unique<const native_image>(_impl->image);
   _impl->native = std::move(native);
   _impl->prepared.store(true, std::memory_order_release);
}

bool artifact::is_prepared() const noexcept {
   return _impl->prepared.load(std::memory_order_acquire);
}

std::span<const std::byte> artifact::native_code() const {
   if (!_impl->prepared.load(std::memory_order_acquire))
      FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, "native artifact has not been explicitly prepared");
   return _impl->native->code();
}

std::span<const native_function> artifact::functions() const {
   if (!_impl->prepared.load(std::memory_order_acquire))
      FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, "native artifact has not been explicitly prepared");
   return _impl->native->functions();
}

entry_function artifact::entry(std::uint32_t index) const {
   const auto values = functions();
   if (index >= values.size())
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "native entry index outside the prepared function table");
   return values[index].entry;
}

} // namespace forge::vm::wasm::optimizing
