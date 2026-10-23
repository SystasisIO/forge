#include "details/backend.hxx"
#include "details/arithmetic.hxx"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/log/internal/globals.h"
#include "absl/synchronization/mutex.h"
#include "checker/checker_options.h"
#include "checker/standard_library.h"
#include "checker/type_checker_builder_factory.h"
#include "checker/type_checker_subset_factory.h"
#include "common/ast.h"
#include "common/ast_proto.h"
#include "common/ast_traverse.h"
#include "common/ast_visitor_base.h"
#include "common/decl.h"
#include "common/minimal_descriptor_pool.h"
#include "common/standard_definitions.h"
#include "common/type.h"
#include "common/value.h"
#include "internal/utf8.h"
#include "parser/parser.h"
#include "runtime/activation.h"
#include "runtime/reference_resolver.h"
#include "runtime/runtime.h"
#include "runtime/runtime_options.h"
#include "runtime/standard_runtime_builder_factory.h"
#include "google/protobuf/arena.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"

namespace forge::expression::detail::backend {
namespace {

using overload = cel::StandardOverloadIds;

constexpr std::array allowed_overloads = {overload::kAddInt,
                                          overload::kAddUint,
                                          overload::kSubtractInt,
                                          overload::kSubtractUint,
                                          overload::kMultiplyInt,
                                          overload::kMultiplyUint,
                                          overload::kDivideInt,
                                          overload::kDivideUint,
                                          overload::kModuloInt,
                                          overload::kModuloUint,
                                          overload::kNegateInt,
                                          overload::kNot,
                                          overload::kAnd,
                                          overload::kOr,
                                          overload::kConditional,
                                          overload::kEquals,
                                          overload::kNotEquals,
                                          overload::kLessInt,
                                          overload::kLessUint,
                                          overload::kLessString,
                                          overload::kLessEqualsInt,
                                          overload::kLessEqualsUint,
                                          overload::kLessEqualsString,
                                          overload::kGreaterInt,
                                          overload::kGreaterUint,
                                          overload::kGreaterString,
                                          overload::kGreaterEqualsInt,
                                          overload::kGreaterEqualsUint,
                                          overload::kGreaterEqualsString,
                                          overload::kIndexList,
                                          overload::kIndexMap,
                                          overload::kSizeList,
                                          overload::kSizeMap,
                                          overload::kSizeString,
                                          overload::kSizeListMember,
                                          overload::kSizeMapMember,
                                          overload::kSizeStringMember};

constexpr std::array<std::string_view, 4> rounding_names = {"rounding.trunc", "rounding.floor", "rounding.ceil",
                                                            "rounding.half_even"};

absl::string_view view(std::string_view value) {
   return {value.data(), value.size()};
}

bool permitted_overload(absl::string_view id) {
   return std::find(allowed_overloads.begin(), allowed_overloads.end(), id) != allowed_overloads.end() ||
          id == R"(math\.mul_div(int,int,int,int))" || id == R"(math\.mul_div(uint,uint,uint,int))";
}

bool valid_limits(const limits& budget) {
   constexpr limits ceiling;
   const auto within = [](std::size_t value, std::size_t maximum) { return value > 0 && value <= maximum; };
   return within(budget.source_bytes, ceiling.source_bytes) && within(budget.nodes, ceiling.nodes) &&
          within(budget.depth, ceiling.depth) && within(budget.variables, ceiling.variables) &&
          within(budget.collection_elements, ceiling.collection_elements) &&
          within(budget.string_bytes, ceiling.string_bytes) &&
          within(budget.total_input_bytes, ceiling.total_input_bytes) && within(budget.ast_events, ceiling.ast_events);
}

bool valid_name(std::string_view name, const limits& budget) {
   if (name.empty() || name.size() > budget.string_bytes || name == "math" || name == "rounding") {
      return false;
   }
   const auto initial = [](char value) {
      return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
   };
   if (!initial(name.front())) {
      return false;
   }
   return std::all_of(name.begin(), name.end(),
                      [&](char value) { return initial(value) || (value >= '0' && value <= '9'); });
}

bool numeric(const cel::TypeSpec& type) {
   return type.has_primitive() &&
          (type.primitive() == cel::PrimitiveType::kInt64 || type.primitive() == cel::PrimitiveType::kUint64);
}

bool scalar(const cel::TypeSpec& type) {
   return numeric(type) || (type.has_primitive() && (type.primitive() == cel::PrimitiveType::kBool ||
                                                     type.primitive() == cel::PrimitiveType::kString));
}

bool permitted_type(const cel::TypeSpec& type) {
   if (scalar(type)) {
      return true;
   }
   if (type.has_list_type()) {
      return type.list_type().has_elem_type() && numeric(type.list_type().elem_type());
   }
   if (type.has_map_type()) {
      const auto& map = type.map_type();
      return map.has_key_type() && map.has_value_type() && numeric(map.value_type()) &&
             (numeric(map.key_type()) ||
              (map.key_type().has_primitive() && map.key_type().primitive() == cel::PrimitiveType::kString));
   }
   return false;
}

struct owned_declaration {
   std::string name;
   schema_kind kind;
};

// Traverses the upstream CEL tree twice: structural admission before the
// checker, then types and resolved overloads after the checker. It creates no
// alternate AST, IR, parser or execution plan.
class admission final : public cel::AstVisitorBase {
 public:
   admission(const cel::Ast& ast, const limits& budget, std::span<const owned_declaration> declarations, bool checked)
       : _ast(ast), _budget(budget), _declarations(declarations), _checked(checked) {}

   void PreVisitExpr(const cel::Expr& expr) override {
      ++_nodes;
      ++_depth;
      if (_nodes > _budget.nodes || _depth > _budget.depth) {
         fail(status::resource_exhausted);
      }
      if (expr.id() <= 0 || expr.id() > std::numeric_limits<std::int32_t>::max()) {
         fail(status::invalid_program);
      }
      if (expr.has_comprehension_expr() || expr.has_struct_expr()) {
         fail(status::invalid_program);
      }
      if (!expr.has_const_expr() && !expr.has_ident_expr() && !expr.has_select_expr() && !expr.has_call_expr() &&
          !expr.has_list_expr() && !expr.has_map_expr()) {
         fail(status::invalid_program);
      }
      if (_checked) {
         const auto* type = _ast.GetType(expr.id());
         if (type == nullptr || !permitted_type(*type)) {
            fail(status::invalid_program);
         }
      }
   }

   void PostVisitExpr(const cel::Expr&) override {
      --_depth;
   }

   void PostVisitConst(const cel::Expr&, const cel::Constant& constant) override {
      if (constant.has_string_value()) {
         if (constant.string_value().size() > _budget.string_bytes) {
            fail(status::resource_exhausted);
         }
      } else if (!constant.has_int_value() && !constant.has_uint_value() && !constant.has_bool_value()) {
         fail(status::invalid_program);
      }
   }

   void PostVisitIdent(const cel::Expr& expr, const cel::IdentExpr&) override {
      if (_checked && !known_reference(expr)) {
         fail(status::invalid_program);
      }
   }

   void PostVisitSelect(const cel::Expr& expr, const cel::SelectExpr& select) override {
      if (!_checked || known_reference(expr)) {
         return;
      }
      const auto* type = _ast.GetType(select.operand().id());
      if (type != nullptr && type->has_map_type() && select.field().size() > _budget.string_bytes) {
         fail(status::resource_exhausted);
         return;
      }
      if (select.test_only() || type == nullptr || !type->has_map_type() ||
          !type->map_type().key_type().has_primitive() ||
          type->map_type().key_type().primitive() != cel::PrimitiveType::kString) {
         fail(status::invalid_program);
      }
   }

   void PostVisitCall(const cel::Expr& expr, const cel::CallExpr& call) override {
      if (!_checked) {
         return;
      }
      const auto* reference = _ast.GetReference(expr.id());
      if (reference == nullptr || reference->overload_id().empty()) {
         fail(status::invalid_program);
         return;
      }
      for (const auto& id : reference->overload_id()) {
         if (!permitted_overload(id)) {
            fail(status::invalid_program);
         }
      }
      // CEL equality is polymorphic; an overload whitelist alone would admit
      // recursive collection comparisons, so check every operand's type too.
      if (call.function() == cel::StandardFunctions::kEqual || call.function() == cel::StandardFunctions::kInequal) {
         for (const auto& argument : call.args()) {
            const auto* type = _ast.GetType(argument.id());
            if (type == nullptr || !scalar(*type)) {
               fail(status::invalid_program);
            }
         }
      }
   }

   void PostVisitList(const cel::Expr&, const cel::ListExpr& list) override {
      if (list.elements().size() > _budget.collection_elements) {
         fail(status::resource_exhausted);
      }
      for (const auto& element : list.elements()) {
         if (element.optional()) {
            fail(status::invalid_program);
         }
      }
   }

   void PostVisitMap(const cel::Expr&, const cel::MapExpr& map) override {
      if (map.entries().size() > _budget.collection_elements) {
         fail(status::resource_exhausted);
      }
      for (const auto& entry : map.entries()) {
         if (entry.optional()) {
            fail(status::invalid_program);
         }
      }
   }

   status code() const {
      return _code;
   }

 private:
   void fail(status code) {
      if (_code == status::ok) {
         _code = code;
      }
   }

   bool known_reference(const cel::Expr& expr) const {
      const auto* reference = _ast.GetReference(expr.id());
      if (reference == nullptr || !reference->overload_id().empty()) {
         return false;
      }
      const std::string_view name(reference->name().data(), reference->name().size());
      if (std::find(rounding_names.begin(), rounding_names.end(), name) != rounding_names.end()) {
         return true;
      }
      const auto found = std::lower_bound(_declarations.begin(), _declarations.end(), name,
                                          [](const auto& item, std::string_view target) { return item.name < target; });
      return found != _declarations.end() && found->name == name;
   }

   const cel::Ast& _ast;
   const limits& _budget;
   std::span<const owned_declaration> _declarations;
   bool _checked;
   std::size_t _nodes = 0;
   std::size_t _depth = 0;
   status _code = status::ok;
};

status admit(const cel::Ast& ast, const limits& budget, std::span<const owned_declaration> declarations, bool checked) {
   admission visitor(ast, budget, declarations, checked);
   auto traversal = cel::AstTraversal::Create(ast.root_expr());
   while (!traversal.IsDone() && visitor.code() == status::ok) {
      traversal.Step(visitor);
   }
   return visitor.code();
}

bool valid_schema(schema_kind kind) {
   switch (kind) {
   case schema_kind::int64:
   case schema_kind::uint64:
   case schema_kind::boolean:
   case schema_kind::string:
   case schema_kind::list_int64:
   case schema_kind::list_uint64:
   case schema_kind::map_int64:
   case schema_kind::map_uint64:
      return true;
   }
   return false;
}

cel::Type make_type(schema_kind kind, google::protobuf::Arena* arena) {
   switch (kind) {
   case schema_kind::int64:
      return cel::IntType();
   case schema_kind::uint64:
      return cel::UintType();
   case schema_kind::boolean:
      return cel::BoolType();
   case schema_kind::string:
      return cel::StringType();
   case schema_kind::list_int64:
      return cel::ListType(arena, cel::IntType());
   case schema_kind::list_uint64:
      return cel::ListType(arena, cel::UintType());
   case schema_kind::map_int64:
      return cel::MapType(arena, cel::StringType(), cel::IntType());
   case schema_kind::map_uint64:
      return cel::MapType(arena, cel::StringType(), cel::UintType());
   }
   return cel::DynType();
}

status add_bytes(std::size_t amount, std::size_t& total, const limits& budget) {
   if (amount > budget.total_input_bytes - total) {
      return status::resource_exhausted;
   }
   total += amount;
   return status::ok;
}

status check_scalar(const value_view& value, value_kind expected, std::size_t& total, const limits& budget) {
   if (value.kind != expected) {
      return status::invalid_input;
   }
   if (expected == value_kind::string) {
      if (value.text.size() > budget.string_bytes) {
         return status::resource_exhausted;
      }
      if (!cel::internal::Utf8IsValid(view(value.text))) {
         return status::invalid_input;
      }
      return add_bytes(value.text.size(), total, budget);
   }
   return add_bytes(expected == value_kind::boolean ? 1 : 8, total, budget);
}

status check_value(const value_view& value, schema_kind schema, std::size_t& total, const limits& budget) {
   switch (schema) {
   case schema_kind::int64:
      return check_scalar(value, value_kind::int64, total, budget);
   case schema_kind::uint64:
      return check_scalar(value, value_kind::uint64, total, budget);
   case schema_kind::boolean:
      return check_scalar(value, value_kind::boolean, total, budget);
   case schema_kind::string:
      return check_scalar(value, value_kind::string, total, budget);
   default:
      break;
   }
   const bool map = schema == schema_kind::map_int64 || schema == schema_kind::map_uint64;
   if (value.kind != (map ? value_kind::map : value_kind::list) ||
       (value.count != 0 && (value.context == nullptr || value.element == nullptr || (map && value.key == nullptr)))) {
      return status::invalid_input;
   }
   if (value.count > budget.collection_elements) {
      return status::resource_exhausted;
   }
   const auto expected =
       schema == schema_kind::list_int64 || schema == schema_kind::map_int64 ? value_kind::int64 : value_kind::uint64;
   // Fixed-capacity borrowed keys avoid a second owning input representation.
   std::array<std::string_view, limits{}.collection_elements> keys;
   for (std::size_t index = 0; index < value.count; ++index) {
      if (map) {
         const auto key = value.key(value.context, index);
         keys[index] = key;
         if (key.size() > budget.string_bytes) {
            return status::resource_exhausted;
         }
         if (!cel::internal::Utf8IsValid(view(key))) {
            return status::invalid_input;
         }
         auto result = add_bytes(key.size(), total, budget);
         if (result != status::ok) {
            return result;
         }
      }
      auto result = check_scalar(value.element(value.context, index), expected, total, budget);
      if (result != status::ok) {
         return result;
      }
   }
   if (map) {
      const auto end = keys.begin() + value.count;
      std::sort(keys.begin(), end);
      if (std::adjacent_find(keys.begin(), end) != end) {
         return status::invalid_input;
      }
   }
   return status::ok;
}

cel::Value make_scalar(const value_view& value, google::protobuf::Arena* arena) {
   switch (value.kind) {
   case value_kind::int64:
      return cel::IntValue(value.i64);
   case value_kind::uint64:
      return cel::UintValue(value.u64);
   case value_kind::boolean:
      return cel::BoolValue(value.boolean);
   // Wrapping is safe: caller storage outlives this call's arena and activation,
   // and only primitive results can leave evaluate().
   case value_kind::string:
      return cel::StringValue::Wrap(view(value.text), arena);
   default:
      return cel::NullValue();
   }
}

absl::StatusOr<cel::Value> make_value(const value_view& value, google::protobuf::Arena* arena) {
   if (value.kind == value_kind::list) {
      auto builder = cel::NewListValueBuilder(arena);
      builder->Reserve(value.count);
      for (std::size_t index = 0; index < value.count; ++index) {
         auto result = builder->Add(make_scalar(value.element(value.context, index), arena));
         if (!result.ok()) {
            return result;
         }
      }
      return std::move(*builder).Build();
   }
   if (value.kind == value_kind::map) {
      auto builder = cel::NewMapValueBuilder(arena);
      builder->Reserve(value.count);
      for (std::size_t index = 0; index < value.count; ++index) {
         auto key = cel::StringValue::Wrap(view(value.key(value.context, index)), arena);
         auto result = builder->Put(std::move(key), make_scalar(value.element(value.context, index), arena));
         if (!result.ok()) {
            return result;
         }
      }
      return std::move(*builder).Build();
   }
   return make_scalar(value, arena);
}

class cel_program final : public program {
 public:
   cel_program(std::unique_ptr<cel::TraceableProgram> executable, std::vector<owned_declaration> declarations,
               limits budget)
       : _executable(std::move(executable)), _declarations(std::move(declarations)), _budget(budget) {}

   evaluation evaluate(std::span<const named_value> values) const override {
      try {
         return evaluate_impl(values);
      } catch (const std::bad_alloc&) {
         return {.code = status::resource_exhausted, .message = "native allocation failed"};
      } catch (...) {
         return {.code = status::internal, .message = "native evaluation failed"};
      }
   }

 private:
   evaluation evaluate_impl(std::span<const named_value> values) const {
      if (values.size() != _declarations.size()) {
         return {.code = status::invalid_input, .message = "input declarations do not match"};
      }
      std::size_t bytes = 0;
      std::array<bool, limits{}.variables> seen{};
      // Validate the entire activation before handing any value to CEL. Unknown,
      // duplicate and missing fields are errors, including unused declarations.
      for (const auto& input : values) {
         const auto found =
             std::lower_bound(_declarations.begin(), _declarations.end(), input.name,
                              [](const auto& item, std::string_view target) { return item.name < target; });
         if (found == _declarations.end() || found->name != input.name) {
            return {.code = status::invalid_input, .message = "unknown input declaration"};
         }
         const auto index = static_cast<std::size_t>(found - _declarations.begin());
         if (seen[index]) {
            return {.code = status::invalid_input, .message = "duplicate input declaration"};
         }
         seen[index] = true;
         auto code = add_bytes(input.name.size(), bytes, _budget);
         if (code == status::ok) {
            code = check_value(input.value, found->kind, bytes, _budget);
         }
         if (code != status::ok) {
            return {.code = code,
                    .message = code == status::invalid_input ? "input type does not match" : "input limit exceeded"};
         }
      }

      google::protobuf::Arena arena;
      cel::Activation activation;
      for (const auto& input : values) {
         auto value = make_value(input.value, &arena);
         if (!value.ok()) {
            return {.code = status::invalid_input, .message = "input binding failed"};
         }
         activation.InsertOrAssignValue(view(input.name), std::move(*value));
      }
      std::size_t events = 0;
      auto value = _executable->Trace(&arena, activation,
                                      [&](std::int64_t, const cel::Value&, const google::protobuf::DescriptorPool*,
                                          google::protobuf::MessageFactory*, google::protobuf::Arena*) {
                                         // The upstream hook runs AFTER a step. The first event over the
                                         // cap has already executed; report its actual count honestly.
                                         if (++events > _budget.ast_events) {
                                            return absl::ResourceExhaustedError("AST event budget exceeded");
                                         }
                                         return absl::OkStatus();
                                      });
      if (!value.ok()) {
         const bool exhausted = value.status().code() == absl::StatusCode::kResourceExhausted;
         return {.code = exhausted ? status::resource_exhausted : status::evaluation_failed,
                 .message = exhausted ? "AST event limit exceeded" : "CEL evaluation failed",
                 .ast_events = events};
      }
      if (value->IsError()) {
         return {
             .code = status::evaluation_failed, .message = "CEL expression returned an error", .ast_events = events};
      }
      if (value->IsInt()) {
         return {.kind = value_kind::int64, .i64 = value->GetInt().NativeValue(), .ast_events = events};
      }
      if (value->IsUint()) {
         return {.kind = value_kind::uint64, .u64 = value->GetUint().NativeValue(), .ast_events = events};
      }
      if (value->IsBool()) {
         return {.kind = value_kind::boolean, .boolean = value->GetBool().NativeValue(), .ast_events = events};
      }
      return {.code = status::internal, .message = "invalid CEL result type", .ast_events = events};
   }

   std::unique_ptr<cel::TraceableProgram> _executable;
   std::vector<owned_declaration> _declarations;
   limits _budget;
};

constexpr std::size_t expression_byte_limit = 64U * 1024U;

// Validate the upstream protobuf tree before conversion or type checking. This
// owns no alternate tree: frames borrow the decoded CEL message for this pass.
status admit_proto(const cel::expr::CheckedExpr& checked, const limits& budget) {
   if (!checked.has_expr() || checked.has_source_info() || !checked.expr_version().empty()) {
      return status::invalid_program;
   }
   if (checked.type_map().size() > budget.nodes || checked.reference_map().size() > budget.nodes) {
      return status::resource_exhausted;
   }
   struct frame {
      const cel::expr::Expr* expression;
      std::size_t depth;
   };
   std::vector<frame> pending{{&checked.expr(), 1}};
   std::set<std::int64_t> identifiers;
   std::size_t nodes = 0;
   const auto identify = [&](std::int64_t id) {
      return id > 0 && id <= std::numeric_limits<std::int32_t>::max() && identifiers.insert(id).second;
   };
   const auto text = [&](const std::string& value) {
      return value.size() <= limits{}.string_bytes && cel::internal::Utf8IsValid(view(value));
   };
   while (!pending.empty()) {
      const auto current = pending.back();
      pending.pop_back();
      const auto& expr = *current.expression;
      if (++nodes > budget.nodes || current.depth > budget.depth)
         return status::resource_exhausted;
      if (!identify(expr.id()))
         return status::invalid_program;
      const auto child = [&](const cel::expr::Expr& value) { pending.push_back({&value, current.depth + 1}); };
      switch (expr.expr_kind_case()) {
      case cel::expr::Expr::kConstExpr: {
         const auto& constant = expr.const_expr();
         switch (constant.constant_kind_case()) {
         case cel::expr::Constant::kInt64Value:
         case cel::expr::Constant::kUint64Value:
         case cel::expr::Constant::kBoolValue:
            break;
         case cel::expr::Constant::kStringValue:
            if (constant.string_value().size() > budget.string_bytes)
               return status::resource_exhausted;
            if (!text(constant.string_value()))
               return status::invalid_program;
            break;
         default:
            return status::invalid_program;
         }
         break;
      }
      case cel::expr::Expr::kIdentExpr:
         if (expr.ident_expr().name().empty() || !text(expr.ident_expr().name()))
            return status::invalid_program;
         break;
      case cel::expr::Expr::kSelectExpr:
         if (expr.select_expr().field().size() > budget.string_bytes)
            return status::resource_exhausted;
         if (!expr.select_expr().has_operand() || expr.select_expr().test_only() ||
             expr.select_expr().field().empty() || !text(expr.select_expr().field()))
            return status::invalid_program;
         child(expr.select_expr().operand());
         break;
      case cel::expr::Expr::kCallExpr:
         if (expr.call_expr().function().empty() || !text(expr.call_expr().function()))
            return status::invalid_program;
         if (expr.call_expr().has_target())
            child(expr.call_expr().target());
         for (const auto& argument : expr.call_expr().args())
            child(argument);
         break;
      case cel::expr::Expr::kListExpr:
         if (expr.list_expr().elements_size() > budget.collection_elements)
            return status::resource_exhausted;
         if (!expr.list_expr().optional_indices().empty())
            return status::invalid_program;
         for (const auto& element : expr.list_expr().elements())
            child(element);
         break;
      case cel::expr::Expr::kStructExpr:
         if (!expr.struct_expr().message_name().empty())
            return status::invalid_program;
         if (expr.struct_expr().entries_size() > budget.collection_elements)
            return status::resource_exhausted;
         for (const auto& entry : expr.struct_expr().entries()) {
            if (!identify(entry.id()) || !entry.has_map_key() || !entry.has_value() || entry.optional_entry())
               return status::invalid_program;
            child(entry.map_key());
            child(entry.value());
         }
         break;
      default:
         return status::invalid_program;
      }
      // Avoid a wide malformed tree allocating work beyond the admitted cap.
      if (pending.size() > budget.nodes - nodes)
         return status::resource_exhausted;
   }
   return status::ok;
}

bool canonical_expression(const cel::Ast& ast, std::string& output) {
   cel::expr::CheckedExpr checked;
   if (!cel::AstToCheckedExpr(ast, &checked).ok())
      return false;
   checked.clear_source_info();
   checked.clear_expr_version();
   if (checked.ByteSizeLong() > expression_byte_limit)
      return false;
   output.clear();
   google::protobuf::io::StringOutputStream stream(&output);
   google::protobuf::io::CodedOutputStream coded(&stream);
   coded.SetSerializationDeterministic(true);
   return checked.SerializeToCodedStream(&coded) && !coded.HadError();
}

preparation prepare_impl(std::string_view source, std::span<const declaration> declarations, limits budget,
                         bool loading = false, value_kind expected_result = value_kind::null) {
   if (!valid_limits(budget)) {
      return {.code = status::invalid_profile, .message = "invalid V1 profile limits"};
   }
   // Byte cap precedes parser/source construction, including Unicode decoding.
   if ((!loading && source.size() > budget.source_bytes) || (loading && source.size() > expression_byte_limit)) {
      return {.code = status::resource_exhausted, .message = "source byte limit exceeded"};
   }
   if (declarations.size() > budget.variables) {
      return {.code = status::resource_exhausted, .message = "variable limit exceeded"};
   }
   std::vector<owned_declaration> owned;
   owned.reserve(declarations.size());
   for (const auto& item : declarations) {
      if (!valid_name(item.name, budget) || !valid_schema(item.kind)) {
         return {.code = status::invalid_profile, .message = "invalid variable declaration"};
      }
      owned.push_back({std::string(item.name), item.kind});
   }
   std::sort(owned.begin(), owned.end(), [](const auto& left, const auto& right) { return left.name < right.name; });
   if (std::adjacent_find(owned.begin(), owned.end(),
                          [](const auto& left, const auto& right) { return left.name == right.name; }) != owned.end()) {
      return {.code = status::invalid_profile, .message = "duplicate variable declaration"};
   }

   std::unique_ptr<cel::Ast> parsed;
   if (loading) {
      if (source.empty())
         return {.code = status::invalid_program, .message = "empty CEL artifact"};
      cel::expr::CheckedExpr checked;
      google::protobuf::io::ArrayInputStream input(source.data(), static_cast<int>(source.size()));
      google::protobuf::io::CodedInputStream coded(&input);
      coded.SetTotalBytesLimit(static_cast<int>(expression_byte_limit));
      // Each expression level includes protobuf wrapper messages. This is a
      // physical decode cap; the following pass enforces logical AST depth.
      coded.SetRecursionLimit(static_cast<int>(limits{}.depth * 3 + 8));
      if (!checked.ParseFromCodedStream(&coded) || !coded.ConsumedEntireMessage())
         return {.code = status::invalid_program, .message = "invalid CEL artifact encoding"};
      const auto admitted = admit_proto(checked, budget);
      if (admitted != status::ok)
         return {.code = admitted, .message = "CEL artifact structure rejected"};
      // Never import the supplied type/reference maps as checked state. The
      // upstream checker derives them afresh from this parsed-only AST.
      auto converted = cel::CreateAstFromParsedExpr(checked.expr());
      if (!converted.ok())
         return {.code = status::invalid_program, .message = "CEL artifact conversion rejected"};
      parsed = std::move(*converted);
   } else {
      cel::ParserOptions parser_options;
      parser_options.enable_pratt_parser = true;
      parser_options.disable_standard_macros = true;
      parser_options.enable_optional_syntax = false;
      parser_options.fold_unary_operators = false;
      parser_options.expression_size_codepoint_limit = static_cast<int>(budget.source_bytes);
      // With macros disabled, source bytes bound raw parser expansion. Enforce
      // the caller's smaller node limit with the typed pre-checker traversal,
      // rather than parsing CEL's untyped InvalidArgument diagnostic text.
      parser_options.expression_node_limit = static_cast<int>(limits{}.source_bytes);
      parser_options.max_recursion_depth = static_cast<int>(limits{}.depth);
      auto parser = cel::NewParserBuilder(parser_options)->Build();
      if (!parser.ok()) {
         return {.code = status::internal, .message = "CEL parser construction failed"};
      }
      auto prepared_source = (*parser)->PrepareSource(view(source));
      if (!prepared_source.ok()) {
         return {.code = status::invalid_program, .message = "invalid CEL source"};
      }
      auto result = (*parser)->Parse(**prepared_source);
      if (!result.ok()) {
         const bool depth_limit = result.status().code() == absl::StatusCode::kCancelled;
         return {.code = depth_limit ? status::resource_exhausted : status::invalid_program,
                 .message = depth_limit ? "parser depth limit exceeded" : "CEL source rejected"};
      }
      parsed = std::move(*result);
   }
   auto admitted = admit(*parsed, budget, owned, false);
   if (admitted != status::ok) {
      return {.code = admitted, .message = "CEL structure rejected"};
   }

   cel::CheckerOptions checker_options;
   // The checker counts traversal records (including call arguments), not
   // just Expr nodes. Our preceding admission enforces the exact Expr cap;
   // allow its additional traversal records without rejecting boundary cases.
   checker_options.max_expression_node_count = static_cast<int>(budget.nodes * 2);
   checker_options.max_error_issues = 1;
   // stdlib also declares type(map(dyn,dyn)); these trusted declarations need
   // three levels. Admitted expression values still allow only flat containers.
   checker_options.max_type_decl_nesting = 3;
   checker_options.enable_cross_numeric_comparisons = false;
   checker_options.enable_legacy_null_assignment = false;
   auto builder = cel::CreateTypeCheckerBuilder(cel::GetMinimalDescriptorPool(), checker_options);
   if (!builder.ok() || !(*builder)->AddLibrary(cel::StandardCheckerLibrary()).ok() ||
       !(*builder)->AddLibrarySubset({"stdlib", cel::IncludeOverloadsByIdPredicate(allowed_overloads)}).ok() ||
       !add_arithmetic_declarations(**builder).ok()) {
      return {.code = status::internal, .message = "CEL checker construction failed"};
   }
   for (std::size_t index = 0; index < rounding_names.size(); ++index) {
      cel::Constant value;
      value.set_int_value(static_cast<std::int64_t>(index));
      if (!(*builder)
               ->AddVariable(
                   cel::MakeConstantVariableDecl(std::string(rounding_names[index]), cel::IntType(), std::move(value)))
               .ok()) {
         return {.code = status::internal, .message = "CEL constant declaration failed"};
      }
   }
   for (const auto& item : owned) {
      if (!(*builder)
               ->AddOrReplaceVariable(cel::MakeVariableDecl(item.name, make_type(item.kind, (*builder)->arena())))
               .ok()) {
         return {.code = status::invalid_profile, .message = "CEL variable declaration rejected"};
      }
   }
   auto checker = (*builder)->Build();
   if (!checker.ok()) {
      return {.code = status::internal, .message = "CEL checker construction failed"};
   }
   auto checked = (*checker)->Check(std::move(parsed));
   if (!checked.ok() || !checked->IsValid()) {
      return {.code = status::invalid_program, .message = "CEL type checking rejected the expression"};
   }
   auto ast = checked->ReleaseAst();
   if (!ast.ok()) {
      return {.code = status::internal, .message = "CEL checked expression unavailable"};
   }
   admitted = admit(**ast, budget, owned, true);
   if (admitted != status::ok ||
       (!numeric((*ast)->GetReturnType()) && (!(*ast)->GetReturnType().has_primitive() ||
                                              (*ast)->GetReturnType().primitive() != cel::PrimitiveType::kBool))) {
      return {.code = admitted == status::ok ? status::invalid_program : admitted,
              .message = "CEL V1 profile rejected the expression"};
   }

   const auto result_type = (*ast)->GetReturnType().primitive();
   const auto result = result_type == cel::PrimitiveType::kInt64    ? value_kind::int64
                       : result_type == cel::PrimitiveType::kUint64 ? value_kind::uint64
                                                                    : value_kind::boolean;
   std::string expression;
   if (!canonical_expression(**ast, expression))
      return {.code = status::resource_exhausted, .message = "CEL artifact byte limit exceeded"};
   if (loading && (result != expected_result || expression != source))
      return {.code = status::invalid_program, .message = "CEL artifact checked metadata rejected"};

   cel::RuntimeOptions options;
   options.enable_comprehension = false;
   options.enable_regex = false;
   options.enable_string_conversion = false;
   options.enable_string_concat = false;
   options.enable_list_concat = false;
   options.enable_list_contains = false;
   options.enable_fast_builtins = false;
   options.short_circuiting = true;
   options.fail_on_warnings = true;
   auto runtime_builder = cel::CreateStandardRuntimeBuilder(cel::GetMinimalDescriptorPool(), options);
   // The standard builder does not resolve checker constant references by
   // default. Use CEL's own checked-reference pass for rounding.* constants.
   if (!runtime_builder.ok() || !register_arithmetic_functions(runtime_builder->function_registry()).ok() ||
       !cel::EnableReferenceResolver(*runtime_builder, cel::ReferenceResolverEnabled::kCheckedExpressionOnly).ok()) {
      return {.code = status::internal, .message = "CEL runtime construction failed"};
   }
   auto runtime = std::move(*runtime_builder).Build();
   if (!runtime.ok()) {
      return {.code = status::internal, .message = "CEL runtime construction failed"};
   }
   auto executable = (*runtime)->CreateTraceableProgram(std::move(*ast));
   if (!executable.ok()) {
      return {.code = status::invalid_program, .message = "CEL planning rejected the expression"};
   }
   return {.executable = std::make_unique<cel_program>(std::move(*executable), std::move(owned), budget),
           .expression = std::move(expression),
           .result = result};
}

} // namespace

program::~program() = default;

void program::initialize_diagnostics() noexcept {
   // These setters store only constant-initialized atomics. Do not initialize
   // Abseil logging here: InitializeLog() resolves the system's local timezone.
   absl::log_internal::EnableSymbolizeLogStackTrace(false);
   absl::log_internal::SetMaxFramesInLogStackTrace(0);
   absl::SetMutexDeadlockDetectionMode(absl::OnDeadlockCycle::kIgnore);
}

preparation program::prepare(std::string_view source, std::span<const declaration> declarations, limits budget) {
   try {
      return prepare_impl(source, declarations, budget);
   } catch (const std::bad_alloc&) {
      return {.code = status::resource_exhausted, .message = "native allocation failed"};
   } catch (...) {
      return {.code = status::internal, .message = "native preparation failed"};
   }
}

preparation program::load(std::string_view expression, std::span<const declaration> declarations, limits budget,
                          value_kind result) {
   try {
      return prepare_impl(expression, declarations, budget, true, result);
   } catch (const std::bad_alloc&) {
      return {.code = status::resource_exhausted, .message = "native allocation failed"};
   } catch (...) {
      return {.code = status::internal, .message = "native artifact loading failed"};
   }
}

} // namespace forge::expression::detail::backend
