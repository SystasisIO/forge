module;

#include <string>
#include <utility>

module forge.plugins.net.http.server.plugin;

import forge.crypto.core.secret_bytes;

#include "details/tls_secret_material.hxx"

namespace forge::plugins::net::http::server {
namespace {

void clear_text(std::string& value) noexcept {
   forge::crypto::core::secure_erase(value);
}

} // namespace

tls_secret_material::tls_secret_material() = default;

tls_secret_material::~tls_secret_material() {
   clear_text(certificate_chain);
   clear_text(private_key);
   clear_text(client_ca);
}

tls_secret_material::tls_secret_material(tls_secret_material&&) noexcept = default;

tls_secret_material& tls_secret_material::operator=(tls_secret_material&& other) noexcept {
   if (this != &other) {
      clear_text(certificate_chain);
      clear_text(private_key);
      clear_text(client_ca);
      certificate_chain = std::move(other.certificate_chain);
      private_key = std::move(other.private_key);
      client_ca = std::move(other.client_ca);
   }
   return *this;
}

} // namespace forge::plugins::net::http::server
