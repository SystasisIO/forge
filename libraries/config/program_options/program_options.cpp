module;

#include <boost/program_options.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.config.program_options;

import forge.config.core.component;
import forge.config.core.decode;
import forge.config.core.document;
import forge.config.core.value;
import forge.schema.diagnostic;
import forge.schema.exceptions;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.enums;
import forge.schema.scalar;

namespace forge::config::program_options {
namespace {

namespace po = boost::program_options;

[[nodiscard]] bool supports_field(schema::value_kind kind) {
   return kind != schema::value_kind::object && kind != schema::value_kind::object_list &&
          kind != schema::value_kind::dynamic;
}

[[nodiscard]] std::string option_name(const config::core::component_descriptor& component, const std::string& field) {
   if (component.section.empty()) {
      return field;
   }
   return component.section + "." + field;
}

void add_field_option(po::options_description& description, const std::string& name, schema::value_kind kind,
                      const std::string& text) {
   auto display = text.empty() ? name : text;
   if (kind == schema::value_kind::boolean) {
      description.add_options()(name.c_str(), po::value<std::string>()->implicit_value("true"), display.c_str());
   } else if (kind == schema::value_kind::string_list) {
      description.add_options()(name.c_str(), po::value<std::vector<std::string>>()->composing(), display.c_str());
   } else {
      description.add_options()(name.c_str(), po::value<std::string>(), display.c_str());
   }
}

[[nodiscard]] po::options_description build_description(const config::core::component_registry& registry,
                                                        std::string caption,
                                                        bool include_ingestion_only) {
   auto description = po::options_description{std::move(caption)};
   for (const auto& component : registry.components()) {
      for (const auto& field : component.fields) {
         if ((!include_ingestion_only && field.ingestion_only) || !supports_field(field.kind)) {
            continue;
         }
         add_field_option(description, option_name(component, field.name), field.kind, field.description);
         for (const auto& alias : field.aliases) {
            add_field_option(description, option_name(component, alias), field.kind,
                             "alias for " + option_name(component, field.name));
         }
      }
   }
   return description;
}

[[nodiscard]] config::core::value cli_value(schema::value_kind kind, const std::string& input) {
   switch (kind) {
   case schema::value_kind::dynamic:
      throw std::invalid_argument{"dynamic command-line values are not supported"};
   case schema::value_kind::boolean: {
      return schema::parse_scalar_text<bool>(input);
   }
   case schema::value_kind::signed_integer:
      return schema::parse_scalar_text<std::int64_t>(input);
   case schema::value_kind::unsigned_integer:
      return schema::parse_scalar_text<std::uint64_t>(input);
   case schema::value_kind::floating:
      return schema::parse_scalar_text<double>(input);
   case schema::value_kind::string:
      return input;
   case schema::value_kind::string_list:
      return config::core::value::array_type{config::core::value{input}};
   case schema::value_kind::object:
      throw std::invalid_argument{"structured object options are not supported"};
   case schema::value_kind::object_list:
      throw std::invalid_argument{"structured object-list options are not supported"};
   }
   return input;
}

[[nodiscard]] std::string option_token(std::string_view argument) {
   if (argument.size() == 2U && argument.front() == '-' && argument[1] != '-') {
      return std::string{argument.substr(1U)};
   }
   if (!argument.starts_with("--")) {
      return {};
   }
   argument.remove_prefix(2U);
   const auto equals = argument.find('=');
   if (equals != std::string_view::npos) {
      argument = argument.substr(0, equals);
   }
   return std::string{argument};
}

[[nodiscard]] bool reserved_name_matches(const reserved_option& option, std::string_view name) {
   if (option.name == name) {
      return true;
   }
   return std::ranges::any_of(option.aliases, [&](const std::string& alias) { return alias == name; });
}

[[nodiscard]] const reserved_option* find_reserved(const std::vector<reserved_option>& reserved,
                                                   std::string_view name) {
   const auto found = std::ranges::find_if(reserved, [&](const reserved_option& option) {
      return reserved_name_matches(option, name);
   });
   return found == reserved.end() ? nullptr : &*found;
}

[[nodiscard]] std::optional<std::string> inline_value(std::string_view argument) {
   const auto equals = argument.find('=');
   if (equals == std::string_view::npos) {
      return std::nullopt;
   }
   return std::string{argument.substr(equals + 1U)};
}

[[nodiscard]] std::string consume_required_value(int& index,
                                                 int argc,
                                                 const char* const* argv,
                                                 std::string_view argument,
                                                 const reserved_option& option) {
   if (auto value = inline_value(argument)) {
      return *value;
   }
   if (index + 1 >= argc || argv[index + 1] == nullptr) {
      throw std::invalid_argument{"missing value for --" + option.name};
   }
   ++index;
   return std::string{argv[index]};
}

void set_reserved_value(pre_scan_result& result, const reserved_option& option, std::string value) {
   if (option.kind == schema::value_kind::string_list) {
      auto array = config::core::value::array_type{};
      if (const auto* existing = result.document.try_get(option.path); existing != nullptr) {
         if (const auto* values = existing->as_array()) {
            array = *values;
         }
      }
      array.emplace_back(std::move(value));
      result.document.set(option.path, std::move(array));
   } else {
      result.document.set(option.path, cli_value(option.kind, value));
   }
   if (!std::ranges::contains(result.present_paths, option.path)) {
      result.present_paths.push_back(option.path);
   }
}

} // namespace

bool pre_scan_result::present(std::string_view path) const {
   return std::ranges::any_of(present_paths, [&](const std::string& value) { return value == path; });
}

parse_result parse(int argc, const char* const* argv, const config::core::component_registry& registry) {
   auto result = parse_result{};
   auto description = build_description(registry, "FORGE options", true);
   try {
      auto parsed = po::command_line_parser(argc, argv).options(description).run();
      auto variables = po::variables_map{};
      po::store(parsed, variables);
      po::notify(variables);

      for (const auto& component : registry.components()) {
         for (const auto& field : component.fields) {
            if (!supports_field(field.kind)) {
               continue;
            }
            auto names = std::vector<std::string>{field.name};
            names.insert(names.end(), field.aliases.begin(), field.aliases.end());

            for (const auto& name : names) {
               const auto full_name = option_name(component, name);
               if (!variables.count(full_name)) {
                  continue;
               }

               const auto target = option_name(component, field.name);
               try {
                  if (field.kind == schema::value_kind::string_list) {
                     const auto values = variables[full_name].as<std::vector<std::string>>();
                     auto array = config::core::value::array_type{};
                     array.reserve(values.size());
                     for (const auto& value : values) {
                        array.emplace_back(value);
                     }
                     result.document.set(target, std::move(array));
                  } else {
                     result.document.set(target, cli_value(field.kind, variables[full_name].as<std::string>()));
                  }
               } catch (const std::exception& error) {
                  const auto* typed = dynamic_cast<const forge::exceptions::base*>(&error);
                  result.diagnostics.push_back(schema::diagnostic{
                      .path = target,
                      .code = "program_options.convert",
                      .level = schema::severity::error,
                      .message = typed ? typed->message() : "command line option has an invalid value",
                  });
               }
               break;
            }
         }
      }
   } catch (const std::exception& error) {
      const auto* typed = dynamic_cast<const forge::exceptions::base*>(&error);
      result.diagnostics.push_back(schema::diagnostic{
          .path = {},
          .code = "program_options.parse",
          .level = schema::severity::error,
          .message = typed ? typed->message() : "command line options could not be parsed",
      });
   }
   return result;
}

pre_scan_result pre_scan_reserved(int argc, const char* const* argv, const std::vector<reserved_option>& reserved) {
   auto result = pre_scan_result{};
   if (argc > 0 && argv != nullptr && argv[0] != nullptr) {
      result.filtered_args.emplace_back(argv[0]);
   }

   for (auto index = 1; index < argc; ++index) {
      const auto argument = std::string_view{argv[index] == nullptr ? "" : argv[index]};
      const auto token = option_token(argument);
      const auto* option = token.empty() ? nullptr : find_reserved(reserved, token);
      if (option == nullptr) {
         result.filtered_args.emplace_back(argument);
         continue;
      }

      try {
         if (option->kind == schema::value_kind::boolean) {
            const auto value = inline_value(argument);
            set_reserved_value(result, *option, value.has_value() && !value->empty() ? *value : std::string{"true"});
         } else {
            set_reserved_value(result, *option, consume_required_value(index, argc, argv, argument, *option));
         }
      } catch (const std::exception& error) {
         const auto* typed = dynamic_cast<const forge::exceptions::base*>(&error);
         result.diagnostics.push_back(schema::diagnostic{
             .path = option->path,
             .code = "program_options.convert",
             .level = schema::severity::error,
             .message = typed ? typed->message() : "command line option has an invalid value",
         });
      }
   }

   return result;
}

std::string help(const config::core::component_registry& registry, std::string caption) {
   auto description = build_description(registry, std::move(caption), false);
   auto output = std::ostringstream{};
   output << description;
   return output.str();
}

} // namespace forge::config::program_options
