#include <forge/exceptions/macros.hpp>
#include <forge/log/macros.hpp>

#include <memory>
#include <chrono>
#include <string>

import forge.asio.blocking;
import forge.asio.compute;
import forge.asio.runtime;
import forge.app.application;
import forge.app.service_registry;
import forge.crypto.digest.sha256;
import forge.exceptions;
import forge.log.record;
import forge.log.async_sink;
import forge.log.logger;
import forge.raw.raw;

class capture_sink final : public forge::sink {
 public:
   void log(const forge::log_record& record) override {
      last_message = record.message;
   }

   std::string last_message;
};

struct package_service {
   int value = 0;
};

int main() {
   auto logger = forge::logger{"consumer"};
   logger.set_log_level(forge::log_level::debug);
   auto sink = std::make_shared<capture_sink>();
   auto workers = forge::asio::compute::pool{{.worker_threads = 1}};
   auto runtime = forge::asio::runtime{{.worker_threads = 1}};
   auto delivery = std::make_shared<forge::async_sink>(sink, workers.get_executor());
   logger.add_sink(delivery);
   ilog(logger, "package works", ("component", "smoke"));
   const auto drained = forge::asio::blocking::run(
       runtime, delivery->async_shutdown(std::chrono::seconds{2}));
   forge::asio::blocking::run(runtime, workers.shutdown());

   const auto digest = forge::crypto::digest::sha256::hash(std::string{"package works"});
   const auto bytes = forge::raw::pack(std::string{digest});
   FORGE_ASSERT(!bytes.empty(), "raw pack should produce bytes", forge::exceptions::ctx("size", bytes.size()));

   auto services = forge::app::service_registry{};
   services.publish(std::make_shared<package_service>(package_service{.value = 42}));
   services.close();
   FORGE_ASSERT(services.view().get<package_service>()->value == 42,
                "installed App service registry should preserve exact types");

   return drained && sink->last_message == "package works" ? 0 : 1;
}
