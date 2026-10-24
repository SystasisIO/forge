#include "benchmark.hpp"

#include "checker/checker_options.h"
#include "checker/standard_library.h"
#include "checker/type_checker_builder_factory.h"
#include "common/decl.h"
#include "common/minimal_descriptor_pool.h"
#include "common/type.h"
#include "common/value.h"
#include "parser/parser.h"
#include "runtime/activation.h"
#include "runtime/runtime.h"
#include "runtime/runtime_options.h"
#include "runtime/standard_runtime_builder_factory.h"
#include "google/protobuf/arena.h"

namespace expression_native {
namespace {

// These are actual CEL admission/evaluation options, independent of the common
// runner's byte guard and the external process timeout. No custom AST is built.
constexpr int source_codepoint_limit = 8192;
constexpr int parser_depth_limit = 64;
constexpr int expression_node_limit = 4096;
constexpr int comprehension_iteration_limit = 1024;

class cel_program final : public program {
 public:
   cel_program(std::unique_ptr<cel::Program> value, std::unique_ptr<cel::TypeChecker> checker)
       : _checker(std::move(checker)), _program(std::move(value)) {}

   absl::Status bind(const input& item) {
      if (item.type == "int64") {
         std::int64_t value = 0;
         if (!parse_integer(item.value, value)) {
            return absl::InvalidArgumentError("invalid int64 input");
         }
         _activation.InsertOrAssignValue(item.name, cel::IntValue(value));
      } else if (item.type == "uint64") {
         std::uint64_t value = 0;
         if (!parse_integer(item.value, value)) {
            return absl::InvalidArgumentError("invalid uint64 input");
         }
         _activation.InsertOrAssignValue(item.name, cel::UintValue(value));
      } else if (item.type == "bool") {
         if (item.value != "true" && item.value != "false") {
            return absl::InvalidArgumentError("invalid bool input");
         }
         _activation.InsertOrAssignValue(item.name, cel::BoolValue(item.value == "true"));
      } else if (item.type == "list_int64") {
         auto builder = cel::NewListValueBuilder(&_input_arena);
         builder->Reserve(item.items.size());
         for (const auto& text : item.items) {
            std::int64_t value = 0;
            if (!parse_integer(text, value)) {
               return absl::InvalidArgumentError("invalid list int64 input");
            }
            const auto added = builder->Add(cel::IntValue(value));
            if (!added.ok()) {
               return added;
            }
         }
         _activation.InsertOrAssignValue(item.name, std::move(*builder).Build());
      } else {
         return absl::InvalidArgumentError("unsupported input type");
      }
      return absl::OkStatus();
   }

   evaluation evaluate() override {
      // Fresh per-evaluation storage prevents allocations from accumulating
      // across benchmark iterations. Input lists use a separate retained arena.
      google::protobuf::Arena arena;
      auto result = _program->Evaluate(&arena, _activation);
      if (!result.ok()) {
         return {evaluation::status::evaluation_error, "engine_status:" + result.status().ToString()};
      }
      if (result->IsError()) {
         return {evaluation::status::evaluation_error, "cel_error:" + result->GetError().ToStatus().ToString()};
      }
      if (result->IsInt()) {
         return {evaluation::status::ok, "native", result->GetInt().NativeValue()};
      }
      if (result->IsUint()) {
         return {evaluation::status::ok, "native", result->GetUint().NativeValue()};
      }
      if (result->IsDouble()) {
         return {evaluation::status::ok, "native", result->GetDouble().NativeValue()};
      }
      if (result->IsBool()) {
         return {evaluation::status::ok, "native", result->GetBool().NativeValue()};
      }
      return {evaluation::status::unsupported, "non_scalar_result"};
   }

 private:
   std::unique_ptr<cel::TypeChecker> _checker;
   std::unique_ptr<cel::Program> _program;
   google::protobuf::Arena _input_arena;
   cel::Activation _activation;
};

class cel_backend final : public backend {
 public:
   compilation compile(const test_case& value) override {
      cel::ParserOptions parser_options;
      parser_options.enable_pratt_parser = true;
      parser_options.expression_size_codepoint_limit = source_codepoint_limit;
      parser_options.expression_node_limit = expression_node_limit;
      parser_options.max_recursion_depth = parser_depth_limit;
      auto parser_builder = cel::NewParserBuilder(parser_options);
      auto parser = parser_builder->Build();
      if (!parser.ok()) {
         return {nullptr, "parser_build:" + parser.status().ToString()};
      }
      auto source = (*parser)->PrepareSource(value.cel);
      if (!source.ok()) {
         return {nullptr, "source:" + source.status().ToString()};
      }
      auto parsed = (*parser)->Parse(**source);
      if (!parsed.ok()) {
         return {nullptr, "parse:" + parsed.status().ToString()};
      }

      cel::CheckerOptions checker_options;
      checker_options.max_expression_node_count = expression_node_limit;
      auto checker_builder = cel::CreateTypeCheckerBuilder(cel::GetMinimalDescriptorPool(), checker_options);
      if (!checker_builder.ok()) {
         return {nullptr, "checker_build:" + checker_builder.status().ToString()};
      }
      auto added = (*checker_builder)->AddLibrary(cel::StandardCheckerLibrary());
      if (!added.ok()) {
         return {nullptr, "checker_library:" + added.ToString()};
      }
      for (const auto& item : value.inputs) {
         cel::Type type;
         if (item.type == "int64") {
            type = cel::IntType();
         } else if (item.type == "uint64") {
            type = cel::UintType();
         } else if (item.type == "bool") {
            type = cel::BoolType();
         } else if (item.type == "list_int64") {
            type = cel::ListType((*checker_builder)->arena(), cel::IntType());
         } else {
            return {nullptr, "unsupported input type:" + item.type};
         }
         // Fixture inputs shadow builtin type identifiers such as `bytes` using
         // CEL's native declaration API; the source expression is unchanged.
         added = (*checker_builder)->AddOrReplaceVariable(cel::MakeVariableDecl(item.name, type));
         if (!added.ok()) {
            return {nullptr, "variable:" + added.ToString()};
         }
      }
      auto checker = (*checker_builder)->Build();
      if (!checker.ok()) {
         return {nullptr, "checker_build:" + checker.status().ToString()};
      }
      auto checked = (*checker)->Check(std::move(*parsed));
      if (!checked.ok()) {
         return {nullptr, "check:" + checked.status().ToString()};
      }
      if (!checked->IsValid()) {
         return {nullptr, "check:" + checked->FormatError()};
      }
      auto ast = checked->ReleaseAst();
      if (!ast.ok()) {
         return {nullptr, "checked_ast:" + ast.status().ToString()};
      }

      cel::RuntimeOptions runtime_options;
      runtime_options.short_circuiting = true;
      runtime_options.comprehension_max_iterations = comprehension_iteration_limit;
      runtime_options.regex_max_program_size = 256;
      auto runtime_builder = cel::CreateStandardRuntimeBuilder(cel::GetMinimalDescriptorPool(), runtime_options);
      if (!runtime_builder.ok()) {
         return {nullptr, "runtime_build:" + runtime_builder.status().ToString()};
      }
      auto runtime = std::move(*runtime_builder).Build();
      if (!runtime.ok()) {
         return {nullptr, "runtime_build:" + runtime.status().ToString()};
      }
      auto executable = (*runtime)->CreateProgram(std::move(*ast));
      if (!executable.ok()) {
         return {nullptr, "program:" + executable.status().ToString()};
      }
      auto result = std::make_unique<cel_program>(std::move(*executable), std::move(*checker));
      for (const auto& item : value.inputs) {
         const auto bound = result->bind(item);
         if (!bound.ok()) {
            return {nullptr, "activation:" + bound.ToString()};
         }
      }
      return {std::move(result), {}};
   }
};

} // namespace
} // namespace expression_native

int main(int argc, char** argv) {
   expression_native::cel_backend engine;
   return expression_native::run(argc, argv, engine, "cel", "9c562a306569e90c8dea6d3af71f4433aa386162",
                                 "native-int64-uint64-double");
}
