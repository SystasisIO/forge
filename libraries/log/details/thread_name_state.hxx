#pragma once

#include <string>

namespace forge::detail {

class thread_name_state {
 public:
   static void set(const std::string& name);
   static const std::string& get();

 private:
   ~thread_name_state();
   static thread_name_state& current();

   std::string _value;
   static thread_local bool _retired;
};

} // namespace forge::detail
