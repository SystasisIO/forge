#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "forge_pubsub_partial.hxx"

// Include after the node, endpoint, PubSub, SHA-256 and Variant modules. No private router access.
namespace forge::test::libp2p_interop {

class forge_pubsub_fixture {
 public:
   using arguments = std::map<std::string, std::string>;
   struct support {
      std::function<forge::net::p2p::node::options(const arguments&)> make_options;
   };

   forge_pubsub_fixture(std::string token, std::string actor);
   static int run(const arguments&, const support&);
   static void self_test(); // Synthetic fixture regression only, never live interoperability proof.

 private:
   class control_reader {
    public:
      explicit control_reader(const std::filesystem::path&);
      void poll(const std::filesystem::path&,
                const std::function<void(const forge::variant&, std::uint64_t)>&, bool finishing = false);

    private:
      bool _present = false;
      bool _admission_closed = false;
      std::uintmax_t _observed = 0;
      std::uint64_t _sequence = 0;
      std::optional<forge::crypto::digest::sha256> _hash;
      std::string _pending;
   };

   struct native_owner {
      forge::net::p2p::peer_id peer;
      std::string remote_address;
      std::string transport;
      std::string security;
      std::string muxer;
   };

   struct received_part {
      std::vector<std::uint8_t> encoded;
      forge::net::p2p::peer_id peer;
      std::uint64_t observation = 0;
   };
   struct partial_peer {
      forge::net::p2p::peer_id peer;
      std::optional<forge_pubsub_partial::metadata> remote;
      std::uint32_t announced_revision = 0;
      std::uint32_t requested_revision = 0;
      std::uint8_t sent = 0;
   };
   struct partial_state {
      bool initialized = false;
      bool owned = false;
      bool reconstructed = false;
      std::uint8_t local_have = 0;
      forge_pubsub_partial::metadata local{1, 0, 7};
      std::array<std::vector<std::uint8_t>, 3> generated;
      std::array<std::optional<received_part>, 3> received;
      std::map<std::string, partial_peer> peers;
   };
   struct partial_action {
      forge::net::p2p::peer_id peer;
      forge::net::p2p::pubsub::partial_message value;
      std::uint64_t observation = 0;
      bool gossip_metadata_only = false;
   };
   struct partial_work {
      forge_pubsub_fixture* owner;
      ~partial_work();
      partial_work(const partial_work&) = delete;
      partial_work& operator=(const partial_work&) = delete;
      explicit partial_work(forge_pubsub_fixture* fixture) : owner{fixture} {}
   };
   struct unsubscribe_result {
      std::exception_ptr operation_error;
      std::exception_ptr watchdog_error;
      std::exception_ptr stop_error;
      bool interrupted = false;
   };

   void record(std::string_view kind, std::string_view source, forge::mutable_variant_object fields);
   void record_locked(std::string_view kind, std::string_view source, forge::mutable_variant_object fields);
   [[nodiscard]] bool cached_owner_locked(std::uint64_t, const forge::net::p2p::peer_id&) const;
   static std::string ready_address(forge::net::p2p::endpoint, const forge::net::p2p::peer_id&);
   static void configure_stream_security(forge::net::p2p::node::options&, std::string_view transport);
   static forge::net::p2p::pubsub::version protocol_version(std::string_view);
   static void validate_extension(std::string_view, forge::net::p2p::pubsub::version);
   static void canonical_paths(arguments&, bool private_profile);
   static void write_atomic(const std::filesystem::path&, const forge::variant&);
   void trace(const forge::net::p2p::pubsub::trace_event&);
   void sample(std::string_view label);
   void command(const forge::variant&, std::uint64_t sequence, forge::asio::runtime&);
   void prepare_shutdown(const forge::variant&, std::uint64_t sequence);
   void check_prepare_locked(const forge::variant&) const;
   static void run_extension_prepare(forge::asio::runtime&, boost::asio::awaitable<void>,
                                     std::chrono::steady_clock::time_point);
   boost::asio::awaitable<void> prepare_extension(const forge::variant&, std::chrono::steady_clock::time_point);
   boost::asio::awaitable<unsubscribe_result> drain_unsubscribe(boost::asio::awaitable<void>,
       boost::asio::awaitable<void>, std::chrono::steady_clock::time_point);
   boost::asio::awaitable<void> stop_extension_node();
   [[nodiscard]] forge::variant result(bool finalized, bool joined, std::string_view error) const;
   [[nodiscard]] forge::net::p2p::pubsub::options pubsub_options() const;
   boost::asio::awaitable<forge::net::p2p::pubsub::validation_result>
   validate_message(forge::net::p2p::pubsub::event);
   bool extension_command(const forge::variant&, std::uint64_t, forge::asio::runtime&);
   boost::asio::awaitable<void> subscribe_partial();
   boost::asio::awaitable<void> partial_offer(std::uint8_t, std::uint64_t);
   boost::asio::awaitable<void> receive_partial(forge::net::p2p::pubsub::partial_event, std::stop_token);
   boost::asio::awaitable<void> gossip_partial(forge::net::p2p::pubsub::partial_gossip_event, std::stop_token);
   boost::asio::awaitable<void> send_partial(std::vector<partial_action>,
                                          forge::net::p2p::pubsub::partial_topic, std::stop_token);
   bool admit_partial_locked();
   void finish_partial() noexcept;
   void partial_failure(std::string_view);
   void offer_partial_locked(std::uint8_t);
   bool apply_partial_locked(const forge::net::p2p::peer_id&,
                             const forge::net::p2p::pubsub::partial_message&, std::uint64_t);
   std::vector<partial_action> plan_partial_locked(const forge::net::p2p::peer_id&, std::uint64_t, bool);
   [[nodiscard]] std::uint8_t received_have_locked() const;
   [[nodiscard]] forge::mutable_variant_object partial_fields_locked() const;
   void reconstruct_partial_locked();

   std::string _token;
   std::string _actor;
   std::string _peer;
   std::string _fingerprint;
   std::string _extension;
   std::string _version;
   std::string _held_payload;
   // Control-thread only; retained after failed Prepare for its final stop join.
   std::optional<std::chrono::steady_clock::time_point> _prepare_process_deadline;
   std::uint64_t _hold_command = 0;
   std::uint64_t _hold_observation = 0;
   bool _hold_released = false;
   std::size_t _extension_validators = 0;
   std::size_t _extension_work = 0;
   std::size_t _extension_inputs = 0;
   bool _extension_admission_closed = false;
   bool _extension_drained = false;
   std::string _extension_drain_error;
   forge::asio::notification _extension_notification;
   forge::asio::notification _extension_drain_notification;
   std::stop_source _extension_stop;
   std::optional<forge::net::p2p::pubsub::partial_topic> _partial_registration;
   partial_state _partial;
   forge::net::p2p::node* _node = nullptr;
   mutable std::mutex _mutex;
   forge::variants _events;
   std::map<std::uint64_t, native_owner> _connections;
   std::set<std::tuple<std::uint64_t, std::int64_t, std::string>> _streams;
   std::size_t _trace_bytes = 0;
   bool _overflow = false;
   bool _prepared = false;
   std::string _capture_error;
};

} // namespace forge::test::libp2p_interop
