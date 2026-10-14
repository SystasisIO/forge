#include "details/enum_wire_resolver.hxx"

#include <clang/AST/APValue.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Sema/Lookup.h>
#include <clang/Sema/Sema.h>
#include <llvm/ADT/APSInt.h>

#include <array>
#include <optional>
#include <utility>

namespace forge::tooling::abi::detail {

namespace {

bool supported_integer_kind(clang::QualType type) {
   const auto* builtin = type->getAs<clang::BuiltinType>();
   if (builtin == nullptr) {
      return false;
   }
   switch (builtin->getKind()) {
   case clang::BuiltinType::SChar:
   case clang::BuiltinType::UChar:
   case clang::BuiltinType::Short:
   case clang::BuiltinType::UShort:
   case clang::BuiltinType::Int:
   case clang::BuiltinType::UInt:
   case clang::BuiltinType::Long:
   case clang::BuiltinType::ULong:
   case clang::BuiltinType::LongLong:
   case clang::BuiltinType::ULongLong:
      return true;
   default:
      return false;
   }
}

std::optional<std::pair<unsigned, bool>> fixed_integer_alias(clang::ASTContext& context, clang::QualType input) {
   constexpr auto aliases = std::array{
       std::pair{"int8_t", std::pair{8U, false}}, std::pair{"uint8_t", std::pair{8U, true}},
       std::pair{"int16_t", std::pair{16U, false}}, std::pair{"uint16_t", std::pair{16U, true}},
       std::pair{"int32_t", std::pair{32U, false}}, std::pair{"uint32_t", std::pair{32U, true}},
       std::pair{"int64_t", std::pair{64U, false}}, std::pair{"uint64_t", std::pair{64U, true}}};
   auto type = input;
   while (!type.isNull()) {
      if (const auto* alias = llvm::dyn_cast<clang::TypedefType>(type.getTypePtr())) {
         const auto* declaration = alias->getDecl();
         auto* scope = declaration->getDeclContext();
         auto standard_scope = scope->isTranslationUnit();
         while (const auto* current = llvm::dyn_cast<clang::NamespaceDecl>(scope)) {
            if (current->isStdNamespace()) {
               standard_scope = true;
               break;
            }
            if (!current->isInline()) {
               break;
            }
            scope = current->getParent();
         }
         // <cstdint> may expose std::intN_t through a using-declaration for the global C typedef.
         if (standard_scope && context.getSourceManager().isInSystemHeader(declaration->getLocation())) {
            for (const auto& [name, shape] : aliases) {
               if (declaration->getName() == name) {
                  return shape;
               }
            }
         }
      }
      const auto next = type.getSingleStepDesugaredType(context);
      if (next == type) {
         break;
      }
      type = next;
   }
   return std::nullopt;
}

} // namespace

enum_wire_resolver::enum_wire_resolver(clang::ASTContext& context, clang::Sema& sema)
    : _context(context), _sema(sema) {}

clang::ClassTemplateDecl* enum_wire_resolver::find_template() {
   auto* scope = static_cast<clang::DeclContext*>(_context.getTranslationUnitDecl());
   constexpr auto names = std::array<std::string_view, 3>{"forge", "raw", "enum_wire_type"};
   for (const auto name : names) {
      auto lookup = clang::LookupResult{
          _sema, _context.DeclarationNames.getIdentifier(&_context.Idents.get(name)),
          clang::SourceLocation{}, clang::Sema::LookupOrdinaryName};
      lookup.suppressDiagnostics();
      // Exported specializations can depend on a reachable, non-re-exported primary template.
      lookup.setAllowHidden(true);
      if (!_sema.LookupQualifiedName(lookup, scope) || !lookup.isSingleResult()) {
         return nullptr;
      }
      auto* declaration = lookup.getFoundDecl()->getUnderlyingDecl();
      if (!_sema.isReachable(declaration)) {
         return nullptr;
      }
      if (name == names.back()) {
         return llvm::dyn_cast<clang::ClassTemplateDecl>(declaration);
      }
      auto* current = llvm::dyn_cast<clang::NamespaceDecl>(declaration);
      if (current == nullptr) {
         return nullptr;
      }
      scope = current;
   }
   return nullptr;
}

enum_wire_resolver::result enum_wire_resolver::resolve(const clang::EnumDecl& input) {
   const auto& declaration = *input.getDefinitionOrSelf();
   const auto fallback = result{.type = declaration.getIntegerType()};
   auto* trait = find_template();
   if (trait == nullptr) {
      return fallback;
   }

   const auto location = declaration.getLocation();
   const auto enum_type = _context.getTypeDeclType(clang::ElaboratedTypeKeyword::None, std::nullopt, &declaration);
   auto arguments = clang::TemplateArgumentListInfo{location, location};
   arguments.addArgument(clang::TemplateArgumentLoc{
       clang::TemplateArgument{enum_type}, _context.getTrivialTypeSourceInfo(enum_type, location)});
   // Semantic lookup loads imported specializations even if no codec was instantiated.
   trait->LoadLazySpecializations();
   const auto specialized_type = _sema.CheckTemplateIdType(
       clang::ElaboratedTypeKeyword::None, clang::TemplateName{trait}, location, arguments, nullptr, false);
   if (specialized_type.isNull()) {
      report(declaration, location, "cannot resolve enum_wire_type specialization");
      return {.valid = false};
   }
   const auto incomplete = _context.getDiagnostics().getCustomDiagID(
       clang::DiagnosticsEngine::Error, "incomplete enum_wire_type specialization '%0'");
   if (_sema.RequireCompleteType(location, specialized_type, incomplete)) {
      return {.valid = false};
   }
   auto* record = specialized_type->getAsCXXRecordDecl();
   if (record == nullptr) {
      report(declaration, location, "enum_wire_type must resolve to a class specialization");
      return {.valid = false};
   }

   auto shared_lookup = clang::LookupResult{
       _sema, _context.DeclarationNames.getIdentifier(&_context.Idents.get("shared")),
       location, clang::Sema::LookupOrdinaryName};
   shared_lookup.suppressDiagnostics();
   shared_lookup.setAllowHidden(true);
   _sema.LookupQualifiedName(shared_lookup, record);
   if (shared_lookup.empty()) {
      return fallback;
   }
   auto* shared = shared_lookup.isSingleResult()
                      ? llvm::dyn_cast<clang::VarDecl>(shared_lookup.getFoundDecl()->getUnderlyingDecl())
                      : nullptr;
   // Sema's lookup access includes base paths and using-declarations, unlike the underlying member.
   if (shared == nullptr || !_sema.isReachable(shared) || !shared->isStaticDataMember() || !shared->isConstexpr() ||
       !shared->getType()->isBooleanType() || shared_lookup.begin().getAccess() != clang::AS_public) {
      report(declaration, location, "shared marker must be a public static constexpr bool");
      return {.valid = false};
   }
   if (!shared->hasInit() && shared->getInstantiatedFromStaticDataMember() != nullptr) {
      _sema.InstantiateVariableDefinition(location, shared, true, true);
   }
   const auto* value = shared->evaluateValue();
   if (value == nullptr || !value->isInt()) {
      report(declaration, shared->getLocation(), "shared marker must have a constant bool value");
      return {.valid = false};
   }
   if (value->getInt().isZero()) {
      return fallback;
   }

   const auto underlying = declaration.getIntegerType().getCanonicalType();
   const auto underlying_width = _context.getTypeSize(underlying);
   if (!supported_integer_kind(underlying) ||
       (underlying_width != 8U && underlying_width != 16U && underlying_width != 32U && underlying_width != 64U)) {
      report(declaration, location,
             "shared enum underlying type must be a supported signed or unsigned 8, 16, 32, or 64-bit integer");
      return {.valid = false};
   }

   auto type_lookup = clang::LookupResult{
       _sema, _context.DeclarationNames.getIdentifier(&_context.Idents.get("type")),
       location, clang::Sema::LookupOrdinaryName};
   type_lookup.suppressDiagnostics();
   type_lookup.setAllowHidden(true);
   _sema.LookupQualifiedName(type_lookup, record);
   const auto* alias = type_lookup.isSingleResult()
                           ? llvm::dyn_cast<clang::TypedefNameDecl>(type_lookup.getFoundDecl()->getUnderlyingDecl())
                           : nullptr;
   if (alias == nullptr || !_sema.isReachable(alias) ||
       type_lookup.begin().getAccess() != clang::AS_public) {
      report(declaration, location, "shared enum wire contract requires a public type alias");
      return {.valid = false};
   }
   const auto wire_type = alias->getUnderlyingType().getCanonicalType();
   if (wire_type.hasQualifiers() || !wire_type->isIntegerType() || wire_type->isBooleanType()) {
      report(declaration, alias->getLocation(), "shared enum wire type must be a signed or unsigned integer");
      return {.valid = false};
   }
   const auto width = _context.getTypeSize(wire_type);
   if (width != 8U && width != 16U && width != 32U && width != 64U) {
      report(declaration, alias->getLocation(), "shared enum wire integer width must be 8, 16, 32, or 64 bits");
      return {.valid = false};
   }
   if (!supported_integer_kind(wire_type)) {
      report(declaration, alias->getLocation(), "shared enum wire type must be a supported fixed-width integer");
      return {.valid = false};
   }

   const auto is_unsigned = wire_type->isUnsignedIntegerType();
   const auto fixed = fixed_integer_alias(_context, alias->getUnderlyingType());
   if (!fixed.has_value()) {
      report(declaration, alias->getLocation(),
             "shared enum wire type must use a fixed-width intN_t or uintN_t alias; platform-sized types are not portable");
      return {.valid = false};
   }
   if (fixed->first != width || fixed->second != is_unsigned) {
      report(declaration, alias->getLocation(), "shared enum wire fixed-width alias has contradictory width or signedness");
      return {.valid = false};
   }
   const auto minimum = llvm::APSInt::getMinValue(static_cast<unsigned>(width), is_unsigned);
   const auto maximum = llvm::APSInt::getMaxValue(static_cast<unsigned>(width), is_unsigned);
   for (const auto* enumerator : declaration.enumerators()) {
      const auto& integer = enumerator->getInitVal();
      if (llvm::APSInt::compareValues(integer, minimum) < 0 || llvm::APSInt::compareValues(integer, maximum) > 0) {
         report(declaration, enumerator->getLocation(), "enumerator is out of range for the shared enum wire type");
         return {.valid = false};
      }
   }
   return {.type = wire_type, .shared = true};
}

void enum_wire_resolver::report(const clang::EnumDecl& declaration, clang::SourceLocation location,
                                std::string_view message) const {
   const auto id = _context.getDiagnostics().getCustomDiagID(
       clang::DiagnosticsEngine::Error, "invalid shared enum wire contract for '%0': %1");
   _context.getDiagnostics().Report(location, id) << declaration.getQualifiedNameAsString() << message;
}

} // namespace forge::tooling::abi::detail
