#pragma once

#include "fixture.hxx"

#include <boost/describe.hpp>
#include <boost/scope/scope_exit.hpp>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <spawn.h>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

import forge.crypto.asymmetric;
import forge.crypto.digest.sha256;
import forge.exceptions;
import forge.raw.raw;
import forge.raw.exceptions;

extern char** environ;

namespace tlsn_test {
namespace asymmetric = forge::crypto::asymmetric;

// This is an application trust envelope, not a TLSNotary attestation. A reader
// must obtain the expected signer/context independently of the received record.
struct evidence_statement {
   std::string domain = "forge.tests.tlsnotary.evidence.v1";
   std::string context;
   tlsn::evidence authenticated;
};

struct signed_evidence {
   evidence_statement statement;
   asymmetric::signature signature;
};

struct evidence_trust {
   std::string context;
   asymmetric::public_key signer;
};

BOOST_DESCRIBE_STRUCT(evidence_statement, (), (domain, context, authenticated))
BOOST_DESCRIBE_STRUCT(signed_evidence, (), (statement, signature))
BOOST_DESCRIBE_STRUCT(evidence_trust, (), (context, signer))

inline bool authorize(const signed_evidence& retained, const evidence_trust& expected) {
   return retained.statement.domain == "forge.tests.tlsnotary.evidence.v1" &&
          retained.statement.context == expected.context &&
          asymmetric::verify(expected.signer, forge::raw::pack(retained.statement), retained.signature);
}

inline void private_write(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
   const auto fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
   check(fd >= 0, "evidence fixture output creation failed");
   const auto close = boost::scope::scope_exit{[&] { ::close(fd); }};
   while (!bytes.empty()) {
      const auto count = ::write(fd, bytes.data(), bytes.size());
      check(count > 0, "evidence fixture output write failed");
      bytes = bytes.subspan(static_cast<std::size_t>(count));
   }
   check(::fsync(fd) == 0, "evidence fixture output durability failed");
}

inline std::vector<std::uint8_t> private_read(const std::filesystem::path& path) {
   check(std::filesystem::file_size(path) <= 65536, "evidence fixture input exceeds bound");
   auto input = std::ifstream{path, std::ios::binary};
   check(static_cast<bool>(input), "evidence fixture input open failed");
   return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

template <typename T> T read_record(const std::filesystem::path& path) {
   const auto bytes = private_read(path);
   return forge::raw::unpack_bounded_exact<T>(bytes, {
       .allocations = {.max_container_elements = 1024, .max_total_container_elements = 4096, .max_bytes = 65536},
       .max_total_bytes = 65536,
       .reject_noncanonical_varints = true,
       .reject_unknown_enum_values = true});
}

inline void no_private_bytes(std::span<const std::uint8_t> bytes) {
   const auto text = std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
   check(text.find("secret-sentinel") == std::string_view::npos, "retained evidence disclosed request credential");
   check(text.find("7318921") == std::string_view::npos, "retained evidence disclosed committed response");
}

inline tlsn::session_options evidence_options(std::uint64_t id) {
   auto selected = options(id);
   const auto value = https_fixture::response().find("7318921");
   check(value != std::string::npos, "test response commitment marker missing");
   selected.reveal_received = {{0, static_cast<std::uint32_t>(value)}};
   selected.commitments = {
       {.direction = tlsn::direction::sent, .ranges = {{0, 18}}},
       {.direction = tlsn::direction::received,
        .ranges = {{static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(value + 7)}}}};
   return selected;
}

inline void check_opening(forge::asio::runtime& runtime, tlsn::engine& owner, const tlsn::evidence& authenticated,
                          const tlsn::opening& opening, std::uint64_t id) {
   auto request = tlsn::check_request{.authenticated = authenticated, .opening = opening};
   request.options.id = id;
   request.options.timeout = 10s;
   forge::asio::blocking::run(runtime, owner.check(std::move(request)));
}

template <typename Work> void evidence_rejects(tlsn::exceptions::code expected, Work work) {
   auto rejected = false;
   try {
      work();
   } catch (const forge::exceptions::context_error& error) {
      rejected = std::string_view{error.code().category().name()} == "forge.net.tlsnotary" &&
                 error.code().value() == static_cast<int>(expected);
      check(std::string_view{error.what()}.find("secret-sentinel") == std::string_view::npos,
            "opening failure disclosed request credential");
      check(std::string_view{error.what()}.find("7318921") == std::string_view::npos,
            "opening failure disclosed committed response");
   }
   check(rejected, "expected exact evidence failure code absent");
}

inline void evidence_wire_tests(const tlsn::evidence& authenticated, const tlsn::opening& opening) {
   // Native described enums use Forge Raw's default int64 wire policy, even
   // when their C++ underlying type is uint8 (enum_wire_type + codec_traits).
   static_assert(std::is_same_v<forge::raw::enum_wire_type_t<tlsn::direction>, std::int64_t>);
   static_assert(!forge::raw::shared_enum_wire_v<tlsn::direction>);
   const auto received_wire = std::vector<std::uint8_t>{2, 0, 0, 0, 0, 0, 0, 0};
   check(forge::raw::pack(tlsn::direction::received) == received_wire,
         "commitment direction wire width or value changed");
   check(forge::raw::unpack_bounded_exact<tlsn::direction>(received_wire) == tlsn::direction::received,
         "commitment direction canonical bytes rejected");
   for (const auto invalid : std::array<std::int64_t, 4>{0, 3, 257, -255}) {
      auto rejected = false;
      try {
         static_cast<void>(forge::raw::unpack_bounded_exact<tlsn::direction>(forge::raw::pack(invalid)));
      } catch (const forge::raw::exceptions::codec_error&) {
         rejected = true;
      }
      check(rejected, "bounded Raw accepted an unknown or narrowing commitment direction");
   }
   check(forge::raw::pack(tlsn::byte_range{1, 7}) == std::vector<std::uint8_t>{1, 0, 0, 0, 7, 0, 0, 0},
         "commitment range wire changed");
   const auto encoded = forge::raw::pack(authenticated);
   check(encoded == forge::raw::pack(std::tie(authenticated.format, authenticated.implementation, authenticated.server_name,
                                             authenticated.sent_size, authenticated.received_size, authenticated.commitments)),
         "retained evidence canonical field order changed");
   const auto restored = forge::raw::unpack_bounded_exact<tlsn::evidence>(encoded);
   check(forge::raw::pack(restored) == encoded, "retained evidence Raw roundtrip changed bytes");
   check(forge::raw::pack(opening) == forge::raw::pack(std::tie(opening.commitment, opening.plaintext, opening.blinder)),
         "opening canonical field order changed");
   check(forge::raw::pack(authenticated.commitments.back()) ==
             forge::raw::pack(std::tie(authenticated.commitments.back().direction, authenticated.commitments.back().ranges,
                                       authenticated.commitments.back().digest)),
         "commitment canonical field order changed");
   check(authenticated.format == 1 &&
             authenticated.implementation == "tlsn/5f67389de3f8962227fddffd71d78a852acc9b01",
         "retained evidence implementation pin changed");
   auto suffix = encoded;
   suffix.push_back(0);
   auto rejected = false;
   try {
      static_cast<void>(forge::raw::unpack_bounded_exact<tlsn::evidence>(suffix));
   } catch (const std::exception&) {
      rejected = true;
   }
   check(rejected, "retained evidence accepted a Raw suffix");
   auto truncated = encoded;
   truncated.pop_back();
   rejected = false;
   try {
      static_cast<void>(forge::raw::unpack_bounded_exact<tlsn::evidence>(truncated));
   } catch (const std::exception&) {
      rejected = true;
   }
   check(rejected, "retained evidence accepted truncated Raw");
}

inline void retained_evidence_test(const std::filesystem::path& worker, const std::filesystem::path& output) {
   const auto retained = read_record<signed_evidence>(output / "evidence.raw");
   const auto expected = read_record<evidence_trust>(output / "trust.raw");
   check(authorize(retained, expected), "retained evidence signature or context rejected after restart");
   // Private openings are read only after the application signature/context gate.
   const auto openings = read_record<std::vector<tlsn::opening>>(output / "openings.private.raw");
   check(openings.size() == 2, "retained private openings missing after restart");
   auto runtime = forge::asio::runtime{{.worker_threads = 4}};
   auto compute = forge::asio::compute::pool{{.worker_threads = 2, .max_pending_tasks = 4}};
   auto owner = tlsn::engine{runtime, compute.get_executor(), {.worker_path = worker, .max_active = 1}};
   const auto cleanup = engine_cleanup{runtime, owner};
   for (std::size_t i = 0; i < openings.size(); ++i)
      check_opening(runtime, owner, retained.statement.authenticated, openings[i], 400 + i);
   forge::asio::blocking::run(runtime, owner.shutdown());
   forge::asio::blocking::run(runtime, compute.shutdown());
}

inline void restart_evidence_test(const std::filesystem::path& executable, const std::filesystem::path& worker,
                                  const std::filesystem::path& output) {
   const auto program = std::filesystem::absolute(executable).string();
   auto arguments = std::array<std::string, 4>{program, "--retained", worker.string(), output.string()};
   auto argv = std::array<char*, 5>{};
   for (std::size_t i = 0; i < arguments.size(); ++i)
      argv[i] = arguments[i].data();
   auto pid = pid_t{};
   check(::posix_spawn(&pid, program.c_str(), nullptr, nullptr, argv.data(), environ) == 0,
         "retained evidence restart spawn failed");
   auto status = int{};
   auto waited = pid_t{};
   do {
      waited = ::waitpid(pid, &status, 0);
   } while (waited < 0 && errno == EINTR);
   check(waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "retained evidence verification failed in fresh host process");
}

inline void evidence_test(const std::filesystem::path& worker, const std::filesystem::path& output) {
   auto runtime = forge::asio::runtime{{.worker_threads = 4}};
   auto compute = forge::asio::compute::pool{{.worker_threads = 2, .max_pending_tasks = 4}};
   auto owner = tlsn::engine{runtime, compute.get_executor(), {.worker_path = worker, .max_active = 2}};
   const auto cleanup = engine_cleanup{runtime, owner};
   const auto identity = make_identity();
   auto https = https_fixture{runtime, identity};
   auto service = asio::co_spawn(runtime.context(), https.serve(), asio::use_future);
   auto peers = pair(runtime);
   auto proof = tlsn::prover_request{.peer = std::move(peers.first),
                                     .server = https.connect(runtime),
                                     .source = {.server_name = "tlsn.local", .trust_roots_der = {identity.root}},
                                     .request_bytes = request(),
                                     .options = evidence_options(301)};
   auto verification = tlsn::verifier_request{.peer = std::move(peers.second),
                                              .source = {.server_name = "tlsn.local", .trust_roots_der = {identity.root}},
                                              .options = evidence_options(302)};
   auto verified = asio::co_spawn(runtime.context(), owner.verify(std::move(verification)), asio::use_future);
   auto proved = asio::co_spawn(runtime.context(), owner.prove(std::move(proof)), asio::use_future);
   await_ready(verified);
   await_ready(proved);
   const auto authenticated = verified.get();
   const auto private_result = proved.get();
   static_cast<void>(service.get());
   check(authenticated.retained.has_value() && private_result.retained.has_value(),
         "MPC-confirmed commitments absent");
   check(authenticated.openings.empty() && private_result.openings.size() == 2,
         "MPC opening ownership violated");
   check(forge::raw::pack(*authenticated.retained) == forge::raw::pack(*private_result.retained),
         "prover and verifier commitments differ");
   check(authenticated.retained->commitments.size() == 2, "MPC commitment count differs from requested selection");
   const auto selected = evidence_options(1).commitments;
   for (std::size_t i = 0; i < selected.size(); ++i) {
      check(authenticated.retained->commitments[i].direction == selected[i].direction &&
                authenticated.retained->commitments[i].ranges == selected[i].ranges,
            "MPC authenticated a different commitment selection");
      check(private_result.openings[i].commitment == i, "MPC opening index does not preserve selected order");
   }
   check(authenticated.retained->server_name == authenticated.server_name &&
             authenticated.retained->sent_size == request().size() &&
             authenticated.retained->received_size == https_fixture::response().size(),
         "retained evidence origin or transcript lengths differ from the authenticated session");
   check(private_result.sent.empty() && private_result.received.empty(), "prover result contains authenticated plaintext");
   for (const auto& range : authenticated.sent)
      no_private_bytes(range.bytes);
   for (const auto& range : authenticated.received)
      no_private_bytes(range.bytes);
   no_private_bytes(forge::raw::pack(*authenticated.retained));
   check(std::string{private_result.openings.back().plaintext.begin(), private_result.openings.back().plaintext.end()} ==
             "7318921",
         "upstream commitment opening is not the requested hidden response range");
   evidence_wire_tests(*authenticated.retained, private_result.openings.back());

   const auto key = asymmetric::private_key::generate();
   auto retained = signed_evidence{.statement = {.context = "paired-local-https:42", .authenticated = *authenticated.retained}};
   retained.signature = key.sign(forge::raw::pack(retained.statement));
   const auto trust = evidence_trust{.context = retained.statement.context, .signer = key.get_public_key()};
   auto disclosed = unsigned{};
   const auto guarded = [&](const signed_evidence& candidate, const evidence_trust& expected) {
      if (!authorize(candidate, expected))
         return false;
      ++disclosed;
      check_opening(runtime, owner, candidate.statement.authenticated, private_result.openings.back(), 310 + disclosed);
      return true;
   };
   auto wrong = retained;
   wrong.statement.context += ":other";
   check(!guarded(wrong, trust) && disclosed == 0, "changed context disclosed an opening");
   wrong = retained;
   wrong.statement.domain += ".other";
   check(!guarded(wrong, trust) && disclosed == 0, "changed signature domain disclosed an opening");
   wrong = retained;
   wrong.statement.authenticated.received_size += 1;
   check(!guarded(wrong, trust) && disclosed == 0, "changed signed evidence disclosed an opening");
   wrong = retained;
   wrong.statement.authenticated.commitments.back().direction = tlsn::direction::sent;
   check(!guarded(wrong, trust) && disclosed == 0, "changed commitment direction disclosed an opening");
   wrong = retained;
   ++wrong.statement.authenticated.commitments.back().ranges.front().begin;
   ++wrong.statement.authenticated.commitments.back().ranges.front().end;
   check(!guarded(wrong, trust) && disclosed == 0, "translated commitment range disclosed an opening");
   wrong = retained;
   wrong.statement.authenticated.server_name = "other.local";
   check(!guarded(wrong, trust) && disclosed == 0, "changed signed origin disclosed an opening");
   wrong = retained;
   wrong.signature = asymmetric::private_key::generate().sign(forge::raw::pack(wrong.statement));
   check(!guarded(wrong, trust) && disclosed == 0, "wrong signer disclosed an opening");
   auto wrong_trust = trust;
   wrong_trust.signer = asymmetric::private_key::generate().get_public_key();
   check(!guarded(retained, wrong_trust) && disclosed == 0, "untrusted signer disclosed an opening");
   check(guarded(retained, trust) && disclosed == 1, "authorized opening rejected");
   // Signature seals a verifier assertion. It does not create an independently
   // verifiable MPC attestation or prove any application-level response meaning.
   private_write(output / "evidence.raw", forge::raw::pack(retained));
   private_write(output / "trust.raw", forge::raw::pack(trust));
   private_write(output / "openings.private.raw", forge::raw::pack(private_result.openings));
   no_private_bytes(private_read(output / "evidence.raw"));

   auto id = std::uint64_t{330};
   const auto reject = [&](const tlsn::evidence& evidence, const tlsn::opening& opening,
                            tlsn::exceptions::code expected = tlsn::exceptions::code::verification_failed) {
      evidence_rejects(expected, [&] { check_opening(runtime, owner, evidence, opening, ++id); });
   };
   auto opening = private_result.openings.back();
   opening.plaintext.front() ^= 1;
   reject(*authenticated.retained, opening);
   opening = private_result.openings.back();
   opening.blinder.front() ^= 1;
   reject(*authenticated.retained, opening);
   opening = private_result.openings.back();
   opening.commitment = 0;
   reject(*authenticated.retained, opening);
   opening.commitment = 2;
   reject(*authenticated.retained, opening, tlsn::exceptions::code::limit_exceeded);
   opening = private_result.openings.back();
   opening.plaintext.pop_back();
   reject(*authenticated.retained, opening);
   auto changed = *authenticated.retained;
   changed.commitments.back().digest = forge::crypto::digest::sha256{};
   reject(changed, private_result.openings.back());
   changed = *authenticated.retained;
   changed.commitments.back().ranges.front().begin += 1;
   reject(changed, private_result.openings.back());
   changed = *authenticated.retained;
   changed.commitments.back().ranges.front().end += 1;
   reject(changed, private_result.openings.back());
   changed = *authenticated.retained;
   changed.commitments.back().ranges = {{4, 9}, {3, 4}};
   reject(changed, private_result.openings.back(), tlsn::exceptions::code::invalid_request);
   changed = *authenticated.retained;
   changed.commitments.back().ranges.front().end = changed.received_size + 1;
   reject(changed, private_result.openings.back(), tlsn::exceptions::code::invalid_request);
   changed = *authenticated.retained;
   changed.commitments.back().ranges.clear();
   reject(changed, private_result.openings.back(), tlsn::exceptions::code::invalid_request);
   changed = *authenticated.retained;
   changed.commitments.back().direction = static_cast<tlsn::direction>(255);
   reject(changed, private_result.openings.back(), tlsn::exceptions::code::invalid_request);
   opening = private_result.openings.back();
   opening.plaintext.resize(65537);
   reject(*authenticated.retained, opening, tlsn::exceptions::code::limit_exceeded);
   changed = *authenticated.retained;
   changed.format += 1;
   reject(changed, private_result.openings.back(), tlsn::exceptions::code::invalid_request);
   changed = *authenticated.retained;
   changed.implementation.back() = changed.implementation.back() == '0' ? '1' : '0';
   reject(changed, private_result.openings.back(), tlsn::exceptions::code::invalid_request);
   changed = *authenticated.retained;
   changed.server_name.clear();
   reject(changed, private_result.openings.back(), tlsn::exceptions::code::invalid_request);
   changed = *authenticated.retained;
   changed.commitments.clear();
   reject(changed, private_result.openings.back(), tlsn::exceptions::code::limit_exceeded);
   changed = *authenticated.retained;
   changed.commitments.push_back(changed.commitments.back());
   reject(changed, private_result.openings.back(), tlsn::exceptions::code::invalid_request);
   for (const auto capacity_case : {0, 1, 2, 3, 4}) {
      auto bounded = tlsn::check_request{.authenticated = *authenticated.retained,
                                         .opening = private_result.openings.back()};
      bounded.options.id = ++id;
      if (capacity_case == 0)
         bounded.opening.plaintext.reserve(65537);
      if (capacity_case == 1)
         bounded.authenticated.commitments.reserve(17);
      if (capacity_case == 2)
         bounded.authenticated.commitments.back().ranges.reserve(1025);
      if (capacity_case == 3)
         bounded.authenticated.implementation.reserve(129);
      if (capacity_case == 4)
         bounded.authenticated.server_name.reserve(254);
      const auto expected = capacity_case < 3 ? tlsn::exceptions::code::limit_exceeded
                                              : tlsn::exceptions::code::invalid_request;
      evidence_rejects(expected, [&] { forge::asio::blocking::run(runtime, owner.check(std::move(bounded))); });
   }
   for (std::size_t i = 0; i < private_result.openings.size(); ++i)
      check_opening(runtime, owner, *authenticated.retained, private_result.openings[i], 380 + i);
   const auto finished = owner.snapshot();
   check(finished.active == 0 && finished.queued == 0, "opening checks retained native session resources");
   forge::asio::blocking::run(runtime, owner.shutdown());
   forge::asio::blocking::run(runtime, compute.shutdown());
}

inline void commitment_policy_test(const std::filesystem::path& worker) {
   auto runtime = forge::asio::runtime{{.worker_threads = 4}};
   auto compute = forge::asio::compute::pool{{.worker_threads = 2, .max_pending_tasks = 4}};
   auto owner = tlsn::engine{runtime, compute.get_executor(), {.worker_path = worker, .max_active = 2}};
   const auto cleanup = engine_cleanup{runtime, owner};
   const auto identity = make_identity();
   auto https = https_fixture{runtime, identity};
   auto service = asio::co_spawn(runtime.context(), https.serve(), asio::use_future);
   auto peers = pair(runtime);
   auto proof = tlsn::prover_request{.peer = std::move(peers.first),
                                     .server = https.connect(runtime),
                                     .source = {.server_name = "tlsn.local", .trust_roots_der = {identity.root}},
                                     .request_bytes = request(),
                                     .options = evidence_options(701)};
   auto verification = tlsn::verifier_request{.peer = std::move(peers.second),
                                              .source = {.server_name = "tlsn.local", .trust_roots_der = {identity.root}},
                                              .options = evidence_options(702)};
   // Disclosure masks and selected byte counts agree. Only the independent
   // verifier's commitment policy selects a translated response range.
   ++verification.options.commitments.back().ranges.front().begin;
   ++verification.options.commitments.back().ranges.front().end;
   auto verified = asio::co_spawn(runtime.context(), owner.verify(std::move(verification)), asio::use_future);
   auto proved = asio::co_spawn(runtime.context(), owner.prove(std::move(proof)), asio::use_future);
   await_ready(verified);
   await_ready(proved);
   auto rejected = false;
   try {
      static_cast<void>(verified.get());
   } catch (const forge::exceptions::context_error& error) {
      rejected = std::string_view{error.code().category().name()} == "forge.net.tlsnotary" &&
                 error.code().value() == static_cast<int>(tlsn::exceptions::code::verification_failed);
   }
   check(rejected, "MPC verifier accepted a different same-size commitment range");
   try {
      static_cast<void>(proved.get());
   } catch (const forge::exceptions::context_error&) {
   }
   static_cast<void>(service.get());
   forge::asio::blocking::run(runtime, owner.shutdown());
   forge::asio::blocking::run(runtime, compute.shutdown());
}
} // namespace tlsn_test
