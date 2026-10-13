module;

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

module forge.api.core.error_projection;

namespace forge::api::core {

namespace {

std::string external_message(const forge::exceptions::base& error) {
   if (error.message().empty()) {
      return "request failed";
   }
   return error.message();
}

const error_descriptor* find_error(const method_descriptor& method, const forge::exceptions::base& error) noexcept {
   const auto& code = error.code();
   for (const auto& descriptor : method.errors) {
      if (descriptor.identity.category == code.category().name() &&
          descriptor.identity.code == static_cast<std::uint32_t>(code.value())) {
         return &descriptor;
      }
   }
   return nullptr;
}

struct core_error_descriptor {
   const char* name;
   status status_code;
   bool retryable;
};

[[nodiscard]] core_error_descriptor describe_core_error(exceptions::code code) noexcept {
   switch (code) {
   case exceptions::code::method_not_found:
      return {"method_not_found", status::not_found, false};
   case exceptions::code::incompatible_version:
      return {"incompatible_version", status::failed_precondition, false};
   case exceptions::code::codec_failed:
      return {"codec_failed", status::invalid_argument, false};
   case exceptions::code::deadline_exceeded:
      return {"deadline_exceeded", status::deadline_exceeded, true};
   case exceptions::code::cancelled:
      return {"cancelled", status::unavailable, true};
   case exceptions::code::remote_internal:
      return {"internal", status::internal, false};
   case exceptions::code::protocol_error:
      return {"protocol_error", status::invalid_argument, false};
   case exceptions::code::resource_exhausted:
      return {"resource_exhausted", status::resource_exhausted, true};
   case exceptions::code::server_supplied_unavailable:
      return {"server_supplied_unavailable", status::failed_precondition, false};
   case exceptions::code::invocation_denied:
      return {"invocation_denied", status::permission_denied, false};
   }
   return {"internal", status::internal, false};
}

[[nodiscard]] bool is_core_error(const forge::exceptions::base& error) noexcept {
   return std::string_view{error.code().category().name()} == "forge.api";
}

[[nodiscard]] std::string remote_message(const error_payload& payload) {
   return payload.message.empty() ? std::string{"remote API error"} : payload.message;
}

[[nodiscard]] auto remote_fields(const error_payload& payload) {
   return forge::exceptions::make_fields(forge::exceptions::ctx("remote.error", payload.error),
                                         forge::exceptions::ctx("remote.category", payload.identity.category),
                                         forge::exceptions::ctx("remote.code", payload.identity.code));
}

error_payload make_error_payload(const forge::exceptions::base& error, const error_descriptor* descriptor) {
   if (descriptor == nullptr) {
      return make_internal_error_payload();
   }

   const auto& code = error.code();
   return error_payload{
       .error = descriptor->name,
       .message = external_message(error),
       .retryable = descriptor->retryable,
       .status_code = descriptor->status_code,
       .identity =
           {
               .category = code.category().name(),
               .code = static_cast<std::uint32_t>(code.value()),
           },
   };
}

} // namespace

error_payload project_error(const method_descriptor& method, const forge::exceptions::base& error) {
   if (const auto* descriptor = find_error(method, error)) {
      return make_error_payload(error, descriptor);
   }
   if (is_core_error(error)) {
      return make_core_error_payload(static_cast<exceptions::code>(error.code().value()), external_message(error));
   }
   return make_internal_error_payload();
}

error_payload make_core_error_payload(exceptions::code code, std::string message) {
   const auto descriptor = describe_core_error(code);
   return error_payload{
       .error = descriptor.name,
       .message = std::move(message),
       .retryable = descriptor.retryable,
       .status_code = descriptor.status_code,
       .identity =
           {
               .category = "forge.api",
               .code = static_cast<std::uint32_t>(code),
           },
   };
}

error_payload make_internal_error_payload(std::string safe_message) {
   return make_core_error_payload(exceptions::code::remote_internal, std::move(safe_message));
}

void raise_remote_error(const error_payload& payload, const method_descriptor* method) {
   if (method != nullptr) {
      for (const auto& descriptor : method->errors) {
         if (descriptor.identity == payload.identity && descriptor.thrower) {
            descriptor.thrower(payload);
         }
      }
   }

   if (payload.identity.category == "forge.api") {
      const auto message = remote_message(payload);
      const auto fields = remote_fields(payload);
      switch (static_cast<exceptions::code>(payload.identity.code)) {
      case exceptions::code::method_not_found:
         throw exceptions::method_not_found{message, fields};
      case exceptions::code::incompatible_version:
         throw exceptions::incompatible_version{message, fields};
      case exceptions::code::codec_failed:
         throw exceptions::codec_failed{message, fields};
      case exceptions::code::deadline_exceeded:
         throw exceptions::deadline_exceeded{message, fields};
      case exceptions::code::cancelled:
         throw exceptions::cancelled{message, fields};
      case exceptions::code::remote_internal:
         throw exceptions::remote_internal{message, fields};
      case exceptions::code::protocol_error:
         throw exceptions::protocol_error{message, fields};
      case exceptions::code::resource_exhausted:
         throw exceptions::resource_exhausted{message, fields};
      case exceptions::code::server_supplied_unavailable:
         throw exceptions::server_supplied_unavailable{message, fields};
      case exceptions::code::invocation_denied:
         throw exceptions::invocation_denied{message, fields};
      }
   }

   throw exceptions::remote_internal{remote_message(payload), remote_fields(payload)};
}

} // namespace forge::api::core
