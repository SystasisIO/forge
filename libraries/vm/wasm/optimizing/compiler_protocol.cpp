module;

#include <algorithm>
#include <forge/exceptions/macros.hpp>
#include <limits>
#include <string_view>
#include <utility>

module forge.vm.wasm.optimizing.compiler_protocol;

import forge.raw.raw;

namespace forge::vm::wasm::optimizing::compiler_protocol {
namespace {

[[noreturn]] void invalid_message(const char* message) {
   FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, message);
}

void validate(const header& value) {
   if (value.magic != magic || value.version != current_version || value.exchange.session == 0 ||
       value.exchange.job == 0 || value.exchange.attempt == 0 || value.body_bytes > raw::max_byte_array_size)
      invalid_message("invalid compiler protocol header");
   switch (value.kind) {
   case message_kind::hello:
   case message_kind::preflight:
      if (value.identity != crypto::digest::sha256{})
         invalid_message("compiler control cannot establish a compilation identity");
      break;
   case message_kind::request:
   case message_kind::reply:
      break;
   case message_kind::hello_ack:
      if (value.body_bytes != 0)
         invalid_message("compiler hello acknowledgement must be header-only");
      return;
   default:
      invalid_message("unknown compiler protocol message kind");
   }
   if (value.body_bytes == 0)
      invalid_message("compiler protocol body is empty");
}

void validate(const target_profile& value) {
   auto configuration = compile_configuration{};
   configuration.target = value;
   if (canonicalize(std::move(configuration)).target != value)
      invalid_message("noncanonical compiler protocol target");
}

void validate(const hello& value) {
   if (value.artifact_version != artifact::current_version ||
       value.configuration_version != compile_configuration::current_version || value.limits.cpu_seconds == 0 ||
       value.limits.address_space_bytes == 0 || value.limits.stack_bytes == 0)
      invalid_message("incompatible compiler protocol hello");
   // Reuse the sole target/profile validator. This is not a capabilities list.
   validate(value.target);
   const auto revision = std::string_view{value.build_revision};
   if ((revision.size() != 40 && revision.size() != 64) || !std::all_of(revision.begin(), revision.end(), [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
       }))
      invalid_message("compiler build revision is unavailable");
}

void validate(const reply& value) {
   if (!value.failure) {
      if (value.image.empty())
         invalid_message("successful compiler reply has no artifact");
      return;
   }
   const auto reason = static_cast<std::uint32_t>(*value.failure);
   // Existing closed contiguous compiler category; lifecycle-only extensions
   // are deliberately not transportable as guest compilation failures.
   if (reason < static_cast<std::uint32_t>(exceptions::code::invalid_module) ||
       reason > static_cast<std::uint32_t>(exceptions::code::cache_corruption) || !value.image.empty())
      invalid_message("invalid compiler reply failure");
}

template <typename T> std::vector<std::uint8_t> encode_message(const T& value) {
   validate(value);
   if (raw::pack_size(value) > raw::max_byte_array_size)
      invalid_message("compiler protocol message exceeds the Raw byte ceiling");
   return raw::pack(value);
}

template <typename T> T decode_message(std::span<const std::uint8_t> bytes, std::uint64_t max_bytes) {
   if (max_bytes == 0 || bytes.empty() || bytes.size() > max_bytes || bytes.size() > raw::max_byte_array_size)
      invalid_message("compiler protocol body exceeds the byte ceiling");
   auto limits = raw::unpack_limits{};
   limits.max_bytes = static_cast<std::uint32_t>(bytes.size());
   limits.max_container_elements = 64;
   limits.max_total_container_elements = 128;
   try {
      auto value = raw::unpack_exact<T>(bytes, limits);
      validate(value);
      return value;
   } catch (const exceptions::compiler_unavailable&) {
      throw;
   } catch (...) {
      invalid_message("malformed compiler protocol body");
   }
}

} // namespace

std::size_t header_size() {
   static const auto bytes = raw::pack_size(header{});
   return bytes;
}

std::vector<std::uint8_t> encode(const header& value) {
   return encode_message(value);
}

std::vector<std::uint8_t> encode(const hello& value) {
   return encode_message(value);
}

std::vector<std::uint8_t> encode(const reply& value) {
   return encode_message(value);
}

std::vector<std::uint8_t> encode(const target_profile& value) {
   return encode_message(value);
}

header decode_header(std::span<const std::uint8_t> bytes) {
   if (bytes.size() != header_size())
      invalid_message("truncated or oversized compiler protocol header");
   return decode_message<header>(bytes, header_size());
}

hello decode_hello(std::span<const std::uint8_t> bytes, std::uint64_t max_bytes) {
   return decode_message<hello>(bytes, max_bytes);
}

reply decode_reply(std::span<const std::uint8_t> bytes, std::uint64_t max_bytes) {
   return decode_message<reply>(bytes, max_bytes);
}

target_profile decode_target(std::span<const std::uint8_t> bytes, std::uint64_t max_bytes) {
   return decode_message<target_profile>(bytes, max_bytes);
}

} // namespace forge::vm::wasm::optimizing::compiler_protocol
