#pragma once

// Include the ordinary private mdns_registry/interface_watcher/mdns_wire headers
// in the global module fragment before this node-module-owned component.
#include <atomic>
#include <chrono>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/strand.hpp>

#include "lifecycle_tracker.hxx"

namespace forge::net::p2p::detail {

class worker_terminal_owner;

class mdns_service final : public std::enable_shared_from_this<mdns_service> {
 public:
   struct callbacks {
      // Integration must capture weak node ownership (or an independent owner),
      // never raw node::impl references: synchronous stop is not async_join.
      // Called on the service strand. Must return at most 256 current listeners.
      std::function<std::vector<endpoint>()> listeners;
      // Full mDNS-only replacement, including an empty replacement on shutdown.
      // Must not dial synchronously or retain references to this vector.
      std::function<void(std::vector<mdns_registry::lease>)> replace;
      std::function<void(std::exception_ptr)> error;
   };

   mdns_service(boost::asio::any_io_executor executor, resource_manager& resources, mdns_policy policy,
                peer_id local, mdns_codec::name service, callbacks value);
   ~mdns_service();
   void start(lifecycle_tracker& tracker);
   void request_stop() noexcept;
   boost::asio::awaitable<void> async_join();
   void notify_addresses_changed() noexcept;

 private:
   friend struct mdns_service_fixture;
   using time_point = std::chrono::steady_clock::time_point;
   enum class packet_kind { query, advertisement, response, legacy };
   struct packet {
      mdns_codec::bytes bytes;
      boost::asio::ip::udp::endpoint destination;
      boost::asio::ip::address source;
      time_point due;
      packet_kind kind;
   };
   struct worker {
      worker(boost::asio::any_io_executor executor, interface_state::interface value, bool v6,
             boost::asio::ip::address source_address, std::size_t packet_size);
      interface_state::interface interface;
      bool ipv6;
      boost::asio::ip::address source;
      boost::asio::ip::udp::socket socket;
      forge::asio::gate send_gate;
      forge::asio::notification changed;
      std::vector<std::uint8_t> buffer;
      std::deque<packet> outbound;
      mdns_codec::message advertisement;
      time_point response_at{};
      std::size_t active = 0;
      bool stopped = false;
   };

   static boost::asio::awaitable<void> run_lifecycle(std::shared_ptr<mdns_service> self,
                                                    std::shared_ptr<lifecycle_stop_source> stop);
   static boost::asio::awaitable<void> run_bridged(std::shared_ptr<mdns_service> self,
                                                  std::shared_ptr<worker_terminal_owner> terminal);
   static boost::asio::awaitable<void> run(std::shared_ptr<mdns_service> self);
   static boost::asio::awaitable<void> join(std::shared_ptr<mdns_service> self);
   boost::asio::awaitable<void> watch();
   boost::asio::awaitable<void> receive(std::shared_ptr<worker> owner);
   void process_packet(worker& owner, std::span<const std::uint8_t> bytes,
                       const forge::net::transport::datagram_io::received& route, time_point now);
   boost::asio::awaitable<void> maintain();
   boost::asio::awaitable<void> send(std::shared_ptr<worker> owner);
   boost::asio::awaitable<void> reconcile(std::vector<interface_state::interface> interfaces);
   boost::asio::awaitable<void> join_worker(const std::shared_ptr<worker>& owner);
   void open(worker& owner);
   void stop_worker(worker& owner) noexcept;
   void spawn(const std::shared_ptr<worker>& owner, bool reader);
   void refresh();
   void publish(time_point now = std::chrono::steady_clock::now());
   void respond(worker& owner, const mdns_codec::message& request,
                const forge::net::transport::datagram_io::received& route, time_point now);
   void enqueue(worker& owner, mdns_codec::message value, boost::asio::ip::udp::endpoint destination,
                boost::asio::ip::address source, time_point due, packet_kind kind,
                time_point now = std::chrono::steady_clock::now());
   void failed(std::exception_ptr error) noexcept;
   [[nodiscard]] bool registered(const worker& owner) const;

   boost::asio::strand<boost::asio::any_io_executor> _strand;
   // Copy shares the admission ledger without retaining a node::impl reference.
   resource_manager _resources;
   mdns_policy _policy;
   peer_id _local;
   mdns_codec::name _service;
   callbacks _callbacks;
   std::string _instance;
   std::optional<resource_manager::lifecycle_reservation> _reservation;
   std::optional<resource_manager::memory_reservation> _memory;
   std::optional<resource_manager::file_descriptor_reservation> _descriptors;
   std::unique_ptr<interface_watcher> _watcher;
   std::unique_ptr<mdns_registry> _registry;
   std::map<std::pair<std::uint32_t, bool>, std::shared_ptr<worker>> _workers;
   forge::asio::notification _changed;
   std::atomic_bool _stopping = false;
   std::atomic_bool _addresses_changed = true;
   std::mutex _mutex;
   bool _started = false;
   bool _finished = false;
   bool _run_entered = false; // Owner strand only; bridge can skip work on pre-stop.
   std::exception_ptr _failure;
   std::size_t _tasks = 0;
   std::size_t _queued = 0;
   // Across all workers: at most min(max_pending_packets, 32) unique queued
   // unicast responses per one-second admission window, even after sends drain.
   time_point _unicast_window{};
   std::size_t _unicast_admitted = 0;
};

} // namespace forge::net::p2p::detail
