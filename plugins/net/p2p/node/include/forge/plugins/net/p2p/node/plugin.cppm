module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/any_io_executor.hpp>

#include <memory>
#include <optional>
#include <string>

export module forge.plugins.net.p2p.node.plugin;

export import forge.plugins.net.p2p.node.descriptor;

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.api.p2p.binding;
import forge.api.p2p.publication;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.config.core.component;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.plugins.net.p2p.node.types;

namespace forge::plugins::net::p2p::node::detail {

class api_publication_registry;

[[nodiscard]] std::shared_ptr<api_publication_registry> make_api_publication_registry();
[[nodiscard]] forge::api::p2p::publication
publish_api_publication(const std::shared_ptr<api_publication_registry>& registry,
                        forge::api::p2p::api_binding binding);
void bind_api_publication_registry_executor(
   const std::shared_ptr<api_publication_registry>& registry,
   boost::asio::any_io_executor owner_executor);
void attach_api_publication_registry(
   const std::shared_ptr<api_publication_registry>& registry,
   const std::shared_ptr<forge::net::p2p::node>& node);
[[nodiscard]] bool api_publication_registry_contains(
   const std::shared_ptr<api_publication_registry>& registry,
   const forge::net::p2p::protocol_id& protocol) noexcept;
void request_close_api_publication_registry(
   const std::shared_ptr<api_publication_registry>& registry) noexcept;
boost::asio::awaitable<void> async_close_api_publication_registry(
   const std::shared_ptr<api_publication_registry>& registry);

} // namespace forge::plugins::net::p2p::node::detail

export namespace forge::plugins::net::p2p::node {

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
   boost::asio::awaitable<void> after_initialize() override;
   boost::asio::awaitable<void> startup() override;
   void request_stop() noexcept override;
   boost::asio::awaitable<void> shutdown() override;

 private:
   struct impl;
   class api_impl;
   class diagnostics_source_adapter;
   class pubsub_source_adapter;

   friend void apply_config(impl&, const config&);

   std::shared_ptr<impl> impl_;
};

[[nodiscard]] forge::app::plugin_descriptor descriptor();

} // namespace forge::plugins::net::p2p::node
