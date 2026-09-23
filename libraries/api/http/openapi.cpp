module;

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <typeinfo>
#include <utility>
#include <vector>

module forge.api.http.openapi;

import forge.api.core.exceptions;
import forge.net.http.types;

namespace forge::api::http::detail {
namespace {

[[nodiscard]] std::string verb_name(forge::net::http::method verb) {
   using forge::net::http::method;
   switch (verb) {
   case method::get:
      return "get";
   case method::head:
      return "head";
   case method::post:
      return "post";
   case method::put:
      return "put";
   case method::patch:
      return "patch";
   case method::delete_:
      return "delete";
   default:
      return "post";
   }
}

[[nodiscard]] std::string status_name(forge::net::http::status value) {
   return std::to_string(static_cast<unsigned>(value));
}

struct target_description {
   std::string path;
   std::string collision_path;
   std::vector<std::string> path_fields;
   std::vector<field_binding> query;
};

[[nodiscard]] std::string normalize_base_path(std::string_view base_path) {
   if (base_path.empty() || base_path == "/") {
      return {};
   }
   if (base_path.front() != '/' || base_path.find('?') != std::string_view::npos ||
       base_path.find('#') != std::string_view::npos) {
      throw forge::api::core::exceptions::protocol_error{"OpenAPI base path is malformed"};
   }
   while (base_path.size() > 1U && base_path.ends_with('/')) {
      base_path.remove_suffix(1U);
   }
   return std::string{base_path};
}

[[nodiscard]] std::string with_base_path(std::string_view base_path, std::string_view target) {
   const auto base = normalize_base_path(base_path);
   if (base.empty()) {
      return std::string{target};
   }
   const auto query = target.find('?');
   const auto path = target.substr(0U, query);
   if (path.empty() || path.front() != '/') {
      throw forge::api::core::exceptions::protocol_error{"OpenAPI route path is malformed"};
   }
   auto result = path == "/" ? base : base + std::string{path};
   if (query != std::string_view::npos) {
      result.append(target.substr(query));
   }
   return result;
}

[[nodiscard]] target_description describe_target(std::string_view target) {
   auto output = target_description{};
   const auto question = target.find('?');
   const auto path = target.substr(0, question);
   output.path.reserve(path.size());
   output.collision_path.reserve(path.size());
   for (auto position = std::size_t{}; position < path.size();) {
      if (path[position] == ':' && (position == 0U || path[position - 1U] == '/')) {
         const auto end = path.find('/', position + 1U);
         const auto size = (end == std::string_view::npos ? path.size() : end) - position - 1U;
         auto name = std::string{path.substr(position + 1U, size)};
         output.path_fields.push_back(name);
         output.path.push_back('{');
         output.path += name;
         output.path.push_back('}');
         output.collision_path += "{}";
         position += size + 1U;
         continue;
      }
      if (path[position] != '{') {
         output.path.push_back(path[position]);
         output.collision_path.push_back(path[position++]);
         continue;
      }
      const auto close = path.find('}', position + 1U);
      if (close == std::string_view::npos) {
         output.path.append(path.substr(position));
         output.collision_path.append(path.substr(position));
         break;
      }
      auto name = std::string{path.substr(position + 1U, close - position - 1U)};
      output.path_fields.push_back(name);
      output.path.push_back('{');
      output.path += name;
      output.path.push_back('}');
      output.collision_path += "{}";
      position = close + 1U;
   }

   if (question == std::string_view::npos) {
      return output;
   }
   auto query = target.substr(question + 1U);
   while (!query.empty()) {
      const auto separator = query.find('&');
      const auto entry = query.substr(0, separator);
      const auto equals = entry.find('=');
      if (equals != std::string_view::npos) {
         auto name = std::string{entry.substr(0, equals)};
         auto field = std::string{entry.substr(equals + 1U)};
         if (field.size() >= 2U && field.front() == '{' && field.back() == '}') {
            field = field.substr(1U, field.size() - 2U);
         }
         output.query.push_back(field_binding{.field = std::move(field), .name = std::move(name)});
      }
      if (separator == std::string_view::npos) {
         break;
      }
      query.remove_prefix(separator + 1U);
   }
   return output;
}

[[nodiscard]] const openapi_field* find_field(const std::vector<openapi_field>& fields, std::string_view name) {
   const auto iterator = std::ranges::find(fields, name, &openapi_field::name);
   return iterator == fields.end() ? nullptr : &*iterator;
}

[[nodiscard]] forge::variant parameter(const std::vector<openapi_field>& fields, std::string_view field_name,
                                       std::string wire_name, std::string location, bool force_required) {
   const auto* field = find_field(fields, field_name);
   auto value = forge::mutable_variant_object{}("name", std::move(wire_name))("in", std::move(location))(
       "required", force_required || (field != nullptr && field->required));
   if (field != nullptr && field->json_parameter) {
      auto content = forge::mutable_variant_object{};
      content.set("application/json", forge::variant{forge::mutable_variant_object{}("schema", field->schema)});
      value("content", std::move(content));
   } else {
      value("schema", field == nullptr ? unconstrained_schema("unknown request field") : field->schema);
   }
   return forge::variant{std::move(value)};
}

[[nodiscard]] forge::variant media_schema(const forge::variant& schema, std::string_view content_type) {
   auto content = forge::mutable_variant_object{};
   content.set(std::string{content_type}, forge::variant{forge::mutable_variant_object{}("schema", schema)});
   return forge::variant{std::move(content)};
}

[[nodiscard]] forge::variant binary_schema() {
   return forge::variant{forge::mutable_variant_object{}("type", "string")("format", "binary")};
}

[[nodiscard]] forge::variant string_schema() {
   return forge::variant{forge::mutable_variant_object{}("type", "string")};
}

struct request_body_description {
   bool present = false;
   bool required = false;
   forge::variant schema;
   std::string content_type;
};

[[nodiscard]] bool mapped_request_field(const target_description& target, const route& mapping,
                                        const openapi_field& field) {
   switch (field.source) {
   case openapi_field_source::query:
   case openapi_field_source::header:
   case openapi_field_source::cookie:
   case openapi_field_source::form:
   case openapi_field_source::upload:
   case openapi_field_source::server_supplied:
      return true;
   case openapi_field_source::body:
   case openapi_field_source::body_stream:
   case openapi_field_source::body_bytes:
      return false;
   case openapi_field_source::value:
      break;
   }
   const auto matches_field = [&field](const field_binding& entry) { return entry.field == field.name; };
   return std::ranges::find(target.path_fields, field.name) != target.path_fields.end() ||
          std::ranges::find_if(target.query, matches_field) != target.query.end() ||
          std::ranges::find_if(mapping.headers, matches_field) != mapping.headers.end() ||
          std::ranges::find_if(mapping.forms, matches_field) != mapping.forms.end() ||
          (mapping.body_stream_field.has_value() && *mapping.body_stream_field == field.name);
}

[[nodiscard]] bool empty_object_schema(const forge::variant& schema) {
   if (!schema.is_object()) {
      return false;
   }
   const auto& object = schema.get_object();
   const auto type = object.find("type");
   if (type == object.end() || !type->value().is_string() || type->value().as_string() != "object") {
      return false;
   }
   const auto maximum = object.find("maxProperties");
   if (maximum != object.end() && maximum->value().is_uint64() && maximum->value().as_uint64() == 0U) {
      return true;
   }
   const auto properties = object.find("properties");
   const auto additional = object.find("additionalProperties");
   return properties != object.end() && properties->value().is_object() &&
          properties->value().get_object().size() == 0U && additional != object.end() &&
          additional->value().is_bool() && !additional->value().as_bool();
}

[[nodiscard]] bool has_binding(const std::vector<field_binding>& bindings, std::string_view field) {
   return std::ranges::find(bindings, field, &field_binding::field) != bindings.end();
}

[[nodiscard]] std::string header_name_from_field(std::string_view name) {
   auto output = std::string{};
   output.reserve(name.size());
   for (const auto character : name) {
      output.push_back(character == '_' ? '-' : character);
   }
   return output;
}

[[nodiscard]] std::string mapped_name(const std::vector<field_binding>& bindings, std::string_view field) {
   const auto found = std::ranges::find(bindings, field, &field_binding::field);
   return found == bindings.end() ? std::string{field} : found->name;
}

[[nodiscard]] request_body_description multipart_request_body(const openapi_operation& operation,
                                                              const std::vector<openapi_field>& fields) {
   auto properties = forge::mutable_variant_object{};
   auto required = forge::variants{};
   for (const auto& field : fields) {
      if (field.source != openapi_field_source::form && field.source != openapi_field_source::upload) {
         continue;
      }
      auto name = mapped_name(operation.mapping.forms, field.name);
      if (properties.find(name) != properties.end()) {
         throw forge::api::core::exceptions::protocol_error{"OpenAPI multipart field name is ambiguous"};
      }
      properties.set(name, field.source == openapi_field_source::upload ? binary_schema() : field.schema);
      if (field.required) {
         required.emplace_back(std::move(name));
      }
   }
   if (properties.size() == 0U) {
      throw forge::api::core::exceptions::protocol_error{"OpenAPI multipart request has no form fields"};
   }
   auto schema = forge::mutable_variant_object{}("type", "object")("properties", std::move(properties))(
       "additionalProperties", false);
   if (!required.empty()) {
      schema("required", std::move(required));
   }
   return {.present = true,
           .required = true,
           .schema = forge::variant{std::move(schema)},
           .content_type = "multipart/form-data"};
}

[[nodiscard]] request_body_description describe_request_body(const openapi_operation& operation,
                                                             const target_description& target,
                                                             const forge::api::core::method_descriptor* method,
                                                             const std::vector<openapi_field>& fields) {
   if (!uses_request_body(operation.mapping.verb)) {
      return {};
   }
   const auto has_multipart =
       !operation.mapping.forms.empty() || std::ranges::any_of(fields, [](const openapi_field& field) {
          return field.source == openapi_field_source::form || field.source == openapi_field_source::upload;
       });
   const auto has_raw =
       operation.mapping.body_stream_field.has_value() || std::ranges::any_of(fields, [](const openapi_field& field) {
          return field.source == openapi_field_source::body_stream || field.source == openapi_field_source::body_bytes;
       });
   const auto has_typed_body = std::ranges::any_of(
       fields, [](const openapi_field& field) { return field.source == openapi_field_source::body; });
   if (static_cast<unsigned>(has_multipart) + static_cast<unsigned>(has_raw) + static_cast<unsigned>(has_typed_body) >
       1U) {
      throw forge::api::core::exceptions::protocol_error{"OpenAPI request mixes incompatible body sources"};
   }
   if (has_multipart) {
      return multipart_request_body(operation, fields);
   }
   const auto raw = std::ranges::find_if(fields, [](const openapi_field& field) {
      return field.source == openapi_field_source::body_stream || field.source == openapi_field_source::body_bytes;
   });
   if (has_raw) {
      if (raw == fields.end()) {
         throw forge::api::core::exceptions::protocol_error{"OpenAPI raw request body field is missing"};
      }
      const auto another = std::ranges::find_if(std::next(raw), fields.end(), [](const openapi_field& field) {
         return field.source == openapi_field_source::body_stream || field.source == openapi_field_source::body_bytes;
      });
      if (another != fields.end()) {
         throw forge::api::core::exceptions::protocol_error{"OpenAPI request has multiple raw body fields"};
      }
      if (operation.mapping.body_stream_field.has_value() &&
          (raw->source != openapi_field_source::body_stream || raw->name != *operation.mapping.body_stream_field)) {
         throw forge::api::core::exceptions::protocol_error{"OpenAPI body stream field does not match the route"};
      }
      return {.present = true, .required = true, .schema = binary_schema(), .content_type = "*/*"};
   }
   if (fields.empty()) {
      if (operation.positional_request) {
         return {};
      }
      if (empty_object_schema(operation.request_schema)) {
         return {};
      }
      if (method != nullptr && (method->request_type == typeid(void) || method->request_type == typeid(std::tuple<>))) {
         return {};
      }
      if (method != nullptr && !method->argument_names.empty()) {
         const auto unmapped = std::ranges::find_if(method->argument_names, [&](std::string_view name) {
            const auto field = openapi_field{.name = std::string{name}};
            return !mapped_request_field(target, operation.mapping, field);
         });
         if (unmapped == method->argument_names.end()) {
            return {};
         }
      }
      return {.present = true,
              .required = true,
              .schema = operation.request_schema,
              .content_type = std::string{content_type(operation.mapping.request_body_codec)}};
   }

   auto candidates = std::vector<const openapi_field*>{};
   for (const auto& field : fields) {
      if (!mapped_request_field(target, operation.mapping, field)) {
         candidates.push_back(&field);
      }
   }
   if (candidates.empty()) {
      return {};
   }
   if (operation.positional_request && candidates.size() > 1U) {
      throw forge::api::core::exceptions::protocol_error{"OpenAPI positional request has multiple body candidates"};
   }
   const auto explicit_body = std::ranges::find_if(
       candidates, [](const openapi_field* field) { return field->source == openapi_field_source::body; });
   if (explicit_body != candidates.end() && candidates.size() > 1U) {
      throw forge::api::core::exceptions::protocol_error{"OpenAPI request has multiple body candidates"};
   }
   const auto schema = operation.positional_request || explicit_body != candidates.end() ? candidates.front()->schema
                                                                                         : operation.request_schema;
   const auto required = std::ranges::any_of(candidates, &openapi_field::required);
   auto result =
       request_body_description{.present = true,
                                .required = required,
                                .schema = schema,
                                .content_type = std::string{content_type(operation.mapping.request_body_codec)}};
   return result;
}

[[nodiscard]] forge::variant declared_error_document(const forge::api::core::error_descriptor& error) {
   return forge::variant{forge::mutable_variant_object{}("name", error.name)(
       "status_code", static_cast<std::uint64_t>(error.status_code))("retryable", error.retryable)(
       "identity", forge::mutable_variant_object{}("category", error.identity.category)(
                       "code", static_cast<std::uint64_t>(error.identity.code)))};
}

[[nodiscard]] forge::variant error_response_schema(const forge::api::core::method_descriptor* method) {
   auto schema = forge::mutable_variant_object{make_json_schema<forge::api::core::error_payload>()};
   if (method != nullptr && !method->errors.empty()) {
      auto errors = forge::variants{};
      errors.reserve(method->errors.size());
      for (const auto& error : method->errors) {
         errors.push_back(declared_error_document(error));
      }
      schema("x-forge-declared-errors", std::move(errors));
   }
   return forge::variant{std::move(schema)};
}

[[nodiscard]] forge::variant operation_document(const forge::api::core::descriptor& api,
                                                const openapi_operation& operation) {
   auto value = forge::mutable_variant_object{}("operationId", api.id.value + "." + operation.mapping.method_name);
   const auto* method = forge::api::core::find_method(api, operation.mapping.method_name);
   auto request_fields = operation.request_fields;
   if (operation.positional_request) {
      if (method == nullptr || method->argument_names.size() != request_fields.size()) {
         throw forge::api::core::exceptions::protocol_error{"OpenAPI positional argument metadata is incomplete"};
      }
      for (auto index = std::size_t{}; index < request_fields.size(); ++index) {
         request_fields[index].name = method->argument_names[index];
         if (request_fields[index].source != openapi_field_source::value &&
             request_fields[index].source != openapi_field_source::server_supplied) {
            throw forge::api::core::exceptions::protocol_error{
                "OpenAPI positional methods cannot use HTTP parameter wrappers"};
         }
      }
      const auto target = describe_target(operation.mapping.target);
      for (const auto& field : request_fields) {
         if (field.source != openapi_field_source::server_supplied) {
            continue;
         }
         if (std::ranges::find(target.path_fields, field.name) != target.path_fields.end() ||
             has_binding(target.query, field.name) || has_binding(operation.mapping.headers, field.name) ||
             has_binding(operation.mapping.forms, field.name) ||
             (operation.mapping.body_stream_field && *operation.mapping.body_stream_field == field.name)) {
            throw forge::api::core::exceptions::protocol_error{
                "OpenAPI server-supplied argument cannot be bound to a request field"};
         }
      }
      std::erase_if(request_fields, [](const openapi_field& field) {
         return field.source == openapi_field_source::server_supplied;
      });
   }
   auto parameters = forge::variants{};
   struct parameter_identity {
      std::string field;
      std::string wire_name;
      std::string location;
   };
   auto parameter_identities = std::vector<parameter_identity>{};
   const auto append_parameter = [&](std::string_view field, std::string wire_name, std::string location,
                                     bool force_required) {
      auto comparison_name = wire_name;
      if (location == "header") {
         std::ranges::transform(comparison_name, comparison_name.begin(),
                                [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
      }
      const auto existing = std::ranges::find_if(parameter_identities, [&](const parameter_identity& value) {
         return value.wire_name == comparison_name && value.location == location;
      });
      if (existing != parameter_identities.end()) {
         if (existing->field == field) {
            return;
         }
         throw forge::api::core::exceptions::protocol_error{"OpenAPI parameter wire name is ambiguous"};
      }
      parameter_identities.push_back(
          parameter_identity{.field = std::string{field}, .wire_name = comparison_name, .location = location});
      parameters.push_back(parameter(request_fields, field, std::move(wire_name), std::move(location), force_required));
   };
   const auto target = describe_target(operation.mapping.target);
   auto query_object_fields = std::vector<std::string>{};
   for (const auto& query_object : operation.query_objects) {
      if (query_object.name.empty() || query_object.fields.empty() || !query_object.schema.is_object()) {
         throw forge::api::core::exceptions::protocol_error{"OpenAPI query object metadata is incomplete"};
      }
      const auto& schema = query_object.schema.get_object();
      const auto properties = schema.find("properties");
      if (properties == schema.end() || !properties->value().is_object()) {
         throw forge::api::core::exceptions::protocol_error{"OpenAPI query object schema has no properties"};
      }
      for (const auto& field : query_object.fields) {
         if (find_field(request_fields, field) == nullptr ||
             !properties->value().get_object().contains(field.c_str())) {
            throw forge::api::core::exceptions::protocol_error{"OpenAPI query object field is not described"};
         }
         const auto binding = std::ranges::find(target.query, field, &field_binding::field);
         if (binding == target.query.end() || binding->name != field ||
             std::ranges::find(query_object_fields, field) != query_object_fields.end()) {
            throw forge::api::core::exceptions::protocol_error{
                "OpenAPI query object fields require unique flat query bindings"};
         }
         query_object_fields.push_back(field);
      }
      const auto duplicate = std::ranges::find_if(parameter_identities, [&](const parameter_identity& identity) {
         return identity.location == "query" && identity.wire_name == query_object.name;
      });
      if (duplicate != parameter_identities.end()) {
         throw forge::api::core::exceptions::protocol_error{"OpenAPI query object parameter name is ambiguous"};
      }
      parameter_identities.push_back(
          parameter_identity{.field = query_object.name, .wire_name = query_object.name, .location = "query"});
      parameters.emplace_back(forge::mutable_variant_object{}("name", query_object.name)("in", "query")(
          "required", query_object.required)("style", "form")("explode", true)("schema", query_object.schema));
   }
   for (const auto& name : target.path_fields) {
      append_parameter(name, name, "path", true);
   }
   for (const auto& entry : target.query) {
      if (std::ranges::find(query_object_fields, entry.field) == query_object_fields.end()) {
         append_parameter(entry.field, entry.name, "query", false);
      }
   }
   for (const auto& entry : operation.mapping.headers) {
      append_parameter(entry.field, entry.name, "header", false);
   }
   for (const auto& field : request_fields) {
      switch (field.source) {
      case openapi_field_source::query:
         if (!has_binding(target.query, field.name) &&
             std::ranges::find(query_object_fields, field.name) == query_object_fields.end()) {
            append_parameter(field.name, field.name, "query", false);
         }
         break;
      case openapi_field_source::header:
         if (!has_binding(operation.mapping.headers, field.name)) {
            append_parameter(field.name, header_name_from_field(field.name), "header", false);
         }
         break;
      case openapi_field_source::cookie:
         append_parameter(field.name, field.name, "cookie", false);
         break;
      default:
         break;
      }
   }
   if (!parameters.empty()) {
      value("parameters", std::move(parameters));
   }

   const auto request_body = describe_request_body(operation, target, method, request_fields);
   if (request_body.present) {
      value("requestBody", forge::mutable_variant_object{}("required", request_body.required)(
                               "content", media_schema(request_body.schema, request_body.content_type)));
   }

   auto response = forge::mutable_variant_object{}("description", "Successful response");
   if (operation.mapping.verb != forge::net::http::method::head &&
       operation.response_body == openapi_response_body::binary) {
      response("content", media_schema(binary_schema(), "*/*"));
   } else if (operation.mapping.verb != forge::net::http::method::head &&
              operation.response_body == openapi_response_body::codec) {
      response("content", media_schema(operation.response_schema, content_type(operation.mapping.response_body_codec)));
   }
   auto responses = forge::mutable_variant_object{};
   responses.set(status_name(operation.mapping.success_status), forge::variant{std::move(response)});
   if (operation.mapping.response_file) {
      auto partial = forge::mutable_variant_object{}("description", "Partial file response");
      if (operation.mapping.verb != forge::net::http::method::head) {
         partial("content", media_schema(binary_schema(), "*/*"));
      }
      responses.set(status_name(forge::net::http::status::partial_content), forge::variant{std::move(partial)});
      responses.set(status_name(forge::net::http::status::not_modified),
                    forge::variant{forge::mutable_variant_object{}("description", "File not modified")});
      auto missing = forge::mutable_variant_object{}("description", "File not found");
      if (operation.mapping.verb != forge::net::http::method::head) {
         missing("content", media_schema(string_schema(), "text/plain"));
      }
      responses.set(status_name(forge::net::http::status::not_found), forge::variant{std::move(missing)});
      responses.set(status_name(forge::net::http::status::range_not_satisfiable),
                    forge::variant{forge::mutable_variant_object{}("description", "File range not satisfiable")});
   }
   responses.set("default", forge::variant{forge::mutable_variant_object{}("description", "Forge API error")(
                                "content", media_schema(error_response_schema(method),
                                                        content_type(operation.mapping.error_body_codec)))});
   value("responses", std::move(responses));
   if (operation.mapping.cache == cache_policy::no_store) {
      value("x-forge-cache-policy", "no-store");
   }
   return forge::variant{std::move(value)};
}

} // namespace

struct registered_operation {
   forge::api::core::descriptor api;
   openapi_operation operation;
};

[[nodiscard]] forge::variant build_openapi_document(std::vector<registered_operation> operations, openapi_info info) {
   if (info.title.empty()) {
      info.title = "Forge API";
   }
   if (info.version.empty()) {
      info.version = "1.0.0";
   }

   std::ranges::sort(operations, [](const auto& left, const auto& right) {
      const auto left_target = describe_target(left.operation.mapping.target);
      const auto right_target = describe_target(right.operation.mapping.target);
      if (left_target.path != right_target.path) {
         return left_target.path < right_target.path;
      }
      const auto left_verb = verb_name(left.operation.mapping.verb);
      const auto right_verb = verb_name(right.operation.mapping.verb);
      if (left_verb != right_verb) {
         return left_verb < right_verb;
      }
      return left.api.id.value + "." + left.operation.mapping.method_name <
             right.api.id.value + "." + right.operation.mapping.method_name;
   });

   auto paths = forge::mutable_variant_object{};
   for (const auto& registered : operations) {
      const auto target = describe_target(registered.operation.mapping.target);
      auto path = paths.find(target.path);
      auto methods =
          path == paths.end() ? forge::mutable_variant_object{} : forge::mutable_variant_object{path->value()};
      methods.set(verb_name(registered.operation.mapping.verb),
                  operation_document(registered.api, registered.operation));
      paths.set(target.path, forge::variant{std::move(methods)});
   }

   auto metadata = forge::mutable_variant_object{}("title", std::move(info.title))("version", std::move(info.version));
   if (!info.description.empty()) {
      metadata("description", std::move(info.description));
   }
   auto document =
       forge::mutable_variant_object{}("openapi", "3.1.0")("info", std::move(metadata))("paths", std::move(paths));
   if (!info.servers.empty()) {
      auto servers = forge::variants{};
      servers.reserve(info.servers.size());
      for (auto& url : info.servers) {
         servers.emplace_back(forge::mutable_variant_object{}("url", std::move(url)));
      }
      document("servers", std::move(servers));
   }
   return forge::variant{std::move(document)};
}

forge::variant build_openapi_document(const forge::api::core::descriptor& api,
                                      std::vector<openapi_operation> operations, openapi_info info) {
   if (info.title.empty()) {
      info.title = api.id.value;
   }
   if (info.version.empty()) {
      info.version = std::to_string(api.version.major) + "." + std::to_string(api.version.revision);
   }
   auto registered = std::vector<registered_operation>{};
   registered.reserve(operations.size());
   for (auto& operation : operations) {
      registered.push_back({.api = api, .operation = std::move(operation)});
   }
   return build_openapi_document(std::move(registered), std::move(info));
}

} // namespace forge::api::http::detail

namespace forge::api::http {

struct openapi_document_builder::impl {
   std::vector<detail::registered_operation> operations;
};

openapi_document_builder::openapi_document_builder() : impl_(std::make_unique<impl>()) {}

openapi_document_builder::~openapi_document_builder() = default;

openapi_document_builder::openapi_document_builder(openapi_document_builder&&) noexcept = default;

openapi_document_builder& openapi_document_builder::operator=(openapi_document_builder&&) noexcept = default;

void openapi_document_builder::add(forge::api::core::descriptor api, std::vector<openapi_operation> operations,
                                   std::string_view base_path) {
   for (auto& operation : operations) {
      operation.mapping.target = detail::with_base_path(base_path, operation.mapping.target);
   }

   auto candidates = std::vector<detail::registered_operation>{};
   candidates.reserve(operations.size());
   for (auto& operation : operations) {
      candidates.push_back({.api = api, .operation = std::move(operation)});
   }

   const auto reject_collision = [](const detail::registered_operation& candidate,
                                    const detail::registered_operation& existing) {
      const auto target = detail::describe_target(candidate.operation.mapping.target);
      const auto verb = detail::verb_name(candidate.operation.mapping.verb);
      const auto operation_id = candidate.api.id.value + "." + candidate.operation.mapping.method_name;
      const auto existing_target = detail::describe_target(existing.operation.mapping.target);
      if (existing_target.collision_path == target.collision_path &&
          detail::verb_name(existing.operation.mapping.verb) == verb) {
         throw forge::api::core::exceptions::protocol_error{"OpenAPI document has a duplicate HTTP verb and path"};
      }
      const auto existing_id = existing.api.id.value + "." + existing.operation.mapping.method_name;
      if (existing_id == operation_id) {
         if (existing.operation.request_schema != candidate.operation.request_schema ||
             existing.operation.response_schema != candidate.operation.response_schema) {
            throw forge::api::core::exceptions::protocol_error{"OpenAPI operation id has incompatible schemas"};
         }
         throw forge::api::core::exceptions::protocol_error{"OpenAPI document has a duplicate operation id"};
      }
   };
   for (auto candidate = candidates.begin(); candidate != candidates.end(); ++candidate) {
      for (const auto& existing : impl_->operations) {
         reject_collision(*candidate, existing);
      }
      for (auto prior = candidates.begin(); prior != candidate; ++prior) {
         reject_collision(*candidate, *prior);
      }
   }
   impl_->operations.insert(impl_->operations.end(), std::make_move_iterator(candidates.begin()),
                            std::make_move_iterator(candidates.end()));
}

forge::variant openapi_document_builder::build(openapi_info info) const {
   return detail::build_openapi_document(impl_->operations, std::move(info));
}

} // namespace forge::api::http
