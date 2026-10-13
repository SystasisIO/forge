#pragma once

// Include after importing forge.auth.oauth2.token_provider/interactive_authorizer.
#include "common.hxx"

namespace auth_live {

boost::asio::awaitable<void> probe(forge::auth::oauth2::token_provider& provider,
                                 forge::auth::oauth2::interactive_authorizer* authorizer,
                                 const options& config);

} // namespace auth_live
