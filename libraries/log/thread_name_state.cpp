module;

#define BOOST_DLL_USE_STD_FS
#include <boost/dll/runtime_symbol_info.hpp>
#include <string>

module forge.log.record;

#include "details/thread_name_state.hxx"

namespace forge::detail {

thread_local bool thread_name_state::_retired = false;

thread_name_state::~thread_name_state() {
   _retired = true;
}

thread_name_state& thread_name_state::current() {
   static thread_local auto state = thread_name_state{};
   return state;
}

void thread_name_state::set(const std::string& name) {
   if (!_retired) {
      current()._value = name;
   }
}

const std::string& thread_name_state::get() {
   if (_retired) {
      // One process-lifetime fallback, not one leaked allocation per thread.
      static const auto* name = new std::string{"shutdown"};
      return *name;
   }
   auto& name = current()._value;
   if (name.empty()) {
      try {
         name = boost::dll::program_location().filename().generic_string();
      } catch (...) {
         name = "unknown";
      }
   }
   return name;
}

} // namespace forge::detail
