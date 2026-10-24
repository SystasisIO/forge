#include "../libraries/expression/details/backend.hxx"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "cel/expr/checked.pb.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"

namespace {

namespace backend = forge::expression::detail::backend;

void require(bool condition, std::string_view message) {
   if (!condition) {
      std::cerr << message << '\n';
      std::exit(EXIT_FAILURE);
   }
}

std::string encode(const cel::expr::CheckedExpr& value) {
   std::string output;
   {
      google::protobuf::io::StringOutputStream stream(&output);
      google::protobuf::io::CodedOutputStream coded(&stream);
      coded.SetSerializationDeterministic(true);
      require(value.SerializeToCodedStream(&coded), "fixture serialization failed");
   }
   return output;
}

} // namespace

int main() {
   const std::array declarations = {backend::declaration{"x", backend::schema_kind::int64}};
   const std::array inputs = {backend::named_value{"x", {.kind = backend::value_kind::int64, .i64 = 5}}};
   for (const auto* source : {"x + 9007199254740993", "math.mul_div(x, 2, 3, rounding.ceil)", "x > 0 ? x : 1 / 0",
                              "x == 5", "[1, 2, x][2]", "{'a': x}.a", "18446744073709551615u"}) {
      auto compiled = backend::program::prepare(source, declarations);
      require(compiled.code == backend::status::ok && compiled.executable && !compiled.expression.empty(),
              "source preparation failed");
      auto loaded = backend::program::load(compiled.expression, declarations, {}, compiled.result);
      require(loaded.code == backend::status::ok && loaded.executable, "artifact recheck failed");
      require(loaded.expression == compiled.expression, "canonical artifact changed during load");
      const auto before = compiled.executable->evaluate(inputs);
      const auto after = loaded.executable->evaluate(inputs);
      require(before.code == backend::status::ok && after.code == backend::status::ok && before.kind == after.kind &&
                  before.i64 == after.i64 && before.u64 == after.u64 && before.boolean == after.boolean &&
                  before.ast_events == after.ast_events,
              "loaded evaluation differs from source evaluation");
   }

   const auto compiled = backend::program::prepare("x + 1", declarations);
   require(compiled.code == backend::status::ok, "mutation baseline failed");
   cel::expr::CheckedExpr baseline;
   require(baseline.ParseFromString(compiled.expression), "mutation baseline decode failed");
   const auto reject = [&](const cel::expr::CheckedExpr& forged) {
      const auto loaded = backend::program::load(encode(forged), declarations, {}, compiled.result);
      require(loaded.code == backend::status::invalid_program && !loaded.executable,
              "untrusted checked metadata or malformed tree was accepted");
   };
   {
      auto forged = baseline;
      (*forged.mutable_type_map())[forged.expr().id()].set_primitive(cel::expr::Type::BOOL);
      reject(forged);
   }
   {
      auto forged = baseline;
      auto& reference = (*forged.mutable_reference_map())[forged.expr().id()];
      reference.clear_overload_id();
      reference.add_overload_id("add_double");
      reject(forged);
   }
   {
      auto forged = baseline;
      forged.mutable_expr()->mutable_call_expr()->mutable_args(1)->set_id(forged.expr().call_expr().args(0).id());
      reject(forged);
   }
   {
      auto forged = baseline;
      forged.mutable_expr()->set_id(0);
      reject(forged);
   }
   {
      auto forged = baseline;
      forged.mutable_expr()->mutable_call_expr()->mutable_args(1)->clear_expr_kind();
      reject(forged);
   }
   {
      auto forged = baseline;
      forged.mutable_expr()->mutable_call_expr()->clear_args();
      reject(forged);
   }
   {
      auto forged = baseline;
      forged.mutable_source_info()->set_location("untrusted");
      reject(forged);
   }
   {
      auto forged = baseline;
      forged.set_expr_version("untrusted");
      reject(forged);
   }
   {
      auto forged = baseline;
      (*forged.mutable_type_map())[10000].set_primitive(cel::expr::Type::INT64);
      reject(forged);
   }
   {
      auto forged = baseline;
      forged.mutable_expr()->mutable_call_expr()->mutable_args(1)->mutable_const_expr()->set_double_value(1.0);
      reject(forged);
   }
   {
      const auto rounding = backend::program::prepare("math.mul_div(x, 2, 3, rounding.ceil)", declarations);
      cel::expr::CheckedExpr forged;
      require(forged.ParseFromString(rounding.expression), "constant reference baseline decode failed");
      bool changed = false;
      for (auto& item : *forged.mutable_reference_map()) {
         if (item.second.name() == "rounding.ceil") {
            item.second.mutable_value()->set_int64_value(99);
            changed = true;
         }
      }
      require(changed, "constant reference baseline is missing");
      reject(forged);
   }
   {
      const auto map = backend::program::prepare("{'a': x}.a", declarations);
      cel::expr::CheckedExpr forged;
      require(forged.ParseFromString(map.expression), "select baseline decode failed");
      forged.mutable_expr()->mutable_select_expr()->clear_operand();
      reject(forged);
   }
   {
      const auto map = backend::program::prepare("{'a': x}['a']", declarations);
      cel::expr::CheckedExpr forged;
      require(forged.ParseFromString(map.expression), "map baseline decode failed");
      forged.mutable_expr()
          ->mutable_call_expr()
          ->mutable_args(0)
          ->mutable_struct_expr()
          ->mutable_entries(0)
          ->clear_map_key();
      reject(forged);
   }
   const auto wrong_result =
       backend::program::load(compiled.expression, declarations, {}, backend::value_kind::boolean);
   require(wrong_result.code == backend::status::invalid_program, "forged result kind was accepted");
   const std::array wrong_declarations = {backend::declaration{"x", backend::schema_kind::uint64}};
   require(backend::program::load(compiled.expression, wrong_declarations, {}, compiled.result).code ==
               backend::status::invalid_program,
           "artifact was detached from its declarations");
   auto budget = backend::limits{};
   budget.nodes = 2;
   require(backend::program::load(compiled.expression, declarations, budget, compiled.result).code ==
               backend::status::resource_exhausted,
           "artifact bypassed its node budget");
   require(backend::program::load(std::string(65537, 'a'), declarations, {}, compiled.result).code ==
               backend::status::resource_exhausted,
           "artifact bypassed its byte budget");
   // A second protobuf encoding of the root expression is semantically equal
   // under protobuf's last-field rule, but is outside the canonical artifact.
   cel::expr::CheckedExpr repeated_root;
   *repeated_root.mutable_expr() = baseline.expr();
   const auto duplicate_field = compiled.expression + encode(repeated_root);
   require(backend::program::load(duplicate_field, declarations, {}, compiled.result).code ==
               backend::status::invalid_program,
           "noncanonical protobuf was accepted");
   // Field 127, wire type 0, value 1: a valid unknown protobuf field cannot
   // become part of the pinned canonical CheckedExpr representation.
   const auto unknown_field = compiled.expression + std::string{"\xf8\x07\x01", 3};
   cel::expr::CheckedExpr unknown;
   require(unknown.ParseFromString(unknown_field) && unknown.expr().id() == baseline.expr().id(),
           "unknown field fixture is not a valid protobuf mutation");
   require(backend::program::load(unknown_field, declarations, {}, compiled.result).code ==
               backend::status::invalid_program,
           "unknown protobuf field was accepted");

   budget = {};
   budget.string_bytes = 3;
   auto short_strings = backend::program::prepare("'abc' == 'abc'", {}, budget);
   require(short_strings.code == backend::status::ok &&
               backend::program::load(short_strings.expression, {}, budget, short_strings.result).code ==
                   backend::status::ok,
           "reserved CEL function names incorrectly consume the user string budget");
   return EXIT_SUCCESS;
}
