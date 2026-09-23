#include <boost/asio/awaitable.hpp>

#include <concepts>
#include <utility>
#include <vector>

import package.chain_api_component.read_e2e;
import package.chain_api_component.surface_checks;
import package.chain_api_component.verifier_fixture;
import package.chain_api_component.write_e2e;
import forge.api.core.connection;
import forge.chain.api.transaction_signer;
import forge.chain.api.block_signer;
import forge.chain.protocol.block_signing;

bool portable_verified_client_package_contract();

int main() {
   static_assert(forge::api::core::remote_interface<forge::chain::api::transaction_signer>);
   static_assert(forge::api::core::remote_interface<forge::chain::api::block_signer>);
   static_assert(std::same_as<decltype(std::declval<forge::chain::api::transaction_signer&>().sign(
                                  std::declval<forge::chain::transaction::unsigned_transaction>(),
                                  std::declval<forge::api::auth::authenticated_caller>())),
                              boost::asio::awaitable<forge::chain::transaction::prepared_transaction>>);
   static_assert(std::same_as<decltype(std::declval<forge::chain::api::block_signer&>().sign(
                                  std::declval<forge::chain::protocol::block_sign_request>(),
                                  std::declval<forge::api::auth::authenticated_caller>())),
                              boost::asio::awaitable<std::vector<forge::chain::protocol::signature>>>);

   package_chain_api_component::check_read_api_surface();
   package_chain_api_component::check_write_api_surface();
   package_chain_api_component::check_state_surface();
   package_chain_api_component::check_block_surface();
   package_chain_api_component::check_info_admin_surface();
   package_chain_api_component::check_codec_surface();
   package_chain_api_component::run_verifier_component_checks();
   package_chain_api_component::run_read_e2e();
   package_chain_api_component::run_write_e2e();
   return portable_verified_client_package_contract() ? 0 : 1;
}
