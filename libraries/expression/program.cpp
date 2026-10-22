module;

#include "details/backend.hxx"

#include <array>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <forge/exceptions/macros.hpp>

module forge.expression.program;

import forge.expression.exceptions;

namespace forge::expression {
namespace {

namespace backend = detail::backend;

void check(backend::status status, std::string_view message) {
   if (status == backend::status::ok) {
      return;
   }
   // The private status is mapped at the Forge boundary. Diagnostics contain
   // static descriptions only; source expressions and input data stay private.
   exceptions::code code = exceptions::code::internal;
   switch (status) {
   case backend::status::invalid_profile:
      code = exceptions::code::invalid_profile;
      break;
   case backend::status::invalid_program:
      code = exceptions::code::invalid_program;
      break;
   case backend::status::invalid_input:
      code = exceptions::code::invalid_input;
      break;
   case backend::status::resource_exhausted:
      code = exceptions::code::resource_exhausted;
      break;
   case backend::status::evaluation_failed:
      code = exceptions::code::evaluation_failed;
      break;
   default:
      break;
   }
   FORGE_THROW_CODE(code, message);
}

backend::value_view view(const forge::variant& value);

backend::value_view element(const void* context, std::size_t index) {
   return view((*static_cast<const forge::variants*>(context))[index]);
}

backend::value_view mapped(const void* context, std::size_t index) {
   return view((static_cast<const forge::variant_object*>(context)->begin() + index)->value());
}

std::string_view key(const void* context, std::size_t index) {
   return (static_cast<const forge::variant_object*>(context)->begin() + index)->key();
}

backend::value_view view(const forge::variant& value) {
   backend::value_view output;
   switch (value.get_type()) {
   case forge::variant::int64_type:
      output.kind = backend::value_kind::int64;
      output.i64 = value.as_int64();
      break;
   case forge::variant::uint64_type:
      output.kind = backend::value_kind::uint64;
      output.u64 = value.as_uint64();
      break;
   case forge::variant::bool_type:
      output.kind = backend::value_kind::boolean;
      output.boolean = value.as_bool();
      break;
   case forge::variant::string_type:
      output.kind = backend::value_kind::string;
      output.text = value.get_string();
      break;
   case forge::variant::array_type:
      output.kind = backend::value_kind::list;
      output.context = &value.get_array();
      output.count = value.get_array().size();
      output.element = element;
      break;
   case forge::variant::object_type:
      output.kind = backend::value_kind::map;
      output.context = &value.get_object();
      output.count = value.get_object().size();
      output.element = mapped;
      output.key = key;
      break;
   default:
      break;
   }
   return output;
}

} // namespace

struct program::impl {
   std::unique_ptr<backend::program> executable;
   std::size_t variables = 0;
};

program::program(std::string_view source, std::span<const variable> variables, limits budget) try {
   if (variables.size() > limits{}.variables) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression declaration limit exceeded");
   }
   std::array<backend::declaration, limits{}.variables> declarations;
   std::size_t count = 0;
   for (const auto& item : variables) {
      backend::schema_kind kind;
      switch (item.kind) {
      case type::int64:
         kind = backend::schema_kind::int64;
         break;
      case type::uint64:
         kind = backend::schema_kind::uint64;
         break;
      case type::boolean:
         kind = backend::schema_kind::boolean;
         break;
      case type::string:
         kind = backend::schema_kind::string;
         break;
      case type::list_int64:
         kind = backend::schema_kind::list_int64;
         break;
      case type::list_uint64:
         kind = backend::schema_kind::list_uint64;
         break;
      case type::map_int64:
         kind = backend::schema_kind::map_int64;
         break;
      case type::map_uint64:
         kind = backend::schema_kind::map_uint64;
         break;
      default:
         FORGE_THROW_EXCEPTION(exceptions::invalid_profile, "unsupported expression variable type");
      }
      declarations[count++] = {item.name, kind};
   }
   auto prepared = backend::program::prepare(source, std::span{declarations.data(), count},
                                             {budget.source_bytes, budget.nodes, budget.depth, budget.variables,
                                              budget.collection_elements, budget.string_bytes, budget.total_input_bytes,
                                              budget.ast_events});
   check(prepared.code, prepared.message);
   _impl = std::make_unique<impl>(std::move(prepared.executable), budget.variables);
} catch (const std::bad_alloc&) {
   FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "native expression allocation failed");
}

program::~program() = default;
program::program(program&&) noexcept = default;
program& program::operator=(program&&) noexcept = default;

evaluation program::evaluate(const forge::variant_object& values) const {
   if (!_impl) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_profile, "expression program has been moved");
   }
   if (values.size() > _impl->variables) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression input count limit exceeded");
   }
   std::array<backend::named_value, limits{}.variables> inputs;
   std::size_t count = 0;
   for (const auto& item : values) {
      inputs[count++] = {item.key(), view(item.value())};
   }
   const auto result = _impl->executable->evaluate(std::span{inputs.data(), count});
   check(result.code, result.message);
   switch (result.kind) {
   case backend::value_kind::int64:
      return {forge::variant{result.i64}, result.ast_events};
   case backend::value_kind::uint64:
      return {forge::variant{result.u64}, result.ast_events};
   case backend::value_kind::boolean:
      return {forge::variant{result.boolean}, result.ast_events};
   default:
      FORGE_THROW_EXCEPTION(exceptions::internal, "expression backend returned an invalid result");
   }
}

} // namespace forge::expression
