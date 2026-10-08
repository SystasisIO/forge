#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

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

   void record(std::string_view kind, std::string_view source, forge::mutable_variant_object fields);
   void record_locked(std::string_view kind, std::string_view source, forge::mutable_variant_object fields);
   [[nodiscard]] bool cached_owner_locked(std::uint64_t, const forge::net::p2p::peer_id&) const;
   static std::string ready_address(forge::net::p2p::endpoint, const forge::net::p2p::peer_id&);
   static void configure_stream_security(forge::net::p2p::node::options&, std::string_view transport);
   static void canonical_paths(arguments&, bool private_profile);
   static void write_atomic(const std::filesystem::path&, const forge::variant&);
   void trace(const forge::net::p2p::pubsub::trace_event&);
   void sample(std::string_view label);
   void command(const forge::variant&, std::uint64_t sequence, forge::asio::runtime&);
   void prepare_shutdown(const forge::variant&, std::uint64_t sequence);
   [[nodiscard]] forge::variant result(bool finalized, bool joined, std::string_view error) const;
   [[nodiscard]] forge::net::p2p::pubsub::options pubsub_options() const;

   std::string _token;
   std::string _actor;
   std::string _peer;
   std::string _fingerprint;
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
