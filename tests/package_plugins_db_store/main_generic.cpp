#include <iostream>

int package_api_contract_checks();

int main() {
   const auto result = package_api_contract_checks();
   if (result == 0) {
      std::cout << "PASS generic installed DB plugin contracts; native registration runtime NOT_RUN: MDBX disabled\n";
   }
   return result;
}
