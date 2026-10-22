#include "../libraries/expression/details/arithmetic.hxx"

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "checker/standard_library.h"
#include "checker/type_checker_builder_factory.h"
#include "common/decl.h"
#include "common/minimal_descriptor_pool.h"
#include "common/type.h"
#include "common/value.h"
#include "parser/parser.h"
#include "runtime/activation.h"
#include "runtime/runtime.h"
#include "runtime/standard_runtime_builder_factory.h"
#include "google/protobuf/arena.h"

namespace {

std::string case_context;

void require(bool condition, std::string_view message) {
   if (!condition) {
      std::cerr << case_context << ": " << message << '\n';
      std::exit(EXIT_FAILURE);
   }
}

void require_ok(const absl::Status& status) {
   require(status.ok(), status.ToString());
}

template <typename Integer> Integer parse(std::string_view text) {
   Integer value{};
   const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
   require(result.ec == std::errc{} && result.ptr == text.data() + text.size(), "invalid integer fixture");
   return value;
}

struct oracle_case {
   bool unsigned_values;
   const char* left;
   const char* right;
   const char* divisor;
   std::int64_t mode;
   int status;
   const char* result;
};

const oracle_case oracle_cases[] = {
#include "expression_arithmetic_cases.inc"
};

class arithmetic_program {
 public:
   explicit arithmetic_program(bool unsigned_values) {
      cel::ParserOptions options;
      options.enable_pratt_parser = true;
      auto parser_builder = cel::NewParserBuilder(options);
      auto parser = parser_builder->Build();
      require_ok(parser.status());
      auto source = (*parser)->PrepareSource("math.mul_div(left, right, divisor, mode)");
      require_ok(source.status());
      auto parsed = (*parser)->Parse(**source);
      require_ok(parsed.status());

      auto checker_builder = cel::CreateTypeCheckerBuilder(cel::GetMinimalDescriptorPool());
      require_ok(checker_builder.status());
      require_ok((*checker_builder)->AddLibrary(cel::StandardCheckerLibrary()));
      require_ok(forge::expression::detail::add_arithmetic_declarations(**checker_builder));
      const cel::Type type = unsigned_values ? cel::Type(cel::UintType()) : cel::Type(cel::IntType());
      for (const auto* name : {"left", "right", "divisor"}) {
         require_ok((*checker_builder)->AddVariable(cel::MakeVariableDecl(name, type)));
      }
      require_ok((*checker_builder)->AddVariable(cel::MakeVariableDecl("mode", cel::IntType())));
      auto checker = (*checker_builder)->Build();
      require_ok(checker.status());
      _checker = std::move(*checker);
      auto checked = _checker->Check(std::move(*parsed));
      require_ok(checked.status());
      require(checked->IsValid(), checked->FormatError());
      auto ast = checked->ReleaseAst();
      require_ok(ast.status());

      auto runtime_builder = cel::CreateStandardRuntimeBuilder(cel::GetMinimalDescriptorPool(), {});
      require_ok(runtime_builder.status());
      require_ok(forge::expression::detail::register_arithmetic_functions(runtime_builder->function_registry()));
      auto runtime = std::move(*runtime_builder).Build();
      require_ok(runtime.status());
      _runtime = std::move(*runtime);
      auto program = _runtime->CreateProgram(std::move(*ast));
      require_ok(program.status());
      _program = std::move(*program);
   }

   void check(const oracle_case& item) const {
      cel::Activation activation;
      if (item.unsigned_values) {
         activation.InsertOrAssignValue("left", cel::UintValue(parse<std::uint64_t>(item.left)));
         activation.InsertOrAssignValue("right", cel::UintValue(parse<std::uint64_t>(item.right)));
         activation.InsertOrAssignValue("divisor", cel::UintValue(parse<std::uint64_t>(item.divisor)));
      } else {
         activation.InsertOrAssignValue("left", cel::IntValue(parse<std::int64_t>(item.left)));
         activation.InsertOrAssignValue("right", cel::IntValue(parse<std::int64_t>(item.right)));
         activation.InsertOrAssignValue("divisor", cel::IntValue(parse<std::int64_t>(item.divisor)));
      }
      activation.InsertOrAssignValue("mode", cel::IntValue(item.mode));
      google::protobuf::Arena arena;
      auto value = _program->Evaluate(&arena, activation);
      require_ok(value.status());
      if (item.status != 0) {
         require(value->IsError(), "arithmetic error did not produce a CEL ErrorValue");
         const auto expected = item.status == 1 ? absl::StatusCode::kInvalidArgument : absl::StatusCode::kOutOfRange;
         require(value->GetError().ToStatus().code() == expected, "wrong arithmetic error category");
      } else if (item.unsigned_values) {
         require(value->IsUint(), "unsigned arithmetic returned another type");
         require(value->GetUint().NativeValue() == parse<std::uint64_t>(item.result), "uint64 oracle mismatch");
      } else {
         require(value->IsInt(), "signed arithmetic returned another type");
         require(value->GetInt().NativeValue() == parse<std::int64_t>(item.result), "int64 oracle mismatch");
      }
   }

 private:
   std::unique_ptr<cel::TypeChecker> _checker;
   std::unique_ptr<cel::Runtime> _runtime;
   std::unique_ptr<cel::Program> _program;
};

void reject_mixed_types() {
   cel::ParserOptions options;
   options.enable_pratt_parser = true;
   auto parser_builder = cel::NewParserBuilder(options);
   auto parser = parser_builder->Build();
   require_ok(parser.status());
   auto checker_builder = cel::CreateTypeCheckerBuilder(cel::GetMinimalDescriptorPool());
   require_ok(checker_builder.status());
   require_ok((*checker_builder)->AddLibrary(cel::StandardCheckerLibrary()));
   require_ok(forge::expression::detail::add_arithmetic_declarations(**checker_builder));
   auto checker = (*checker_builder)->Build();
   require_ok(checker.status());
   for (const auto* expression :
        {"math.mul_div(1, 1u, 1, 0)", "math.mul_div(1u, 1u, 1u, 0u)", "math.mul_div(1.0, 1.0, 1.0, 0)"}) {
      auto source = (*parser)->PrepareSource(expression);
      require_ok(source.status());
      auto parsed = (*parser)->Parse(**source);
      require_ok(parsed.status());
      auto checked = (*checker)->Check(std::move(*parsed));
      require_ok(checked.status());
      require(!checked->IsValid(), "checker accepted unsupported math.mul_div types");
   }
}

} // namespace

int main() {
   case_context = "preparing CEL arithmetic programs";
   const arithmetic_program signed_program(false);
   const arithmetic_program unsigned_program(true);
   std::size_t count = 0;
   for (const auto& item : oracle_cases) {
      case_context = "Case " + std::to_string(count) + ": " + item.left + " * " + item.right + " / " + item.divisor +
                     " mode " + std::to_string(item.mode);
      (item.unsigned_values ? unsigned_program : signed_program).check(item);
      ++count;
   }
   case_context = "rejecting unsupported arithmetic argument types";
   reject_mixed_types();
   std::cout << count << " exact arithmetic oracle cases passed through CEL parser/checker/runtime\n";
   return 0;
}
