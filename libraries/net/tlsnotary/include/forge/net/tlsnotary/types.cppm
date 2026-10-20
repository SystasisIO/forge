module;

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

export module forge.net.tlsnotary.types;

import forge.net.transport.stream;

export namespace forge::net::tlsnotary {

// Experimental source API; no portable or persisted proof format is defined.
struct byte_range {
   std::uint32_t begin = 0;
   std::uint32_t end = 0xffffffff;

   bool operator==(const byte_range&) const = default;
};

struct authenticated_range {
   std::uint32_t begin = 0;
   std::vector<std::uint8_t> bytes;

   bool operator==(const authenticated_range&) const = default;
};

struct source_constraints {
   std::string server_name;
   std::vector<std::vector<std::uint8_t>> trust_roots_der;
};

struct session_options {
   std::uint64_t id = 0;
   std::chrono::milliseconds timeout{120000};
   std::uint32_t max_sent = 16384;
   std::uint32_t max_received = 16384;
   std::uint32_t max_sent_records = 64;
   std::uint32_t max_received_records_online = 64;
   std::uint32_t mpc_threads = 1;
   std::uint64_t max_peer_bytes = 268435456;
   bool deferred_decryption = true;
   std::vector<byte_range> reveal_sent;
   std::vector<byte_range> reveal_received;
   std::stop_token stop;
};

struct prover_request {
   forge::net::transport::stream peer;
   forge::net::transport::stream server;
   source_constraints source;
   std::vector<std::uint8_t> request_bytes;
   session_options options;
};

struct verifier_request {
   forge::net::transport::stream peer;
   source_constraints source;
   session_options options;
};

struct result {
   std::string server_name;
   std::uint32_t sent_size = 0;
   std::uint32_t received_size = 0;
   // Populated only by verifier: hidden bytes are never present here.
   std::vector<authenticated_range> sent;
   std::vector<authenticated_range> received;
};

struct engine_options {
   // Explicit trusted executable. Never searched through PATH or accepted from peer.
   std::filesystem::path worker_path;
   std::uint32_t max_active = 2;
   std::uint32_t max_queued = 8;
};

struct metrics {
   std::uint64_t accepted = 0;
   std::uint64_t completed = 0;
   std::uint64_t failed = 0;
   std::uint64_t rejected = 0;
   std::uint32_t active = 0;
   std::uint32_t queued = 0;
   bool stopped = false;
};
} // namespace forge::net::tlsnotary
