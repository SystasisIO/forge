#pragma once

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "attached_logger.hxx"

namespace forge::plugins::log::otlp {

struct plugin::impl {
   config settings;
   forge::asio::runtime* runtime = nullptr;
   std::function<boost::asio::awaitable<std::vector<std::pair<std::string, forge::crypto::core::secret_bytes>>>(
       const std::vector<header>&)>
       resolve_headers;
   std::shared_ptr<forge::otlp::log_sink> sink;
   forge::otlp::crash_guard crash_guard;
   std::vector<attached_logger> attached_loggers;
   bool started = false;
   [[nodiscard]] std::shared_ptr<forge::otlp::log_exporter> admit_exporter();
   void release_call() noexcept;
   boost::asio::awaitable<void> wait_for_calls();
   void publish_exporter(std::shared_ptr<forge::otlp::log_exporter> value);
   [[nodiscard]] std::shared_ptr<forge::otlp::log_exporter> take_exporter() noexcept;
   void reset_admission() noexcept;
   void clear_exporter() noexcept;
   [[nodiscard]] static metrics current_metrics(const forge::otlp::exporter_metrics& snapshot);
   void detach_sink() noexcept;

 private:
   mutable std::mutex admission_mutex;
   std::shared_ptr<forge::otlp::log_exporter> exporter;
   bool shutting_down = false;
   std::size_t admitted_calls = 0;
   forge::asio::notification calls_changed;
};

} // namespace forge::plugins::log::otlp
