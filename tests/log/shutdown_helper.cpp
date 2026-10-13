#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <forge/log/macros.hpp>

import forge.log.logger;
import forge.log.record;

namespace {

class failure_sink final : public forge::sink {
 public:
   void log(const forge::log_record&) override {
      throw std::runtime_error{"shutdown-secret-must-not-escape"};
   }
};

class counting_sink final : public forge::sink {
 public:
   void log(const forge::log_record& record) override {
      ++calls;
      last_thread_name = record.thread_name;
   }
   unsigned calls = 0;
   std::string last_thread_name;
};

struct shutdown_probe {
   forge::logger route{"shutdown.probe"};
   std::shared_ptr<counting_sink> sink = std::make_shared<counting_sink>();

   shutdown_probe() {
      route.set_log_level(forge::log_level::info);
      route.add_sink(std::make_shared<failure_sink>());
      route.add_sink(sink);
   }

   void emit() {
      ilog(route, "shutdown probe");
   }

   ~shutdown_probe() {
      emit();
      if (sink->calls != 2 || sink->last_thread_name != "shutdown") {
         std::_Exit(2);
      }
      std::fputs("static destructor delivered\n", stdout);
   }
};

} // namespace

int main() {
   static auto probe = shutdown_probe{};
   forge::set_thread_name(std::string(256, 'x'));
   probe.emit();
}
