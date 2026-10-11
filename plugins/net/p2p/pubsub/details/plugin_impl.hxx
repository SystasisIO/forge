#pragma once

#include "topic_state.hxx"

namespace forge::plugins::net::p2p::pubsub {

struct plugin::impl : public std::enable_shared_from_this<plugin::impl> {
   config settings;
   std::shared_ptr<forge::plugins::net::p2p::node::pubsub_source> source;
   std::map<std::string, std::shared_ptr<topic_state>> topics;
   forge::asio::notification changed;
   std::stop_source partial_stop;
   std::size_t active_operations = 0;
   std::size_t active_events = 0;
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
   bool initializing = false;
   bool stopping = false;
   bool shutdown_running = false;
   bool shutdown_done = false;
   std::exception_ptr shutdown_error;

   [[nodiscard]] std::shared_ptr<forge::plugins::net::p2p::node::pubsub_source> require_source_locked() const;
   [[nodiscard]] std::shared_ptr<forge::plugins::net::p2p::node::pubsub_source> begin_operation();
   void finish_operation() noexcept;
   void finish_topic_operation(const std::shared_ptr<topic_state>& topic,
                               std::shared_ptr<handler_record>& record, bool subscribing) noexcept;
   void finish_partial_operation(const std::shared_ptr<topic_state>& topic,
                                 std::shared_ptr<partial_record>& record) noexcept;
   [[nodiscard]] std::shared_ptr<partial_record> require_partial_locked(
       const forge::net::p2p::pubsub::partial_topic& token, bool cleanup = false) const;
   [[nodiscard]] std::shared_ptr<partial_record> admit_partial_event_locked(const std::weak_ptr<partial_record>& expected,
       const forge::net::p2p::pubsub::partial_topic& token);
   static forge::net::p2p::pubsub::handler full_dispatch(std::weak_ptr<impl> self);
   static boost::asio::awaitable<void> receive_partial_owned(std::weak_ptr<impl> owner,
       std::weak_ptr<partial_record> expected, forge::net::p2p::pubsub::partial_event event, std::stop_token native);
   static boost::asio::awaitable<void> gossip_partial_owned(std::weak_ptr<impl> owner,
       std::weak_ptr<partial_record> expected, forge::net::p2p::pubsub::partial_gossip_event event, std::stop_token native);
   static boost::asio::awaitable<void> restore_full_owned(std::shared_ptr<impl> self,
       std::shared_ptr<topic_state> topic, std::shared_ptr<forge::plugins::net::p2p::node::pubsub_source> source);
   void ensure_topic_allowed_locked(const forge::net::p2p::pubsub::topic& subject) const;
   void request_stop() noexcept;
   void configure(config value);
   void initialize(std::shared_ptr<forge::plugins::net::p2p::node::pubsub_source> value);
   static boost::asio::awaitable<void> ready_owned(std::shared_ptr<impl> self);
   static boost::asio::awaitable<void> shutdown_owned(std::shared_ptr<impl> self);
   static boost::asio::awaitable<void> shutdown_joined(std::shared_ptr<impl> self);
   [[nodiscard]] bool try_begin_handler();
   void finish_handler() noexcept;
   void record_handler_failure();
   boost::asio::awaitable<forge::net::p2p::pubsub::validation_result>
   call_handler(std::shared_ptr<handler_record> handler, message value);
   static boost::asio::awaitable<forge::net::p2p::pubsub::validation_result>
   handle_event_owned(std::shared_ptr<impl> self, forge::net::p2p::pubsub::event event);
};

} // namespace forge::plugins::net::p2p::pubsub
