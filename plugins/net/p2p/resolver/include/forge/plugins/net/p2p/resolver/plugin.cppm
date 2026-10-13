module;

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

export module forge.plugins.net.p2p.resolver.plugin;

import forge.api.core.registry;
import forge.api.core.binding;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.config.core.component;
import forge.net.p2p.protocol;
import forge.plugins.net.p2p.resolver.types;

extern "C++" {
namespace forge::plugins::net::p2p::resolver::detail {

class managed_remote_invoker;

} // namespace forge::plugins::net::p2p::resolver::detail
}

namespace forge::plugins::net::p2p::resolver::detail {

class resolver_protocol;

} // namespace forge::plugins::net::p2p::resolver::detail

export namespace forge::plugins::net::p2p::resolver {

class plugin final : public forge::app::plugin {
 public:
   [[nodiscard]] static forge::app::plugin_descriptor descriptor();

   plugin();
   ~plugin() override;

   plugin(const plugin&) = delete;
   plugin& operator=(const plugin&) = delete;

   [[nodiscard]] forge::app::plugin_id id() const override;
   [[nodiscard]] std::string version() const override;
   [[nodiscard]] std::optional<forge::config::core::component_descriptor> describe_config() const override;
   boost::asio::awaitable<void> configure(forge::config::core::component_view view) override;
   boost::asio::awaitable<void> provide(forge::api::core::provider& provider) override;
   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override;
   boost::asio::awaitable<void> startup() override;
   void request_stop() noexcept override;
   boost::asio::awaitable<void> shutdown() override;

 private:
   struct impl;

   friend class detail::managed_remote_invoker;
   friend class detail::resolver_protocol;
   friend response query_resolver_protocol(const std::shared_ptr<impl>&, const query&);
   friend forge::api::core::binding_plan
   make_resolver_protocol_plan(forge::api::core::registry&, std::weak_ptr<impl>);

   class api_impl;
   class managed_api_impl;
   std::shared_ptr<impl> impl_;
};

[[nodiscard]] forge::net::p2p::protocol_id default_protocol();

} // namespace forge::plugins::net::p2p::resolver
