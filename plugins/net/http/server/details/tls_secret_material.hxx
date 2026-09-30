#pragma once

#include <string>

namespace forge::plugins::net::http::server {

struct tls_secret_material {
   std::string certificate_chain;
   std::string private_key;
   std::string client_ca;

   tls_secret_material();
   ~tls_secret_material();

   tls_secret_material(const tls_secret_material&) = delete;
   tls_secret_material& operator=(const tls_secret_material&) = delete;
   tls_secret_material(tls_secret_material&&) noexcept;
   tls_secret_material& operator=(tls_secret_material&& other) noexcept;
};

} // namespace forge::plugins::net::http::server
