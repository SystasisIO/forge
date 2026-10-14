#pragma once

#include <clang/AST/Type.h>
#include <string_view>

namespace clang {
class ASTContext;
class ClassTemplateDecl;
class EnumDecl;
class Sema;
class SourceLocation;
}

namespace forge::tooling::abi::detail {

class enum_wire_resolver {
 public:
   struct result {
      clang::QualType type;
      bool shared = false;
      bool valid = true;
   };

   enum_wire_resolver(clang::ASTContext& context, clang::Sema& sema);
   result resolve(const clang::EnumDecl& declaration);

 private:
   clang::ClassTemplateDecl* find_template();
   void report(const clang::EnumDecl& declaration, clang::SourceLocation location, std::string_view message) const;

   clang::ASTContext& _context;
   clang::Sema& _sema;
};

} // namespace forge::tooling::abi::detail
