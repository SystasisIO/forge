module;

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

module forge.app.events;

#include "details/event_journal_impl.hxx"

namespace forge::app {

event_journal::event_journal(event_journal_options options) : impl_{std::make_shared<impl>()} {
   impl_->options = options;
}
event_journal::~event_journal() = default;

void event_journal::publish(event_severity severity, std::string topic, std::string message) noexcept {
   try {
      auto lock = std::scoped_lock{impl_->mutex};
      ++impl_->metrics.published;
      const auto limit = impl_->options.max_record_bytes;
      if (topic.size() > limit || message.size() > limit - topic.size()) {
         ++impl_->metrics.dropped;
         return;
      }
      auto record = event_record{
         .id = impl_->next_id++,
         .time = std::chrono::system_clock::now(),
         .severity = severity,
         .topic = std::move(topic),
         .message = std::move(message),
      };
      if (severity == event_severity::critical && impl_->options.max_critical_events != 0) {
         impl_->critical.push_back(record);
         if (impl_->critical.size() > impl_->options.max_critical_events) {
            impl_->critical.pop_front();
         }
      }
      if (impl_->options.max_recent_events != 0) {
         impl_->recent.push_back(std::move(record));
         if (impl_->recent.size() > impl_->options.max_recent_events) {
            impl_->recent.pop_front();
         }
      }
   } catch (...) {
      // Diagnostic history must never prevent resource cleanup.
   }
}

std::vector<event_record> event_journal::critical_events() const {
   auto lock = std::scoped_lock{impl_->mutex};
   return {impl_->critical.begin(), impl_->critical.end()};
}

std::vector<event_record> event_journal::recent_events() const {
   auto lock = std::scoped_lock{impl_->mutex};
   return {impl_->recent.begin(), impl_->recent.end()};
}

event_journal_metrics event_journal::metrics() const {
   auto lock = std::scoped_lock{impl_->mutex};
   return impl_->metrics;
}

} // namespace forge::app
