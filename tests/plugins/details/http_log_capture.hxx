#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace forge::tests::plugins {

class http_capture_sink final : public forge::sink {
 public:
   void log(const forge::log_record& record) override {
      const auto lock = std::scoped_lock{mutex_};
      records_.push_back(record);
   }
   std::vector<forge::log_record> snapshot() const {
      const auto lock = std::scoped_lock{mutex_};
      return records_;
   }
 private:
   mutable std::mutex mutex_;
   std::vector<forge::log_record> records_;
};

class http_throwing_sink final : public forge::sink {
 public:
   void log(const forge::log_record&) override {
      ++calls;
      throw std::runtime_error{"http-sink-secret-marker"};
   }
   std::atomic_uint calls = 0;
};

struct http_log_capture {
   explicit http_log_capture(const char* name = "forge.plugins.net.http.server", bool fail = false)
       : route{forge::logger::get(name)}, previous{route.get_log_level()} {
      route.set_log_level(forge::log_level::debug);
      route.add_sink(captured);
      if (fail) {
         route.add_sink(throwing);
      }
   }
   ~http_log_capture() {
      route.remove_sink(captured);
      route.remove_sink(throwing);
      route.set_log_level(previous);
   }
   std::size_t count(std::string_view event, forge::log_level level) const {
      const auto records = captured->snapshot();
      return std::ranges::count_if(records, [&](const auto& record) {
         return record.level == level && std::ranges::any_of(record.fields, [&](const auto& field) {
            return field.key == "event" && field.value == event;
         });
      });
   }
   forge::logger route;
   forge::log_level previous;
   std::shared_ptr<http_capture_sink> captured = std::make_shared<http_capture_sink>();
   std::shared_ptr<http_throwing_sink> throwing = std::make_shared<http_throwing_sink>();
};

} // namespace forge::tests::plugins
