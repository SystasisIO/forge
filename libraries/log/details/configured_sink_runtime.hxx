#pragma once

#include <boost/asio/awaitable.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace forge::detail {

class configured_sink_runtime {
 public:
   class configuration_operation;

   configured_sink_runtime();
   ~configured_sink_runtime();
   static configured_sink_runtime& get();
   [[nodiscard]] static bool terminated() noexcept;
   [[nodiscard]] configuration_operation begin_configuration(const std::vector<sink_config>& config);
   boost::asio::awaitable<bool> flush(std::chrono::milliseconds timeout);
   boost::asio::awaitable<bool> shutdown(std::chrono::milliseconds timeout);
   [[nodiscard]] async_sink_metrics snapshot();

 private:
   void finish_configuration(std::size_t reserved) noexcept;
   static boost::asio::awaitable<bool> shutdown_owned(
       configured_sink_runtime* owner, std::vector<boost::asio::awaitable<bool>> waits,
       std::chrono::steady_clock::time_point deadline);

   static constexpr std::size_t max_retained_sinks = 64;
   std::mutex _mutex;
   std::vector<std::shared_ptr<async_sink>> _sinks;
   std::size_t _reserved = 0;
   std::size_t _active_configurations = 0;
   bool _stopping = false;
   async_sink_metrics _retired;
   forge::asio::notification _changed;
   forge::asio::compute::pool _workers;
};

class configured_sink_runtime::configuration_operation {
 public:
   ~configuration_operation();
   configuration_operation(const configuration_operation&) = delete;
   configuration_operation& operator=(const configuration_operation&) = delete;
   configuration_operation(configuration_operation&&) = delete;
   configuration_operation& operator=(configuration_operation&&) = delete;

   std::unordered_map<std::string, std::shared_ptr<sink>> prepare(const std::vector<sink_config>& config);
   // Hold this through the registry swap; only publication, never output
   // construction or retired-route destruction, runs inside the stop boundary.
   [[nodiscard]] std::unique_lock<std::mutex> acquire_publication();

 private:
   explicit configuration_operation(configured_sink_runtime& owner, std::size_t count);

   configured_sink_runtime& _owner;
   std::size_t _reserved;
   std::vector<std::shared_ptr<async_sink>> _prepared;
   bool _published = false;

   friend class configured_sink_runtime;
};

} // namespace forge::detail
