module;

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

export module forge.plugins.net.p2p.resolver.plugin;

export import forge.plugins.net.p2p.resolver.descriptor;

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.api.p2p.publication;
import forge.api.transport.options;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.asio.task;
import forge.config.core.component;
import forge.net.p2p.identity;
import forge.net.p2p.protocol;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.types;
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

[[nodiscard]] forge::app::plugin_descriptor descriptor();
[[nodiscard]] forge::net::p2p::protocol_id default_protocol();

} // namespace forge::plugins::net::p2p::resolver

namespace forge::plugins::net::p2p::resolver::detail {

class publication_catalog;

[[nodiscard]] std::shared_ptr<publication_catalog>
make_publication_catalog(forge::asio::task::scheduler& scheduler);
[[nodiscard]] forge::api::p2p::publication
publish_catalog_api(const std::shared_ptr<publication_catalog>& catalog,
                    forge::plugins::net::p2p::node::api& p2p, forge::api::core::binding_plan plan,
                    forge::net::p2p::protocol_id protocol, forge::api::transport::options options,
                    std::vector<entry> entries, std::size_t max_apis);
[[nodiscard]] std::vector<entry>
publication_catalog_snapshot(const std::shared_ptr<publication_catalog>& catalog);
void request_close_publication_catalog(const std::shared_ptr<publication_catalog>& catalog) noexcept;
boost::asio::awaitable<void>
async_close_publication_catalog(const std::shared_ptr<publication_catalog>& catalog);

} // namespace forge::plugins::net::p2p::resolver::detail

namespace forge::plugins::net::p2p::resolver {

[[nodiscard]] response query_resolver_protocol(const std::shared_ptr<plugin::impl>& owner, const query& request);

[[nodiscard]] forge::api::core::binding_plan
make_resolver_protocol_plan(forge::api::core::registry& registry, std::weak_ptr<plugin::impl> owner);

[[nodiscard]] boost::asio::awaitable<std::vector<entry>>
query_resolver_peer(forge::plugins::net::p2p::node::api& p2p, forge::net::p2p::peer_id peer,
                    forge::net::p2p::protocol_id protocol,
                    forge::plugins::net::p2p::node::remote_options options);

} // namespace forge::plugins::net::p2p::resolver
