module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.pubsub.plugin;

import forge.config.core.component;
import forge.config.core.decode;
import forge.exceptions;
import forge.net.p2p.pubsub;
import forge.plugins.net.p2p.pubsub.exceptions;
import forge.plugins.net.p2p.pubsub.types;

#include "details/config.hxx"

namespace forge::plugins::net::p2p::pubsub {
namespace {

void validate_topic_list(const std::vector<std::string>& values, std::string_view name) {
   auto seen = std::set<std::string>{};
   for (const auto& value : values) {
      if (value.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P PubSub topic policy contains empty topic",
                             forge::exceptions::ctx("list", std::string{name}));
      }
      if (!seen.insert(value).second) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                             "P2P PubSub topic policy contains duplicate topic",
                             forge::exceptions::ctx("list", std::string{name}), forge::exceptions::ctx("topic", value));
      }
   }
}

} // namespace

std::chrono::milliseconds to_ms(std::uint64_t value) {
   return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(value)};
}

config decode_config(const forge::config::core::component_view& view) {
   if (view.source().try_get("plugins.p2p.pubsub")) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                            "retired plugin configuration: plugins.p2p.pubsub; use plugins.net.p2p.pubsub");
   }
   auto decoded = forge::config::core::decode<config>(view.source(), view.section());
   if (!decoded.ok()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                          forge::config::core::format_decode_diagnostics("invalid P2P PubSub config",
                                                                 decoded.diagnostics));
   }
   return std::move(decoded.value);
}

void validate_config(const config& value) {
   validate_topic_list(value.allowed_topics, "allowed-topics");
   validate_topic_list(value.denied_topics, "denied-topics");
   forge::net::p2p::pubsub::validate(core_options_for(value));
}

forge::net::p2p::pubsub::options core_options_for(const config& settings) {
   auto out = forge::net::p2p::pubsub::options{};
   if (settings.partial_messages) {
      out.preferred = forge::net::p2p::pubsub::version::v1_3;
      out.partial_messages = true;
   }
   out.signatures =
      settings.sign_publishes ? forge::net::p2p::pubsub::signature_policy::strict_sign
                              : forge::net::p2p::pubsub::signature_policy::lax_no_sign;
   out.limits.max_data_size = static_cast<std::size_t>(settings.max_message_size);
   out.limits.max_message_size = static_cast<std::size_t>(settings.max_message_size) + 1024;
   out.limits.max_topics = static_cast<std::size_t>(settings.max_topics);
   out.limits.max_validation_queue = static_cast<std::size_t>(settings.max_active_handlers);
   return out;
}

} // namespace forge::plugins::net::p2p::pubsub
