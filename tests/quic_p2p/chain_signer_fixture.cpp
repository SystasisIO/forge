#include <memory>
#include <string>
#include <utility>

import forge.api.core.registry;
import forge.app.events;
import forge.app.plugin_context;
import forge.app.signals;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.asio.task;
import forge.chain.protocol.block;
import forge.crypto.asymmetric;
import forge.crypto.signer.configured_provider;
import forge.plugins.chain.signer.plugin;
import forge.plugins.chain.signer.types;

#include "chain_signer_fixture.hxx"

namespace forge::tests::p2p {
namespace signer = forge::plugins::chain::signer;

struct chain_signer_fixture::impl {
   explicit impl(forge::asio::runtime& value) : runtime{value}, scheduler{value} {}
   forge::asio::runtime& runtime;
   forge::asio::task::scheduler scheduler;
   forge::api::core::registry registry;
   forge::app::signal_bus signals;
   forge::app::event_bus events;
   std::unique_ptr<signer::plugin> plugin;
};

chain_signer_fixture::chain_signer_fixture(forge::asio::runtime& runtime, forge::chain::protocol::chain_id chain,
                                           forge::crypto::asymmetric::private_key private_key,
                                           forge::crypto::digest::sha256 fingerprint)
    : impl_{std::make_unique<impl>(runtime)} {
   const auto public_key = private_key.get_public_key();
   const auto provider = forge::crypto::signer::configured_provider::from_private_key(
       {.value = "producer-key"},
       forge::crypto::core::secret_string{forge::crypto::asymmetric::encoding::forge().format(private_key)});
   auto settings = signer::config{};
   settings.transaction_profiles.push_back({
       .name = "writer-transaction",
       .chain_id = chain.str(),
       .signing = {.provider = "k1",
                   .key_id = "producer-key",
                   .expected_public_key = forge::crypto::asymmetric::encoding::forge().format(public_key)},
       .authorization = signer::remote_authorization::profile_only,
       .callers = {{.source = forge::api::auth::caller_source::p2p_peer, .fingerprint = fingerprint.str()}},
       .actions = {{.account = "storage", .action = "write", .actor = "writer", .permission = "active"}},
   });
   settings.block_profiles.push_back({
       .name = "writer-block",
       .chain_id = chain.str(),
       .producer = "writer",
       .signing = {{.provider = "k1",
                    .key_id = "producer-key",
                    .expected_public_key = forge::crypto::asymmetric::encoding::forge().format(public_key)}},
       .callers = {{.source = forge::api::auth::caller_source::p2p_peer, .fingerprint = fingerprint.str()}},
   });
   impl_->plugin = std::make_unique<signer::plugin>(signer::plugin_options{
       .providers = {{.name = "k1", .value = provider}},
       .initial_config = std::move(settings),
       .now = [] { return forge::chain::protocol::time_point_sec{1'700'000'000U}; },
   });
   auto installer = forge::api::core::installer{impl_->registry};
   forge::asio::blocking::run(runtime, impl_->plugin->provide(installer));
   auto context = forge::app::plugin_context{impl_->scheduler, impl_->registry, impl_->signals, impl_->events};
   forge::asio::blocking::run(runtime, impl_->plugin->initialize(context));
   forge::asio::blocking::run(runtime, impl_->plugin->startup());
}

chain_signer_fixture::~chain_signer_fixture() {
   impl_->plugin->request_stop();
   forge::asio::blocking::run(impl_->runtime, impl_->plugin->shutdown());
}

forge::api::core::registry& chain_signer_fixture::apis() {
   return impl_->registry;
}

} // namespace forge::tests::p2p
