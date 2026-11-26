module;

#include <boost/asio/awaitable.hpp>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

export module test.api_chain.counter_client;

import forge.app.application_shell;
import forge.app.plugin_registry;
import forge.api.core.registry;
import forge.chain.api.finality;
import forge.chain.protocol.transaction_query;
import forge.config.core.document;
import forge.crypto.asymmetric;
import forge.api.chain.destination;

export namespace demo {

enum class replay { reconcile_only, same_packed };

struct signer_fixture {
   forge::crypto::asymmetric::public_key key;
   std::shared_ptr<std::atomic_uint> calls;
   std::function<void(forge::app::application_context&)> provide;
};

signer_fixture make_signer();
void provide_counter(forge::app::application_context& context);
forge::app::plugin_descriptor database_plugin();
forge::config::core::document databases(const std::filesystem::path& root);
forge::app::plugin_descriptor delivery_plugin(std::shared_ptr<forge::chain::api::finality_verifier> finality,
                                              replay mode, bool p2p, std::shared_ptr<std::atomic_bool> reject = {});
std::unique_ptr<forge::app::application_shell>
start_client(forge::config::core::document config, forge::app::plugin_descriptor delivery,
             std::function<void(forge::app::application_context&)> signer, std::string p2p_bootstrap);
std::string endpoint(forge::app::application_shell& server);
boost::asio::awaitable<forge::chain::protocol::transaction_id> admitted_transaction(forge::api::core::view apis);
boost::asio::awaitable<void> test_staged_http_profile(const std::filesystem::path& path,
                                                      forge::chain::protocol::chain_id chain,
                                                      const forge::chain::protocol::packed_transaction& packet);
boost::asio::awaitable<forge::chain::protocol::transaction_id>
increment(forge::api::core::view apis, forge::api::chain::destination target, std::uint64_t expected);
boost::asio::awaitable<forge::chain::protocol::transaction_id>
increment(forge::api::core::view apis, forge::api::chain::destination target, std::uint64_t expected,
          forge::chain::protocol::block_id block, forge::chain::protocol::time_point_sec expiration);
boost::asio::awaitable<forge::chain::protocol::block_id> finalized(forge::api::core::view apis,
                                                                   forge::api::chain::destination target);
boost::asio::awaitable<std::optional<std::uint64_t>>
find_counter(forge::api::core::view apis, forge::api::chain::destination target, forge::chain::protocol::block_id block,
             std::uint64_t key);
boost::asio::awaitable<void> check_read_session(forge::api::core::view apis, forge::api::chain::destination target,
                                                forge::chain::protocol::block_id block);
boost::asio::awaitable<forge::chain::protocol::transaction_status_response>
status(forge::api::core::view apis, forge::api::chain::destination target, forge::chain::protocol::transaction_id id);

} // namespace demo
