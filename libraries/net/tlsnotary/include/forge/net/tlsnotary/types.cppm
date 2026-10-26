module;

#include <boost/describe.hpp>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

export module forge.net.tlsnotary.types;

import forge.net.transport.stream;
import forge.crypto.core.types;
import forge.crypto.digest.sha256;

export namespace forge::net::tlsnotary {

// Experimental source API. Retained verifier records are not offline TLS proofs.
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

enum class direction : std::uint8_t { sent = 1, received = 2 };

struct commitment_selection {
   forge::net::tlsnotary::direction direction = direction::received;
   std::vector<byte_range> ranges;
};

// Fixed SHA-256 plaintext commitment from the pinned upstream implementation.
struct commitment {
   forge::net::tlsnotary::direction direction = direction::received;
   std::vector<byte_range> ranges;
   forge::crypto::digest::sha256 digest;

   bool operator==(const commitment&) const = default;
};

// Sensitive prover-owned material. Never included in the verifier record.
struct opening {
   std::uint32_t commitment = 0;
   forge::crypto::core::bytes plaintext;
   std::array<std::uint8_t, 16> blinder{};

   bool operator==(const opening&) const = default;
};

// Persist separately from openings. Authenticity requires trusted local storage
// or an application signature over this entire record and its own context.
struct evidence {
   std::uint32_t format = 1;
   std::string implementation = "tlsn/5f67389de3f8962227fddffd71d78a852acc9b01";
   std::string server_name;
   std::uint32_t sent_size = 0;
   std::uint32_t received_size = 0;
   std::vector<commitment> commitments;

   bool operator==(const evidence&) const = default;
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
   // Opt-in; default sessions retain the original worker protocol.
   std::vector<commitment_selection> commitments;
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

struct check_request {
   evidence authenticated;
   forge::net::tlsnotary::opening opening;
   session_options options;
};

struct result {
   std::string server_name;
   std::uint32_t sent_size = 0;
   std::uint32_t received_size = 0;
   // Populated only by verifier: hidden bytes are never present here.
   std::vector<authenticated_range> sent;
   std::vector<authenticated_range> received;
   std::optional<evidence> retained;
   // Only prover receives openings, only for explicitly selected ranges.
   std::vector<opening> openings;
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
BOOST_DESCRIBE_ENUM(direction, sent, received)
BOOST_DESCRIBE_STRUCT(byte_range, (), (begin, end))
BOOST_DESCRIBE_STRUCT(authenticated_range, (), (begin, bytes))
BOOST_DESCRIBE_STRUCT(commitment_selection, (), (direction, ranges))
BOOST_DESCRIBE_STRUCT(commitment, (), (direction, ranges, digest))
BOOST_DESCRIBE_STRUCT(opening, (), (commitment, plaintext, blinder))
BOOST_DESCRIBE_STRUCT(evidence, (), (format, implementation, server_name, sent_size, received_size, commitments))
BOOST_DESCRIBE_STRUCT(result, (), (server_name, sent_size, received_size, sent, received, retained, openings))

} // namespace forge::net::tlsnotary
