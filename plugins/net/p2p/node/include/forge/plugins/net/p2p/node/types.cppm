module;

#include <boost/describe.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <vector>

export module forge.plugins.net.p2p.node.types;

import forge.api.transport.options;
import forge.net.p2p.identity;
import forge.net.p2p.endpoint;
import forge.net.p2p.pubsub;

export namespace forge::plugins::net::p2p::node {

struct info {
   forge::net::p2p::peer_id local_peer;
   std::vector<forge::net::p2p::endpoint> local_endpoints;
   bool started = false;
};

struct remote_options {
   std::chrono::milliseconds open_deadline{10'000};
   std::optional<forge::api::core::codec_id> codec;
   std::optional<std::size_t> max_inflight;
   std::optional<std::chrono::milliseconds> deadline;
   std::optional<std::uint32_t> max_frame_size;
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

struct pubsub_snapshot {
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

struct peer_filter {
   std::optional<forge::net::p2p::peer_id> peer;
   bool only_connected = false;
   bool only_protected = false;
   std::uint64_t limit = 0;
};

BOOST_DESCRIBE_STRUCT(publish_options, (), (sign))
BOOST_DESCRIBE_STRUCT(subscribe_options, (), (handler_deadline))
BOOST_DESCRIBE_STRUCT(message, (), (source, author, subject, data, seqno))
BOOST_DESCRIBE_STRUCT(subscription, (), (id, subject))
BOOST_DESCRIBE_STRUCT(pubsub_snapshot, (),
                      (topics, subscriptions, active_handlers, messages_published, messages_delivered,
                       messages_accepted, messages_rejected, messages_ignored, messages_retried, messages_dropped,
                       handler_failures, core))
BOOST_DESCRIBE_STRUCT(peer_filter, (), (peer, only_connected, only_protected, limit))

} // namespace forge::plugins::net::p2p::node
