#include <array>
#include <cstddef>
#include <iostream>

extern "C" void jit_load_args_capi_first(std::size_t*);
extern "C" void jit_load_args_export_first(std::size_t*);

int main() {
   std::array<std::size_t, 19> capi{}, exports{};
   jit_load_args_capi_first(capi.data());
   jit_load_args_export_first(exports.data());
   if (capi != exports) {
      std::cerr << "LoadArgs or WASMLoadBudget ABI depends on header include order\n";
      return 1;
   }
   std::cout << "PASS LoadArgs defaults and header-order ABI";
   for (const auto value : capi)
      std::cout << ' ' << value;
   std::cout << '\n';
}
