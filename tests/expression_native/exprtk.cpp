#include "benchmark.hpp"

#include <exprtk.hpp>

#include <map>
#include <exception>

namespace expression_native {
namespace {

class compilation_budget final : public exprtk::compilation_check {
 public:
   bool continue_compilation(compilation_context& context) override {
      if (++_checkpoints > 10'000) {
         context.error_message = "compilation_checkpoint_budget_exceeded";
         return false;
      }
      return true;
   }

 private:
   std::size_t _checkpoints = 0;
};

class exprtk_program final : public program {
 public:
   exprtk_program() : _symbols(exprtk::symbol_table<double>::e_immutable) {}

   std::string prepare(const test_case& value) {
      for (const auto& item : value.inputs) {
         if (item.type == "int64") {
            std::int64_t number = 0;
            if (!parse_integer(item.value, number)) {
               return "invalid_int64_input:" + item.name;
            }
            _scalars.emplace(item.name, static_cast<double>(number));
         } else if (item.type == "uint64") {
            std::uint64_t number = 0;
            if (!parse_integer(item.value, number)) {
               return "invalid_uint64_input:" + item.name;
            }
            _scalars.emplace(item.name, static_cast<double>(number));
         } else if (item.type == "bool") {
            if (item.value != "true" && item.value != "false") {
               return "invalid_bool_input:" + item.name;
            }
            _scalars.emplace(item.name, item.value == "true" ? 1.0 : 0.0);
         } else if (item.type == "list_int64") {
            auto& numbers = _vectors[item.name];
            numbers.reserve(item.items.size());
            for (const auto& text : item.items) {
               std::int64_t number = 0;
               if (!parse_integer(text, number)) {
                  return "invalid_list_int64_input:" + item.name;
               }
               numbers.push_back(static_cast<double>(number));
            }
         } else {
            return "unsupported_input_type:" + item.type;
         }
      }

      for (auto& [name, number] : _scalars) {
         if (!_symbols.add_variable(name, number)) {
            return "scalar_registration_failed:" + name;
         }
      }
      for (auto& [name, numbers] : _vectors) {
         if (!_symbols.add_vector(name, numbers)) {
            return "vector_registration_failed:" + name;
         }
      }
      _expression.register_symbol_table(_symbols);

      using parser_type = exprtk::parser<double>;
      using settings_type = parser_type::settings_t;
      settings_type settings;
      settings.disable_strength_reduction()
          .disable_all_assignment_ops()
          .disable_local_vardef()
          .disable_all_control_structures()
          .enable_control_structure(settings_type::e_ctrl_ifelse)
          .disable_all_base_functions()
          .enable_base_function(settings_type::e_bf_abs)
          .enable_base_function(settings_type::e_bf_min)
          .enable_base_function(settings_type::e_bf_max)
          .enable_base_function(settings_type::e_bf_sum)
          .enable_base_function(settings_type::e_bf_ceil)
          .enable_base_function(settings_type::e_bf_floor)
          .enable_base_function(settings_type::e_bf_round)
          .enable_base_function(settings_type::e_bf_trunc)
          .enable_base_function(settings_type::e_bf_clamp);
      settings.set_max_stack_depth(128);
      settings.set_max_node_depth(256);
      settings.set_max_local_vector_size(4096);
      settings.set_max_total_local_symbol_size_bytes(64 * 1024);

      compilation_budget budget;
      parser_type parser(settings);
      parser.register_compilation_timeout_check(budget);
      parser.register_vector_access_runtime_check(_bounds);
      if (!parser.compile(value.exprtk, _expression)) {
         return parser.error();
      }
      return {};
   }

   evaluation evaluate() override {
      try {
         // Keep native floating point results, including NaN and infinity.
         // The shared checker compares them with the exact corpus expectation.
         return {evaluation::status::ok, {}, _expression.value()};
      } catch (const std::exception&) {
         return {evaluation::status::evaluation_error, "native_exception", std::int64_t{0}};
      }
   }

 private:
   std::map<std::string, double> _scalars;
   std::map<std::string, std::vector<double>> _vectors;
   exprtk::vector_access_runtime_check _bounds;
   exprtk::symbol_table<double> _symbols;
   exprtk::expression<double> _expression;
};

class exprtk_backend final : public backend {
 public:
   compilation compile(const test_case& value) override {
      auto executable = std::make_unique<exprtk_program>();
      try {
         auto error = executable->prepare(value);
         if (!error.empty()) {
            return {nullptr, std::move(error)};
         }
         return {std::move(executable), {}};
      } catch (const std::exception& error) {
         return {nullptr, std::string("native_compile_exception:") + error.what()};
      }
   }
};

} // namespace
} // namespace expression_native

int main(int argc, char** argv) {
   expression_native::exprtk_backend engine;
   return expression_native::run(argc, argv, engine, "exprtk", "1e4a80b5ec9b4832ed59c6faa65f625a01b18ef0", "double");
}
