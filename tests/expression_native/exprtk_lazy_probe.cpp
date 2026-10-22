#include <exprtk.hpp>

#include <array>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>

namespace {

using parser_type = exprtk::parser<double>;
using settings_type = parser_type::settings_t;

settings_type restricted_settings() {
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
   return settings;
}

class touch_function final : public exprtk::ifunction<double> {
 public:
   touch_function() : exprtk::ifunction<double>(0) {}

   double operator()() override {
      ++calls;
      return 1.0;
   }

   std::size_t calls = 0;
};

void lazy_probe(const std::string& source) {
   // This diagnostic intentionally registers a side-effecting host function.
   // The measured restricted backend does not register any host functions.
   double flag = 0;
   touch_function touch;
   exprtk::symbol_table<double> symbols(exprtk::symbol_table<double>::e_immutable);
   symbols.add_variable("flag", flag);
   symbols.add_function("touch", touch);
   exprtk::expression<double> expression;
   expression.register_symbol_table(symbols);
   parser_type parser(restricted_settings());
   if (!parser.compile(source, expression)) {
      std::cout << "lazy\t" << source << "\tcompile_error\t" << parser.error() << '\n';
      return;
   }
   for (const auto input : {0.0, 1.0}) {
      flag = input;
      touch.calls = 0;
      const auto result = expression.value();
      std::cout << "lazy\t" << source << "\tflag=" << input << "\tcalls=" << touch.calls << "\tvalue=" << result
                << '\n';
   }
}

void restriction_probe(const std::string& source, bool evaluate) {
   double x = 2;
   double y = 3;
   exprtk::symbol_table<double> symbols(exprtk::symbol_table<double>::e_immutable);
   symbols.add_variable("x", x);
   symbols.add_variable("y", y);
   exprtk::expression<double> expression;
   expression.register_symbol_table(symbols);
   parser_type parser(restricted_settings());
   const auto accepted = parser.compile(source, expression);
   std::cout << "restriction\t" << source << "\taccepted=" << accepted;
   if (accepted && evaluate) {
      const auto result = expression.value();
      std::cout << "\tvalue=" << result << "\tx=" << x << "\ty=" << y;
   } else if (!accepted) {
      std::cout << "\terror=" << parser.error();
   }
   std::cout << '\n';
}

void strength_probe(const std::string& source, double x, double y, double z) {
   for (const auto enabled : {false, true}) {
      exprtk::symbol_table<double> symbols(exprtk::symbol_table<double>::e_immutable);
      symbols.add_variable("x", x);
      symbols.add_variable("y", y);
      symbols.add_variable("z", z);
      exprtk::expression<double> expression;
      expression.register_symbol_table(symbols);
      auto settings = restricted_settings();
      if (enabled) {
         settings.enable_strength_reduction();
      }
      parser_type parser(settings);
      if (parser.compile(source, expression)) {
         std::cout << "strength\t" << source << "\tenabled=" << enabled << "\tvalue=" << expression.value() << '\n';
      } else {
         std::cout << "strength\t" << source << "\tenabled=" << enabled << "\tcompile_error=" << parser.error() << '\n';
      }
   }
}

class checkpoint_limit final : public exprtk::compilation_check {
 public:
   bool continue_compilation(compilation_context& context) override {
      if (++checkpoints > 3) {
         context.error_message = "diagnostic_checkpoint_limit";
         return false;
      }
      return true;
   }

   std::size_t checkpoints = 0;
};

void limit_probe(const std::string& name, const std::string& source, std::size_t stack_depth, std::size_t node_depth,
                 bool checkpoint) {
   double x = 1;
   std::array<double, 20> conditions;
   conditions.fill(1);
   exprtk::symbol_table<double> symbols;
   symbols.add_variable("x", x);
   for (std::size_t index = 0; index < conditions.size(); ++index) {
      symbols.add_variable("condition" + std::to_string(index), conditions[index]);
   }
   exprtk::expression<double> expression;
   expression.register_symbol_table(symbols);
   auto settings = restricted_settings();
   settings.set_max_stack_depth(stack_depth);
   settings.set_max_node_depth(node_depth);
   checkpoint_limit limit;
   parser_type parser(settings);
   if (checkpoint) {
      parser.register_compilation_timeout_check(limit);
   }
   const auto accepted = parser.compile(source, expression);
   std::cout << "limit\t" << name << "\taccepted=" << accepted << "\tmax_stack_depth=" << stack_depth
             << "\tmax_node_depth=" << node_depth << "\tcheckpoints=" << limit.checkpoints;
   if (!accepted) {
      std::cout << "\terror=" << parser.error();
   }
   std::cout << '\n';
}

void bounds_probe() {
   double index = 8;
   std::vector<double> values{1, 2};
   exprtk::vector_access_runtime_check bounds;
   exprtk::symbol_table<double> symbols(exprtk::symbol_table<double>::e_immutable);
   symbols.add_variable("index", index);
   symbols.add_vector("values", values);
   exprtk::expression<double> expression;
   expression.register_symbol_table(symbols);
   parser_type parser(restricted_settings());
   parser.register_vector_access_runtime_check(bounds);
   if (!parser.compile("values[index]", expression)) {
      std::cout << "bounds\tcompile_error\t" << parser.error() << '\n';
      return;
   }
   try {
      const auto result = expression.value();
      std::cout << "bounds\tvalue=" << result << '\n';
   } catch (const std::exception& error) {
      std::cout << "bounds\truntime_exception=" << error.what() << '\n';
   }
}

} // namespace

int main() {
   std::cout << std::setprecision(std::numeric_limits<double>::max_digits10);
   for (const auto* source : {"flag ? touch() : 7", "if(flag,touch(),7)", "flag & touch()", "flag | touch()",
                              "flag and touch()", "flag or touch()"}) {
      lazy_probe(source);
   }
   for (const auto* source : {"x := 4", "x += 1", "swap(x,y)", "x <=> y", "var z := 1; z", "const var z := 1; z",
                              "sin(x)", "print(x)", "unknown(x)", "return [x]"}) {
      restriction_probe(source, true);
   }
   for (const auto* source :
        {"while (x > 0) { x += 1; }", "for (var i := 0; i < 2; i += 1) { x; }", "repeat { x; } until (0)"}) {
      restriction_probe(source, false);
   }
   strength_probe("(0.5 * x) + (y * 0.5)", 1.6e308, 1.6e308, 0);
   strength_probe("(x / y) / z", 1e308, 1e308, 1e308);
   limit_probe("parser_stack", std::string(160, '(') + "x" + std::string(160, ')'), 128, 256, false);
   std::string nested = "0";
   for (std::size_t index = 0; index < 20; ++index) {
      nested = "condition" + std::to_string(index) + " > 0 ? " + std::to_string(index) + " : (" + nested + ")";
   }
   limit_probe("ast_ternary_depth", nested, 128, 8, false);
   std::string unary = "x";
   for (std::size_t index = 0; index < 20; ++index) {
      unary = "abs(" + unary + ")";
   }
   limit_probe("ast_unary_depth", unary, 128, 8, false);
   limit_probe("compile_checkpoints", "(x + x) / (x - x)", 128, 256, true);
   bounds_probe();
}
