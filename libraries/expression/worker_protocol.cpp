module;

#include <cstdint>
#include <forge/exceptions/macros.hpp>
#include <new>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

module forge.expression.worker_protocol;

import forge.raw.raw;
import forge.raw.datastream;
import forge.raw.exceptions;
import forge.expression.exceptions;

namespace forge::expression::worker_protocol {
namespace {

template <typename T, typename Values> auto request_fields(T& value, Values& values) {
   return std::tie(value.version, value.sequence, value.op, value.source, value.declarations, value.limits,
                   value.prepared_id, value.probe_bytes, value.expression, value.result, values);
}

template <typename T> auto response_fields(T& value) {
   return std::tie(value.version, value.sequence, value.code, value.kind, value.i64, value.u64, value.boolean,
                   value.ast_events, value.prepared_id, value.heap_live, value.heap_peak, value.heap_cap,
                   value.heap_failures, value.expression, value.result);
}

template <typename T> struct owned_tuple;
template <typename... T> struct owned_tuple<std::tuple<T...>> {
   using type = std::tuple<std::remove_cvref_t<T>...>;
};
template <typename T> using owned_tuple_t = typename owned_tuple<T>::type;

void validate_request(std::uint32_t message_version, std::uint64_t sequence) {
   if (message_version != version || sequence == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_input, "invalid expression worker protocol identity");
   }
}

void validate_response(std::uint32_t message_version, std::uint64_t sequence) {
   if (message_version != version || sequence == 0) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "invalid expression worker response identity");
   }
}

void bounded_name(std::string_view name) {
   if (name.size() > 1024) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression worker input name exceeds transport limits");
   }
}

void bounded_scalar(const forge::variant& value) {
   switch (value.get_type()) {
   case forge::variant::int64_type:
   case forge::variant::uint64_type:
   case forge::variant::bool_type:
      return;
   case forge::variant::string_type:
      if (value.get_string().size() <= 1024) {
         return;
      }
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression worker input string exceeds transport limits");
   default:
      break;
   }
   FORGE_THROW_EXCEPTION(exceptions::invalid_input, "expression worker input exceeds scalar transport limits");
}

// This shallow transport admission bounds the host's Raw packing recursion.
// The child retains ownership of the expression profile and declaration checks.
void bounded_values(const forge::variant_object& values) {
   if (values.size() > 64) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted,
                            "expression worker input field count exceeds transport limits");
   }
   for (const auto& field : values) {
      bounded_name(field.key());
      const auto& value = field.value();
      if (value.get_type() == forge::variant::array_type) {
         const auto& elements = value.get_array();
         if (elements.size() > 256) {
            FORGE_THROW_EXCEPTION(exceptions::resource_exhausted,
                                  "expression worker input list exceeds transport limits");
         }
         for (const auto& item : elements) {
            bounded_scalar(item);
         }
      } else if (value.get_type() == forge::variant::object_type) {
         const auto& elements = value.get_object();
         if (elements.size() > 256) {
            FORGE_THROW_EXCEPTION(exceptions::resource_exhausted,
                                  "expression worker input map exceeds transport limits");
         }
         for (const auto& item : elements) {
            bounded_name(item.key());
            bounded_scalar(item.value());
         }
      } else {
         bounded_scalar(value);
      }
   }
}

template <typename T> std::size_t encode_fields(const T& fields, std::span<std::uint8_t> output) {
   if (output.empty() || output.size() > frame_limit) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "invalid expression worker output frame");
   }
   auto stream = forge::datastream<std::uint8_t*>{output.data(), output.size()};
   raw::pack(stream, fields);
   return stream.tellp();
}

raw::unpack_limits decode_limits(std::size_t size) {
   if (size == 0 || size > frame_limit) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_input, "invalid expression worker input frame");
   }
   return {.max_container_elements = 65536,
           .max_total_container_elements = 100000,
           .max_bytes = static_cast<std::uint32_t>(size),
           .first_container_elements = 65536};
}

} // namespace

std::size_t encode_into(const request& value, const forge::variant_object& values, std::span<std::uint8_t> output) {
   validate_request(value.version, value.sequence);
   bounded_values(values);
   try {
      return encode_fields(request_fields(value, values), output);
   } catch (const forge::exceptions::base& error) {
      if (error.code() == raw::exceptions::make_error_code(raw::exceptions::code::range_error) ||
          error.code() == raw::exceptions::make_error_code(raw::exceptions::code::codec_error) ||
          error.code() == raw::exceptions::make_error_code(raw::exceptions::code::allocation_limit)) {
         FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression worker request exceeds transport capacity");
      }
      throw;
   }
}

std::size_t encode_into(const request& value, std::span<std::uint8_t> output) {
   return encode_into(value, value.values, output);
}

std::size_t encode_into(const response& value, std::span<std::uint8_t> output) {
   validate_response(value.version, value.sequence);
   return encode_fields(response_fields(value), output);
}

request decode_request(std::span<const std::uint8_t> input) {
   request result;
   using fields = owned_tuple_t<decltype(request_fields(result, result.values))>;
   request_fields(result, result.values) = raw::unpack_exact<fields>(input, decode_limits(input.size()));
   validate_request(result.version, result.sequence);
   if (result.op > operation::load) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_input, "unknown expression worker operation");
   }
   return result;
}

response decode_response(std::span<const std::uint8_t> input) {
   response result;
   using fields = owned_tuple_t<decltype(response_fields(result))>;
   try {
      response_fields(result) = raw::unpack_exact<fields>(input, decode_limits(input.size()));
   } catch (const std::bad_alloc&) {
      throw;
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "malformed expression worker response");
   }
   validate_response(result.version, result.sequence);
   if (result.code > status::internal || result.kind > value_kind::map) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "invalid expression worker response");
   }
   return result;
}

void encode_length(std::uint32_t length, std::span<std::uint8_t> output) {
   if (output.size() != prefix_bytes || length == 0 || length > frame_limit) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "invalid expression worker frame length");
   }
   auto stream = forge::datastream<std::uint8_t*>{output.data(), output.size()};
   raw::pack(stream, length);
}

std::uint32_t decode_length(std::span<const std::uint8_t> input) {
   if (input.size() != prefix_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "invalid expression worker frame prefix");
   }
   const auto length = raw::unpack_exact<std::uint32_t>(input, decode_limits(input.size()));
   if (length == 0 || length > frame_limit) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "invalid expression worker frame length");
   }
   return length;
}

} // namespace forge::expression::worker_protocol
