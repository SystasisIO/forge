#include <forge/log/macros.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <memory>
#include <string>

import forge.asio.compute;
import forge.log.async_sink;
import forge.log.logger;
import forge.log.record;

class capture_sink final : public forge::sink {
 public:
   void log(const forge::log_record& record) override { message = record.message; }
   std::string message;
};

boost::asio::awaitable<bool> finish(forge::async_sink& delivery, forge::asio::compute::pool& workers) {
   const auto drained = co_await delivery.async_shutdown(std::chrono::seconds{2});
   co_await workers.shutdown();
   co_return drained;
}

int main() {
   auto runtime = boost::asio::io_context{};
   auto workers = forge::asio::compute::pool{{.worker_threads = 1}};
   auto output = std::make_shared<capture_sink>();
   auto delivery = std::make_shared<forge::async_sink>(output, workers.get_executor());
   auto route = forge::logger{"installed-log"};
   route.set_log_level(forge::log_level::info);
   route.add_sink(delivery);
   ilog(route, "component-only asynchronous log");
   auto completed = boost::asio::co_spawn(runtime, finish(*delivery, workers), boost::asio::use_future);
   runtime.run();
   return completed.get() && output->message == "component-only asynchronous log" ? 0 : 1;
}
