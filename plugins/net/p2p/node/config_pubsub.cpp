module;

#include <boost/describe.hpp>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <vector>

module forge.plugins.net.p2p.node.config;

import forge.schema.object;

forge::schema::object_schema<forge::plugins::net::p2p::node::pubsub_config>
forge::schema::rules<forge::plugins::net::p2p::node::pubsub_config>::define() {
   using config = forge::plugins::net::p2p::node::pubsub_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::max_topics>("max-topics").default_value(std::uint64_t{1'024}).range(1, 1'000'000);
   schema.field<&config::max_handlers_per_topic>("max-handlers-per-topic")
       .default_value(std::uint64_t{64}).range(1, 1'000'000);
   schema.field<&config::max_active_handlers>("max-active-handlers")
       .default_value(std::uint64_t{4'096}).range(1, 1'000'000);
   schema.field<&config::max_message_size>("max-message-size")
       .default_value(std::uint64_t{1024 * 1024}).range(1, 1024 * 1024 * 1024)
       .description("Consumer payload bytes; native message envelope and RPC framing budgets are derived");
   schema.field<&config::handler_deadline_ms>("handler-deadline-ms")
       .default_value(std::uint64_t{5'000}).range(1, 86'400'000);
   schema.field<&config::allowed_topics>("allowed-topics").default_value(std::vector<std::string>{}).each_non_empty();
   schema.field<&config::denied_topics>("denied-topics").default_value(std::vector<std::string>{}).each_non_empty();
   schema.field<&config::sign_publishes>("sign-publishes").default_value(true);
   schema.field<&config::partial_messages>("partial-messages").default_value(false);
   schema.field<&config::enabled>("enabled").default_value(false);
   schema.field<&config::preferred_version>("preferred-version");
   schema.field<&config::allow_v1_0_fallback>("allow-v1-0-fallback");
   schema.field<&config::signature_policy>("signature-policy")
       .description("Receive policy; strict modes must agree with sign-publishes");
   schema.field<&config::flood_publish>("flood-publish");
   schema.field<&config::peer_exchange>("peer-exchange");
   schema.field<&config::wire>("wire").default_value(config::wire_config{});
   schema.field<&config::validation>("validation").default_value(config::validation_config{});
   schema.field<&config::idontwant>("idontwant").default_value(config::idontwant_config{});
   schema.field<&config::partial>("partial").default_value(config::partial_config{});
   schema.field<&config::scoring>("scoring");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::pubsub_config::validation_config>
forge::schema::rules<forge::plugins::net::p2p::node::pubsub_config::validation_config>::define() {
   using config = forge::plugins::net::p2p::node::pubsub_config::validation_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::max_attempts>("max-attempts");
   schema.field<&config::max_redeliveries>("max-redeliveries");
   schema.field<&config::max_requests>("max-requests");
   schema.field<&config::retry_initial_delay_ms>("retry-initial-delay-ms");
   schema.field<&config::retry_max_delay_ms>("retry-max-delay-ms");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::pubsub_config::idontwant_config>
forge::schema::rules<forge::plugins::net::p2p::node::pubsub_config::idontwant_config>::define() {
   using config = forge::plugins::net::p2p::node::pubsub_config::idontwant_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::threshold>("threshold");
   schema.field<&config::ttl_heartbeats>("ttl-heartbeats").description("Retention in native heartbeats, not seconds");
   schema.field<&config::max_entries_per_peer>("max-entries-per-peer");
   schema.field<&config::max_bytes_per_peer>("max-bytes-per-peer");
   schema.field<&config::max_entries>("max-entries");
   schema.field<&config::max_bytes>("max-bytes");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::pubsub_config::partial_config>
forge::schema::rules<forge::plugins::net::p2p::node::pubsub_config::partial_config>::define() {
   using config = forge::plugins::net::p2p::node::pubsub_config::partial_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::max_groups>("max-groups");
   schema.field<&config::max_groups_per_topic>("max-groups-per-topic");
   schema.field<&config::max_group_bytes>("max-group-bytes").description("Aggregate retained group ID bytes");
   schema.field<&config::max_metadata_size>("max-metadata-size");
   schema.field<&config::ttl_heartbeats>("ttl-heartbeats").description("Retention in native heartbeats, not seconds");
   schema.field<&config::max_callbacks>("max-callbacks");
   schema.field<&config::max_callback_bytes>("max-callback-bytes");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::pubsub_config::scoring_config::topic_config>
forge::schema::rules<forge::plugins::net::p2p::node::pubsub_config::scoring_config::topic_config>::define() {
   using config = forge::plugins::net::p2p::node::pubsub_config::scoring_config::topic_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::topic>("topic").required().non_empty();
   schema.field<&config::topic_weight>("topic-weight");
   schema.field<&config::time_in_mesh_weight>("time-in-mesh-weight");
   schema.field<&config::time_in_mesh_quantum_ms>("time-in-mesh-quantum-ms");
   schema.field<&config::time_in_mesh_cap>("time-in-mesh-cap");
   schema.field<&config::first_message_deliveries_weight>("first-message-deliveries-weight");
   schema.field<&config::first_message_deliveries_decay>("first-message-deliveries-decay");
   schema.field<&config::first_message_deliveries_cap>("first-message-deliveries-cap");
   schema.field<&config::mesh_message_deliveries_weight>("mesh-message-deliveries-weight");
   schema.field<&config::mesh_message_deliveries_decay>("mesh-message-deliveries-decay");
   schema.field<&config::mesh_message_deliveries_cap>("mesh-message-deliveries-cap");
   schema.field<&config::mesh_message_deliveries_threshold>("mesh-message-deliveries-threshold");
   schema.field<&config::mesh_message_deliveries_window_ms>("mesh-message-deliveries-window-ms");
   schema.field<&config::mesh_message_deliveries_activation_ms>("mesh-message-deliveries-activation-ms");
   schema.field<&config::mesh_failure_penalty_weight>("mesh-failure-penalty-weight");
   schema.field<&config::mesh_failure_penalty_decay>("mesh-failure-penalty-decay");
   schema.field<&config::invalid_message_deliveries_weight>("invalid-message-deliveries-weight");
   schema.field<&config::invalid_message_deliveries_decay>("invalid-message-deliveries-decay");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::pubsub_config::scoring_config::thresholds_config>
forge::schema::rules<forge::plugins::net::p2p::node::pubsub_config::scoring_config::thresholds_config>::define() {
   using config = forge::plugins::net::p2p::node::pubsub_config::scoring_config::thresholds_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::gossip_threshold>("gossip-threshold");
   schema.field<&config::publish_threshold>("publish-threshold");
   schema.field<&config::graylist_threshold>("graylist-threshold");
   schema.field<&config::accept_px_threshold>("accept-px-threshold");
   schema.field<&config::opportunistic_graft_threshold>("opportunistic-graft-threshold");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::pubsub_config::scoring_config::limits_config>
forge::schema::rules<forge::plugins::net::p2p::node::pubsub_config::scoring_config::limits_config>::define() {
   using config = forge::plugins::net::p2p::node::pubsub_config::scoring_config::limits_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::max_connected_peers>("max-connected-peers");
   schema.field<&config::max_retained_peers>("max-retained-peers");
   schema.field<&config::max_messages>("max-messages");
   schema.field<&config::max_delivery_peers>("max-delivery-peers");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::pubsub_config::scoring_config>
forge::schema::rules<forge::plugins::net::p2p::node::pubsub_config::scoring_config>::define() {
   using config = forge::plugins::net::p2p::node::pubsub_config::scoring_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::enabled>("enabled").default_value(false);
   schema.field<&config::topics>("topics").items<config::topic_config>().default_value(std::vector<config::topic_config>{});
   schema.field<&config::topic_score_cap>("topic-score-cap");
   schema.field<&config::ip_colocation_factor_weight>("ip-colocation-factor-weight");
   schema.field<&config::ip_colocation_factor_threshold>("ip-colocation-factor-threshold");
   schema.field<&config::ip_colocation_factor_allowlist>("ip-colocation-factor-allowlist").each_non_empty();
   schema.field<&config::behaviour_penalty_weight>("behaviour-penalty-weight");
   schema.field<&config::behaviour_penalty_threshold>("behaviour-penalty-threshold");
   schema.field<&config::behaviour_penalty_decay>("behaviour-penalty-decay");
   schema.field<&config::decay_interval_ms>("decay-interval-ms");
   schema.field<&config::decay_to_zero>("decay-to-zero");
   schema.field<&config::retain_score_ms>("retain-score-ms");
   schema.field<&config::seen_message_ttl_ms>("seen-message-ttl-ms");
   schema.field<&config::thresholds>("thresholds").default_value(config::thresholds_config{});
   schema.field<&config::limits>("limits").default_value(config::limits_config{});
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::pubsub_config::wire_config>
forge::schema::rules<forge::plugins::net::p2p::node::pubsub_config::wire_config>::define() {
   using config = forge::plugins::net::p2p::node::pubsub_config::wire_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::max_rpc_size>("max-rpc-size")
       .description("Unframed RPC bytes; an explicit value must fit the derived single-message envelope");
   schema.field<&config::max_outbound_queue_bytes>("max-outbound-queue-bytes");
   schema.field<&config::max_remote_topics_per_peer>("max-remote-topics-per-peer");
   schema.field<&config::max_remote_topic_entries>("max-remote-topic-entries");
   schema.field<&config::max_iwant_promises>("max-iwant-promises");
   return schema;
}
