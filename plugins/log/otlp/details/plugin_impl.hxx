#pragma once

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <memory>
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
   std::shared_ptr<forge::otlp::log_exporter> exporter;
   std::shared_ptr<forge::otlp::log_sink> sink;
   forge::otlp::crash_guard crash_guard;
   std::vector<attached_logger> attached_loggers;
   bool started = false;
   bool stopping = false;

   [[nodiscard]] bool available() const noexcept;
   [[nodiscard]] metrics current_metrics() const;
   boost::asio::awaitable<void> flush();
   void detach_sink() noexcept;
};

} // namespace forge::plugins::log::otlp
