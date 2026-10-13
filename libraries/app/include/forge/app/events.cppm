module;

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

export module forge.app.events;

export namespace forge::app {

enum class event_severity : std::uint8_t {
   debug,
   info,
   warning,
   error,
   critical,
};

struct event_record {
   std::uint64_t id = 0;
   std::chrono::system_clock::time_point time;
   event_severity severity = event_severity::info;
   std::string topic;
   std::string message;
};

struct event_journal_options {
   std::uint64_t max_critical_events = 128;
   std::uint64_t max_recent_events = 128;
   std::uint64_t max_record_bytes = 4096;
};

struct event_journal_metrics {
   std::uint64_t published = 0;
   std::uint64_t dropped = 0;
};

class event_journal {
 public:
   explicit event_journal(event_journal_options options = {});
   ~event_journal();

   event_journal(const event_journal&) = delete;
   event_journal& operator=(const event_journal&) = delete;

   void publish(event_severity severity, std::string topic, std::string message) noexcept;
   [[nodiscard]] std::vector<event_record> critical_events() const;
   [[nodiscard]] std::vector<event_record> recent_events() const;
   [[nodiscard]] event_journal_metrics metrics() const;

 private:
   struct impl;
   std::shared_ptr<impl> impl_;
};

} // namespace forge::app
