#include <forge/log/macros.hpp>
#include <chrono>
#include <memory>
#include <string>

import forge.asio.blocking;
import forge.asio.compute;
import forge.asio.runtime;
import forge.log.async_sink;
import forge.log.logger;
import forge.log.record;

class capture_sink final : public forge::sink {
 public:
   void log(const forge::log_record& record) override { message = record.message; }
   std::string message;
};

int main() {
   auto runtime = forge::asio::runtime{};
   auto workers = forge::asio::compute::pool{{.worker_threads = 1}};
   auto output = std::make_shared<capture_sink>();
   auto delivery = std::make_shared<forge::async_sink>(output, workers.get_executor());
   auto route = forge::logger{"installed-log"};
   route.set_log_level(forge::log_level::info);
   route.add_sink(delivery);
   ilog(route, "component-only asynchronous log");
   const auto flushed = forge::asio::blocking::run(
       runtime, delivery->async_shutdown(std::chrono::seconds{2}));
   forge::asio::blocking::run(runtime, workers.shutdown());
   return flushed && output->message == "component-only asynchronous log" ? 0 : 1;
}
