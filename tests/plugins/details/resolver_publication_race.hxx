#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace forge::tests::plugins {

class resolver_publish_barrier final {
 public:
   void bind_owner_executor(boost::asio::any_io_executor executor);
   void arm();
   [[nodiscard]] bool wait_until_entered(std::chrono::milliseconds timeout);
   void release();
   void wait_if_armed();
   [[nodiscard]] boost::asio::any_io_executor owner_executor() const;
   void track_protocol(std::string protocol);
   void record_close(const std::string& protocol) noexcept;
   [[nodiscard]] std::size_t close_calls(const std::string& protocol) const noexcept;
   void record_drain(const std::string& protocol) noexcept;
   [[nodiscard]] std::size_t drain_calls(const std::string& protocol) const noexcept;
   [[nodiscard]] std::size_t publish_calls(const std::string& protocol) const noexcept;

 private:
   mutable std::mutex mutex_;
   std::condition_variable changed_;
   boost::asio::any_io_executor owner_executor_;
   bool armed_ = false;
   bool entered_ = false;
   bool released_ = false;
   std::map<std::string, std::size_t, std::less<>> close_calls_;
   std::map<std::string, std::size_t, std::less<>> drain_calls_;
   std::map<std::string, std::size_t, std::less<>> publish_calls_;
};

class resolver_publication_race_node final : public forge::plugins::net::p2p::node::api {
 public:
   explicit resolver_publication_race_node(std::shared_ptr<resolver_publish_barrier> barrier);

   [[nodiscard]] forge::net::p2p::peer_id local_peer() const override;
   [[nodiscard]] std::optional<forge::net::p2p::endpoint> local_endpoint() const override;
   [[nodiscard]] std::vector<forge::net::p2p::endpoint> local_endpoints() const override;
   [[nodiscard]] forge::plugins::net::p2p::node::info network_info() const override;
   boost::asio::awaitable<forge::net::p2p::node::session_info>
   connect(forge::net::p2p::endpoint, forge::net::p2p::node::connect_options) override;
   boost::asio::awaitable<forge::net::p2p::node::session_info>
   connect(forge::multiformats::multiaddr, forge::net::p2p::node::connect_options) override;
   boost::asio::awaitable<std::chrono::milliseconds>
   ping(forge::net::p2p::peer_id, forge::net::p2p::node::open_options) override;
   [[nodiscard]] bool is_peer_protected(const forge::net::p2p::peer_id&) const override;
   [[nodiscard]] forge::plugins::net::p2p::node::peer_contribution protect_peer(forge::net::p2p::peer_id) override;
   [[nodiscard]] forge::plugins::net::p2p::node::peer_contribution tag_peer(forge::net::p2p::peer_id, std::int64_t) override;
   [[nodiscard]] forge::api::p2p::publication
   publish_api(forge::api::core::binding_plan plan, forge::net::p2p::protocol_id protocol) override;
   [[nodiscard]] forge::api::p2p::publication
   publish_api(forge::api::core::binding_plan plan, forge::net::p2p::protocol_id protocol,
               forge::api::transport::options options) override;
   boost::asio::awaitable<forge::api::transport::connection>
   open_api_connection(forge::net::p2p::peer_id peer, forge::net::p2p::protocol_id protocol,
                       forge::plugins::net::p2p::node::remote_options options) override;

 private:
   std::shared_ptr<resolver_publish_barrier> barrier_;
};

class resolver_publication_race_node_plugin final : public forge::app::plugin {
 public:
   explicit resolver_publication_race_node_plugin(std::shared_ptr<resolver_publish_barrier> barrier);

   [[nodiscard]] forge::app::plugin_id id() const override;
   [[nodiscard]] std::string version() const override;
   boost::asio::awaitable<void> on_provide(forge::api::core::provider& provider) override;
   boost::asio::awaitable<void> on_initialize(forge::app::plugin_context& context) override;
   boost::asio::awaitable<void> on_startup() override;
   boost::asio::awaitable<void> on_shutdown() override;

 private:
   std::shared_ptr<resolver_publish_barrier> barrier_;
};

class resolver_publication_race_application final : public forge::app::application_shell {
 public:
   explicit resolver_publication_race_application(
      std::shared_ptr<resolver_publish_barrier> barrier,
      forge::app::application_shell_options options = {});

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override;

 private:
   std::shared_ptr<resolver_publish_barrier> barrier_;
};

} // namespace forge::tests::plugins
