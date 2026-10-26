#include "evidence.hxx"

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <tuple>
#include <vector>

import forge.crypto.exchange;
import forge.net.tlsnotary.exchange;

namespace {

forge::crypto::digest::sha256 digest(std::span<const std::uint8_t> bytes)
{
   return forge::crypto::digest::sha256::hash(bytes);
}

void actual_mpc_block_binding(const std::filesystem::path& worker)
{
   auto runtime = forge::asio::runtime{{.worker_threads = 4}};
   auto compute = forge::asio::compute::pool{{.worker_threads = 2, .max_pending_tasks = 4}};
   auto engine = forge::net::tlsnotary::engine{
       runtime, compute.get_executor(), {.worker_path = worker, .max_active = 2}};
   const auto cleanup = tlsn_test::engine_cleanup{runtime, engine};
   const auto identity = tlsn_test::make_identity();
   auto body_bytes = std::string{"hidden-exchange-response:"};
   body_bytes.append(2200, 'b');
   const auto header = std::string{"HTTP/1.1 200 OK\r\nContent-Length: "} + std::to_string(body_bytes.size()) +
                       "\r\nConnection: close\r\n\r\n";
   const auto response = header + body_bytes;
   const auto body = forge::net::tlsnotary::byte_range{
       static_cast<std::uint32_t>(header.size()), static_cast<std::uint32_t>(response.size())};
   FORGE_ASSERT(body_bytes.size() > 2048U && body_bytes.size() < 4096U,
                "MPC fixture must cross exactly one direct exchange block boundary");
   // This public per-operation nonce/context is selected independently before
   // TLS. It is visible while the adjacent credential stays undisclosed.
   const auto operation_nonce = digest(forge::raw::pack(
       forge::crypto::asymmetric::private_key::generate().get_public_key())).str();
   const auto expected_context = digest(forge::raw::pack(
       std::tuple{std::string{"forge.tests.direct.operation.v1"}, operation_nonce}));
   const auto base_request = tlsn_test::request();
   auto request_text = std::string(base_request.begin(), base_request.end());
   const auto nonce_position = request_text.find("Connection: close\r\n");
   FORGE_ASSERT(nonce_position != std::string::npos, "fixture request header boundary missing");
   request_text.insert(nonce_position, "X-Operation: " + operation_nonce + "\r\n");
   const auto request_bytes = std::vector<std::uint8_t>(request_text.begin(), request_text.end());
   auto https = tlsn_test::https_fixture{runtime, identity, response};
   auto service = boost::asio::co_spawn(runtime.context(), https.serve(), boost::asio::use_future);
   auto peers = tlsn_test::pair(runtime);
   const auto credential_begin = request_text.find("secret-sentinel");
   FORGE_ASSERT(credential_begin != std::string::npos, "fixture credential marker missing");
   const auto credential_end = credential_begin + std::string_view{"secret-sentinel"}.size();
   auto selected = tlsn_test::options(801);
   selected.max_received = 4096;
   selected.reveal_sent = {{0, static_cast<std::uint32_t>(credential_begin)},
                           {static_cast<std::uint32_t>(credential_end), static_cast<std::uint32_t>(request_bytes.size())}};
   selected.reveal_received = {{0, body.begin}};
   selected.commitments = {
       {.direction = forge::net::tlsnotary::direction::received,
        .ranges = {{body.begin, body.begin + 2048U}}},
       {.direction = forge::net::tlsnotary::direction::received,
        .ranges = {{body.begin + 2048U, body.end}}}};
   auto prover = forge::net::tlsnotary::prover_request{
       .peer = std::move(peers.first), .server = https.connect(runtime),
       .source = {.server_name = "tlsn.local", .trust_roots_der = {identity.root}},
       .request_bytes = request_bytes, .options = selected};
   selected.id = 802;
   auto verifier = forge::net::tlsnotary::verifier_request{
       .peer = std::move(peers.second),
       .source = {.server_name = "tlsn.local", .trust_roots_der = {identity.root}}, .options = selected};
   auto verified = boost::asio::co_spawn(runtime.context(), engine.verify(std::move(verifier)), boost::asio::use_future);
   auto proved = boost::asio::co_spawn(runtime.context(), engine.prove(std::move(prover)), boost::asio::use_future);
   tlsn_test::await_ready(verified);
   tlsn_test::await_ready(proved);
   const auto authenticated = verified.get();
   const auto private_result = proved.get();
   static_cast<void>(service.get());
   FORGE_ASSERT(authenticated.retained && private_result.retained && authenticated.openings.empty() &&
                    private_result.openings.size() == 2,
                "MPC fixture did not retain verifier-owned commitments and prover-only openings");
   FORGE_ASSERT(forge::raw::pack(*authenticated.retained) == forge::raw::pack(*private_result.retained),
                "MPC participants retained different source commitments");
   // Exact known request prefix/suffix excluding the credential and exact
   // response header. No generic HTTP/JSON/parser/content extraction claim.
   FORGE_ASSERT(authenticated.sent.size() == 2 && authenticated.sent.front().begin == 0 &&
                    authenticated.sent.front().bytes ==
                        std::vector<std::uint8_t>(request_bytes.begin(), request_bytes.begin() + credential_begin) &&
                    authenticated.sent.back().begin == credential_end && authenticated.sent.back().bytes ==
                        std::vector<std::uint8_t>(request_bytes.begin() + credential_end, request_bytes.end()) &&
                    authenticated.received.size() == 1 && authenticated.received.front().begin == 0 &&
                    std::string(authenticated.received.front().bytes.begin(),
                                authenticated.received.front().bytes.end()) == header,
                "actual authenticated request/header bytes differ from independently expected raw fixture");
   const auto key = forge::crypto::exchange::key{
       0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
       16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
   auto manifest = forge::crypto::exchange::manifest{
       .version = 1, .context = expected_context,
       .evidence = digest(forge::raw::pack(*authenticated.retained)), .key_hash = digest(key)};
   auto ciphertexts = std::vector<forge::crypto::core::bytes>{};
   for (auto index = std::uint32_t{}; index < private_result.openings.size(); ++index)
   {
      const auto& opening = private_result.openings[index];
      FORGE_ASSERT(opening.commitment == index, "upstream opening order differs from commitment selection");
      tlsn_test::check_opening(runtime, engine, *authenticated.retained, opening, 810U + index);
      auto bytes = opening.plaintext;
      bytes.insert(bytes.end(), opening.blinder.begin(), opening.blinder.end());
      FORGE_ASSERT(digest(bytes) == authenticated.retained->commitments[index].digest,
                   "upstream H differs from genuine plaintext plus blinder");
      auto ciphertext = forge::crypto::exchange::encrypt(key, index, bytes);
      manifest.blocks.push_back({.size = static_cast<std::uint32_t>(opening.plaintext.size()),
                                 .source = authenticated.retained->commitments[index].digest,
                                 .ciphertext = digest(ciphertext)});
      ciphertexts.push_back(std::move(ciphertext));
   }
   forge::net::tlsnotary::bind_exchange(authenticated, manifest, expected_context, "tlsn.local", body, ciphertexts);
   auto authorized = std::uint32_t{};
   const auto authorize_after_binding = [&](const forge::net::tlsnotary::result& source,
                                           const forge::crypto::exchange::manifest& offer,
                                           forge::net::tlsnotary::byte_range selected_body,
                                           const std::vector<forge::crypto::core::bytes>& delivered,
                                           const std::vector<std::uint8_t>& policy_request,
                                           const forge::crypto::digest::sha256& policy_context)
   {
      if (source.sent.size() != 2 || source.sent.front().begin != 0 || source.sent.front().bytes !=
              std::vector<std::uint8_t>(policy_request.begin(), policy_request.begin() + credential_begin) ||
          source.sent.back().begin != credential_end || source.sent.back().bytes !=
              std::vector<std::uint8_t>(policy_request.begin() + credential_end, policy_request.end()) ||
          source.received.size() != 1 || source.received.front().begin != 0 ||
          std::string(source.received.front().bytes.begin(), source.received.front().bytes.end()) != header)
      {
         FORGE_THROW_EXCEPTION(forge::net::tlsnotary::exceptions::invalid_request,
                               "authenticated request/header differs from fixed raw fixture policy");
      }
      forge::net::tlsnotary::bind_exchange(source, offer, policy_context, "tlsn.local", selected_body, delivered);
      ++authorized;
   };
   const auto reject = [&](const forge::net::tlsnotary::result& source,
                           const forge::crypto::exchange::manifest& offer,
                           forge::net::tlsnotary::byte_range selected_body,
                           const std::vector<forge::crypto::core::bytes>& delivered)
   {
      auto rejected = false;
      try
      {
         authorize_after_binding(source, offer, selected_body, delivered, request_bytes, expected_context);
      }
      catch (const forge::net::tlsnotary::exceptions::invalid_request&)
      {
         rejected = true;
      }
      FORGE_ASSERT(rejected && authorized == 0, "source/ciphertext mismatch reached buyer authorization");
   };
   auto wrong = manifest;
   wrong.blocks.back().source = forge::crypto::digest::sha256::hash(std::string{"seller invented H"});
   reject(authenticated, wrong, body, ciphertexts);
   wrong = manifest;
   std::swap(wrong.blocks.front().source, wrong.blocks.back().source);
   reject(authenticated, wrong, body, ciphertexts);
   wrong = manifest;
   wrong.blocks.pop_back();
   reject(authenticated, wrong, body, ciphertexts);
   wrong = manifest;
   --wrong.blocks.back().size;
   reject(authenticated, wrong, body, ciphertexts);
   wrong = manifest;
   wrong.context = forge::crypto::digest::sha256::hash(std::string{"seller supplied context"});
   reject(authenticated, wrong, body, ciphertexts);
   auto changed = authenticated;
   ++changed.retained->commitments.back().ranges.front().begin;
   wrong = manifest;
   wrong.evidence = digest(forge::raw::pack(*changed.retained));
   reject(changed, wrong, body, ciphertexts);
   changed = authenticated;
   changed.retained->commitments.back().direction = forge::net::tlsnotary::direction::sent;
   wrong = manifest;
   wrong.evidence = digest(forge::raw::pack(*changed.retained));
   reject(changed, wrong, body, ciphertexts);
   changed = authenticated;
   changed.server_name = "other.local";
   reject(changed, manifest, body, ciphertexts);
   changed = authenticated;
   changed.openings.push_back(private_result.openings.front());
   reject(changed, manifest, body, ciphertexts);
   auto trimmed = body;
   --trimmed.end;
   reject(authenticated, manifest, trimmed, ciphertexts);
   auto missing = ciphertexts;
   missing.pop_back();
   reject(authenticated, manifest, body, missing);
   missing = ciphertexts;
   missing.back().front() ^= 1U;
   reject(authenticated, manifest, body, missing);
   missing = ciphertexts;
   missing.back().pop_back();
   reject(authenticated, manifest, body, missing);
   changed = authenticated;
   changed.received.front().bytes.pop_back();
   reject(changed, manifest, body, ciphertexts);
   changed = authenticated;
   changed.received.front().bytes.push_back('b');
   reject(changed, manifest, body, ciphertexts);
   changed = authenticated;
   changed.received.clear();
   reject(changed, manifest, body, ciphertexts);
   changed = authenticated;
   changed.received.front().bytes[9] = '4'; // Same-length HTTP status change.
   reject(changed, manifest, body, ciphertexts);
   changed = authenticated;
   changed.sent.front().bytes[5] = 'x'; // Same-length request-path change.
   reject(changed, manifest, body, ciphertexts);
   changed = authenticated;
   changed.retained->received_size += 1U;
   wrong = manifest;
   wrong.evidence = digest(forge::raw::pack(*changed.retained));
   reject(changed, wrong, body, ciphertexts);
   wrong = manifest;
   wrong.evidence = forge::crypto::digest::sha256::hash(std::string{"seller evidence hash"});
   reject(authenticated, wrong, body, ciphertexts);

   // The retained evidence digest excludes revealed request/header bytes. Even
   // authentic old engine output cannot be rebound to a fresh operation merely
   // by replacing the offered context. The consumer's exact nonce policy must
   // reject before signing; bind_exchange itself does not establish freshness.
   const auto next_nonce = forge::crypto::digest::sha256::hash(operation_nonce + ":next-operation").str();
   FORGE_ASSERT(next_nonce != operation_nonce, "fresh operation nonce unexpectedly repeated");
   const auto next_context = digest(forge::raw::pack(
       std::tuple{std::string{"forge.tests.direct.operation.v1"}, next_nonce}));
   auto next_request_text = request_text;
   const auto previous_nonce = next_request_text.find(operation_nonce);
   FORGE_ASSERT(previous_nonce != std::string::npos, "original public operation nonce is not in request policy");
   next_request_text.replace(previous_nonce, operation_nonce.size(), next_nonce);
   const auto next_request = std::vector<std::uint8_t>(next_request_text.begin(), next_request_text.end());
   auto rebound = manifest;
   rebound.context = next_context;
   forge::net::tlsnotary::bind_exchange(authenticated, rebound, next_context, "tlsn.local", body, ciphertexts);
   auto replay_rejected = false;
   try
   {
      authorize_after_binding(authenticated, rebound, body, ciphertexts, next_request, next_context);
   }
   catch (const forge::net::tlsnotary::exceptions::invalid_request&)
   {
      replay_rejected = true;
   }
   FORGE_ASSERT(replay_rejected && authorized == 0 && rebound.evidence == manifest.evidence,
                "old genuine MPC result reached new-operation authorization after context relabeling");
   authorize_after_binding(authenticated, manifest, body, ciphertexts, request_bytes, expected_context);
   FORGE_ASSERT(authorized == 1, "complete genuine binding did not reach the simulated arm signature once");
   auto recovered = forge::crypto::core::bytes{};
   for (auto index = std::uint32_t{}; index < ciphertexts.size(); ++index)
   {
      const auto witness = forge::crypto::exchange::fault{.index = index, .ciphertext = ciphertexts[index]};
      FORGE_ASSERT(!forge::crypto::exchange::proves_fault(manifest, key, witness),
                   "honest genuine MPC block was classified as invalid delivery");
      const auto plaintext = forge::crypto::exchange::open(manifest, key, witness);
      recovered.insert(recovered.end(), plaintext.begin(), plaintext.end());
   }
   FORGE_ASSERT(std::string(recovered.begin(), recovered.end()) == body_bytes,
                "decrypted ordered file differs from the complete authenticated response body");
   const auto public_bytes = forge::raw::pack(*authenticated.retained);
   const auto public_text = std::string_view{reinterpret_cast<const char*>(public_bytes.data()), public_bytes.size()};
   FORGE_ASSERT(public_text.find("hidden-exchange-response") == std::string_view::npos &&
                    public_text.find("secret-sentinel") == std::string_view::npos,
                "retained verifier record disclosed hidden body or request credential");
   forge::asio::blocking::run(runtime, engine.shutdown());
   forge::asio::blocking::run(runtime, compute.shutdown());
   std::cout << "PASS real local MPC: visible exact request/header -> hidden ordered body H -> ciphertext -> plaintext\n";
   std::cout << "PASS production bind_exchange rejects H/coverage/order/origin/context/body/cipher mutations before authorization\n";
   std::cout << "PASS consumer nonce policy rejects authentic old MPC result relabeled with a fresh operation context\n";
}

} // namespace

int main(int argc, char** argv)
{
   try
   {
      actual_mpc_block_binding(std::filesystem::path{argc > 1 ? argv[1] : FORGE_TLSNOTARY_WORKER_PATH});
      std::cout << "Restricted raw HTTPS body exchange fixture PASS; generic HTTP/Labs/commercial proof NOT_RUN\n";
   }
   catch (const std::exception& error)
   {
      std::cerr << error.what() << '\n';
      return 1;
   }
   return 0;
}
