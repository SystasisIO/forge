module;

#include <forge/exceptions/macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

module forge.plugins.net.p2p.resolver.plugin;

import forge.api.transport.options;
import forge.config.core.component;
import forge.config.core.decode;
import forge.exceptions;
import forge.plugins.net.p2p.resolver.exceptions;
import forge.plugins.net.p2p.resolver.types;

#include "details/config.hxx"

namespace forge::plugins::net::p2p::resolver {
namespace {

[[nodiscard]] bool valid_protocol(std::string_view value) noexcept {
   return !value.empty() && value.front() == '/';
}

} // namespace

std::chrono::milliseconds to_ms(std::uint64_t value) {
   return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(value)};
}

config decode_config(const forge::config::core::component_view& view) {
   if (view.source().try_get("plugins.p2p.resolver")) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                            "retired plugin configuration: plugins.p2p.resolver; use plugins.net.p2p.resolver");
   }
   auto decoded = forge::config::core::decode<config>(view.source(), view.section());
   if (!decoded.ok()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, forge::config::core::format_decode_diagnostics(
                                                            "invalid P2P API resolver config", decoded.diagnostics));
   }
   return std::move(decoded.value);
}

void validate_config(const config& value) {
   if (!valid_protocol(value.protocol_id)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "resolver protocol id is invalid",
                            forge::exceptions::ctx("protocol", value.protocol_id));
   }
}

void validate_transport_options(const forge::api::transport::options& value) {
   if (value.codec.value.empty() || value.max_inflight == 0 || value.max_frame_size == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "resolver API transport options are invalid",
                            forge::exceptions::ctx("codec", value.codec.value),
                            forge::exceptions::ctx("max_inflight", value.max_inflight),
                            forge::exceptions::ctx("max_frame_size", value.max_frame_size));
   }
}

} // namespace forge::plugins::net::p2p::resolver
