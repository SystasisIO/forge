#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <functional>
#include <string>
#include <typeinfo>
#include <vector>

#if !defined(_LIBCPP_VERSION) || _LIBCPP_VERSION != 220108
#error HARNESS_ERROR: the reviewed libc++22.1.8 headers are required
#endif

int main() {
   // Both returned strings and the stream's destination cross LLVM's C++ ABI.
   const std::string triple = llvm::sys::getDefaultTargetTriple();
   const std::string normalized = llvm::Triple::normalize(triple);
   std::string output(256, 'x');
   llvm::raw_string_ostream stream(output);
   llvm::raw_ostream& base = stream;
   base << normalized;
   if (typeid(base) != typeid(llvm::raw_string_ostream))
      return 1;
   base.flush();
   if (!triple.starts_with("x86_64") || !normalized.starts_with("x86_64"))
      return 2;
   return output == std::string(256, 'x') + normalized ? 0 : 3;
}
