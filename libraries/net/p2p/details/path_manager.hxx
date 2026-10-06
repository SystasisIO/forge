#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>

namespace forge::net::p2p {
class cancellation_latch;
}

namespace forge::net::p2p::detail {

class lifecycle_wakeup;
class resource_stream;
class worker_stop_bridge;

// Admission and completion ownership only; node lifecycle owns every worker.
class path_manager final {
 public:
   using time_point = std::chrono::steady_clock::time_point;
   enum class role { initiator, responder };
   static constexpr std::size_t max_parallel_operations = 8;
   static constexpr std::size_t max_retained_peers = 128;
   static constexpr std::size_t max_attempts = 3;
   static constexpr std::size_t max_parallel_dials = 4;
   static constexpr std::size_t max_cancel_waiters = 8;
   static constexpr auto failure_backoff = std::chrono::seconds{30};
   class dial_batch;

   struct exchange {
      const std::uint64_t session_id;
      const role side;
      const std::shared_ptr<cancellation_latch> cancellation;
   };

   struct operation {
      peer_id peer;
      std::uint64_t session_id = 0;
      role side = role::initiator;
      time_point deadline;
      std::shared_ptr<cancellation_latch> cancellation;
      // Guarded by the manager, never read directly by node workers.
      std::size_t attempts = 0;
      std::shared_ptr<exchange> current;
      std::shared_ptr<exchange> pending;
      bool handed_over = false;
      bool closing = false;
      bool completed = false;
      hole_punch::status result = hole_punch::status::not_attempted;
      std::weak_ptr<dial_batch> dials;
      std::vector<std::weak_ptr<dial_batch>> terminal_waiters;
   };

   struct claim {
      std::shared_ptr<operation> owner;
      bool leader = false;
   };

   struct progress {
      std::size_t attempts = 0;
      bool exchanging = false;
      bool handed_over = false;
      bool completed = false;
      hole_punch::status result = hole_punch::status::not_attempted;
   };

   using exchange_work = std::function<boost::asio::awaitable<void>(
       boost::asio::cancellation_slot, std::shared_ptr<worker_stop_bridge>)>;

   explicit path_manager(std::shared_ptr<lifecycle_wakeup> wakeup);
   [[nodiscard]] claim begin(peer_id peer, std::uint64_t session_id, role side, time_point deadline,
                              time_point now = std::chrono::steady_clock::now());
   [[nodiscard]] bool begin_exchange(const std::shared_ptr<operation>& owner,
                                      time_point now = std::chrono::steady_clock::now());
   [[nodiscard]] std::shared_ptr<exchange> start_exchange(const std::shared_ptr<operation>& owner,
       bool count_attempt = true, time_point now = std::chrono::steady_clock::now());
   // Only a cross-circuit active initiator collision is arbitrated. The smaller
   // canonical peer ID keeps its initiated exchange; this is not a wire rule.
   boost::asio::awaitable<bool> async_accept_exchange(const std::shared_ptr<operation>& owner,
       const std::shared_ptr<exchange>& incoming, const peer_id& local);
   void end_exchange(const std::shared_ptr<operation>& owner,
                      time_point now = std::chrono::steady_clock::now()) noexcept;
   void end_exchange(const std::shared_ptr<operation>& owner, const std::shared_ptr<exchange>& ticket,
                      time_point now = std::chrono::steady_clock::now()) noexcept;
   [[nodiscard]] progress inspect(const std::shared_ptr<operation>& owner) const;
   // Linearize the original driver's failure against incoming admission. A
   // handed-over driver may retire only for owner cancellation/deadline/result.
   [[nodiscard]] bool seal(const std::shared_ptr<operation>& owner, bool original_only = false);
   void finish(const std::shared_ptr<operation>& owner, hole_punch::status result,
               time_point now = std::chrono::steady_clock::now()) noexcept;
   void request_stop() noexcept;
   boost::asio::awaitable<bool> async_cancel(peer_id peer);
   boost::asio::awaitable<hole_punch::status> async_wait(std::shared_ptr<operation> owner, time_point deadline);
   boost::asio::awaitable<bool> async_delay(std::shared_ptr<operation> owner, time_point target,
                                           std::shared_ptr<exchange> ticket = {});
   boost::asio::awaitable<void> async_run_exchange(const std::shared_ptr<operation>& owner, exchange_work work,
                                                  std::shared_ptr<exchange> ticket = {});
   static boost::asio::awaitable<void> async_close_exchange(const std::shared_ptr<resource_stream>& resource,
                                                           std::exception_ptr failure = {});
   // After validated CONNECT/CONNECT/SYNC (and RTT/2 for the initiator), a
   // peer's Yamux RST ends this stream, not the completed path negotiation.
   static boost::asio::awaitable<void> async_close_completed_exchange(const std::shared_ptr<resource_stream>& resource);
   // Caller arms the owned deadline on batch cancellation before entering;
   // launch is invoked only after terminal readiness has been registered.
   boost::asio::awaitable<bool> async_wait_dials(const std::shared_ptr<dial_batch>& batch,
       const std::shared_ptr<operation>& owner, time_point deadline, std::function<bool()> has_direct,
       std::function<void()> launch, std::shared_ptr<exchange> ticket = {});
   void notify_direct(const peer_id& peer, path::kind kind, peer_authentication authentication) noexcept;
   boost::asio::awaitable<void> async_join();
   [[nodiscard]] std::size_t active() const;
   [[nodiscard]] std::size_t retained() const;
   [[nodiscard]] static time_point dial_deadline(time_point owner_deadline, time_point now) noexcept;
   [[nodiscard]] static endpoint coordinated_source(std::span<const endpoint> listeners, const endpoint& candidate,
                                                     const std::optional<endpoint>& carrier_local,
                                                     const std::optional<endpoint>& routed_source = {});

   [[nodiscard]] static bool eligible(path::kind kind, peer_authentication authentication,
                                      bool inbound_connection, role side) noexcept;
   [[nodiscard]] static std::vector<endpoint> direct_endpoints(const std::vector<endpoint>& values,
                                                               const peer_id& expected, std::size_t limit);

 private:
   std::shared_ptr<lifecycle_wakeup> _wakeup;
   [[nodiscard]] std::vector<std::weak_ptr<dial_batch>>
   retire_locked(const std::shared_ptr<operation>& owner, time_point now) noexcept;
   mutable std::mutex _mutex;
   std::map<peer_id, std::shared_ptr<operation>> _active;
   std::map<peer_id, time_point> _backoffs;
   bool _stopping = false;
};

} // namespace forge::net::p2p::detail
