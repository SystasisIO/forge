module;

#include <clang/AST/Attr.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/TypeLoc.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/Module.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Sema/Lookup.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/DynamicLibrary.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.tooling.abi.generator;

import forge.chain.protocol.abi;
import forge.chain.protocol.abi_metadata;
import forge.chain.protocol.types;
import forge.codec.json;
import forge.db.ids.typed_id;
import forge.variant.value;

namespace {

namespace protocol = forge::chain::protocol;

struct field_shape {
   std::string name;
   std::string type;

   bool operator==(const field_shape&) const = default;
};

struct struct_shape {
   std::string name;
   std::string base;
   std::vector<field_shape> fields;

   bool operator==(const struct_shape&) const = default;
};

struct record_codec_shape {
   std::string name;
   std::string base;
   std::string forward_declaration;
   std::vector<std::string> fields;
};

struct action_shape {
   std::string name;
   std::string type;
   std::string result;
   std::string class_name;
   std::string method_name;
   std::filesystem::path source;
   bool dispatchable = true;
};

struct notification_shape {
   std::uint64_t code = 0;
   std::uint64_t action = 0;
   std::string class_name;
   std::string method_name;
   std::filesystem::path source;
};

struct type_shape {
   std::string name;
   std::string type;
};

struct type_declaration {
   std::string type;
   std::string identity;
};

struct variant_shape {
   std::string name;
   std::vector<std::string> types;
};

struct table_shape {
   std::string name;
   std::string type;
};

struct call_shape {
   std::string name;
   std::string type;
   std::string result;
   std::uint64_t id = 0;
   std::string class_name;
   std::string method_name;
   std::filesystem::path source;
};

struct schema {
   std::vector<type_shape> types;
   std::vector<struct_shape> structs;
   std::vector<record_codec_shape> record_codecs;
   std::vector<action_shape> actions;
   std::vector<notification_shape> notifications;
   std::vector<variant_shape> variants;
   std::vector<table_shape> tables;
   std::vector<protocol::clause_pair> clauses;
   std::vector<call_shape> calls;
   std::map<std::string, type_declaration> type_declarations;
   std::map<std::string, std::string> struct_declarations;
   std::set<std::string> variant_names;
   std::map<std::string, std::string> table_declarations;
   std::map<std::string, std::string> inferred_table_names;
   std::set<std::string> explicit_table_declarations;
   std::map<std::string, std::string> action_declarations;
   std::map<std::string, std::string> action_methods;
   std::map<std::string, std::string> notification_declarations;
   std::map<std::string, std::string> call_declarations;
   std::map<std::filesystem::path, std::set<std::string>> source_record_codecs;
   bool has_apply = false;
   bool has_eosio_dispatch = false;
   bool failed = false;
   std::vector<std::string> requested_roots;
   protocol::abi_metadata metadata;
   bool collect_metadata = false;
};

std::string make_forward_declaration(const clang::RecordDecl& declaration) {
   if (declaration.getIdentifier() == nullptr || llvm::isa<clang::ClassTemplateSpecializationDecl>(declaration)) {
      return {};
   }

   auto namespaces = std::vector<const clang::NamespaceDecl*>{};
   auto* context = declaration.getDeclContext();
   while (const auto* current = llvm::dyn_cast<clang::NamespaceDecl>(context)) {
      if (current->isAnonymousNamespace()) {
         return {};
      }
      namespaces.push_back(current);
      context = current->getDeclContext();
   }
   if (!context->isTranslationUnit()) {
      return {};
   }

   auto output = std::ostringstream{};
   for (auto iterator = namespaces.rbegin(); iterator != namespaces.rend(); ++iterator) {
      if ((*iterator)->isInline()) {
         output << "inline ";
      }
      output << "namespace " << (*iterator)->getNameAsString() << " { ";
   }
   output << (declaration.isClass() ? "class " : "struct ") << declaration.getNameAsString() << ';';
   for (auto index = std::size_t{0}; index < namespaces.size(); ++index) {
      output << " }";
   }
   return output.str();
}

bool has_nameable_scope(const clang::RecordDecl& declaration) {
   if (declaration.getIdentifier() == nullptr) {
      return false;
   }

   auto* context = declaration.getDeclContext();
   while (context != nullptr) {
      if (context->isTranslationUnit()) {
         return true;
      }
      if (const auto* current = llvm::dyn_cast<clang::NamespaceDecl>(context)) {
         if (current->isAnonymousNamespace()) {
            return false;
         }
         context = current->getParent();
         continue;
      }
      if (const auto* current = llvm::dyn_cast<clang::RecordDecl>(context)) {
         if (current->getIdentifier() == nullptr) {
            return false;
         }
         context = current->getDeclContext();
         continue;
      }
      if (const auto* current = llvm::dyn_cast<clang::LinkageSpecDecl>(context)) {
         context = current->getParent();
         continue;
      }
      if (const auto* current = llvm::dyn_cast<clang::ExportDecl>(context)) {
         context = current->getDeclContext();
         continue;
      }
      return false;
   }
   return false;
}

bool has_publicly_accessible_name(const clang::RecordDecl& declaration) {
   const auto* current = &declaration;
   while (current != nullptr) {
      if (current->getAccess() == clang::AS_private || current->getAccess() == clang::AS_protected) {
         return false;
      }
      current = llvm::dyn_cast<clang::RecordDecl>(current->getDeclContext());
   }
   return true;
}

std::string declaration_identity(const clang::NamedDecl& declaration) {
   auto identity = llvm::SmallString<128>{};
   if (!clang::index::generateUSRForDecl(&declaration, identity) && !identity.empty()) {
      return identity.str().str();
   }
   auto fallback = declaration.getQualifiedNameAsString();
   if (const auto* value = llvm::dyn_cast<clang::ValueDecl>(&declaration); value != nullptr) {
      fallback += ':' + value->getType().getCanonicalType().getAsString();
   }
   return fallback;
}

std::string record_layout_identity(const clang::RecordDecl& declaration) {
   const auto* record = &declaration;
   if (const auto* cpp = llvm::dyn_cast<clang::CXXRecordDecl>(record); cpp != nullptr) {
      if (const auto* definition = cpp->getDefinition(); definition != nullptr) {
         record = definition;
      }
   }

   auto result = std::string{record->isUnion() ? "union" : "record"};
   if (const auto* cpp = llvm::dyn_cast<clang::CXXRecordDecl>(record); cpp != nullptr && cpp->hasDefinition()) {
      for (const auto& base : cpp->bases()) {
         result += "|base:" + base.getType().getCanonicalType().getAsString();
      }
   }
   for (const auto* field : record->fields()) {
      result += "|field:" + field->getNameAsString() + ':' + field->getType().getCanonicalType().getAsString();
   }
   return result;
}

bool claim_struct(schema& output, clang::ASTContext& context, const std::string& name, std::string identity,
                  clang::SourceLocation location = {}) {
   const auto [existing, inserted] = output.struct_declarations.try_emplace(name, std::move(identity));
   if (inserted) {
      return true;
   }
   if (existing->second == identity) {
      return false;
   }
   const auto id = context.getDiagnostics().getCustomDiagID(clang::DiagnosticsEngine::Error,
                                                            "duplicate contract ABI struct name '%0'");
   context.getDiagnostics().Report(location, id) << name;
   output.failed = true;
   return false;
}

std::optional<std::string> annotation(const clang::Decl& declaration, std::string_view prefix) {
   const auto expected = llvm::StringRef{prefix.data(), prefix.size()};
   for (const auto* attribute : declaration.specific_attrs<clang::AnnotateAttr>()) {
      const auto value = attribute->getAnnotation();
      if (value == expected) {
         return std::string{};
      }
      if (value.starts_with(prefix) && value.size() > prefix.size() && value[prefix.size()] == ':') {
         return value.substr(prefix.size() + 1U).str();
      }
   }
   return std::nullopt;
}

bool has_annotation(const clang::Decl& declaration, std::string_view expected) {
   const auto value = llvm::StringRef{expected.data(), expected.size()};
   return std::ranges::any_of(declaration.specific_attrs<clang::AnnotateAttr>(),
                              [&](const auto* attribute) { return attribute->getAnnotation() == value; });
}

std::string record_name(const clang::RecordDecl& declaration) {
   if (declaration.getIdentifier() == nullptr) {
      throw std::runtime_error{"anonymous records are not supported in contract ABI"};
   }
   return declaration.getNameAsString();
}

bool is_global_function(const clang::FunctionDecl& declaration) {
   auto* context = declaration.getDeclContext();
   while (llvm::isa_and_nonnull<clang::LinkageSpecDecl>(context)) {
      context = context->getParent();
   }
   return context != nullptr && context->isTranslationUnit();
}

class type_encoder {
 public:
   type_encoder(clang::ASTContext& context, schema& output, std::set<std::string>& source_record_codecs)
       : context_(context), output_(output), source_record_codecs_(source_record_codecs) {}

   std::string encode(clang::QualType input) {
      auto type = input.getNonReferenceType().getUnqualifiedType();
      if (is_typed_id(type)) {
         return "uint64";
      }
      if (const auto* alias = llvm::dyn_cast_or_null<clang::TypedefType>(type.getTypePtrOrNull())) {
         const auto* declaration = alias->getDecl();
         const auto qualified = declaration->getQualifiedNameAsString();
         const auto canonical = qualified == "forge::chain::protocol::extensions" || known_alias(qualified).has_value();
         if (!canonical && !context_.getSourceManager().isInSystemHeader(declaration->getLocation())) {
            const auto name = declaration->getNameAsString();
            const auto target = encode(declaration->getUnderlyingType());
            add_alias(name, target, declaration->getLocation(), declaration_identity(*declaration));
            return name.empty() ? target : name;
         }
      }
      if (const auto known = known_type_alias(type); known.has_value()) {
         return *known;
      }
      if (const auto* alias = llvm::dyn_cast_or_null<clang::TypedefType>(type.getTypePtrOrNull())) {
         const auto* declaration = alias->getDecl();
         if (!context_.getSourceManager().isInSystemHeader(declaration->getLocation())) {
            const auto name = declaration->getNameAsString();
            const auto target = encode(declaration->getUnderlyingType());
            add_alias(name, target, declaration->getLocation(), declaration_identity(*declaration));
            return name.empty() ? target : name;
         }
         type = alias->desugar().getUnqualifiedType();
      }
      type = type.getDesugaredType(context_).getUnqualifiedType();
      if (const auto* builtin = type->getAs<clang::BuiltinType>()) {
         return encode_builtin(*builtin);
      }
      if (const auto* enumeration = type->getAs<clang::EnumType>()) {
         const auto* declaration = enumeration->getDecl();
         const auto target = encode(declaration->getIntegerType());
         if (declaration->getIdentifier() == nullptr ||
             context_.getSourceManager().isInSystemHeader(declaration->getLocation())) {
            return target;
         }
         const auto name = declaration->getNameAsString();
         add_alias(name, target, declaration->getLocation(), declaration_identity(*declaration));
         if (output_.collect_metadata &&
             std::ranges::none_of(output_.metadata.enums, [&](const auto& entry) { return entry.name == name; })) {
            const auto integer_type = encode(declaration->getIntegerType().getCanonicalType());
            static constexpr auto metadata_integer_types = std::array<std::string_view, 8>{
                "int8", "uint8", "int16", "uint16", "int32", "uint32", "int64", "uint64"};
            if (std::ranges::find(metadata_integer_types, integer_type) == metadata_integer_types.end()) {
               fail("enum metadata integer type " + integer_type, declaration->getLocation());
            }
            auto definition = protocol::abi_enum_def{.name = name, .type = integer_type};
            auto values = std::set<std::string>{};
            for (const auto* enumerator : declaration->enumerators()) {
               auto value = llvm::toString(enumerator->getInitVal(), 10);
               if (!values.insert(value).second) {
                  fail("enum with duplicate numeric values", enumerator->getLocation());
               }
               definition.values.push_back({enumerator->getNameAsString(), std::move(value)});
            }
            if (definition.values.empty()) {
               fail("enum with no values", declaration->getLocation());
            }
            output_.metadata.enums.push_back(std::move(definition));
         }
         return name;
      }
      if (const auto* array = context_.getAsConstantArrayType(type)) {
         return encode(array->getElementType()) + '[' + llvm::toString(array->getSize(), 10, false) + ']';
      }
      if (const auto* record = type->getAs<clang::RecordType>()) {
         return encode_record(*record->getDecl());
      }
      fail(type.getAsString(), {});
      return {};
   }

   std::string encode(const clang::DeclaratorDecl& declaration) {
      const auto* source = declaration.getTypeSourceInfo();
      return source == nullptr ? encode(declaration.getType()) : encode_location(source->getTypeLoc());
   }

   void add_table(const clang::CXXRecordDecl& declaration, std::string name, bool inferred = false) {
      const auto identity = declaration_identity(declaration);
      if (inferred && output_.explicit_table_declarations.contains(identity)) {
         return;
      }
      if (!inferred) {
         output_.explicit_table_declarations.insert(identity);
         if (const auto existing = output_.inferred_table_names.find(identity);
             existing != output_.inferred_table_names.end()) {
            const auto inferred_name = existing->second;
            output_.tables.erase(std::remove_if(output_.tables.begin(), output_.tables.end(),
                                                [&](const table_shape& table) { return table.name == inferred_name; }),
                                 output_.tables.end());
            output_.table_declarations.erase(inferred_name);
            output_.inferred_table_names.erase(existing);
         }
      }
      if (name.empty()) {
         name = record_name(declaration);
      }
      const auto [existing, inserted] = output_.table_declarations.try_emplace(name, identity);
      if (!inserted && existing->second == identity) {
         return;
      }
      if (!inserted) {
         const auto id = context_.getDiagnostics().getCustomDiagID(clang::DiagnosticsEngine::Error,
                                                                   "duplicate contract ABI table name '%0'");
         context_.getDiagnostics().Report(declaration.getLocation(), id) << name;
         output_.failed = true;
         return;
      }
      output_.tables.push_back(table_shape{std::move(name), encode_record(declaration)});
      if (inferred) {
         output_.inferred_table_names.try_emplace(identity, output_.tables.back().name);
      }
   }

 private:
   static std::optional<std::string> known_alias(std::string_view name) {
      static constexpr auto aliases = std::array{
          std::pair{"forge::chain::protocol::int128_t", "int128"},
          std::pair{"forge::chain::protocol::uint128_t", "uint128"},
          std::pair{"forge::chain::protocol::account_name", "name"},
          std::pair{"forge::chain::protocol::action_name", "name"},
          std::pair{"forge::chain::protocol::permission_name", "name"},
          std::pair{"forge::chain::protocol::table_name", "name"},
          std::pair{"forge::chain::protocol::bytes", "bytes"},
          std::pair{"forge::chain::protocol::digest", "checksum256"},
          std::pair{"forge::chain::protocol::chain_id", "checksum256"},
          std::pair{"forge::chain::protocol::block_id", "checksum256"},
          std::pair{"forge::chain::protocol::checksum", "checksum256"},
          std::pair{"forge::chain::protocol::checksum256", "checksum256"},
          std::pair{"forge::chain::protocol::checksum512", "checksum512"},
          std::pair{"forge::chain::protocol::checksum160", "checksum160"},
          std::pair{"forge::chain::protocol::transaction_id", "checksum256"},
          std::pair{"forge::chain::protocol::public_key", "public_key"},
          std::pair{"forge::chain::protocol::signature", "signature"},
          std::pair{"forge::crypto::asymmetric::public_key", "public_key"},
          std::pair{"forge::crypto::asymmetric::signature", "signature"},
          std::pair{"forge::contract::public_key", "public_key"},
          std::pair{"forge::contract::signature", "signature"},
      };
      for (const auto& [cpp_name, abi_name] : aliases) {
         if (cpp_name == name) {
            return std::string{abi_name};
         }
      }
      return std::nullopt;
   }

   std::optional<std::string> known_type_alias(clang::QualType input) {
      auto type = input;
      while (!type.isNull()) {
         if (const auto* alias = llvm::dyn_cast_or_null<clang::TypedefType>(type.getTypePtrOrNull())) {
            if (alias->getDecl()->getQualifiedNameAsString() == "forge::chain::protocol::extensions") {
               add_extension();
               return "extension[]";
            }
            if (const auto known = known_alias(alias->getDecl()->getQualifiedNameAsString()); known.has_value()) {
               return known;
            }
         }
         const auto desugared = type.getSingleStepDesugaredType(context_).getUnqualifiedType();
         if (desugared == type) {
            break;
         }
         type = desugared;
      }
      return std::nullopt;
   }

   bool is_byte_type(clang::QualType input) const {
      const auto type = input.getDesugaredType(context_).getUnqualifiedType();
      const auto* builtin = type->getAs<clang::BuiltinType>();
      if (builtin == nullptr) {
         return false;
      }
      return builtin->getKind() == clang::BuiltinType::Char_S || builtin->getKind() == clang::BuiltinType::Char_U ||
             builtin->getKind() == clang::BuiltinType::SChar || builtin->getKind() == clang::BuiltinType::UChar;
   }

   static bool is_std_template(const clang::TemplateDecl& declaration, std::string_view name) {
      if (declaration.getName() != llvm::StringRef{name.data(), name.size()}) {
         return false;
      }
      auto* context = declaration.getDeclContext();
      while (const auto* current = llvm::dyn_cast<clang::NamespaceDecl>(context)) {
         if (current->isStdNamespace()) {
            return true;
         }
         if (!current->isInline()) {
            return false;
         }
         context = current->getParent();
      }
      return false;
   }

   static clang::TypeLoc unwrap(clang::TypeLoc location) {
      while (true) {
         if (const auto qualified = location.getAs<clang::QualifiedTypeLoc>(); !qualified.isNull()) {
            location = qualified.getUnqualifiedLoc();
         } else if (const auto reference = location.getAs<clang::ReferenceTypeLoc>(); !reference.isNull()) {
            location = reference.getPointeeLoc();
         } else {
            return location;
         }
      }
   }

   std::string encode_location(clang::TypeLoc input) {
      const auto location = unwrap(input);
      const auto specialization = location.getAs<clang::TemplateSpecializationTypeLoc>();
      if (specialization.isNull()) {
         return encode(location.getType());
      }
      const auto* declaration = specialization.getTypePtr()->getTemplateName().getAsTemplateDecl();
      if (declaration == nullptr) {
         return encode(location.getType());
      }
      const auto type_argument = [&](std::size_t index) -> clang::TypeLoc {
         const auto& argument = specialization.getArgLoc(static_cast<unsigned>(index));
         const auto* source = argument.getTypeSourceInfo();
         return source == nullptr ? clang::TypeLoc{} : source->getTypeLoc();
      };
      if ((is_std_template(*declaration, "vector") || is_std_template(*declaration, "set") ||
           is_std_template(*declaration, "deque") || is_std_template(*declaration, "list")) &&
          specialization.getNumArgs() >= 1U) {
         if (is_std_template(*declaration, "vector") && is_byte_type(type_argument(0).getType())) {
            return "bytes";
         }
         return template_part_location(type_argument(0)) + "[]";
      }
      if (is_std_template(*declaration, "optional") && specialization.getNumArgs() >= 1U) {
         return encode_location(type_argument(0)) + '?';
      }
      if (is_std_template(*declaration, "array") && specialization.getNumArgs() >= 2U) {
         return encode_location(type_argument(0)) + '[' +
                std::to_string(integral_argument(specialization.getArgLoc(1))) + ']';
      }
      if ((is_std_template(*declaration, "pair") || is_std_template(*declaration, "map")) &&
          specialization.getNumArgs() >= 2U) {
         const auto first = type_argument(0);
         const auto second = type_argument(1);
         const auto pair = add_pair(template_part_location(first), template_part_location(second));
         return is_std_template(*declaration, "map") ? pair + "[]" : pair;
      }
      if (is_std_template(*declaration, "tuple")) {
         auto result = std::string{"tuple"};
         auto fields = std::vector<field_shape>{};
         for (auto index = std::size_t{0}; index < specialization.getNumArgs(); ++index) {
            const auto argument = type_argument(index);
            const auto part = template_part_location(argument);
            result += '_' + part;
            fields.push_back(field_shape{"field_" + std::to_string(index), part});
         }
         add_synthetic_struct(result, std::move(fields));
         return result;
      }
      if (is_std_template(*declaration, "variant")) {
         auto shape = variant_shape{.name = "variant"};
         for (auto index = std::size_t{0}; index < specialization.getNumArgs(); ++index) {
            const auto argument = type_argument(index);
            shape.name += '_' + template_part_location(argument);
            shape.types.push_back(encode_location(argument));
         }
         if (output_.variant_names.insert(shape.name).second) {
            output_.variants.push_back(shape);
         }
         return shape.name;
      }
      return encode(location.getType());
   }

   std::string template_part_location(clang::TypeLoc input) {
      const auto location = unwrap(input);
      const auto encoded = encode_location(location);
      const auto specialization = location.getAs<clang::TemplateSpecializationTypeLoc>();
      if (specialization.isNull()) {
         return encoded;
      }
      const auto* declaration = specialization.getTypePtr()->getTemplateName().getAsTemplateDecl();
      if (declaration == nullptr || is_std_template(*declaration, "basic_string")) {
         return encoded;
      }
      auto result = std::string{"B_"} + declaration->getNameAsString();
      for (auto index = std::size_t{0}; index < specialization.getNumArgs(); ++index) {
         const auto& argument = specialization.getArgLoc(static_cast<unsigned>(index));
         if (const auto* source = argument.getTypeSourceInfo(); source != nullptr) {
            result += '_' + template_part_location(source->getTypeLoc());
         } else if (argument.getArgument().getKind() == clang::TemplateArgument::Integral ||
                    argument.getArgument().getKind() == clang::TemplateArgument::Expression) {
            result += '_' + std::to_string(integral_argument(argument));
         }
      }
      result += "_E";
      add_alias(result, encoded);
      return result;
   }

   std::uint64_t integral_argument(const clang::TemplateArgumentLoc& location) const {
      const auto& argument = location.getArgument();
      if (argument.getKind() == clang::TemplateArgument::Integral) {
         return argument.getAsIntegral().getZExtValue();
      }
      if (argument.getKind() == clang::TemplateArgument::Expression) {
         auto value = clang::Expr::EvalResult{};
         if (argument.getAsExpr()->EvaluateAsInt(value, context_)) {
            return value.Val.getInt().getZExtValue();
         }
      }
      fail("non-integral template argument", location.getLocation());
      return 0;
   }

   std::string encode_builtin(const clang::BuiltinType& type) const {
      switch (type.getKind()) {
      case clang::BuiltinType::Bool:
         return "bool";
      case clang::BuiltinType::Char_S:
      case clang::BuiltinType::SChar:
         return "int8";
      case clang::BuiltinType::Char_U:
      case clang::BuiltinType::UChar:
         return "uint8";
      case clang::BuiltinType::Short:
         return "int16";
      case clang::BuiltinType::UShort:
         return "uint16";
      case clang::BuiltinType::Int:
         return "int32";
      case clang::BuiltinType::UInt:
         return "uint32";
      case clang::BuiltinType::Long:
         return context_.getTypeSize(clang::QualType{&type, 0}) == 32U ? "int32" : "int64";
      case clang::BuiltinType::LongLong:
         return "int64";
      case clang::BuiltinType::ULong:
         return context_.getTypeSize(clang::QualType{&type, 0}) == 32U ? "uint32" : "uint64";
      case clang::BuiltinType::ULongLong:
         return "uint64";
      case clang::BuiltinType::Int128:
         return "int128";
      case clang::BuiltinType::UInt128:
         return "uint128";
      case clang::BuiltinType::Float:
         return "float32";
      case clang::BuiltinType::Double:
         return "float64";
      case clang::BuiltinType::LongDouble:
         return "float128";
      default:
         break;
      }
      fail(type.getName(clang::PrintingPolicy{context_.getLangOpts()}).str(), {});
      return {};
   }

   std::string encode_record(const clang::RecordDecl& declaration) {
      const auto* record = &declaration;
      if (const auto* cpp = llvm::dyn_cast<clang::CXXRecordDecl>(record); cpp != nullptr) {
         if (const auto* definition = cpp->getDefinition(); definition != nullptr) {
            record = definition;
         }
      }
      const auto qualified = record->getQualifiedNameAsString();
      const auto* specialization = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record);
      if (specialization != nullptr && is_std_template(*specialization->getSpecializedTemplate(), "basic_string")) {
         return "string";
      }

      if (qualified == "eosio::transaction") {
         const auto* cpp = llvm::dyn_cast<clang::CXXRecordDecl>(record);
         if (cpp == nullptr || cpp->getNumBases() != 1U || !cpp->field_empty() || cpp->bases_begin()->isVirtual()) {
            fail(qualified, record->getLocation());
            return {};
         }
         return encode(cpp->bases_begin()->getType());
      }

      if (qualified == "eosio::name") {
         return "name";
      }

      if (qualified == "forge::chain::protocol::permission_level") {
         add_permission_level();
         return "permission_level";
      }

      if (qualified == "forge::chain::protocol::action") {
         add_action();
         return "action";
      }

      static const auto known = std::vector<std::pair<std::string_view, std::string_view>>{
          {"forge::chain::protocol::name", "name"},
          {"forge::contract::compatibility::name", "name"},
          {"forge::chain::protocol::symbol_code", "symbol_code"},
          {"forge::chain::protocol::symbol", "symbol"},
          {"forge::chain::protocol::asset", "asset"},
          {"forge::contract::compatibility::asset", "asset"},
          {"eosio::asset", "asset"},
          {"forge::chain::protocol::extended_symbol", "extended_symbol"},
          {"forge::chain::protocol::extended_asset", "extended_asset"},
          {"forge::contract::compatibility::extended_asset", "extended_asset"},
          {"eosio::extended_asset", "extended_asset"},
          {"forge::chain::protocol::time_point", "time_point"},
          {"forge::chain::protocol::time_point_sec", "time_point_sec"},
          {"forge::chain::protocol::block_timestamp", "block_timestamp_type"},
          {"forge::chain::protocol::float64", "float64"},
          {"forge::chain::protocol::float128", "float128"},
          {"forge::unsigned_int", "varuint32"},
          {"forge::signed_int", "varint32"},
          {"forge::crypto::digest::sha256", "checksum256"},
          {"forge::crypto::digest::sha512", "checksum512"},
          {"forge::crypto::digest::ripemd160", "checksum160"},
          {"forge::crypto::bls::public_key", "bytes"},
          {"forge::crypto::bls::signature", "bytes"},
          {"forge::crypto::bls::aggregate_signature", "bytes"},
      };
      for (const auto& [cpp_name, abi_name] : known) {
         if (qualified == cpp_name) {
            return std::string{abi_name};
         }
      }

      if (specialization != nullptr) {
         const auto template_qualified = specialization->getSpecializedTemplate()->getQualifiedNameAsString();
         const auto arguments = flatten(specialization->getTemplateArgs());
         if ((template_qualified == "forge::contract::ignore" ||
              template_qualified == "forge::contract::ignore_wrapper") &&
             !arguments.empty() && arguments[0].getKind() == clang::TemplateArgument::Type) {
            return encode(arguments[0].getAsType());
         }
         if (template_qualified == "forge::db::ids::typed_id") {
            return "uint64";
         }
         if (template_qualified == "forge::contract::binary_extension" && !arguments.empty() &&
             arguments[0].getKind() == clang::TemplateArgument::Type) {
            return encode(arguments[0].getAsType()) + '$';
         }
         if ((template_qualified == "forge::chain::protocol::fixed_key" ||
              template_qualified == "eosio::fixed_bytes") &&
             arguments.size() >= 1U && arguments[0].getKind() == clang::TemplateArgument::Integral) {
            switch (arguments[0].getAsIntegral().getZExtValue()) {
            case 20U:
               return "checksum160";
            case 32U:
               return "checksum256";
            case 64U:
               return "checksum512";
            default:
               break;
            }
         }
         if ((is_std_template(*specialization->getSpecializedTemplate(), "vector") ||
              is_std_template(*specialization->getSpecializedTemplate(), "set") ||
              is_std_template(*specialization->getSpecializedTemplate(), "deque") ||
              is_std_template(*specialization->getSpecializedTemplate(), "list")) &&
             arguments.size() >= 1U && arguments[0].getKind() == clang::TemplateArgument::Type) {
            if (is_std_template(*specialization->getSpecializedTemplate(), "vector") &&
                is_byte_type(arguments[0].getAsType())) {
               return "bytes";
            }
            return encode(arguments[0].getAsType()) + "[]";
         }
         if (is_std_template(*specialization->getSpecializedTemplate(), "optional") && arguments.size() >= 1U &&
             arguments[0].getKind() == clang::TemplateArgument::Type) {
            return encode(arguments[0].getAsType()) + '?';
         }
         if (is_std_template(*specialization->getSpecializedTemplate(), "array") && arguments.size() >= 2U &&
             arguments[0].getKind() == clang::TemplateArgument::Type &&
             arguments[1].getKind() == clang::TemplateArgument::Integral) {
            return encode(arguments[0].getAsType()) + '[' +
                   std::to_string(arguments[1].getAsIntegral().getZExtValue()) + ']';
         }
         if (is_std_template(*specialization->getSpecializedTemplate(), "pair") && arguments.size() >= 2U) {
            return add_pair(arguments[0].getAsType(), arguments[1].getAsType());
         }
         if (is_std_template(*specialization->getSpecializedTemplate(), "map") && arguments.size() >= 2U) {
            return add_pair(arguments[0].getAsType(), arguments[1].getAsType()) + "[]";
         }
         if (is_std_template(*specialization->getSpecializedTemplate(), "tuple")) {
            return add_tuple(arguments);
         }
         if (is_std_template(*specialization->getSpecializedTemplate(), "variant")) {
            return add_variant(arguments);
         }
      }

      const auto name = abi_record_name(*record);
      add_struct(*record, name);
      return name;
   }

   static std::vector<clang::TemplateArgument> flatten(const clang::TemplateArgumentList& input) {
      auto result = std::vector<clang::TemplateArgument>{};
      for (const auto& argument : input.asArray()) {
         if (argument.getKind() == clang::TemplateArgument::Pack) {
            result.insert(result.end(), argument.pack_begin(), argument.pack_end());
         } else {
            result.push_back(argument);
         }
      }
      return result;
   }

   std::string abi_record_name(const clang::RecordDecl& declaration) {
      const auto* specialization = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(&declaration);
      if (specialization == nullptr) {
         return record_name(declaration);
      }

      auto result = specialization->getSpecializedTemplate()->getNameAsString();
      for (const auto& argument : flatten(specialization->getTemplateArgs())) {
         if (argument.getKind() == clang::TemplateArgument::Integral) {
            result += '_' + std::to_string(argument.getAsIntegral().getZExtValue());
         } else if (argument.getKind() == clang::TemplateArgument::Type) {
            result += '_' + template_part(argument.getAsType());
         }
      }
      return result;
   }

   std::string template_part(clang::QualType input) {
      auto type = input.getNonReferenceType().getUnqualifiedType();
      if (const auto* alias = llvm::dyn_cast_or_null<clang::TypedefType>(type.getTypePtrOrNull())) {
         if (!context_.getSourceManager().isInSystemHeader(alias->getDecl()->getLocation())) {
            return encode(type);
         }
      }

      const auto encoded = encode(type);
      const auto* record = type.getDesugaredType(context_)->getAs<clang::RecordType>();
      const auto* specialization =
          record == nullptr ? nullptr : llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record->getDecl());
      if (specialization == nullptr) {
         return encoded;
      }
      const auto* template_declaration = specialization->getSpecializedTemplate();
      const auto name = template_declaration->getNameAsString();
      if (is_std_template(*template_declaration, "basic_string")) {
         return encoded;
      }

      auto result = std::string{"B_"} + name;
      const auto arguments = flatten(specialization->getTemplateArgs());
      const auto semantic_arguments =
          is_std_template(*template_declaration, "map") || is_std_template(*template_declaration, "pair")
              ? std::min<std::size_t>(2U, arguments.size())
          : is_std_template(*template_declaration, "array") ? std::min<std::size_t>(2U, arguments.size())
                                                            : arguments.size();
      for (auto index = std::size_t{0}; index < semantic_arguments; ++index) {
         const auto& argument = arguments[index];
         if (argument.getKind() == clang::TemplateArgument::Type) {
            result += '_' + template_part(argument.getAsType());
         } else if (argument.getKind() == clang::TemplateArgument::Integral) {
            result += '_' + std::to_string(argument.getAsIntegral().getZExtValue());
         }
      }
      result += "_E";
      add_alias(result, encoded);
      return result;
   }

   void add_alias(const std::string& name, const std::string& target, clang::SourceLocation location = {},
                  std::string identity = {}) {
      if (name.empty() || name == target) {
         return;
      }
      const auto [existing, inserted] = output_.type_declarations.try_emplace(name, type_declaration{target, identity});
      if (inserted) {
         output_.types.push_back(type_shape{name, target});
         return;
      }
      if (existing->second.type != target ||
          (!existing->second.identity.empty() && !identity.empty() && existing->second.identity != identity)) {
         const auto id = context_.getDiagnostics().getCustomDiagID(clang::DiagnosticsEngine::Error,
                                                                   "conflicting contract ABI type alias '%0'");
         context_.getDiagnostics().Report(location, id) << name;
         output_.failed = true;
      }
   }

   std::string add_pair(clang::QualType first, clang::QualType second) {
      return add_pair(template_part(first), template_part(second));
   }

   std::string add_pair(std::string first_name, std::string second_name) {
      const auto name = "pair_" + first_name + '_' + second_name;
      add_synthetic_struct(name, {{"first", std::move(first_name)}, {"second", std::move(second_name)}});
      return name;
   }

   void add_permission_level() {
      add_synthetic_struct("permission_level", {{"actor", "name"}, {"permission", "name"}});
   }

   void add_action() {
      add_permission_level();
      add_synthetic_struct(
          "action",
          {{"account", "name"}, {"name", "name"}, {"authorization", "permission_level[]"}, {"data", "bytes"}});
   }

   void add_extension() {
      add_synthetic_struct("extension", {{"type", "uint16"}, {"data", "bytes"}});
   }

   std::string add_tuple(const std::vector<clang::TemplateArgument>& arguments) {
      auto name = std::string{"tuple"};
      auto fields = std::vector<field_shape>{};
      for (auto index = std::size_t{0}; index < arguments.size(); ++index) {
         if (arguments[index].getKind() != clang::TemplateArgument::Type) {
            continue;
         }
         const auto part = template_part(arguments[index].getAsType());
         name += '_' + part;
         fields.push_back(field_shape{"field_" + std::to_string(index), part});
      }
      add_synthetic_struct(name, std::move(fields));
      return name;
   }

   std::string add_variant(const std::vector<clang::TemplateArgument>& arguments) {
      auto shape = variant_shape{.name = "variant"};
      for (const auto& argument : arguments) {
         if (argument.getKind() != clang::TemplateArgument::Type) {
            continue;
         }
         shape.name += '_' + template_part(argument.getAsType());
         shape.types.push_back(encode(argument.getAsType()));
      }
      if (output_.variant_names.insert(shape.name).second) {
         output_.variants.push_back(shape);
      }
      return shape.name;
   }

   void add_synthetic_struct(std::string name, std::vector<field_shape> fields) {
      if (!claim_struct(output_, context_, name, "synthetic:" + name)) {
         return;
      }
      output_.structs.push_back(struct_shape{.name = std::move(name), .fields = std::move(fields)});
   }

   void add_struct(const clang::RecordDecl& declaration, const std::string& name) {
      const auto* record = &declaration;
      if (const auto* cpp = llvm::dyn_cast<clang::CXXRecordDecl>(record); cpp != nullptr) {
         if (const auto* definition = cpp->getDefinition(); definition != nullptr) {
            record = definition;
         }
      }
      if (record->getOwningModule() == nullptr && !has_nameable_scope(*record)) {
         auto& diagnostics = context_.getDiagnostics();
         const auto id = diagnostics.getCustomDiagID(
             clang::DiagnosticsEngine::Error, "contract ABI record '%0' is declared in an anonymous or local scope");
         diagnostics.Report(declaration.getLocation(), id) << record_name(*record);
         output_.failed = true;
         return;
      }
      const auto generate_codec = has_publicly_accessible_name(*record) && record->getOwningModule() == nullptr;
      const auto codec_name = generate_codec ? "::" + record->getQualifiedNameAsString() : std::string{};
      const auto identity = record_layout_identity(*record);
      const auto [claimed, inserted] = output_.struct_declarations.try_emplace(name, identity);
      if (!inserted && claimed->second == identity) {
         if (!generate_codec) {
            return;
         }
         const auto source_inserted = source_record_codecs_.insert(codec_name).second;
         const auto codec_exists =
             std::ranges::any_of(output_.record_codecs, [&](const auto& codec) { return codec.name == codec_name; });
         if (!source_inserted || codec_exists) {
            return;
         }
      }
      const auto duplicate = !inserted;

      auto shape = struct_shape{.name = name};
      auto codec = record_codec_shape{};
      if (record->isUnion()) {
         fail("union record", declaration.getLocation());
         return;
      }
      if (generate_codec) {
         codec.name = codec_name;
         codec.forward_declaration = make_forward_declaration(*record);
         source_record_codecs_.insert(codec.name);
      }
      if (const auto* cpp = llvm::dyn_cast<clang::CXXRecordDecl>(record); cpp != nullptr && cpp->hasDefinition()) {
         if (cpp->getNumBases() > 1U) {
            fail("multiple ABI base classes", declaration.getLocation());
         } else if (cpp->getNumBases() == 1U) {
            const auto& base = *cpp->bases_begin();
            const auto implicit_public = base.getAccessSpecifier() == clang::AS_none && cpp->isStruct();
            if (base.isVirtual() || (base.getAccessSpecifier() != clang::AS_public && !implicit_public)) {
               fail("non-public or virtual ABI base class", base.getBeginLoc());
               return;
            }
            const auto* base_record = base.getType()->getAsCXXRecordDecl();
            if (base_record == nullptr) {
               fail("ABI base class", base.getBeginLoc());
               return;
            }
            shape.base = encode(base.getType());
            if (generate_codec) {
               codec.base = "::" + base_record->getQualifiedNameAsString();
            }
         }
      }
      for (const auto* field : record->fields()) {
         if (field->getIdentifier() == nullptr || field->isBitField() || field->getType().isConstQualified() ||
             field->getType()->isReferenceType() ||
             (field->getAccess() != clang::AS_public && field->getAccess() != clang::AS_none)) {
            fail("non-public, unnamed, const, reference, or bit-field ABI member", field->getLocation());
            return;
         }
         shape.fields.push_back(field_shape{field->getNameAsString(), encode(*field)});
         if (generate_codec) {
            codec.fields.push_back(field->getNameAsString());
         }
      }
      if (duplicate) {
         const auto existing = std::ranges::find(output_.structs, name, &struct_shape::name);
         if (existing == output_.structs.end() || *existing != shape) {
            const auto id = context_.getDiagnostics().getCustomDiagID(clang::DiagnosticsEngine::Error,
                                                                      "duplicate contract ABI struct name '%0'");
            context_.getDiagnostics().Report(declaration.getLocation(), id) << name;
            output_.failed = true;
            return;
         }
      } else {
         output_.structs.push_back(std::move(shape));
      }
      if (generate_codec) {
         output_.record_codecs.push_back(std::move(codec));
      }
   }

   bool is_typed_id(clang::QualType type) const {
      const auto desugared = type.getDesugaredType(context_).getUnqualifiedType();
      const auto* record = desugared->getAs<clang::RecordType>();
      const auto* specialization =
          record == nullptr ? nullptr : llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record->getDecl());
      return specialization != nullptr &&
             specialization->getSpecializedTemplate()->getQualifiedNameAsString() == "forge::db::ids::typed_id";
   }

   void fail(std::string_view type, clang::SourceLocation location) const {
      auto& diagnostics = context_.getDiagnostics();
      const auto id =
          diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Error, "unsupported contract ABI type '%0'");
      diagnostics.Report(location, id) << type;
      output_.failed = true;
   }

   clang::ASTContext& context_;
   schema& output_;
   std::set<std::string>& source_record_codecs_;
};

class visitor final : public clang::RecursiveASTVisitor<visitor> {
 public:
   visitor(clang::ASTContext& context, clang::Sema& sema, schema& output, std::set<std::string>& source_record_codecs,
           std::string_view contract_name, std::filesystem::path source)
       : context_(context), sema_(sema), output_(output), encoder_(context, output, source_record_codecs),
         contract_name_(contract_name), source_(std::move(source)) {}

   bool VisitCXXRecordDecl(clang::CXXRecordDecl* declaration) {
      if (!declaration->isThisDeclarationADefinition() || declaration->isImplicit()) {
         return true;
      }
      if (const auto table = annotation(*declaration, "forge.table"); table.has_value()) {
         const auto owner = owning_contract(*declaration);
         const auto selected = owner.has_value() && *owner == contract_name_;
         if (selected || (!owner.has_value() && !table->empty())) {
            auto table_name = *table;
            if (!table->empty()) {
               const auto typed_name = typed_table_name(*declaration, declaration->getLocation());
               if (typed_name.has_value() && protocol::make_name(*table).value != *typed_name) {
                  report(declaration->getLocation(), "table attribute name does not match row get_table_name()");
                  return true;
               }
               if (typed_name.has_value()) {
                  table_name = protocol::to_string(protocol::table_name{*typed_name});
               }
            }
            encoder_.add_table(*declaration, table_name, table->empty());
         }
         return true;
      }
      if (const auto action = annotation(*declaration, "forge.action");
          action.has_value() && belongs_to_selected_contract(*declaration)) {
         add_record_action(*declaration, *action);
      }
      const auto contract = annotation(*declaration, "forge.contract");
      if (!contract.has_value()) {
         return true;
      }
      const auto declared_name = contract->empty() ? declaration->getNameAsString() : *contract;
      if (declared_name != contract_name_) {
         return true;
      }

      found_contract_ = true;
      for (const auto* method : declaration->methods()) {
         const auto action = annotation(*method, "forge.action");
         if (action.has_value()) {
            add_action(*declaration, *method, *action);
         }
         const auto call = annotation(*method, "forge.call");
         if (call.has_value()) {
            add_call(*method, *call);
         }
         const auto notification = annotation(*method, "forge.on_notify");
         if (notification.has_value()) {
            add_notification(*declaration, *method, *notification);
         }
      }
      return true;
   }

   bool VisitCXXMethodDecl(clang::CXXMethodDecl* declaration) {
      if (!belongs_to_selected_contract(*declaration)) {
         return true;
      }
      found_contract_ = true;
      if (const auto action = annotation(*declaration, "forge.action"); action.has_value()) {
         add_action(*declaration->getParent(), *declaration, *action);
      }
      if (const auto call = annotation(*declaration, "forge.call"); call.has_value()) {
         add_call(*declaration, *call);
      }
      if (const auto notification = annotation(*declaration, "forge.on_notify"); notification.has_value()) {
         add_notification(*declaration->getParent(), *declaration, *notification);
      }
      return true;
   }

   bool VisitFunctionDecl(clang::FunctionDecl* declaration) {
      if (declaration->isThisDeclarationADefinition() && has_annotation(*declaration, "forge.eosio_dispatch")) {
         output_.has_eosio_dispatch = true;
         return true;
      }
      if (!declaration->isThisDeclarationADefinition() || !is_global_function(*declaration) ||
          declaration->getIdentifier() == nullptr || declaration->getIdentifier()->getName() != "apply") {
         return true;
      }
      if (!declaration->isExternC() || !declaration->hasExternalFormalLinkage()) {
         return true;
      }
      if (!declaration->getReturnType()->isVoidType() || declaration->getNumParams() != 3U) {
         report(declaration->getLocation(), "contract apply entry point must be void(uint64_t, uint64_t, uint64_t)");
         return true;
      }
      for (const auto* parameter : declaration->parameters()) {
         if (!parameter->getType()->isIntegerType() || context_.getTypeSize(parameter->getType()) != 64U) {
            report(declaration->getLocation(), "contract apply entry point must be void(uint64_t, uint64_t, uint64_t)");
            return true;
         }
      }
      found_contract_ = true;
      output_.has_apply = true;
      output_.has_eosio_dispatch = output_.has_eosio_dispatch || has_annotation(*declaration, "forge.eosio_dispatch");
      return true;
   }

   bool VisitClassTemplateSpecializationDecl(clang::ClassTemplateSpecializationDecl* declaration) {
      const auto* record = specialization_record(*declaration);
      if (record == nullptr) {
         return true;
      }
      const auto typed_name = typed_table_name(*record, declaration->getLocation());
      if (belongs_to_selected_contract(*record) || typed_name.has_value()) {
         add_table(*declaration, typed_name);
      }
      return true;
   }

   bool VisitTypedefNameDecl(clang::TypedefNameDecl* declaration) {
      const auto* specialization = specialization_type(declaration->getUnderlyingType());
      const auto* record = specialization == nullptr ? nullptr : specialization_record(*specialization);
      const auto owned_record = record != nullptr && belongs_to_selected_contract(*record);
      const auto typed_name =
          record == nullptr ? std::optional<std::uint64_t>{} : typed_table_name(*record, declaration->getLocation());
      if (specialization != nullptr &&
          (belongs_to_selected_contract(*declaration) || owned_record || typed_name.has_value())) {
         add_table(*specialization, typed_name);
      }
      return true;
   }

   bool found_contract() const {
      return found_contract_;
   }

 private:
   const clang::ClassTemplateSpecializationDecl* specialization_type(clang::QualType type) const {
      const auto* record = type.getDesugaredType(context_).getTypePtrOrNull();
      const auto* record_type = record == nullptr ? nullptr : record->getAs<clang::RecordType>();
      return record_type == nullptr ? nullptr
                                    : llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record_type->getDecl());
   }

   static const clang::CXXRecordDecl* specialization_record(const clang::ClassTemplateSpecializationDecl& declaration) {
      const auto template_name = declaration.getSpecializedTemplate()->getNameAsString();
      if (template_name != "multi_index" && template_name != "singleton") {
         return nullptr;
      }
      const auto& arguments = declaration.getTemplateArgs();
      if (arguments.size() < 2U || arguments[0].getKind() != clang::TemplateArgument::Integral ||
          arguments[1].getKind() != clang::TemplateArgument::Type) {
         return nullptr;
      }
      const auto* record = arguments[1].getAsType()->getAsCXXRecordDecl();
      return record == nullptr ? nullptr : record->getDefinition();
   }

   std::optional<std::string> owning_contract(const clang::Decl& declaration) const {
      if (const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(&declaration); record != nullptr) {
         const auto contract = annotation(*record, "forge.contract");
         if (contract.has_value()) {
            return contract->empty() ? record->getNameAsString() : *contract;
         }
      }
      for (auto* context = declaration.getDeclContext(); context != nullptr; context = context->getParent()) {
         const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(context);
         if (record == nullptr) {
            continue;
         }
         const auto contract = annotation(*record, "forge.contract");
         if (contract.has_value()) {
            return contract->empty() ? record->getNameAsString() : *contract;
         }
      }
      return std::nullopt;
   }

   bool belongs_to_selected_contract(const clang::Decl& declaration) const {
      const auto owner = owning_contract(declaration);
      return owner.has_value() && *owner == contract_name_;
   }

   std::optional<std::uint64_t> typed_table_name(const clang::CXXRecordDecl& record,
                                                 clang::SourceLocation use_location) {
      return named_protocol_value(
          record, "get_table_name", use_location, "typed table get_table_name() must have a constant table name",
          "typed table get_table_name() must be a public static constexpr zero-argument function returning table_name");
   }

   void add_table(const clang::ClassTemplateSpecializationDecl& declaration,
                  std::optional<std::uint64_t> typed_name = std::nullopt) {
      const auto* record = specialization_record(declaration);
      if (record == nullptr) {
         return;
      }
      const auto value = declaration.getTemplateArgs()[0].getAsIntegral().getZExtValue();
      if (typed_name.has_value() && *typed_name != value) {
         report(declaration.getLocation(), "multi_index or singleton name does not match row get_table_name()");
         return;
      }
      encoder_.add_table(*record, protocol::to_string(protocol::name{value}));
   }

   struct method_shape {
      std::string name;
      std::string type;
      std::string result;
   };

   std::optional<std::uint64_t> named_protocol_value(const clang::CXXRecordDecl& record, const char* method_name,
                                                     clang::SourceLocation use_location, const char* constant_error,
                                                     const char* declaration_error = nullptr) {
      auto lookup = clang::LookupResult{
          sema_,
          context_.DeclarationNames.getIdentifier(&context_.Idents.get(method_name)),
          use_location,
          clang::Sema::LookupOrdinaryName,
      };
      lookup.suppressDiagnostics();
      if (!sema_.LookupQualifiedName(lookup, const_cast<clang::CXXRecordDecl*>(&record))) {
         return std::nullopt;
      }
      if (lookup.isAmbiguous()) {
         if (declaration_error != nullptr) {
            report(use_location, declaration_error);
         }
         return std::nullopt;
      }

      const clang::CXXMethodDecl* named_method = nullptr;
      for (auto declaration = lookup.begin(); declaration != lookup.end(); ++declaration) {
         const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>((*declaration)->getUnderlyingDecl());
         if (method == nullptr) {
            continue;
         }
         const auto* result_record = method->getReturnType()->getAsCXXRecordDecl();
         if (!method->isStatic() || !method->isConstexpr() || method->getNumParams() != 0U ||
             declaration.getAccess() != clang::AS_public || method->isDeleted() || result_record == nullptr ||
             result_record->getCanonicalDecl()->getQualifiedNameAsString() != "forge::chain::protocol::name") {
            continue;
         }
         if (named_method != nullptr) {
            if (declaration_error != nullptr) {
               report(use_location, declaration_error);
            }
            return std::nullopt;
         }
         named_method = method;
      }
      if (named_method == nullptr) {
         if (declaration_error != nullptr) {
            report(use_location, declaration_error);
         }
         return std::nullopt;
      }

      const auto location = named_method->getLocation();
      auto* callee = sema_.BuildDeclRefExpr(const_cast<clang::CXXMethodDecl*>(named_method), named_method->getType(),
                                            clang::VK_LValue, location);
      auto call = sema_.BuildCallExpr(nullptr, callee, location, {}, location);
      auto result = clang::Expr::EvalResult{};
      if (call.isInvalid() || !call.get()->EvaluateAsConstantExpr(result, context_) || !result.Val.isStruct() ||
          result.Val.getStructNumFields() != 1U || !result.Val.getStructField(0).isInt()) {
         report(location, constant_error);
         return std::nullopt;
      }
      return result.Val.getStructField(0).getInt().getZExtValue();
   }

   std::optional<std::uint64_t> named_action_value(const clang::ParmVarDecl& parameter) {
      const auto* record = parameter.getType().getNonReferenceType()->getAsCXXRecordDecl();
      record = record == nullptr ? nullptr : record->getDefinition();
      if (record == nullptr) {
         return std::nullopt;
      }
      return named_protocol_value(*record, "get_name", parameter.getLocation(),
                                  "typed action get_name() must have a constant action name");
   }

   void register_method_types(const clang::CXXMethodDecl& method) {
      if (!method.getReturnType()->isVoidType()) {
         static_cast<void>(encoder_.encode(method.getReturnType()));
      }
      for (const auto* parameter : method.parameters()) {
         static_cast<void>(encoder_.encode(*parameter));
      }
   }

   method_shape add_method(const clang::CXXMethodDecl& method, std::string_view annotated_name,
                           bool allow_unnamed_parameters) {
      if (method.isStatic()) {
         report(method.getLocation(), "contract entry point must be a non-static member function");
      }
      auto shape = method_shape{};
      shape.name = annotated_name.empty() ? method.getNameAsString() : std::string{annotated_name};
      shape.type = method.getNameAsString();
      if (!method.getReturnType()->isVoidType()) {
         shape.result = encoder_.encode(method.getReturnType());
      }

      auto arguments = struct_shape{.name = shape.type};
      for (std::size_t index = 0; index < method.getNumParams(); ++index) {
         const auto* parameter = method.getParamDecl(index);
         auto name = parameter->getNameAsString();
         if (name.empty() && !allow_unnamed_parameters) {
            report(parameter->getLocation(), "contract entry point parameters must be named");
            continue;
         }
         arguments.fields.push_back(field_shape{name, encoder_.encode(*parameter)});
      }
      if (claim_struct(output_, context_, arguments.name, "method:" + declaration_identity(method),
                       method.getLocation())) {
         output_.structs.push_back(std::move(arguments));
      }
      return shape;
   }

   void add_action(const clang::CXXRecordDecl& declaration, const clang::CXXMethodDecl& method,
                   std::string_view annotated_name) {
      if (method.getCanonicalDecl()->getStorageClass() == clang::SC_Static) {
         report(method.getLocation(), "contract entry point must be a non-static member function");
         return;
      }
      register_method_types(method);
      const auto identity = declaration_identity(method);
      auto action_name = annotated_name.empty() ? method.getNameAsString() : std::string{annotated_name};
      auto action_type = method.getNameAsString();
      const auto* named_parameter = method.getNumParams() == 1U ? method.getParamDecl(0) : nullptr;
      const auto named_value =
          named_parameter == nullptr ? std::optional<std::uint64_t>{} : named_action_value(*named_parameter);
      if (named_value.has_value()) {
         const auto canonical_name = protocol::to_string(protocol::action_name{*named_value});
         if (!annotated_name.empty() && protocol::make_name(annotated_name).value != *named_value) {
            report(method.getLocation(), "action attribute name does not match payload get_name()");
            return;
         }
         action_name = canonical_name;
         action_type = encoder_.encode(*named_parameter);
      }
      const auto [existing, inserted] = output_.action_declarations.try_emplace(action_name, identity);
      if (!inserted && existing->second == identity) {
         return;
      }
      if (!inserted) {
         report(method.getLocation(), "duplicate contract action name");
         return;
      }
      const auto dispatch_name = declaration.getQualifiedNameAsString() + "::" + method.getNameAsString();
      const auto [dispatch, dispatch_inserted] = output_.action_methods.try_emplace(dispatch_name, identity);
      if (!dispatch_inserted && dispatch->second != identity) {
         report(method.getLocation(), "overloaded contract action methods are not supported");
         return;
      }
      const auto method_info = named_value.has_value()
                                   ? method_shape{
                                         .name = action_name,
                                         .type = action_type,
                                         .result = method.getReturnType()->isVoidType()
                                                       ? std::string{}
                                                       : encoder_.encode(method.getReturnType()),
                                     }
                                   : add_method(method, annotated_name,
                                                has_annotation(method, "forge.attribute_scope:action:eosio"));
      output_.actions.push_back(action_shape{
          .name = method_info.name,
          .type = method_info.type,
          .result = method_info.result,
          .class_name = declaration.getQualifiedNameAsString(),
          .method_name = method.getNameAsString(),
          .source = source_,
      });
   }

   void add_record_action(const clang::CXXRecordDecl& declaration, std::string_view annotated_name) {
      const auto identity = declaration_identity(declaration);
      const auto action_name = annotated_name.empty() ? declaration.getNameAsString() : std::string{annotated_name};
      const auto [existing, inserted] = output_.action_declarations.try_emplace(action_name, identity);
      if (!inserted && existing->second == identity) {
         return;
      }
      if (!inserted) {
         report(declaration.getLocation(), "duplicate contract action name");
         return;
      }
      output_.actions.push_back(action_shape{
          .name = action_name,
          .type =
              encoder_.encode(context_.getTypeDeclType(clang::ElaboratedTypeKeyword::None, std::nullopt, &declaration)),
          .dispatchable = false,
      });
   }

   void add_call(const clang::CXXMethodDecl& method, std::string_view annotated_name) {
      register_method_types(method);
      const auto identity = declaration_identity(method);
      const auto call_name = annotated_name.empty() ? method.getNameAsString() : std::string{annotated_name};
      const auto [existing, inserted] = output_.call_declarations.try_emplace(call_name, identity);
      if (!inserted && existing->second == identity) {
         return;
      }
      if (!inserted) {
         report(method.getLocation(), "duplicate synchronous call name");
         return;
      }
      const auto method_info =
          add_method(method, annotated_name, has_annotation(method, "forge.attribute_scope:call:eosio"));
      auto identifier = std::uint64_t{5381U};
      for (const auto value : method_info.name) {
         identifier = identifier * 33U + static_cast<unsigned char>(value);
      }
      output_.calls.push_back(call_shape{
          .name = method_info.name,
          .type = method_info.type,
          .result = method_info.result,
          .id = identifier,
          .class_name = method.getParent()->getQualifiedNameAsString(),
          .method_name = method.getNameAsString(),
          .source = source_,
      });
   }

   void add_notification(const clang::CXXRecordDecl& declaration, const clang::CXXMethodDecl& method,
                         std::string_view filter) {
      register_method_types(method);
      const auto separator = filter.find("::");
      if (separator == std::string_view::npos || separator == 0U || separator + 2U == filter.size() ||
          filter.find("::", separator + 2U) != std::string_view::npos) {
         report(method.getLocation(), "notification filter must be 'account::action' or '*::action'");
         return;
      }
      const auto code_name = filter.substr(0U, separator);
      const auto action_name = filter.substr(separator + 2U);
      auto code = std::uint64_t{};
      auto action = std::uint64_t{};
      try {
         if (code_name != "*") {
            code = protocol::make_name(code_name).value;
         }
         action = protocol::make_name(action_name).value;
      } catch (const std::exception&) {
         report(method.getLocation(), "notification filter contains an invalid Antelope name");
         return;
      }

      const auto identity = declaration_identity(method);
      const auto key = std::to_string(code) + ':' + std::to_string(action);
      const auto [existing, inserted] = output_.notification_declarations.try_emplace(key, identity);
      if (!inserted && existing->second != identity) {
         report(method.getLocation(), "duplicate contract notification route");
         return;
      }
      if (!inserted) {
         return;
      }
      output_.notifications.push_back(notification_shape{
          .code = code,
          .action = action,
          .class_name = declaration.getQualifiedNameAsString(),
          .method_name = method.getNameAsString(),
          .source = source_,
      });
   }

   void report(clang::SourceLocation location, const char* message) {
      const auto id = context_.getDiagnostics().getCustomDiagID(clang::DiagnosticsEngine::Error, "%0");
      context_.getDiagnostics().Report(location, id) << message;
      output_.failed = true;
   }

   clang::ASTContext& context_;
   clang::Sema& sema_;
   schema& output_;
   type_encoder encoder_;
   std::string contract_name_;
   std::filesystem::path source_;
   bool found_contract_ = false;
};

class consumer final : public clang::ASTConsumer {
 public:
   consumer(clang::CompilerInstance& compiler, schema& output, std::set<std::string>& source_record_codecs, bool& found,
            bool& dispatch_source_found, std::string_view contract_name, std::filesystem::path source,
            bool is_dispatch_source)
       : compiler_(compiler), output_(output), source_record_codecs_(source_record_codecs), found_(found),
         dispatch_source_found_(dispatch_source_found), contract_name_(contract_name), source_(std::move(source)),
         is_dispatch_source_(is_dispatch_source) {}

   void HandleTranslationUnit(clang::ASTContext& context) override {
      auto contract_visitor =
          visitor{context, compiler_.getSema(), output_, source_record_codecs_, contract_name_, source_};
      contract_visitor.TraverseDecl(context.getTranslationUnitDecl());
      if (is_dispatch_source_) {
         auto encoder = type_encoder{context, output_, source_record_codecs_};
         for (const auto& name : output_.requested_roots) {
            const auto* declaration = find_root(context, name);
            if (declaration == nullptr) {
               const auto id = context.getDiagnostics().getCustomDiagID(
                   clang::DiagnosticsEngine::Error, "unknown or ambiguous contract ABI root type '%0'");
               context.getDiagnostics().Report(id) << name;
               output_.failed = true;
               continue;
            }
            auto type = encoder.encode(context.getTypeDeclType(declaration));
            if (std::ranges::any_of(output_.metadata.roots, [&](const auto& root) { return root.type == type; })) {
               const auto id = context.getDiagnostics().getCustomDiagID(
                   clang::DiagnosticsEngine::Error, "duplicate contract ABI root type mapping '%0' to '%1'");
               context.getDiagnostics().Report(id) << name << type;
               output_.failed = true;
               continue;
            }
            output_.metadata.roots.push_back({name, std::move(type)});
         }
      }
      const auto found = contract_visitor.found_contract();
      found_ = found_ || found;
      dispatch_source_found_ = dispatch_source_found_ || (is_dispatch_source_ && found);
   }

 private:
   const clang::TypeDecl* find_root(clang::ASTContext& context, std::string_view name) {
      auto* scope = static_cast<clang::DeclContext*>(context.getTranslationUnitDecl());
      auto remaining = name;
      if (remaining.starts_with("::")) {
         remaining.remove_prefix(2U);
      }
      while (!remaining.empty()) {
         const auto separator = remaining.find("::");
         const auto part = remaining.substr(0U, separator);
         if (part.empty() || !std::ranges::all_of(part, [](unsigned char character) {
                return llvm::isAlnum(character) || character == '_';
             }) || llvm::isDigit(static_cast<unsigned char>(part.front()))) {
            return nullptr;
         }
         auto lookup = clang::LookupResult{
             compiler_.getSema(), context.DeclarationNames.getIdentifier(&context.Idents.get(part)),
             clang::SourceLocation{}, clang::Sema::LookupOrdinaryName};
         lookup.suppressDiagnostics();
         if (!compiler_.getSema().LookupQualifiedName(lookup, scope) || !lookup.isSingleResult()) {
            return nullptr;
         }
         auto* declaration = lookup.getFoundDecl()->getUnderlyingDecl();
         if (separator == std::string_view::npos) {
            return llvm::dyn_cast<clang::TypeDecl>(declaration);
         }
         if (auto* alias = llvm::dyn_cast<clang::NamespaceAliasDecl>(declaration)) {
            scope = alias->getNamespace();
         } else if (auto* next_scope = llvm::dyn_cast<clang::DeclContext>(declaration)) {
            scope = next_scope;
         } else {
            return nullptr;
         }
         remaining.remove_prefix(separator + 2U);
      }
      return nullptr;
   }

   clang::CompilerInstance& compiler_;
   schema& output_;
   std::set<std::string>& source_record_codecs_;
   bool& found_;
   bool& dispatch_source_found_;
   std::string contract_name_;
   std::filesystem::path source_;
   bool is_dispatch_source_ = false;
};

using source_dependencies = std::map<std::filesystem::path, bool>;
using module_dependencies = std::set<std::string>;

void collect_module_dependencies(clang::ASTContext& context, module_dependencies& imported,
                                 module_dependencies* exported = nullptr, module_dependencies* provided = nullptr) {
   for (const auto* declaration : context.local_imports()) {
      if (const auto* module = declaration->getImportedModule(); module != nullptr) {
         imported.insert(module->getFullModuleName());
      }
   }
   const auto* current = context.getCurrentNamedModule();
   if (current == nullptr) {
      return;
   }
   if (provided != nullptr) {
      provided->insert(current->getFullModuleName());
   }
   if (exported != nullptr) {
      auto modules = llvm::SmallVector<clang::Module*, 4>{};
      current->getExportedModules(modules);
      for (const auto* module : modules) {
         exported->insert(module->getFullModuleName());
      }
   }
}

void collect_source_dependencies(clang::CompilerInstance& compiler, source_dependencies& dependencies) {
   const auto& source_manager = compiler.getSourceManager();
   auto files = std::vector<clang::FileEntryRef>{};
   files.reserve(std::distance(source_manager.fileinfo_begin(), source_manager.fileinfo_end()));
   for (auto iterator = source_manager.fileinfo_begin(); iterator != source_manager.fileinfo_end(); ++iterator) {
      files.push_back(iterator->first);
   }
   for (const auto& file : files) {
      const auto path = std::filesystem::path{file.getName().str()};
      if (path.empty() || !std::filesystem::exists(path)) {
         continue;
      }
      const auto file_id = source_manager.translateFile(file);
      const auto system =
          file_id.isValid() && source_manager.isInSystemHeader(source_manager.getLocForStartOfFile(file_id));
      const auto canonical = std::filesystem::weakly_canonical(path);
      const auto [entry, inserted] = dependencies.emplace(canonical, system);
      if (!inserted) {
         entry->second = entry->second && system;
      }
   }
}

class action final : public clang::ASTFrontendAction {
 public:
   action(schema& output, bool& found, bool& dispatch_source_found, std::string_view contract_name,
          std::filesystem::path dispatch_source, source_dependencies& dependencies,
          module_dependencies& imported_modules)
       : output_(output), found_(found), dispatch_source_found_(dispatch_source_found), contract_name_(contract_name),
         dispatch_source_(std::move(dispatch_source)), dependencies_(dependencies),
         imported_modules_(imported_modules) {}

   std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance& compiler,
                                                         llvm::StringRef input_file) override {
      const auto input = std::filesystem::weakly_canonical(std::filesystem::path{input_file.str()});
      class combined_consumer final : public clang::ASTConsumer {
       public:
         combined_consumer(std::unique_ptr<clang::ASTConsumer> wrapped, module_dependencies& imported)
             : wrapped_{std::move(wrapped)}, imported_{imported} {}

         void HandleTranslationUnit(clang::ASTContext& context) override {
            collect_module_dependencies(context, imported_);
            wrapped_->HandleTranslationUnit(context);
         }

       private:
         std::unique_ptr<clang::ASTConsumer> wrapped_;
         module_dependencies& imported_;
      };
      return std::make_unique<combined_consumer>(
          std::make_unique<consumer>(compiler, output_, output_.source_record_codecs[input], found_,
                                     dispatch_source_found_, contract_name_, input, input == dispatch_source_),
          imported_modules_);
   }

   void EndSourceFileAction() override {
      collect_source_dependencies(getCompilerInstance(), dependencies_);
   }

 private:
   schema& output_;
   bool& found_;
   bool& dispatch_source_found_;
   std::string contract_name_;
   std::filesystem::path dispatch_source_;
   source_dependencies& dependencies_;
   module_dependencies& imported_modules_;
};

class action_factory final : public clang::tooling::FrontendActionFactory {
 public:
   action_factory(schema& output, bool& found, bool& dispatch_source_found, std::string_view contract_name,
                  std::filesystem::path dispatch_source, source_dependencies& dependencies,
                  module_dependencies& imported_modules)
       : output_(output), found_(found), dispatch_source_found_(dispatch_source_found), contract_name_(contract_name),
         dispatch_source_(std::move(dispatch_source)), dependencies_(dependencies),
         imported_modules_(imported_modules) {}

   std::unique_ptr<clang::FrontendAction> create() override {
      return std::make_unique<action>(output_, found_, dispatch_source_found_, contract_name_, dispatch_source_,
                                      dependencies_, imported_modules_);
   }

 private:
   schema& output_;
   bool& found_;
   bool& dispatch_source_found_;
   std::string contract_name_;
   std::filesystem::path dispatch_source_;
   source_dependencies& dependencies_;
   module_dependencies& imported_modules_;
};

class dependency_consumer final : public clang::ASTConsumer {
 public:
   explicit dependency_consumer(module_dependencies& imported) : imported_{imported} {}

   void HandleTranslationUnit(clang::ASTContext& context) override {
      collect_module_dependencies(context, imported_);
   }

 private:
   module_dependencies& imported_;
};

class dependency_action final : public clang::SyntaxOnlyAction {
 public:
   dependency_action(source_dependencies& dependencies, module_dependencies& imported)
       : dependencies_{dependencies}, imported_{imported} {}

   std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance&, llvm::StringRef) override {
      return std::make_unique<dependency_consumer>(imported_);
   }

   void EndSourceFileAction() override {
      collect_source_dependencies(getCompilerInstance(), dependencies_);
   }

 private:
   source_dependencies& dependencies_;
   module_dependencies& imported_;
};

class dependency_action_factory final : public clang::tooling::FrontendActionFactory {
 public:
   dependency_action_factory(source_dependencies& dependencies, module_dependencies& imported)
       : dependencies_{dependencies}, imported_{imported} {}

   std::unique_ptr<clang::FrontendAction> create() override {
      return std::make_unique<dependency_action>(dependencies_, imported_);
   }

 private:
   source_dependencies& dependencies_;
   module_dependencies& imported_;
};

std::string read_text(const std::filesystem::path& path) {
   auto stream = std::ifstream{path, std::ios::binary};
   if (!stream) {
      throw std::runtime_error{"cannot read contract source: " + path.string()};
   }
   return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void write_text(const std::filesystem::path& path, std::string_view text) {
   if (const auto parent = path.parent_path(); !parent.empty()) {
      std::filesystem::create_directories(parent);
   }
   auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
   if (!stream || !(stream << text)) {
      throw std::runtime_error{"cannot write generated contract file: " + path.string()};
   }
}

std::string depfile_path(const std::filesystem::path& path) {
   auto result = std::string{};
   for (const auto value : path.generic_string()) {
      switch (value) {
      case ' ':
      case '#':
         result += '\\';
         result += value;
         break;
      case '$':
         result += "$$";
         break;
      default:
         result += value;
         break;
      }
   }
   return result;
}

void write_depfile(const forge::tooling::abi::request& options, const source_dependencies& dependencies) {
   if (options.depfile.empty()) {
      return;
   }
   auto output = std::ostringstream{};
   output << depfile_path(options.abi) << ':';
   for (const auto& dependency : dependencies | std::views::keys) {
      output << " \\\n  " << depfile_path(dependency);
   }
   output << '\n';
   write_text(options.depfile, output.str());
}

const forge::variant_object& metadata_object(const forge::variant& value, std::string_view description) {
   if (!value.is_object()) {
      throw std::runtime_error{std::string{description} + " must be an object"};
   }
   return value.get_object();
}

const forge::variants& metadata_array(const forge::variant_object& object, const char* name,
                                      std::string_view description) {
   if (!object.contains(name) || !object[name].is_array()) {
      throw std::runtime_error{std::string{description} + " has no " + name + " array"};
   }
   return object[name].get_array();
}

std::string metadata_string(const forge::variant_object& object, const char* name, std::string_view description) {
   if (!object.contains(name) || !object[name].is_string() || object[name].get_string().empty()) {
      throw std::runtime_error{std::string{description} + " has no " + name};
   }
   return object[name].get_string();
}

struct compilation_metadata {
   std::filesystem::path source;
   source_dependencies dependencies;
   module_dependencies imported_modules;
   module_dependencies exported_modules;
   module_dependencies provided_modules;
};

module_dependencies metadata_modules(const forge::variant_object& object, const char* name,
                                     const std::filesystem::path& path) {
   const auto& values = metadata_array(object, name, "contract compilation metadata");
   auto result = module_dependencies{};
   for (const auto& value : values) {
      if (!value.is_string() || value.get_string().empty() || !result.insert(value.get_string()).second) {
         throw std::runtime_error{"contract compilation metadata has invalid " + std::string{name} + ": " +
                                  path.string()};
      }
   }
   return result;
}

compilation_metadata read_compilation_metadata(const std::filesystem::path& object_path) {
   auto path = object_path;
   path += ".forge-contract-metadata.json";
   if (!std::filesystem::is_regular_file(path)) {
      throw std::runtime_error{"contract compilation metadata does not exist: " + path.string()};
   }
   const auto parsed = forge::codec::json::read_value(read_text(path), {.source_name = path.string()});
   if (!parsed.ok() || !parsed.value.is_object()) {
      throw std::runtime_error{"contract compilation metadata is not valid JSON: " + path.string()};
   }
   const auto& object = parsed.value.get_object();
   if (!object.contains("version") || object["version"].as_uint64() != 1U) {
      throw std::runtime_error{"contract compilation metadata has an unsupported schema: " + path.string()};
   }
   auto result = compilation_metadata{
       .source = std::filesystem::weakly_canonical(metadata_string(object, "source", "contract compilation metadata")),
       .imported_modules = metadata_modules(object, "imports", path),
       .exported_modules = metadata_modules(object, "exports", path),
       .provided_modules = metadata_modules(object, "provides", path),
   };
   for (const auto& value : metadata_array(object, "dependencies", "contract compilation metadata")) {
      const auto& dependency = metadata_object(value, "contract compilation dependency");
      if (!dependency.contains("system") || !dependency["system"].is_bool()) {
         throw std::runtime_error{"contract compilation dependency has no system flag: " + path.string()};
      }
      const auto dependency_path =
          std::filesystem::weakly_canonical(metadata_string(dependency, "path", "contract compilation dependency"));
      const auto [entry, inserted] = result.dependencies.emplace(dependency_path, dependency["system"].as_bool());
      if (!inserted) {
         entry->second = entry->second && dependency["system"].as_bool();
      }
   }
   return result;
}

std::vector<compilation_metadata> read_compilation_list(const std::filesystem::path& path) {
   if (!std::filesystem::is_regular_file(path)) {
      throw std::runtime_error{"contract compilation list does not exist: " + path.string()};
   }
   auto input = std::istringstream{read_text(path)};
   auto result = std::vector<compilation_metadata>{};
   auto line = std::string{};
   while (std::getline(input, line)) {
      if (!line.empty()) {
         result.push_back(read_compilation_metadata(line));
      }
   }
   if (result.empty()) {
      throw std::runtime_error{"contract compilation list is empty: " + path.string()};
   }
   return result;
}

struct validated_contract_libraries {
   std::map<std::string, std::string> module_owners;
};

validated_contract_libraries validate_contract_libraries(const forge::tooling::abi::request& options) {
   auto metadata_by_owner = std::map<std::string, std::vector<compilation_metadata>>{};
   for (const auto& compilation : options.library_compilations) {
      if (compilation.owner.empty() || metadata_by_owner.contains(compilation.owner)) {
         throw std::runtime_error{"contract compilation owner is specified more than once: " + compilation.owner};
      }
      metadata_by_owner.emplace(compilation.owner, read_compilation_list(compilation.object_list));
   }

   auto module_owners = std::map<std::string, std::string>{};
   for (const auto& module : options.known_modules) {
      if (module.name.empty() || module.owner.empty()) {
         throw std::runtime_error{"known contract module has an empty name or owner"};
      }
      const auto [existing, inserted] = module_owners.emplace(module.name, module.owner);
      if (!inserted && existing->second != module.owner) {
         throw std::runtime_error{"contract module has multiple registered owners: " + module.name};
      }
   }
   for (const auto& [owner, metadata_values] : metadata_by_owner) {
      for (const auto& metadata : metadata_values) {
         for (const auto& module : metadata.provided_modules) {
            const auto [existing, inserted] = module_owners.emplace(module, owner);
            if (!inserted && existing->second != owner) {
               throw std::runtime_error{"contract module owner does not match its compilation: " + module};
            }
         }
      }
   }

   for (const auto& [owner, metadata_values] : metadata_by_owner) {
      for (const auto& metadata : metadata_values) {
         for (const auto& module : metadata.exported_modules) {
            const auto found = module_owners.find(module);
            if (found == module_owners.end()) {
               throw std::runtime_error{"contract library " + owner + " exports an unknown module: " + module};
            }
         }
         for (const auto& module : metadata.imported_modules) {
            const auto found = module_owners.find(module);
            if (found == module_owners.end()) {
               throw std::runtime_error{"contract library " + owner + " imports an unknown module: " + module};
            }
         }
      }
   }

   return {
       .module_owners = std::move(module_owners),
   };
}

void validate_contract_root(const validated_contract_libraries& validated,
                            const module_dependencies& contract_modules) {
   for (const auto& module : contract_modules) {
      const auto found = validated.module_owners.find(module);
      if (found == validated.module_owners.end()) {
         throw std::runtime_error{"contract imports an unknown module: " + module};
      }
   }
}

std::string trim_lines(std::string value) {
   while (!value.empty() && (value.front() == '\n' || value.front() == '\r')) {
      value.erase(value.begin());
   }
   while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
      value.pop_back();
   }
   return value;
}

std::map<std::string, std::string> read_ricardian_sections(const std::filesystem::path& path,
                                                           std::string_view section_class) {
   if (path.empty()) {
      return {};
   }
   const auto source = read_text(path);
   const auto prefix = std::string{"<h1 class=\""} + std::string{section_class} + "\">";
   constexpr auto suffix = std::string_view{"</h1>"};
   auto result = std::map<std::string, std::string>{};
   auto cursor = std::size_t{0};
   while ((cursor = source.find(prefix, cursor)) != std::string::npos) {
      const auto name_start = cursor + prefix.size();
      const auto name_end = source.find(suffix, name_start);
      if (name_end == std::string::npos) {
         throw std::runtime_error{"malformed Ricardian section header in " + path.string()};
      }
      const auto body_start = name_end + suffix.size();
      const auto next = source.find(prefix, body_start);
      auto name = trim_lines(source.substr(name_start, name_end - name_start));
      auto body = trim_lines(source.substr(body_start, next == std::string::npos ? next : next - body_start));
      if (name.empty() || !result.emplace(std::move(name), std::move(body)).second) {
         throw std::runtime_error{"duplicate or empty Ricardian section in " + path.string()};
      }
      cursor = next;
   }
   return result;
}

void load_ricardian(schema& output, const forge::tooling::abi::request& options) {
   const auto clauses = read_ricardian_sections(options.ricardian_clauses, "clause");
   for (const auto& [id, body] : clauses) {
      output.clauses.push_back(protocol::clause_pair{id, body});
   }
}

void canonicalize(schema& output) {
   const auto by_name = [](const auto& left, const auto& right) { return left.name < right.name; };
   std::ranges::sort(output.types, by_name);
   std::ranges::sort(output.structs, by_name);
   std::ranges::sort(output.record_codecs, by_name);
   std::ranges::sort(output.actions, by_name);
   std::ranges::sort(output.notifications, [](const auto& left, const auto& right) {
      return std::tie(left.code, left.action, left.class_name, left.method_name) <
             std::tie(right.code, right.action, right.class_name, right.method_name);
   });
   std::ranges::sort(output.variants, by_name);
   std::ranges::sort(output.tables, by_name);
   std::ranges::sort(output.calls, by_name);
   std::ranges::sort(output.clauses, [](const auto& left, const auto& right) { return left.id < right.id; });
   std::ranges::sort(output.metadata.roots, {}, &protocol::abi_root_def::cpp_type);
   std::ranges::sort(output.metadata.enums, {}, &protocol::abi_enum_def::name);
}

void write_abi(const schema& input, const forge::tooling::abi::request& options) {
   auto abi = protocol::abi_def{};
   abi.version = input.calls.empty() ? "eosio::abi/1.2" : "eosio::abi/1.3";
   for (const auto& source : input.types) {
      abi.types.push_back(protocol::type_def{source.name, source.type});
   }
   for (const auto& source : input.structs) {
      auto target = protocol::struct_def{.name = source.name, .base = source.base};
      for (const auto& field : source.fields) {
         target.fields.push_back(protocol::field_def{field.name, field.type});
      }
      abi.structs.push_back(std::move(target));
   }
   const auto contracts = read_ricardian_sections(options.ricardian_contracts, "contract");
   for (const auto& source : input.actions) {
      const auto found = contracts.find(source.name);
      abi.actions.push_back(protocol::action_def{
          protocol::make_name(source.name),
          source.type,
          found == contracts.end() ? std::string{} : found->second,
      });
      if (!source.result.empty()) {
         abi.action_results.value.push_back(
             protocol::action_result_def{protocol::make_name(source.name), source.result});
      }
   }
   for (const auto& source : input.tables) {
      abi.tables.push_back(protocol::table_def{
          protocol::make_name(source.name),
          "i64",
          {},
          {},
          source.type,
      });
   }
   abi.ricardian_clauses = input.clauses;
   for (const auto& source : input.variants) {
      abi.variants.value.push_back(protocol::variant_def{source.name, source.types});
   }

   auto value = forge::variant{};
   protocol::to_variant(abi, value);
   if (!input.calls.empty()) {
      auto calls = forge::variants{};
      for (const auto& source : input.calls) {
         calls.emplace_back(forge::mutable_variant_object{}("name", source.name)("type", source.type)("id", source.id)(
             "result_type", source.result));
      }
      auto root = forge::mutable_variant_object{value.get_object()};
      root.set("calls", forge::variant{std::move(calls)});
      value = forge::variant{std::move(root)};
   }
   const auto encoded = forge::codec::json::write_value(value, {.pretty = true});
   if (!encoded.ok()) {
      throw std::runtime_error{"failed to encode generated contract ABI"};
   }
   write_text(options.abi, encoded.text + '\n');
}

void write_codec_declarations(std::ostream& output, const schema& input) {
   for (const auto& record : input.record_codecs) {
      if (!record.forward_declaration.empty()) {
         output << record.forward_declaration << '\n';
      }
   }
   for (const auto& record : input.record_codecs) {
      if (record.forward_declaration.empty()) {
         continue;
      }
      output << "template <> struct forge::raw::codec_traits<" << record.name << "> {\n";
      output << "   template <typename Stream> static void pack(Stream& stream, const " << record.name << "& value);\n";
      output << "   template <typename Stream> static void unpack(Stream& stream, " << record.name << "& value);\n";
      output << "};\n";
   }
}

void write_codec_definitions(std::ostream& output, const schema& input,
                             const std::set<std::string>* selected = nullptr) {
   for (const auto& record : input.record_codecs) {
      if (selected != nullptr && !selected->contains(record.name)) {
         continue;
      }
      const auto early_declaration = !record.forward_declaration.empty();
      const auto body_indent = early_declaration ? "   " : "      ";
      if (early_declaration) {
         output << "template <typename Stream> void forge::raw::codec_traits<" << record.name
                << ">::pack(Stream& stream, const " << record.name << "& value) {\n";
      } else {
         output << "template <> struct forge::raw::codec_traits<" << record.name << "> {\n";
         output << "   template <typename Stream> static void pack(Stream& stream, const " << record.name
                << "& value) {\n";
      }
      if (!record.base.empty()) {
         output << body_indent << "forge::raw::pack(stream, static_cast<const " << record.base << "&>(value));\n";
      }
      for (const auto& field : record.fields) {
         output << body_indent << "forge::raw::pack(stream, value." << field << ");\n";
      }
      output << (early_declaration ? "}\n" : "   }\n");

      if (early_declaration) {
         output << "template <typename Stream> void forge::raw::codec_traits<" << record.name
                << ">::unpack(Stream& stream, " << record.name << "& value) {\n";
      } else {
         output << "   template <typename Stream> static void unpack(Stream& stream, " << record.name << "& value) {\n";
      }
      if (!record.base.empty()) {
         output << body_indent << "forge::raw::unpack(stream, static_cast<" << record.base << "&>(value));\n";
      }
      for (const auto& field : record.fields) {
         output << body_indent << "forge::raw::unpack(stream, value." << field << ");\n";
      }
      output << (early_declaration ? "}\n" : "   }\n};\n");
   }
}

const std::set<std::string>& source_record_codecs(const schema& input, const std::filesystem::path& source) {
   static const auto empty = std::set<std::string>{};
   const auto canonical = std::filesystem::weakly_canonical(source);
   const auto found = input.source_record_codecs.find(canonical);
   return found == input.source_record_codecs.end() ? empty : found->second;
}

std::string make_codec_prelude(const schema& input) {
   if (input.record_codecs.empty()) {
      return {};
   }
   auto output = std::ostringstream{};
   output << "#pragma once\n";
   output << "import forge.raw.codec;\n";
   write_codec_declarations(output, input);
   return output.str();
}

bool is_dispatch_source(const std::filesystem::path& source, const std::filesystem::path& dispatch_source) {
   return source == dispatch_source;
}

std::string action_thunk(std::size_t index) {
   return "__forge_contract_action_" + std::to_string(index);
}

std::string notification_thunk(std::size_t index) {
   return "__forge_contract_notification_" + std::to_string(index);
}

std::string call_thunk(std::size_t index) {
   return "__forge_contract_call_" + std::to_string(index);
}

void write_dispatch_thunk_declarations(std::ostream& output, const schema& input,
                                       const std::filesystem::path& dispatch_source) {
   for (auto index = std::size_t{0}; index < input.actions.size(); ++index) {
      const auto& entry = input.actions[index];
      if (entry.dispatchable && !is_dispatch_source(entry.source, dispatch_source)) {
         output << "extern \"C\" void " << action_thunk(index) << "(std::uint64_t, std::uint64_t);\n";
      }
   }
   for (auto index = std::size_t{0}; index < input.notifications.size(); ++index) {
      if (!is_dispatch_source(input.notifications[index].source, dispatch_source)) {
         output << "extern \"C\" void " << notification_thunk(index) << "(std::uint64_t, std::uint64_t);\n";
      }
   }
   for (auto index = std::size_t{0}; index < input.calls.size(); ++index) {
      if (!is_dispatch_source(input.calls[index].source, dispatch_source)) {
         output << "extern \"C\" void " << call_thunk(index) << "(std::uint64_t, std::uint64_t);\n";
      }
   }
}

void write_call_dispatcher(std::ostream& output, const schema& input, const std::filesystem::path& dispatch_source) {
   if (input.calls.empty()) {
      return;
   }
   output << "extern \"C\" [[gnu::visibility(\"default\")]] void __forge_call("
             "std::uint64_t sender, std::uint64_t receiver) {\n";
   output << "   using forge::chain::protocol::name;\n";
   output << "   static constexpr forge::contract::call_entry entries[] = {\n";
   for (auto index = std::size_t{0}; index < input.calls.size(); ++index) {
      const auto& entry = input.calls[index];
      if (is_dispatch_source(entry.source, dispatch_source)) {
         output << "      forge::contract::make_call_entry<" << entry.class_name << ", &" << entry.class_name
                << "::" << entry.method_name << ">(" << entry.id << "ULL),\n";
      } else {
         output << "      forge::contract::call_entry{" << entry.id << "ULL, [](name sender, name receiver) { "
                << call_thunk(index) << "(sender.value, receiver.value); }},\n";
      }
   }
   output << "   };\n";
   output << "   forge::contract::dispatch_call(name{sender}, name{receiver}, entries, " << input.calls.size()
          << "U);\n";
   output << "}\n";
}

void write_dispatcher(const schema& input, const forge::tooling::abi::request& options) {
   auto output = std::ostringstream{};
   if (!input.has_apply || input.has_eosio_dispatch || !input.calls.empty()) {
      output << "#include <cstdint>\n";
      output << "import forge.contract.dispatcher;\n";
   } else {
      output << "import forge.raw.codec;\n";
   }
   write_codec_declarations(output, input);
   if (input.has_eosio_dispatch) {
      output << "#define FORGE_CONTRACT_DEFER_EOSIO_DISPATCH 1\n";
   }
   const auto dispatch_source = std::filesystem::weakly_canonical(options.sources.front());
   const auto source = dispatch_source.generic_string();
   output << "#include " << std::quoted(source) << "\n";
   if (input.has_eosio_dispatch) {
      output << "#undef FORGE_CONTRACT_DEFER_EOSIO_DISPATCH\n";
   }
   if (input.has_apply && !input.has_eosio_dispatch) {
      output << "#line 1 \"contract generated codecs\"\n";
      write_codec_definitions(output, input, &source_record_codecs(input, options.sources.front()));
      write_dispatch_thunk_declarations(output, input, dispatch_source);
      write_call_dispatcher(output, input, dispatch_source);
      write_text(options.dispatcher, output.str());
      return;
   }
   output << "#line 1 \"contract generated dispatcher\"\n";
   write_codec_definitions(output, input, &source_record_codecs(input, options.sources.front()));
   write_dispatch_thunk_declarations(output, input, dispatch_source);
   if (input.has_eosio_dispatch) {
      output << "extern \"C\" [[gnu::visibility(\"default\")]] void apply("
                "std::uint64_t receiver, std::uint64_t code, std::uint64_t action) {\n";
      output << "   forge::contract::detail::eosio_dispatch_definition<void>::invoke(receiver, code, action);\n";
      output << "}\n";
      write_call_dispatcher(output, input, dispatch_source);
      write_text(options.dispatcher, output.str());
      return;
   }
   output << "extern \"C\" [[gnu::visibility(\"default\")]] void apply("
             "std::uint64_t receiver, std::uint64_t code, std::uint64_t action) {\n";
   output << "   using forge::chain::protocol::name;\n";
   const auto dispatchable_actions = std::ranges::count_if(input.actions, &action_shape::dispatchable);
   if (dispatchable_actions != 0U) {
      output << "   static constexpr forge::contract::dispatch_entry entries[] = {\n";
      for (auto index = std::size_t{0}; index < input.actions.size(); ++index) {
         const auto& entry = input.actions[index];
         if (!entry.dispatchable) {
            continue;
         }
         if (is_dispatch_source(entry.source, dispatch_source)) {
            output << "      forge::contract::make_dispatch_entry<" << entry.class_name << ", &" << entry.class_name
                   << "::" << entry.method_name << ">(" << protocol::make_name(entry.name).value << "ULL),\n";
         } else {
            output << "      forge::contract::dispatch_entry{" << protocol::make_name(entry.name).value
                   << "ULL, [](name self, name first_receiver) { " << action_thunk(index)
                   << "(self.value, first_receiver.value); }},\n";
         }
      }
      output << "   };\n";
   }
   if (!input.notifications.empty()) {
      output << "   static constexpr forge::contract::notification_entry notifications[] = {\n";
      for (auto index = std::size_t{0}; index < input.notifications.size(); ++index) {
         const auto& entry = input.notifications[index];
         if (is_dispatch_source(entry.source, dispatch_source)) {
            output << "      forge::contract::make_notification_entry<" << entry.class_name << ", &" << entry.class_name
                   << "::" << entry.method_name << ">(" << entry.code << "ULL, " << entry.action << "ULL),\n";
         } else {
            output << "      forge::contract::notification_entry{" << entry.code << "ULL, " << entry.action
                   << "ULL, [](name self, name first_receiver) { " << notification_thunk(index)
                   << "(self.value, first_receiver.value); }},\n";
         }
      }
      output << "   };\n";
   }
   output << "   forge::contract::dispatch(name{receiver}, name{code}, action, ";
   output << (dispatchable_actions == 0U ? "nullptr, 0U" : "entries, " + std::to_string(dispatchable_actions) + "U");
   output << ", ";
   output << (input.notifications.empty() ? "nullptr, 0U"
                                          : "notifications, " + std::to_string(input.notifications.size()) + "U");
   output << ");\n";
   output << "}\n";
   write_call_dispatcher(output, input, dispatch_source);
   write_text(options.dispatcher, output.str());
}

void write_source_wrappers(const schema& input, const forge::tooling::abi::request& options) {
   if (options.source_wrappers.empty()) {
      return;
   }
   if (options.source_wrappers.size() + 1U != options.sources.size()) {
      throw std::runtime_error{"source wrapper count does not match non-dispatch contract sources"};
   }
   for (auto index = std::size_t{0}; index < options.source_wrappers.size(); ++index) {
      const auto& source = options.sources[index + 1U];
      const auto canonical_source = std::filesystem::weakly_canonical(source);
      auto output = std::ostringstream{};
      output << "#include <cstdint>\n";
      output << "import forge.contract.dispatcher;\n";
      write_codec_declarations(output, input);
      output << "#include " << std::quoted(canonical_source.generic_string()) << "\n";
      output << "#line 1 \"contract generated codecs\"\n";
      write_codec_definitions(output, input, &source_record_codecs(input, source));
      for (auto action_index = std::size_t{0}; action_index < input.actions.size(); ++action_index) {
         const auto& entry = input.actions[action_index];
         if (!entry.dispatchable || entry.source != canonical_source) {
            continue;
         }
         output << "extern \"C\" [[gnu::visibility(\"hidden\")]] void " << action_thunk(action_index)
                << "(std::uint64_t receiver, std::uint64_t code) {\n";
         output << "   forge::contract::execute_action<" << entry.class_name
                << ">(forge::chain::protocol::name{receiver}, forge::chain::protocol::name{code}, &" << entry.class_name
                << "::" << entry.method_name << ");\n";
         output << "}\n";
      }
      for (auto notification_index = std::size_t{0}; notification_index < input.notifications.size();
           ++notification_index) {
         const auto& entry = input.notifications[notification_index];
         if (entry.source != canonical_source) {
            continue;
         }
         output << "extern \"C\" [[gnu::visibility(\"hidden\")]] void " << notification_thunk(notification_index)
                << "(std::uint64_t receiver, std::uint64_t code) {\n";
         output << "   forge::contract::execute_action<" << entry.class_name
                << ">(forge::chain::protocol::name{receiver}, forge::chain::protocol::name{code}, &" << entry.class_name
                << "::" << entry.method_name << ");\n";
         output << "}\n";
      }
      for (auto call_index = std::size_t{0}; call_index < input.calls.size(); ++call_index) {
         const auto& entry = input.calls[call_index];
         if (entry.source != canonical_source) {
            continue;
         }
         output << "extern \"C\" [[gnu::visibility(\"hidden\")]] void " << call_thunk(call_index)
                << "(std::uint64_t sender, std::uint64_t receiver) {\n";
         output << "   forge::contract::execute_call<" << entry.class_name
                << ">(forge::chain::protocol::name{sender}, forge::chain::protocol::name{receiver}, &"
                << entry.class_name << "::" << entry.method_name << ");\n";
         output << "}\n";
      }
      write_text(options.source_wrappers[index], output.str());
   }
}

} // namespace

namespace forge::tooling::abi {

artifacts generate(const request& options) {
   auto root_names = std::set<std::string>{};
   for (const auto& root : options.abi_root_types) {
      if (root.empty() || !root_names.insert(root.starts_with("::") ? root.substr(2U) : root).second) {
         throw std::runtime_error{"empty or duplicate contract ABI root type: " + root};
      }
   }
   if (!options.metadata.empty()) {
      const auto metadata_path = std::filesystem::weakly_canonical(std::filesystem::absolute(options.metadata));
      auto other_paths = options.sources;
      other_paths.insert(other_paths.end(), options.source_wrappers.begin(), options.source_wrappers.end());
      other_paths.insert(other_paths.end(), {options.abi, options.dispatcher, options.depfile});
      for (const auto& other : other_paths) {
         if (!other.empty() && metadata_path == std::filesystem::weakly_canonical(std::filesystem::absolute(other))) {
            throw std::runtime_error{"ABI metadata output must have a distinct path"};
         }
      }
   }
   auto load_error = std::string{};
   if (llvm::sys::DynamicLibrary::LoadLibraryPermanently(options.attribute_plugin.c_str(), &load_error)) {
      throw std::runtime_error{"failed to load contract attribute plugin: " + load_error};
   }
   const auto validated_libraries = validate_contract_libraries(options);

   auto arguments = std::vector<std::string>{
       "-std=c++23",
       "--target=wasm32",
       "-fsyntax-only",
   };
   arguments.insert(arguments.end(), options.compiler_arguments.begin(), options.compiler_arguments.end());
   arguments.insert(arguments.end(), {
                                         "-fno-exceptions",
                                         "-fno-rtti",
                                         "-fno-threadsafe-statics",
                                         "-ffreestanding",
                                         "-fvisibility=hidden",
                                         "-fno-ident",
                                         "--sysroot=" + options.sysroot.string(),
                                         "-mcpu=mvp",
                                         "-DFORGE_CONTRACT_GUEST=1",
                                         "-DFORGE_CONTRACT_DEFER_EOSIO_DISPATCH=1",
                                     });
   for (const auto& path : options.include_paths) {
      arguments.push_back("-I" + path.string());
   }
   for (const auto& path : options.sdk_include_paths) {
      arguments.push_back("-I" + path.string());
   }
   for (const auto& path : options.module_paths) {
      arguments.push_back("-fprebuilt-module-path=" + path.string());
   }

   auto source_paths = std::vector<std::string>{};
   source_paths.reserve(options.sources.size());
   for (const auto& source : options.sources) {
      source_paths.push_back(source.string());
   }

   // Discover namespace-scope ABI records before the strict pass so contract code can use their generated codecs.
   auto discovery_compilation = clang::tooling::FixedCompilationDatabase{".", arguments};
   auto discovery_tool = clang::tooling::ClangTool{discovery_compilation, source_paths};
   auto discovery = schema{.requested_roots = options.abi_root_types, .collect_metadata = !options.metadata.empty()};
   auto discovery_found = false;
   auto discovery_dispatch_source_found = false;
   auto discovery_dependencies = source_dependencies{};
   auto discovery_modules = module_dependencies{};
   const auto dispatch_source = std::filesystem::weakly_canonical(options.sources.front());
   auto discovery_factory = action_factory{discovery,        discovery_found, discovery_dispatch_source_found,
                                           options.contract, dispatch_source, discovery_dependencies,
                                           discovery_modules};
   auto ignored_diagnostics = clang::IgnoringDiagConsumer{};
   discovery_tool.setDiagnosticConsumer(&ignored_diagnostics);
   static_cast<void>(discovery_tool.run(&discovery_factory));

   const auto codec_prelude = make_codec_prelude(discovery);
   constexpr auto codec_prelude_path = "/__forge_contract_generated_codecs.hpp";
   if (!codec_prelude.empty()) {
      arguments.push_back("-include");
      arguments.push_back(codec_prelude_path);
   }

   auto compilation = clang::tooling::FixedCompilationDatabase{".", arguments};
   auto tool = clang::tooling::ClangTool{compilation, source_paths};
   if (!codec_prelude.empty()) {
      tool.mapVirtualFile(codec_prelude_path, codec_prelude);
   }
   auto output = schema{.requested_roots = options.abi_root_types, .collect_metadata = !options.metadata.empty()};
   auto found = false;
   auto dispatch_source_found = false;
   auto dependencies = source_dependencies{};
   auto imported_modules = module_dependencies{};
   auto factory = action_factory{output,          found,        dispatch_source_found, options.contract,
                                 dispatch_source, dependencies, imported_modules};
   const auto result = tool.run(&factory);
   if (result != 0 || output.failed) {
      throw std::runtime_error{"contract source analysis failed"};
   }
   if (!found) {
      throw std::runtime_error{"contract '" + options.contract + "' was not found"};
   }
   if (!dispatch_source_found) {
      throw std::runtime_error{"first contract source must declare contract '" + options.contract +
                               "': " + dispatch_source.string()};
   }
   if (!output.has_apply && output.actions.empty() && output.calls.empty() && output.notifications.empty() &&
       output.tables.empty()) {
      throw std::runtime_error{"contract '" + options.contract + "' has no ABI entries"};
   }

   if (!options.dependency_sources.empty()) {
      auto dependency_paths = std::vector<std::string>{};
      dependency_paths.reserve(options.dependency_sources.size());
      for (const auto& source : options.dependency_sources) {
         dependency_paths.push_back(source.string());
      }
      auto dependency_tool = clang::tooling::ClangTool{compilation, dependency_paths};
      if (!codec_prelude.empty()) {
         dependency_tool.mapVirtualFile(codec_prelude_path, codec_prelude);
      }
      auto dependency_factory = dependency_action_factory{dependencies, imported_modules};
      if (dependency_tool.run(&dependency_factory) != 0) {
         throw std::runtime_error{"contract compile-check dependency analysis failed"};
      }
   }

   if (!options.library_compilations.empty() || !options.known_modules.empty()) {
      validate_contract_root(validated_libraries, imported_modules);
   }
   load_ricardian(output, options);
   canonicalize(output);
   write_abi(output, options);
   if (!options.metadata.empty()) {
      const auto encoded = forge::codec::json::write(output.metadata, {.pretty = true});
      if (!encoded.ok()) {
         throw std::runtime_error{"unable to encode contract ABI metadata"};
      }
      write_text(options.metadata, encoded.text + '\n');
   }
   write_dispatcher(output, options);
   write_source_wrappers(output, options);
   write_depfile(options, dependencies);
   return {.abi = options.abi, .dispatcher = options.dispatcher, .source_wrappers = options.source_wrappers,
           .metadata = options.metadata};
}

} // namespace forge::tooling::abi
