module;

#include <concepts>
#include <optional>
#include <type_traits>
#include <vector>

module package.chain_api_component.surface_checks;

import forge.chain.protocol.account_ram_correction;
import forge.chain.protocol.admin;
import forge.chain.protocol.finalizer_vote_record;
import forge.chain.protocol.info;
import forge.chain.protocol.protocol_feature;
import forge.chain.protocol.resource_limits_config;
import forge.chain.protocol.resource_limits_state;

namespace package_chain_api_component {

void check_info_admin_surface() {
   namespace protocol = forge::chain::protocol;

   static_assert(std::derived_from<protocol::supported_protocol_feature, protocol::protocol_feature>);
   static_assert(std::is_same_v<decltype(protocol::info_response{}.resource_config), protocol::resource_limits_config>);
   static_assert(std::is_same_v<decltype(protocol::info_response{}.resource_state), protocol::resource_limits_state>);
   static_assert(std::is_same_v<decltype(protocol::ram_corrections_response{}.rows),
                                std::vector<protocol::account_ram_correction>>);
   static_assert(std::is_same_v<decltype(protocol::operator_identity{}.block_public_key), protocol::public_key>);
   static_assert(
       std::is_same_v<decltype(protocol::operator_identity{}.finalizer_public_key), forge::crypto::bls::public_key>);
   static_assert(std::is_same_v<decltype(protocol::finalizer_status{}.last_emitted_vote),
                                std::optional<protocol::finalizer_vote_record>>);
}

} // namespace package_chain_api_component
