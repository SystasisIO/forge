#include <boost/asio/awaitable.hpp>
#include <concepts>
#include <string>
#include <utility>

import forge.crypto.wallet.api;
import forge.raw.raw;

int main() {
   namespace wallet = forge::crypto::wallet;
   static_assert(forge::api::core::server_supplied<forge::api::auth::authenticated_caller>::required);
   static_assert(std::same_as<decltype(std::declval<wallet::api&>().create({}, {})),
                              boost::asio::awaitable<wallet::wallet_status>>);
   const auto request = wallet::password_request{"alice", "package-test-password"};
   const auto packed = forge::raw::pack(request);
   const auto decoded = forge::raw::unpack<wallet::password_request>(packed);
   return decoded.wallet == request.wallet && decoded.password == request.password ? 0 : 1;
}
