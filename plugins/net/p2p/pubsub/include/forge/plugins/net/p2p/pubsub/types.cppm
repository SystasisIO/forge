module;

#include <boost/describe.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <vector>

export module forge.plugins.net.p2p.pubsub.types;

import forge.net.p2p.identity;
import forge.net.p2p.pubsub;
import forge.schema.diagnostic;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.enums;

export namespace forge::plugins::net::p2p::pubsub {

struct config {
   std::uint64_t max_topics = 1'024;
   std::uint64_t max_handlers_per_topic = 64;
   std::uint64_t max_active_handlers = 4'096;
   std::uint64_t max_message_size = 1024 * 1024;
   std::uint64_t handler_deadline_ms = 5'000;
   std::vector<std::string> allowed_topics;
   std::vector<std::string> denied_topics;
   bool sign_publishes = true;
   bool partial_messages = false;
};

struct publish_options {
   std::optional<bool> sign;
};

struct subscribe_options {
   std::chrono::milliseconds handler_deadline{0};
};

struct message {
   forge::net::p2p::peer_id source;
   std::optional<forge::net::p2p::peer_id> author;
   forge::net::p2p::pubsub::topic subject;
   std::vector<std::uint8_t> data;
   std::vector<std::uint8_t> seqno;
};

template <typename T> struct typed_message {
   forge::net::p2p::peer_id source;
   std::optional<forge::net::p2p::peer_id> author;
   forge::net::p2p::pubsub::topic subject;
   T value;
   std::vector<std::uint8_t> seqno;
};

struct subscription {
   std::uint64_t id = 0;
   forge::net::p2p::pubsub::topic subject;
};

struct snapshot {
   std::size_t topics = 0;
   std::size_t subscriptions = 0;
   std::size_t active_handlers = 0;
   std::uint64_t messages_published = 0;
   std::uint64_t messages_delivered = 0;
   std::uint64_t messages_accepted = 0;
   std::uint64_t messages_rejected = 0;
   std::uint64_t messages_ignored = 0;
   std::uint64_t messages_retried = 0;
   std::uint64_t messages_dropped = 0;
   std::uint64_t handler_failures = 0;
   forge::net::p2p::pubsub::snapshot core;
};

BOOST_DESCRIBE_STRUCT(config, (),
                      (max_topics, max_handlers_per_topic, max_active_handlers, max_message_size, handler_deadline_ms,
                       allowed_topics, denied_topics, sign_publishes, partial_messages))
BOOST_DESCRIBE_STRUCT(publish_options, (), (sign))
BOOST_DESCRIBE_STRUCT(subscribe_options, (), (handler_deadline))
BOOST_DESCRIBE_STRUCT(message, (), (source, author, subject, data, seqno))
BOOST_DESCRIBE_STRUCT(subscription, (), (id, subject))
BOOST_DESCRIBE_STRUCT(snapshot, (),
                      (topics, subscriptions, active_handlers, messages_published, messages_delivered,
                       messages_accepted, messages_rejected, messages_ignored, messages_retried, messages_dropped,
                       handler_failures, core))

} // namespace forge::plugins::net::p2p::pubsub

export template <> struct forge::schema::rules<forge::plugins::net::p2p::pubsub::config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::pubsub::config> define();
};
