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

class tcp_profile final {
   static constexpr std::size_t max_coordinated_candidates = 32;

   struct cancel_current_scope {
      std::shared_ptr<cancellation_latch> value;
      ~cancel_current_scope();
   };

   struct listener_entry {
      std::shared_ptr<forge::net::tcp::listener> value;
      std::shared_ptr<void> native_lifetime;
      forge::net::p2p::endpoint local;
      bool active = true;
   };

 public:
   tcp_profile(forge::asio::runtime& runtime_value, const node::options& options_value,
               const libp2p_identity_material& identity_value, resource_manager resources_value,
               std::shared_ptr<forge::net::p2p::detail::connection_gate> gate);

   [[nodiscard]] bool supports(const forge::net::p2p::endpoint& endpoint) const noexcept;

   [[nodiscard]] bool listening() const noexcept;

   [[nodiscard]] std::vector<forge::net::p2p::endpoint> local_endpoints() const;

   forge::net::p2p::endpoint listen(forge::net::p2p::endpoint endpoint);

   void stop();

   boost::asio::awaitable<void> async_stop();

   boost::asio::awaitable<connection>
   async_connect(forge::net::p2p::endpoint endpoint, const node::connect_options& options,
                 std::shared_ptr<cancellation_latch> cancellation, std::shared_ptr<void> native_lifetime,
                 authenticated_admission_handler authenticated, tcp_transport_progress_handler tcp_transport_progress,
                 upgrade_role role = upgrade_role::initiator, std::shared_ptr<forge::net::tcp::listener> source = {},
                 std::optional<forge::net::p2p::endpoint> local_source = {}, bool strict_identity = false,
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
   // The returned token belongs exclusively to the native transport. The wait
   // captures only its receipt, never the token or its admission resources.
   [[nodiscard]] static std::shared_ptr<void> track_native_terminal(std::shared_ptr<void> resources,
       std::function<boost::asio::awaitable<void>()>& wait);

   [[nodiscard]] bool listener_is_current(const std::string& key,
                                          const std::shared_ptr<forge::net::tcp::listener>& listener) const;

   [[nodiscard]] std::vector<std::shared_ptr<forge::net::tcp::listener>> stop_listeners();

   [[nodiscard]] std::vector<std::shared_ptr<forge::net::tcp::listener>> listener_snapshot() const;

   void track(const std::shared_ptr<cancellation_latch>& operation);

   forge::asio::runtime& runtime_;
   const node::options& options_;
   const libp2p_identity_material& identity_;
   resource_manager resources_;
   std::shared_ptr<forge::net::p2p::detail::connection_gate> gate_;
   mutable std::mutex listeners_mutex_;
   std::map<std::string, listener_entry> listeners_;
   std::map<std::pair<std::string, std::string>, std::shared_ptr<coordinated_tcp_upgrade>> coordinated_;
   bool listeners_stopped_ = false;
   std::mutex active_mutex_;
   std::vector<std::weak_ptr<cancellation_latch>> active_;
   bool stopped_ = false;
};

} // namespace forge::net::p2p::direct
