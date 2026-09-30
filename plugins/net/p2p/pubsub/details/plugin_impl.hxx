#pragma once

#include "topic_state.hxx"

namespace forge::plugins::net::p2p::pubsub {

struct plugin::impl : public std::enable_shared_from_this<plugin::impl> {
   config settings;
   std::shared_ptr<forge::plugins::net::p2p::node::pubsub_source> source;
   std::map<std::string, topic_state> topics;
   std::uint64_t next_subscription = 1;
   std::size_t active_handlers = 0;
   std::uint64_t messages_published = 0;
   std::uint64_t messages_delivered = 0;
   std::uint64_t messages_accepted = 0;
   std::uint64_t messages_rejected = 0;
   std::uint64_t messages_ignored = 0;
   std::uint64_t messages_retried = 0;
   std::uint64_t messages_dropped = 0;
   std::uint64_t handler_failures = 0;
   mutable std::mutex mutex;
   bool initialized = false;
   bool stopping = false;

   [[nodiscard]] forge::plugins::net::p2p::node::pubsub_source& require_source() const;
   void ensure_topic_allowed(const forge::net::p2p::pubsub::topic& subject) const;
   [[nodiscard]] std::vector<handler_record> handlers_for(const std::string& topic) const;
   [[nodiscard]] bool try_begin_handler();
   void finish_handler();
   void record_handler_failure();
   void record_drop();
   boost::asio::awaitable<forge::net::p2p::pubsub::validation_result>
   call_handler(handler_record handler, message value);
   boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> handle_event(forge::net::p2p::pubsub::event event);
};

} // namespace forge::plugins::net::p2p::pubsub
