#include <boost/asio/awaitable.hpp>

#include <concepts>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

import forge.api.core.types;
import forge.api.core.connection;
import forge.chain.api.finality_signer;
import forge.chain.api.block_signer;
import forge.chain.protocol.block_signing;
import forge.chain.api.transaction_signer;
import forge.plugins.chain.signer.descriptor;
import forge.plugins.chain.signer.types;

int main() {
   using transaction_signer = forge::chain::api::transaction_signer;
   using block_signer = forge::chain::api::block_signer;
   using finality_signer = forge::chain::api::finality_signer;

   static_assert(forge::api::core::local_interface<transaction_signer>);
   static_assert(forge::api::core::remote_interface<transaction_signer>);
   static_assert(forge::api::core::local_interface<block_signer>);
   static_assert(forge::api::core::remote_interface<block_signer>);
   static_assert(forge::api::core::local_interface<finality_signer>);
   static_assert(!forge::api::core::remote_interface<finality_signer>);
   static_assert(std::same_as<decltype(std::declval<transaction_signer&>().sign(
                                  std::declval<forge::chain::transaction::unsigned_transaction>(),
                                  std::declval<forge::api::auth::authenticated_caller>())),
                              boost::asio::awaitable<forge::chain::transaction::prepared_transaction>>);
   static_assert(std::same_as<decltype(std::declval<finality_signer&>().sign_vote(
                                  std::declval<forge::chain::savanna::block_ref>(),
                                  std::declval<forge::chain::savanna::vote_kind>())),
                              boost::asio::awaitable<forge::chain::savanna::finalizer_vote>>);
   static_assert(std::same_as<decltype(std::declval<block_signer&>().sign(
                                  std::declval<forge::chain::protocol::block_sign_request>(),
                                  std::declval<forge::api::auth::authenticated_caller>())),
                              boost::asio::awaitable<std::vector<forge::chain::protocol::signature>>>);
   static_assert(std::same_as<decltype(forge::plugins::chain::signer::transaction_profile::signing),
                              forge::plugins::chain::signer::key_binding>);
   static_assert(std::same_as<decltype(forge::plugins::chain::signer::plugin_options::providers),
                              std::vector<forge::plugins::chain::signer::named_provider>>);
   static_assert(std::same_as<decltype(forge::plugins::chain::signer::config::max_inflight), std::uint64_t>);

   const auto descriptor = forge::plugins::chain::signer::default_descriptor();
   return descriptor.id.value == std::string_view{"forge.plugins.chain.signer"} ? 0 : 1;
}
