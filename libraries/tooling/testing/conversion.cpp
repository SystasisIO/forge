module;

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include <forge/exceptions/macros.hpp>

module forge.tooling.testing.conversion;

import forge.expression.exceptions;

namespace forge::core {

conversion<forge::variant_object>::conversion(const forge::expression::arguments& inputs) {
   auto names = std::set<std::string_view>{};
   auto result = forge::mutable_variant_object{};
   for (const auto& input : inputs) {
      if (!names.insert(input.name).second)
         FORGE_THROW_EXCEPTION(forge::expression::exceptions::invalid_input, "duplicate expression input");
      result(input.name, std::visit(
                             [](const auto& item) -> forge::variant {
                                using value_type = std::decay_t<decltype(item)>;
                                if constexpr (std::is_same_v<value_type, std::vector<std::int64_t>> ||
                                              std::is_same_v<value_type, std::vector<std::uint64_t>>) {
                                   auto values = forge::variants{};
                                   values.reserve(item.size());
                                   for (const auto value : item)
                                      values.emplace_back(value);
                                   return forge::variant{std::move(values)};
                                } else if constexpr (std::is_same_v<value_type, std::map<std::string, std::int64_t>> ||
                                                     std::is_same_v<value_type, std::map<std::string, std::uint64_t>>) {
                                   auto values = forge::mutable_variant_object{};
                                   for (const auto& [key, value] : item)
                                      values(key, forge::variant{value});
                                   return forge::variant{std::move(values)};
                                } else {
                                   return forge::variant{item};
                                }
                             },
                             input.data));
   }
   _value.emplace(std::move(result));
}

forge::variant_object conversion<forge::variant_object>::finish() && {
   if (!_value)
      FORGE_THROW_EXCEPTION(forge::expression::exceptions::invalid_input, "expression input conversion was consumed");
   auto result = std::move(*_value);
   _value.reset();
   return result;
}

conversion<forge::expression::result>::conversion(const forge::expression::evaluation& input) {
   auto result = forge::expression::result{};
   if (input.value.is_int64())
      result.value = input.value.as_int64();
   else if (input.value.is_uint64())
      result.value = input.value.as_uint64();
   else if (input.value.is_bool())
      result.value = input.value.as_bool();
   else
      FORGE_THROW_EXCEPTION(forge::expression::exceptions::internal, "invalid expression result type");
   result.ast_events = input.ast_events;
   _value.emplace(std::move(result));
}

forge::expression::result conversion<forge::expression::result>::finish() && {
   if (!_value)
      FORGE_THROW_EXCEPTION(forge::expression::exceptions::internal, "expression result conversion was consumed");
   auto result = std::move(*_value);
   _value.reset();
   return result;
}

} // namespace forge::core
