#include <aot.h>
#include <atomic>
#include <barrier>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <thread>

int main() {
   std::barrier rendezvous{2};
   std::atomic<bool> valid{true};
   const auto worker = [&](const char* label) {
      for (unsigned round = 0; round != 64; ++round) {
         char expected[128];
         std::snprintf(expected, sizeof(expected), "%s-%u", label, round);
         aot_set_last_error_v("%s", expected);
         rendezvous.arrive_and_wait();
         if (std::strcmp(aot_get_last_error(), expected) != 0)
            valid = false;
         rendezvous.arrive_and_wait();
      }
   };
   aot_set_last_error("caller-diagnostic");
   std::jthread first{worker, "first"};
   try {
      std::jthread second{worker, "second"};
      first.join();
      second.join();
   } catch (...) {
      rendezvous.arrive_and_drop();
      first.join();
      throw;
   }
   if (!valid || std::strcmp(aot_get_last_error(), "Error: caller-diagnostic") != 0) {
      std::cerr << "Compiler diagnostics leaked across caller threads\n";
      return 1;
   }
   std::cout << "Compiler diagnostics isolated across two workers and caller, 64 rounds\n";
   return 0;
}
