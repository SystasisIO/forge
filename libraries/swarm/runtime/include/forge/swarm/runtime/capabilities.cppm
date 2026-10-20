module;

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

export module forge.swarm.runtime.capabilities;

export import forge.swarm.api.peer;

export namespace forge::swarm::runtime {

class capabilities {
 public:
   class connection;
   class registration;
   virtual ~capabilities();
   // Hints only. IDs are opaque product-owned routes, never requester identities.
   virtual boost::asio::awaitable<std::vector<std::string>> async_lookup(
      core::swarm_id, std::string realm, std::size_t limit, std::stop_token,
      std::chrono::steady_clock::time_point deadline) = 0;
   virtual boost::asio::awaitable<std::unique_ptr<connection>> async_connect(
      std::string provider, std::stop_token, std::chrono::steady_clock::time_point deadline) = 0;
   virtual boost::asio::awaitable<std::unique_ptr<registration>> async_register(
      core::swarm_id, std::string realm, std::stop_token,
      std::chrono::steady_clock::time_point deadline) = 0;
};

class capabilities::connection {
 public:
   virtual ~connection();
   [[nodiscard]] virtual forge::api::core::handle<api::peer> remote() const = 0;
   // Adapter-authenticated remote principal, never a route/request claim. The
   // source/fingerprint pair is used; no server-injection marker is required.
   [[nodiscard]] virtual forge::api::auth::authenticated_caller remote_identity() const = 0;
   virtual void cancel() noexcept = 0;
   virtual boost::asio::awaitable<void> async_close() = 0;
};

class capabilities::registration {
 public:
   virtual ~registration();
   virtual void cancel() noexcept = 0;
   virtual boost::asio::awaitable<void> async_close() = 0;
};

} // namespace forge::swarm::runtime
