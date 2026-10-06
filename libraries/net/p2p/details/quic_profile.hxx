#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include "direct_transport.hxx"
#include "stream_upgrade.hxx"

namespace forge::net::p2p::direct {

namespace detail {
struct pending_quic_connection;
class quic_client_token_cache;
} // namespace detail

class quic_profile final {
   static constexpr std::size_t max_coordinated_candidates = 32;

   struct listener_entry {
      std::shared_ptr<forge::net::quic::listener> value;
      std::shared_ptr<void> native_lifetime;
      forge::net::p2p::endpoint local;
      bool active = true;
   };

 public:
   quic_profile(forge::asio::runtime& runtime_value, const node::options& options_value,
                resource_manager resources_value, std::shared_ptr<forge::net::p2p::detail::connection_gate> gate);

   [[nodiscard]] bool supports(const forge::net::p2p::endpoint& endpoint) const noexcept;

   [[nodiscard]] bool listening() const noexcept;

   [[nodiscard]] std::vector<forge::net::p2p::endpoint> local_endpoints() const;

   forge::net::p2p::endpoint listen(forge::net::p2p::endpoint endpoint);

   void stop();

   boost::asio::awaitable<void> async_stop();

   boost::asio::awaitable<connection>
   async_connect(forge::net::p2p::endpoint endpoint, const node::connect_options& options,
                 std::shared_ptr<cancellation_latch> cancellation, std::shared_ptr<void> native_lifetime,
                 authenticated_admission_handler authenticated, tcp_transport_progress_handler,
                 std::shared_ptr<forge::net::quic::connector> connector = {}, bool strict_identity = false,
                 native_socket_admission_handler socket_admission = {});

   boost::asio::awaitable<connection> async_connect_coordinated(endpoint remote, peer_id expected_peer,
                                                                upgrade_role role, std::chrono::milliseconds budget,
                                                                std::shared_ptr<cancellation_latch> cancellation,
                                                                authenticated_admission_handler authenticated,
                                                                std::optional<endpoint> requested,
                                                                std::shared_ptr<forge::net::p2p::detail::coordinated_dial> owner = {});
   void prepare_coordinated(const std::shared_ptr<forge::net::p2p::detail::coordinated_dial>& owner);
   void release_coordinated(const std::shared_ptr<forge::net::p2p::detail::coordinated_dial>& owner) noexcept;

   boost::asio::awaitable<connection> async_accept(forge::net::p2p::endpoint endpoint);

 private:
   boost::asio::awaitable<connection> async_promote_inbound(forge::net::quic::connection quic, const std::string& key,
                                                            const std::shared_ptr<forge::net::quic::listener>& listener,
                                                            std::optional<peer_id> expected = {},
                                                            authenticated_admission_handler authenticated = {},
                                                            std::shared_ptr<void> lifetime = {});

   boost::asio::awaitable<connection> async_wait_coordinated_inbound(listener_entry source, const std::string& key,
                                                                     endpoint local, endpoint remote,
                                                                     peer_id expected_peer,
                                                                     std::chrono::milliseconds budget,
                                                                     std::shared_ptr<cancellation_latch> cancellation,
                                                                     authenticated_admission_handler authenticated,
                                                                     std::shared_ptr<detail::pending_quic_connection> prepared = {});

   [[nodiscard]] bool listener_is_current(const std::string& key,
                                          const std::shared_ptr<forge::net::quic::listener>& listener) const;

   [[nodiscard]] std::vector<std::shared_ptr<forge::net::quic::listener>> stop_listeners();

   [[nodiscard]] std::vector<std::shared_ptr<forge::net::quic::listener>> listener_snapshot() const;

   void track(const std::shared_ptr<cancellation_latch>& operation);

   [[nodiscard]] forge::net::quic::server_options server_options() const;

   forge::asio::runtime& runtime_;
   const node::options& options_;
   resource_manager resources_;
   std::shared_ptr<forge::net::p2p::detail::connection_gate> gate_;
   mutable std::mutex listeners_mutex_;
   std::map<std::string, listener_entry> listeners_;
   std::map<std::tuple<std::string, std::string, std::string, std::string>,
            std::weak_ptr<detail::pending_quic_connection>>
       coordinated_inbound_;
   bool listeners_stopped_ = false;
   std::mutex active_mutex_;
   std::vector<std::weak_ptr<cancellation_latch>> active_;
   bool stopped_ = false;
   std::shared_ptr<detail::quic_client_token_cache> client_tokens_;
};

} // namespace forge::net::p2p::direct
