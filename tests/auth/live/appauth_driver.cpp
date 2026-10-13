#include "common.hxx"

#include <utility>

import forge.auth.appauth.provider;

#include "workflow.hxx"

namespace auth_live {

int run_appauth(int argc, char** argv, std::function<void()> run_ui, std::function<void()> stop_ui) {
   return run(argc, argv, [](const options& config) -> boost::asio::awaitable<void> {
      auto provider = forge::auth::appauth::provider::create({
         .issuer = config.issuer,
         .client_id = config.client,
         .scopes = config.scopes,
         .audience = config.audience,
         .keychain_service = "org.forge.tests.auth.live." + config.run_id,
         .keychain_account = config.account,
      });
      co_await probe(*provider, provider.get(), config);
   }, std::move(run_ui), std::move(stop_ui));
}

} // namespace auth_live
