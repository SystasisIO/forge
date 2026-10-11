module;

#include <boost/describe.hpp>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <vector>

module forge.plugins.net.p2p.pubsub.types;

import forge.schema.object;

forge::schema::object_schema<forge::plugins::net::p2p::pubsub::config>
forge::schema::rules<forge::plugins::net::p2p::pubsub::config>::define() {
   auto schema = forge::schema::object<forge::plugins::net::p2p::pubsub::config>();
   schema.field<&forge::plugins::net::p2p::pubsub::config::max_topics>("max-topics")
      .default_value(std::uint64_t{1'024})
      .range(1, 1'000'000);
   schema.field<&forge::plugins::net::p2p::pubsub::config::max_handlers_per_topic>("max-handlers-per-topic")
      .default_value(std::uint64_t{64})
      .range(1, 1'000'000);
   schema.field<&forge::plugins::net::p2p::pubsub::config::max_active_handlers>("max-active-handlers")
      .default_value(std::uint64_t{4'096})
      .range(1, 1'000'000);
   schema.field<&forge::plugins::net::p2p::pubsub::config::max_message_size>("max-message-size")
      .default_value(std::uint64_t{1024 * 1024})
      .range(1, 1024 * 1024 * 1024);
   schema.field<&forge::plugins::net::p2p::pubsub::config::handler_deadline_ms>("handler-deadline-ms")
      .default_value(std::uint64_t{5'000})
      .range(1, 86'400'000);
   schema.field<&forge::plugins::net::p2p::pubsub::config::allowed_topics>("allowed-topics")
      .default_value(std::vector<std::string>{})
      .each_non_empty();
   schema.field<&forge::plugins::net::p2p::pubsub::config::denied_topics>("denied-topics")
      .default_value(std::vector<std::string>{})
      .each_non_empty();
   schema.field<&forge::plugins::net::p2p::pubsub::config::sign_publishes>("sign-publishes").default_value(true);
   schema.field<&forge::plugins::net::p2p::pubsub::config::partial_messages>("partial-messages").default_value(false);
   return schema;
}
