#include <exprtk.hpp>

#include <cstdint>
#include <iostream>

// Compile this target separately. Failure to instantiate the upstream parser
// is an observed capability result, not a successful money implementation.
int main() {
   using number_type = long long;
   static_assert(sizeof(number_type) == sizeof(std::int64_t));
   number_type x = 9'007'199'254'740'992LL;
   exprtk::symbol_table<number_type> symbols;
   symbols.add_variable("x", x);
   exprtk::expression<number_type> expression;
   expression.register_symbol_table(symbols);
   exprtk::parser<number_type> parser;
   parser.settings().disable_strength_reduction();
   if (!parser.compile("(x + 1) - x", expression)) {
      std::cerr << parser.error() << '\n';
      return 1;
   }
   std::cout << expression.value() << '\n';
}
