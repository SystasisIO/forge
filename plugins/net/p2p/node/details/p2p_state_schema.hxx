#pragma once

namespace forge::plugins::net::p2p::node::detail {

struct p2p_state_schema {
   static void register_objects(const forge::plugins::db::store::store_handle& store);
   static boost::asio::awaitable<void> async_prepare(forge::plugins::db::store::api* db,
                                                     forge::plugins::db::store::store_handle store,
                                                     bool reset_incompatible_cache);

   static constexpr std::uint32_t format_version = 3;

   // These IDs are part of the private persisted format and must not be renumbered.
   static constexpr std::uint8_t state_space = 80;
   static constexpr std::uint16_t schema_state_type = 1;
   static constexpr std::uint16_t peer_type = 2;
   static constexpr std::uint16_t legacy_provider_v1_type = 3;
   static constexpr std::uint16_t rendezvous_type = 4;
   static constexpr std::uint16_t dht_value_type = 5;
   static constexpr std::uint16_t dht_provider_type = 6;

   struct endpoint_fact {
      std::string endpoint;
      std::uint16_t kind = 0;
      std::optional<std::string> relay_peer;
      std::uint64_t successes = 0;
      std::uint64_t failures = 0;
      std::int64_t last_latency_ms = 0;
      std::int64_t backoff_until_ns = 0;
      double score = 0.0;

      BOOST_DESCRIBE_CLASS(endpoint_fact, (),
                           (endpoint, kind, relay_peer, successes, failures, last_latency_ms, backoff_until_ns, score),
                           (), ())
   };

   struct relay_fact {
      std::string relay;
      std::uint64_t reservation_id = 0;
      std::int64_t expires_at_ns = 0;
      std::vector<std::string> endpoints;
      std::vector<std::uint8_t> voucher;
      std::uint64_t successes = 0;
      std::uint64_t failures = 0;
      std::int64_t last_latency_ms = 0;
      double score = 0.0;

      BOOST_DESCRIBE_CLASS(relay_fact, (),
                           (relay, reservation_id, expires_at_ns, endpoints, voucher, successes, failures,
                            last_latency_ms, score),
                           (), ())
   };

   struct schema_state : forge::db::object::object<schema_state, state_space, schema_state_type> {
      std::uint32_t format_version = p2p_state_schema::format_version;
      std::uint64_t rendezvous_sequence = 0;

      BOOST_DESCRIBE_CLASS(schema_state, (object_base_type), (format_version, rendezvous_sequence), (), ())
   };

   struct peer_row : forge::db::object::object<peer_row, state_space, peer_type> {
      std::string peer;
      std::uint64_t capabilities = 0;
      std::uint16_t discovered_by = 0;
      std::string protocol_version;
      std::string agent_version;
      std::vector<std::uint8_t> public_key;
      std::vector<std::string> protocols;
      std::vector<std::uint8_t> signed_peer_record;
      std::vector<endpoint_fact> endpoints;
      std::vector<relay_fact> relay_reservations;
      std::uint16_t reachability = 0;
      std::optional<std::string> observed_endpoint;
      std::int64_t reachability_expires_at_ns = 0;
      std::int64_t discovered_at_ns = 0;
      std::int64_t discovery_expires_at_ns = 0;
      std::int64_t discovery_backoff_until_ns = 0;
      std::uint64_t successes = 0;
      std::uint64_t failures = 0;
      std::int64_t last_latency_ms = 0;
      double score = 0.0;
      std::uint64_t hydration_priority = 0;

      BOOST_DESCRIBE_CLASS(peer_row, (object_base_type),
                           (peer, capabilities, discovered_by, protocol_version, agent_version, public_key, protocols,
                            signed_peer_record, endpoints, relay_reservations, reachability, observed_endpoint,
                            reachability_expires_at_ns, discovered_at_ns, discovery_expires_at_ns,
                            discovery_backoff_until_ns, successes, failures, last_latency_ms, score,
                            hydration_priority),
                           (), ())
   };

   // Exact v1 layout retained only so an explicitly requested reset can erase
   // both object rows and their historical secondary indexes.
   struct legacy_provider_v1_row
       : forge::db::object::object<legacy_provider_v1_row, state_space, legacy_provider_v1_type> {
      std::string key;
      std::string peer;
      std::vector<std::string> endpoints;
      std::uint16_t connection = 0;
      std::uint16_t discovered_by = 0;
      std::int64_t expires_at_ns = 0;
      std::uint64_t successes = 0;
      std::uint64_t failures = 0;

      BOOST_DESCRIBE_CLASS(legacy_provider_v1_row, (object_base_type),
                           (key, peer, endpoints, connection, discovered_by, expires_at_ns, successes, failures), (),
                           ())
   };

   struct rendezvous_row : forge::db::object::object<rendezvous_row, state_space, rendezvous_type> {
      std::string namespace_name;
      std::string peer;
      std::vector<std::string> endpoints;
      std::vector<std::uint8_t> signed_peer_record;
      std::int64_t ttl_seconds = 0;
      std::int64_t expires_at_ns = 0;
      std::uint64_t sequence = 0;

      BOOST_DESCRIBE_CLASS(rendezvous_row, (object_base_type),
                           (namespace_name, peer, endpoints, signed_peer_record, ttl_seconds, expires_at_ns, sequence),
                           (), ())
   };

   struct dht_value_row : forge::db::object::object<dht_value_row, state_space, dht_value_type> {
      std::string profile;
      std::string key;
      std::vector<std::uint8_t> value;
      std::string time_received;
      std::optional<std::string> publisher;
      std::int64_t ttl_seconds = 0;
      std::int64_t expires_at_ns = 0;

      BOOST_DESCRIBE_CLASS(dht_value_row, (object_base_type),
                           (profile, key, value, time_received, publisher, ttl_seconds, expires_at_ns), (), ())
   };

   struct dht_provider_row : forge::db::object::object<dht_provider_row, state_space, dht_provider_type> {
      std::string profile;
      std::string key;
      std::string peer;
      std::vector<std::string> endpoints;
      std::int64_t provider_expires_at_ns = 0;
      std::int64_t addresses_expires_at_ns = 0;
      bool local_owned = false;

      BOOST_DESCRIBE_CLASS(dht_provider_row, (object_base_type),
                           (profile, key, peer, endpoints, provider_expires_at_ns, addresses_expires_at_ns,
                            local_owned),
                           (), ())
   };

   struct by_schema_state_id;
   struct by_peer_row_id;
   struct by_peer_id;
   struct by_peer_hydration;
   struct by_peer_expiry;
   struct by_legacy_provider_v1_row_id;
   struct by_legacy_provider_v1_key_peer;
   struct by_legacy_provider_v1_hydration;
   struct by_legacy_provider_v1_expiry;
   struct by_rendezvous_row_id;
   struct by_rendezvous_namespace_peer;
   struct by_rendezvous_sequence;
   struct by_rendezvous_expiry;
   struct by_dht_value_row_id;
   struct by_dht_value_profile_key;
   struct by_dht_value_expiry;
   struct by_dht_provider_row_id;
   struct by_dht_provider_profile_key_peer;
   struct by_dht_provider_expiry;
   struct by_dht_provider_addresses_expiry;

   using schema_state_object = forge::db::object::object_index<
       schema_state, forge::db::object::indexed_by<forge::db::object::primary_unique<by_schema_state_id>>>;

   using peer_object = forge::db::object::object_index<
       peer_row,
       forge::db::object::indexed_by<
           forge::db::object::primary_unique<by_peer_row_id>,
           forge::db::object::ordered_unique<by_peer_id, forge::db::object::member<&peer_row::peer>>,
           forge::db::object::ordered_unique<
               by_peer_hydration,
               forge::db::object::composite_key<
                   forge::db::object::descending<forge::db::object::member<&peer_row::hydration_priority>>,
                   forge::db::object::descending<forge::db::object::member<&peer_row::discovery_expires_at_ns>>,
                   forge::db::object::member<&peer_row::peer>>>,
           forge::db::object::ordered_non_unique<by_peer_expiry,
                                                 forge::db::object::member<&peer_row::discovery_expires_at_ns>>>>;

   using legacy_provider_v1_object = forge::db::object::object_index<
       legacy_provider_v1_row,
       forge::db::object::indexed_by<
           forge::db::object::primary_unique<by_legacy_provider_v1_row_id>,
           forge::db::object::ordered_unique<
               by_legacy_provider_v1_key_peer,
               forge::db::object::composite_key<forge::db::object::member<&legacy_provider_v1_row::key>,
                                                forge::db::object::member<&legacy_provider_v1_row::peer>>>,
           forge::db::object::ordered_unique<
               by_legacy_provider_v1_hydration,
               forge::db::object::composite_key<
                   forge::db::object::descending<forge::db::object::member<&legacy_provider_v1_row::expires_at_ns>>,
                   forge::db::object::member<&legacy_provider_v1_row::key>,
                   forge::db::object::member<&legacy_provider_v1_row::peer>>>,
           forge::db::object::ordered_non_unique<by_legacy_provider_v1_expiry,
                                                 forge::db::object::member<&legacy_provider_v1_row::expires_at_ns>>>>;

   using rendezvous_object = forge::db::object::object_index<
       rendezvous_row,
       forge::db::object::indexed_by<
           forge::db::object::primary_unique<by_rendezvous_row_id>,
           forge::db::object::ordered_unique<
               by_rendezvous_namespace_peer,
               forge::db::object::composite_key<forge::db::object::member<&rendezvous_row::namespace_name>,
                                                forge::db::object::member<&rendezvous_row::peer>>>,
           forge::db::object::ordered_unique<
               by_rendezvous_sequence,
               forge::db::object::composite_key<
                   forge::db::object::descending<forge::db::object::member<&rendezvous_row::sequence>>,
                   forge::db::object::member<&rendezvous_row::namespace_name>,
                   forge::db::object::member<&rendezvous_row::peer>>>,
           forge::db::object::ordered_non_unique<by_rendezvous_expiry,
                                                 forge::db::object::member<&rendezvous_row::expires_at_ns>>>>;

   using dht_value_object = forge::db::object::object_index<
       dht_value_row, forge::db::object::indexed_by<
                          forge::db::object::primary_unique<by_dht_value_row_id>,
                          forge::db::object::ordered_unique<
                              by_dht_value_profile_key,
                              forge::db::object::composite_key<forge::db::object::member<&dht_value_row::profile>,
                                                               forge::db::object::member<&dht_value_row::key>>>,
                          forge::db::object::ordered_non_unique<
                              by_dht_value_expiry,
                              forge::db::object::composite_key<forge::db::object::member<&dht_value_row::profile>,
                                                               forge::db::object::member<&dht_value_row::expires_at_ns>,
                                                               forge::db::object::member<&dht_value_row::key>>>>>;

   using dht_provider_object = forge::db::object::object_index<
       dht_provider_row,
       forge::db::object::indexed_by<
           forge::db::object::primary_unique<by_dht_provider_row_id>,
           forge::db::object::ordered_unique<
               by_dht_provider_profile_key_peer,
               forge::db::object::composite_key<forge::db::object::member<&dht_provider_row::profile>,
                                                forge::db::object::member<&dht_provider_row::key>,
                                                forge::db::object::member<&dht_provider_row::peer>>>,
           forge::db::object::ordered_non_unique<
               by_dht_provider_expiry,
               forge::db::object::composite_key<forge::db::object::member<&dht_provider_row::profile>,
                                                forge::db::object::member<&dht_provider_row::provider_expires_at_ns>,
                                                forge::db::object::member<&dht_provider_row::key>,
                                                forge::db::object::member<&dht_provider_row::peer>>>,
           forge::db::object::ordered_non_unique<
               by_dht_provider_addresses_expiry,
               forge::db::object::composite_key<forge::db::object::member<&dht_provider_row::profile>,
                                                forge::db::object::member<&dht_provider_row::addresses_expires_at_ns>,
                                                forge::db::object::member<&dht_provider_row::key>,
                                                forge::db::object::member<&dht_provider_row::peer>>>>>;

   static constexpr auto schema_state_id = schema_state::id_t{1};
};

} // namespace forge::plugins::net::p2p::node::detail

FORGE_DB_OBJECT(forge::plugins::net::p2p::node::detail::p2p_state_schema::schema_state_object)
FORGE_DB_OBJECT(forge::plugins::net::p2p::node::detail::p2p_state_schema::peer_object)
FORGE_DB_OBJECT(forge::plugins::net::p2p::node::detail::p2p_state_schema::legacy_provider_v1_object)
FORGE_DB_OBJECT(forge::plugins::net::p2p::node::detail::p2p_state_schema::rendezvous_object)
FORGE_DB_OBJECT(forge::plugins::net::p2p::node::detail::p2p_state_schema::dht_value_object)
FORGE_DB_OBJECT(forge::plugins::net::p2p::node::detail::p2p_state_schema::dht_provider_object)
