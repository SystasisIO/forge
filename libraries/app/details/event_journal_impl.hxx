#pragma once

namespace forge::app {

struct event_journal::impl {
   event_journal_options options;
   mutable std::mutex mutex;
   std::uint64_t next_id = 1;
   std::deque<event_record> critical;
   std::deque<event_record> recent;
   event_journal_metrics metrics;
};

} // namespace forge::app
