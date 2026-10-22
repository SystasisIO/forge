#include "bootstrap_abi.hxx"

#include <iostream>
#include <new>
#include <stdexcept>

int main() {
   auto value = bootstrap_abi::make();
   const std::string input(257, 'a');
   if (value->exchange(input) != input + ":libc++220108" || typeid(*value) != bootstrap_abi::dynamic_type())
      return 1;
   value.reset();
   if (bootstrap_abi::destroyed() != 1)
      return 2;
   try {
      bootstrap_abi::throw_bad_alloc();
      return 3;
   } catch (const std::bad_alloc& error) {
      if (typeid(error) != typeid(std::bad_alloc))
         return 4;
   }
   try {
      bootstrap_abi::throw_runtime_error();
      return 5;
   } catch (const std::runtime_error& error) {
      if (typeid(error) != typeid(std::runtime_error) || std::string(error.what()) != "bootstrap-typed-error")
         return 6;
   }
   std::cout << "READY: stage0 DSO/final compiler string, RTTI, exceptions, virtual destruction\n";
}
