#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import test.api_chain.counter_client;
import test.api_chain.counter_peer;
import test.api_chain.counter_http;
import forge.api.chain.destination;
import package.chain_api_component.p2p_runtime;
import package.chain_api_component.test_support;
import forge.app.application_shell;
import forge.asio.blocking;
import forge.api.core.registry;
import forge.chain.api.exceptions;
import forge.chain.protocol.transaction_query;
import forge.config.core.value;
import forge.raw.codec;

namespace {
using namespace std::chrono_literals;
using package_chain_api_component::require;
namespace protocol = forge::chain::protocol;
namespace fixture = package_chain_api_component;
using app = forge::app::application_shell;
using replay = demo::replay;
void stop(app& application) {
   application.request_stop();
   forge::asio::blocking::run(application.runtime(), application.shutdown());
}

void test_restart(const std::filesystem::path& root, replay original, const demo::signer_fixture& signing,
                  demo::counter_peer& peer, const forge::api::chain::destination& target,
                  const std::string& p2p_bootstrap, const std::string& http_url,
                  const std::function<void()>& allow_replies) {
   std::unique_ptr<app> client;
   try {
      auto config = demo::databases(root);
      const bool p2p = !p2p_bootstrap.empty();
      if (!http_url.empty()) {
         using forge::config::core::value;
         config.set("plugins.chain.outbox.http",
                    value::array_type{value::object_type{{"name", "primary"}, {"url", http_url}}});
      }
      client = demo::start_client(config, demo::delivery_plugin(peer.finality(), original, p2p), signing.provide,
                                  p2p_bootstrap);
      const auto view = forge::api::core::view{client->apis()};
      bool pending = false;
      try {
         (void)forge::asio::blocking::run(client->runtime(), demo::increment(view, target, 7));
      } catch (const forge::chain::api::exceptions::unavailable&) {
         pending = true;
      }
      require(pending, "A missing submission response must leave the call pending");
      require(peer.wait_for_submissions(1, 5s), "Counter call did not reach the peer");
      const auto first = peer.submissions().front();
      const auto id = forge::asio::blocking::run(client->runtime(), demo::admitted_transaction(view));
      require(id == first.id(), "Saved client admission does not identify the packet received by the server");
      require(*signing.calls == 1 && peer.value() == 7, "Expected one signer call and an unresolved delivery");
      stop(*client);
      client.reset();

      allow_replies();
      const auto before = peer.submissions().size();
      if (original == replay::reconcile_only) {
         require(before == 1, "Reconciliation resubmitted before restart");
         peer.include_first_submission(true);
      } else {
         // The counter already advanced, but the next status query cannot yet see the receipt.
         peer.include_first_submission(false);
         peer.allow_execution();
      }
      require(peer.value() == 8, "First submission did not execute before recovery");
      // A configuration change affects newly admitted calls only.
      const auto replacement = original == replay::reconcile_only ? replay::same_packed : replay::reconcile_only;
      auto reject = std::make_shared<std::atomic_bool>(false);
      client = demo::start_client(config, demo::delivery_plugin(peer.finality(), replacement, p2p, reject),
                                  signing.provide, p2p_bootstrap);
      if (original == replay::same_packed) {
         require(peer.wait_for_submissions(before + 1, 12s), "Restart did not deliver the saved packet");
      }
      require(peer.wait_for_proof(12s), "Restart did not reconcile the saved transaction ID");
      const auto restarted = forge::api::core::view{client->apis()};
      const auto restored_id = forge::asio::blocking::run(client->runtime(), demo::admitted_transaction(restarted));
      require(restored_id == id, "Restart lost the durable counter admission");
      const auto recovered =
          forge::asio::blocking::run(client->runtime(), demo::status(restarted, target, restored_id));
      require(recovered.id == id && recovered.state == protocol::transaction_lifecycle::finalized &&
                  recovered.receipt && recovered.receipt->status == decltype(recovered.receipt->status)::executed,
              "The verified finalized receipt did not identify the executed counter action");
      const auto sent = peer.submissions();
      if (original == replay::reconcile_only)
         require(sent.size() == 1, "Reconciliation replayed the operation");
      for (const auto& packet : sent) {
         require(packet.id() == id && forge::raw::pack(packet) == forge::raw::pack(first),
                 "Redelivery changed signed bytes or transaction ID");
      }
      require(*signing.calls == 1 && peer.value() == 8, "Restart signed or incremented the counter more than once");
      peer.allow_execution();
      const auto next = forge::asio::blocking::run(client->runtime(), demo::increment(restarted, target, 8));
      require(next == peer.submissions().back().id() && next != id && peer.value() == 9 && *signing.calls == 2,
              "Acknowledged increment must return the ID of the new signed action");
      reject->store(true);
      bool declined = false;
      try {
         (void)forge::asio::blocking::run(client->runtime(), demo::increment(restarted, target, 9));
      } catch (const std::domain_error&) {
         declined = true;
      }
      require(declined && *signing.calls == 3 && peer.submissions().size() == sent.size() + 1 && peer.value() == 9,
              "A failed local admission submitted a signed action");
      // The tentative overwrite of the client's last accepted ID must roll back.
      require(forge::asio::blocking::run(client->runtime(), demo::admitted_transaction(restarted)) == next,
              "Rolled-back admission replaced the accepted transaction");
      if (original == replay::reconcile_only && !http_url.empty()) {
         forge::asio::blocking::run(client->runtime(),
                                    demo::test_staged_http_profile(root / "staged", recovered.context.chain, first));
      }
      stop(*client);
      client.reset();
   } catch (...) {
      if (client)
         fixture::shutdown_after_failure(*client, true);
      throw;
   }
}

void p2p_restart(const std::filesystem::path& root, replay original) {
   auto signing = demo::make_signer();
   demo::counter_peer peer{signing.key};
   auto server_id = fixture::test_peer(0x51);
   auto server_config = fixture::p2p_config(server_id);
   server_config.set("plugins.net.p2p.node.listen", forge::config::core::value::array_type{
                                                        forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});
   fixture::p2p_server_application server{peer.publication()};
   bool server_started = false;
   try {
      server.configure(server_config);
      forge::asio::blocking::run(server.runtime(), server.startup());
      server_started = true;
      test_restart(root, original, signing, peer, server_id, demo::endpoint(server), {}, [] {});
      stop(server);
   } catch (...) {
      fixture::shutdown_after_failure(server, server_started);
      throw;
   }
}

void http_restart(const std::filesystem::path& root, replay original) {
   auto signing = demo::make_signer();
   demo::counter_peer peer{signing.key, true};
   demo::counter_http_server server{[&](forge::api::core::installer apis) { peer.install(apis); }};
   try {
      test_restart(root, original, signing, peer, forge::api::chain::http_endpoint{"primary"}, {}, server.url(), [&] {
         require(server.disconnected() > 0, "The signed POST must be accepted before its HTTP socket is closed");
         server.allow_responses();
      });
   } catch (...) {
      server.stop();
      throw;
   }
   server.stop();
}
} // namespace

int main(int argc, char** argv) {
   const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
   const auto root = std::filesystem::temp_directory_path() / ("forge-api-chain-counter-" + std::to_string(nonce));
   try {
      require(std::filesystem::create_directory(root), "Temporary counter test directory is already in use");
      std::filesystem::permissions(root, std::filesystem::perms::owner_all);
      const std::string_view transport = argc > 1 ? argv[1] : "both";
      require(transport == "p2p" || transport == "http" || transport == "both", "Unknown test transport");
      if (transport != "http") {
         p2p_restart(root / "p2p-reconcile", replay::reconcile_only);
         p2p_restart(root / "p2p-replay", replay::same_packed);
      }
      if (transport != "p2p") {
         http_restart(root / "http-reconcile", replay::reconcile_only);
         http_restart(root / "http-replay", replay::same_packed);
      }
      std::filesystem::remove_all(root);
      std::cout << "PASS counter " << transport
                << ": two named MDBX stores, restart, receipt and one execution per policy\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << "FAIL counter: " << error.what() << " (MDBX files: " << root << ")\n";
      return 1;
   }
}
