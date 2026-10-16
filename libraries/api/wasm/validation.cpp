module;

#include <forge/exceptions/policy.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

module forge.api.wasm.validation;

#if !defined(FORGE_CONTRACT_GUEST)
import forge.exceptions;
#endif

namespace forge::api::wasm {
namespace {

bool token(std::string_view value) {
   return !value.empty() && std::ranges::all_of(value, [](unsigned char character) {
      return character > 0x20U && character < 0x7fU;
   });
}

void byte_bound(std::size_t size, std::size_t limit, const limits& bounds) {
   if (size > limit || size > bounds.wire.max_bytes || size > raw::max_byte_array_size) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::limit_exceeded, "Value exceeds protocol byte limits");
   }
}

void count_bound(std::size_t size, const limits& bounds) {
   const auto limit = std::min({bounds.wire.max_container_elements,
                                bounds.wire.max_total_container_elements,
                                bounds.wire.first_container_elements, raw::max_array_elements});
   if (size > limit) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::limit_exceeded, "Value exceeds protocol container limits");
   }
}

template <typename T> std::size_t wire_size(const T& value, const limits& bounds) {
   const auto size = raw::pack_size(value);
   if (size > bounds.wire.max_bytes) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::limit_exceeded, "Aggregate exceeds the wire byte limit");
   }
   return size;
}

std::size_t payload_size(const payload& value, const limits& bounds) {
   validate_type_name(value.type, bounds);
   byte_bound(value.data.size(), bounds.payload_bytes, bounds);
   return wire_size(value, bounds);
}

void add_size(std::size_t& total, std::size_t size, const limits& bounds) {
   if (size > bounds.wire.max_bytes || total > bounds.wire.max_bytes - size) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::limit_exceeded, "Aggregate exceeds the wire byte limit");
   }
   total += size;
}

bool bodyless_status(std::uint16_t status) {
   return status < 200 || status == 204 || status == 304;
}

void policy_type(std::string_view value, const limits& bounds) {
   if (!token(value)) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_capability, "Capability requires a valid type or binding");
   }
   byte_bound(value.size(), bounds.name_bytes, bounds);
}

void body_policy(const capability::body_policy& allowed, bool response, const limits& bounds) {
   using presence = capability::body_presence;
   switch (allowed.presence) {
      case presence::optional:
      case presence::required:
         policy_type(allowed.type, bounds);
         return;
      case presence::status_only:
         if (!response) {
            FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_capability, "Status-only policy requires a response");
         }
         [[fallthrough]];
      case presence::absent:
         if (!allowed.type.empty()) {
            FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_capability, "Absent body policy must not declare a type");
         }
         return;
   }
   FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_capability, "Unknown body presence policy");
}

void http_policy(const capability::http_policy& allowed, const limits& bounds) {
   body_policy(allowed.request_body, false, bounds);
   count_bound(allowed.responses.size(), bounds);
   if (allowed.responses.empty() || allowed.responses.size() > 500U) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_capability, "HTTP capability requires explicit unique statuses");
   }
   auto seen = std::array<bool, 600>{};
   for (const auto& rule : allowed.responses) {
      if (rule.status < 100 || rule.status > 599 || seen[rule.status]) {
         FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_capability, "Invalid or duplicate HTTP capability status");
      }
      seen[rule.status] = true;
      body_policy(rule.body, true, bounds);
      if (bodyless_status(rule.status) && rule.body.presence != capability::body_presence::absent) {
         FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_capability, "Bodyless HTTP status requires an absent body");
      }
   }
}

template <typename PresenceError>
void body(const std::optional<payload>& value, const capability::body_policy& allowed, const limits& bounds) {
   using presence = capability::body_presence;
   if (allowed.presence == presence::absent || allowed.presence == presence::status_only) {
      if (value) {
         FORGE_POLICY_THROW_EXCEPTION(PresenceError, "HTTP body must be absent");
      }
      return;
   }
   if (!value) {
      if (allowed.presence == presence::required) {
         FORGE_POLICY_THROW_EXCEPTION(PresenceError, "HTTP body is required");
      }
      return;
   }
   validate(*value, allowed.type, bounds);
}

const capability& find_capability(std::string_view binding, std::span<const capability> allowed,
                                  const limits& bounds) {
   count_bound(allowed.size(), bounds);
   const capability* selected = nullptr;
   for (const auto& item : allowed) {
      if (item.binding == binding) {
         if (selected) {
            FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_capability, "Ambiguous capability binding");
         }
         selected = &item;
      }
   }
   if (!selected) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::forbidden_effect, "Effect binding is not allowed");
   }
   return *selected;
}

} // namespace

void validate_type_name(std::string_view value, const limits& bounds) {
   byte_bound(value.size(), bounds.name_bytes, bounds);
   if (!token(value)) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_value, "Invalid protocol type name");
   }
}

void validate(const payload& value, const limits& bounds) {
   payload_size(value, bounds);
}

void validate(const payload& value, std::string_view expected_type, const limits& bounds) {
   validate(value, bounds);
   validate_type_name(expected_type, bounds);
   if (value.type != expected_type) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::wrong_type, "Payload type does not match the pinned type");
   }
}

void validate(const continuation& value, const limits& bounds) {
   validate(value.state, bounds);
   if (wire_size(value, bounds) > bounds.continuation_bytes) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::limit_exceeded, "Continuation exceeds protocol limits");
   }
}

void validate(const continuation& value, std::uint32_t expected_version, std::string_view expected_type,
              const limits& bounds) {
   validate(value, bounds);
   if (value.version != expected_version) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::unsupported_version, "Continuation version does not match the pinned version");
   }
   validate(value.state, expected_type, bounds);
}

void validate(const effect& value, const limits& bounds) {
   validate_type_name(value.binding, bounds);
   validate(value.request, bounds);
   validate_type_name(value.response_type, bounds);
   wire_size(value, bounds);
}

void validate(const awaiting& value, const limits& bounds) {
   validate(value.command, bounds);
   validate(value.next, bounds);
   wire_size(value, bounds);
}

void validate(const completed& value, const limits& bounds) {
   validate(value.output, bounds);
   wire_size(value, bounds);
}

void validate(const step& value, const limits& bounds) {
   if (value.result.valueless_by_exception()) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_value, "Invalid step alternative");
   }
   std::visit([&](const auto& item) { validate(item, bounds); }, value.result);
   wire_size(value, bounds);
}

void validate(const event& value, const limits& bounds) {
   if (value.value.valueless_by_exception()) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_value, "Invalid event alternative");
   }
   std::visit([&](const auto& item) { validate(item, bounds); }, value.value);
   wire_size(value, bounds);
}

void validate(const subject_view& value, const limits& bounds) {
   byte_bound(value.issuer.size(), bounds.name_bytes, bounds);
   byte_bound(value.subject.size(), bounds.name_bytes, bounds);
   if (value.issuer.empty() || value.subject.empty()) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_value, "Subject facts require an issuer and subject");
   }
   wire_size(value, bounds);
}

void validate(const context& value, const limits& bounds) {
   count_bound(value.facts.size(), bounds);
   auto total = std::size_t{};
   if (value.subject) {
      validate(*value.subject, bounds);
      add_size(total, raw::pack_size(*value.subject), bounds);
   }
   for (const auto& fact : value.facts) {
      add_size(total, payload_size(fact, bounds), bounds);
   }
   wire_size(value, bounds);
}

void validate(const http_command& value, const limits& bounds) {
   switch (value.method) {
      case net::http::method::delete_:
      case net::http::method::get:
      case net::http::method::head:
      case net::http::method::options:
      case net::http::method::patch:
      case net::http::method::post:
      case net::http::method::put:
         break;
      default:
         FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_value, "Unknown HTTP intent method");
   }
   byte_bound(value.target.size(), bounds.target_bytes, bounds);
   if (value.target.empty() || value.target.front() != '/' || value.target.starts_with("//") ||
       std::ranges::any_of(value.target, [](unsigned char character) {
          return character <= 0x20U || character == 0x7fU || character == '\\' || character == '#';
       })) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_value, "Expected a bounded origin-form HTTP intent");
   }
   if (value.body) {
      validate(*value.body, bounds);
   }
   wire_size(value, bounds);
}

void validate(const http_response& value, const limits& bounds) {
   if (value.status < 100 || value.status > 599 || (bodyless_status(value.status) && value.body)) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_http_response, "Invalid HTTP response status or body");
   }
   if (value.body) {
      validate(*value.body, bounds);
   }
   wire_size(value, bounds);
}

void validate(const http_stream_open& value, const limits& bounds) {
   validate(value.request, bounds);
   validate_type_name(value.event_type, bounds);
   byte_bound(value.end_marker.size(), 64U, bounds);
   if (value.end_marker.empty() || value.end_marker.find_first_of("\r\n") != std::string::npos) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_value, "Invalid HTTP stream end marker");
   }
   wire_size(value, bounds);
}

void validate(const http_stream_read& value, const limits& bounds) {
   if (value.emission) {
      validate(*value.emission, bounds);
   }
   wire_size(value, bounds);
}

void validate(const http_stream_event& value, const limits& bounds) {
   if (value.status < 100 || value.status > 599) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_http_response, "Invalid HTTP stream response status");
   }
   if (value.item) {
      validate(*value.item, bounds);
   }
   wire_size(value, bounds);
}

void validate(const http_stream_event& value, std::string_view expected_item_type, const limits& bounds) {
   validate(value, bounds);
   validate_type_name(expected_item_type, bounds);
   if (value.item && value.item->type != expected_item_type) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::wrong_type, "Stream item does not match the pinned type");
   }
}

void validate(const core::error_payload& value, const limits& bounds) {
   byte_bound(value.error.size(), bounds.name_bytes, bounds);
   byte_bound(value.message.size(), bounds.wire.max_bytes, bounds);
   byte_bound(value.identity.category.size(), bounds.name_bytes, bounds);
   if (value.details_codec) {
      byte_bound(value.details_codec->value.size(), bounds.name_bytes, bounds);
   }
   if (value.details) {
      byte_bound(value.details->size(), bounds.payload_bytes, bounds);
   }
   wire_size(value, bounds);
}

void validate(const capability& allowed, const limits& bounds) {
   policy_type(allowed.binding, bounds);
   policy_type(allowed.request_type, bounds);
   policy_type(allowed.response_type, bounds);
   if (allowed.http) {
      http_policy(*allowed.http, bounds);
   }
}

void validate(const effect& value, const capability& allowed, const limits& bounds) {
   validate(allowed, bounds);
   validate(value, bounds);
   if (value.binding != allowed.binding) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::forbidden_effect, "Effect binding is not allowed");
   }
   validate(value.request, allowed.request_type, bounds);
   if (value.response_type != allowed.response_type) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::wrong_type, "Effect result type does not match the pinned type");
   }
   if (value.after_ms > allowed.max_delay_ms) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::limit_exceeded, "Effect delay exceeds the allowed limit");
   }
   if (allowed.http) {
      const auto command = raw::unpack_exact<http_command>(value.request.data, bounds.wire);
      validate(command, *allowed.http, bounds);
   }
}

void validate(const effect& value, std::span<const capability> allowed, const limits& bounds) {
   validate(value, find_capability(value.binding, allowed, bounds), bounds);
}

void validate(const step& value, std::span<const capability> allowed, const limits& bounds) {
   validate(value, bounds);
   if (const auto* pending = std::get_if<awaiting>(&value.result)) {
      validate(pending->command, allowed, bounds);
   }
}

void validate(const event& value, const effect& pending, const capability& allowed, const limits& bounds) {
   validate(pending, allowed, bounds);
   validate(value, bounds);
   if (const auto* result = std::get_if<payload>(&value.value)) {
      validate(*result, allowed.response_type, bounds);
      if (allowed.http) {
         const auto response = raw::unpack_exact<http_response>(result->data, bounds.wire);
         validate(response, *allowed.http, bounds);
      }
   }
}

void validate(const event& value, const effect& pending, std::span<const capability> allowed, const limits& bounds) {
   validate(value, pending, find_capability(pending.binding, allowed, bounds), bounds);
}

void validate(const http_command& value, const capability::http_policy& allowed, const limits& bounds) {
   http_policy(allowed, bounds);
   validate(value, bounds);
   body<exceptions::forbidden_effect>(value.body, allowed.request_body, bounds);
}

void validate(const http_response& value, const capability::http_policy& allowed, const limits& bounds) {
   http_policy(allowed, bounds);
   validate(value, bounds);
   const auto found = std::ranges::find(allowed.responses, value.status, &capability::http_status::status);
   if (found == allowed.responses.end()) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_http_response, "HTTP response status is not allowed");
   }
   body<exceptions::invalid_http_response>(value.body, found->body, bounds);
}

} // namespace forge::api::wasm
