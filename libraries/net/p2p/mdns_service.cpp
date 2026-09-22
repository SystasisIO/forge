module;

#include <boost/asio/strand.hpp>

#include "details/mdns_registry.hxx"
#include "details/interface_watcher.hxx"
#include "details/mdns_wire.hxx"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ip/multicast.hpp>
#include <boost/asio/ip/unicast.hpp>
#include <boost/asio/ip/v6_only.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/compat/move_only_function.hpp>
#include <boost/system/system_error.hpp>
#include <forge/exceptions/macros.hpp>
#include <netinet/in.h>
#include <sys/socket.h>

module forge.net.p2p.node;

import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.core.random;
import forge.net.p2p.exceptions;
import forge.net.p2p.lifecycle;
import forge.net.p2p.resource_manager;

#include "details/mdns_service.hxx"
#include "details/worker_stop_bridge.hxx"

namespace forge::net::p2p::detail {
namespace {
namespace asio = boost::asio;
namespace ip = asio::ip;
namespace codec = mdns_codec;
namespace datagram = forge::net::transport::datagram_io;
using namespace std::chrono_literals;

template <typename T>
void option(ip::udp::socket& socket, int level, int name, const T& value) {
   if (::setsockopt(socket.native_handle(), level, name, &value, sizeof(value)) < 0) {
      throw boost::system::system_error{errno, boost::system::generic_category(), "mDNS socket option"};
   }
}

std::string instance_name() {
   const auto random = forge::crypto::core::random_array<64>();
   const auto size = 32U + (random[0] & 31U);
   auto result = std::string(size, 'a');
   for (std::size_t i = 0; i < size; ++i) { result[i] = 'a' + random[i + 1] % 26; }
   return result;
}
} // namespace

mdns_service::worker::worker(asio::any_io_executor executor, interface_state::interface value, bool v6,
                             ip::address source_address, std::size_t packet_size)
    : interface(std::move(value)), ipv6(v6), source(std::move(source_address)), socket(executor), buffer(packet_size) {}

mdns_service::mdns_service(asio::any_io_executor executor, resource_manager& resources, mdns_policy policy,
                           peer_id local, codec::name service, callbacks value)
    : _strand(asio::make_strand(executor)), _resources(resources), _policy(policy), _local(std::move(local)),
      _service(std::move(service)), _callbacks(std::move(value)) {
   validate(_policy);
   if (!_policy.enabled || !_callbacks.listeners || !_callbacks.replace || _local.value.empty() ||
       _policy.max_interfaces > 256 || _policy.max_pending_packets > 1024 ||
       _policy.max_records_per_packet > 1024 || _policy.max_addresses_per_peer > 64 ||
       _service.size() != 3 || _service[0].empty() || _service[0].size() > 63 ||
       _service[0][0] != '_' || _service[1] != "_udp" || _service[2] != "local") {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid mDNS service configuration");
   }
}

mdns_service::~mdns_service() = default;

void mdns_service::start(lifecycle_tracker& tracker) {
   auto self = shared_from_this();
   auto operation = tracker.track();
   if (!operation.active()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "mDNS lifecycle is closed");
   }
   {
      const auto lock = std::scoped_lock{_mutex};
      if (_started || _stopping.load()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "mDNS service cannot be restarted");
      }
      _started = true;
   }
   auto stop = operation.stop_source();
   try {
      asio::co_spawn(_strand, run_lifecycle(self, std::move(stop)),
          [self, operation = std::move(operation)](std::exception_ptr error) mutable {
             operation.release();
             {
                const auto lock = std::scoped_lock{self->_mutex};
                if (error && !self->_failure) { self->_failure = error; }
                self->_finished = true;
             }
             self->_changed.notify();
          });
   } catch (...) {
      const auto lock = std::scoped_lock{_mutex};
      _failure = std::current_exception();
      _finished = true;
      _changed.notify();
      throw;
   }
}

void mdns_service::request_stop() noexcept {
   _stopping.store(true);
   _changed.notify();
}

void mdns_service::notify_addresses_changed() noexcept {
   _addresses_changed.store(true);
   _changed.notify();
}

asio::awaitable<void> mdns_service::async_join() { return join(shared_from_this()); }

asio::awaitable<void> mdns_service::join(std::shared_ptr<mdns_service> self) {
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
   while (true) {
      const auto epoch = self->_changed.epoch();
      auto error = std::exception_ptr{};
      auto done = false;
      {
         const auto lock = std::scoped_lock{self->_mutex};
         done = !self->_started || self->_finished;
         error = self->_failure;
      }
      if (done) {
         if (error) { std::rethrow_exception(error); }
         co_return;
      }
      co_await self->_changed.async_wait(epoch);
   }
}

void mdns_service::failed(std::exception_ptr error) noexcept {
   {
      const auto lock = std::scoped_lock{_mutex};
      if (!_failure) { _failure = error; }
   }
   request_stop();
}

void mdns_service::open(worker& owner) {
   owner.socket.open(owner.ipv6 ? ip::udp::v6() : ip::udp::v4());
   owner.socket.set_option(asio::socket_base::reuse_address{true});
   option(owner.socket, SOL_SOCKET, SO_REUSEPORT, int{1});
   if (owner.ipv6) {
      owner.socket.set_option(ip::v6_only{true});
   }
#if defined(__APPLE__)
   if (!owner.ipv6) {
      option(owner.socket, IPPROTO_IP, IP_BOUND_IF, owner.interface.index);
   }
#elif defined(__linux__)
   if (!owner.ipv6) { option(owner.socket, IPPROTO_IP, IP_MULTICAST_ALL, int{0}); }
#endif
   owner.socket.bind({owner.ipv6 ? ip::address{ip::address_v6::any()} : ip::address{ip::address_v4::any()}, 5353});
   owner.socket.set_option(ip::multicast::hops{255});
   owner.socket.set_option(ip::unicast::hops{255});
   owner.socket.set_option(ip::multicast::enable_loopback{true});
   if (owner.ipv6) {
      owner.socket.set_option(ip::multicast::join_group{ip::make_address_v6("ff02::fb"), owner.interface.index});
      owner.socket.set_option(ip::multicast::outbound_interface{owner.interface.index});
   } else {
      // RFC3678 identifies membership by index, even with duplicate IPv4 addresses.
      auto membership = group_req{};
      membership.gr_interface = owner.interface.index;
      auto group_address = sockaddr_in{};
      group_address.sin_family = AF_INET;
#if defined(__APPLE__)
      group_address.sin_len = sizeof(group_address);
#endif
      const auto octets = ip::make_address_v4("224.0.0.251").to_bytes();
      std::memcpy(&group_address.sin_addr, octets.data(), octets.size());
      std::memcpy(&membership.gr_group, &group_address, sizeof(group_address));
      option(owner.socket, IPPROTO_IP, MCAST_JOIN_GROUP, membership);
      auto outbound = ip_mreqn{};
      outbound.imr_ifindex = owner.interface.index;
      option(owner.socket, IPPROTO_IP, IP_MULTICAST_IF, outbound);
   }
   datagram::configure(owner.socket);
}

bool mdns_service::registered(const worker& owner) const {
   const auto found = _workers.find({owner.interface.index, owner.ipv6});
   return !_stopping.load() && !owner.stopped && found != _workers.end() && found->second.get() == &owner;
}

void mdns_service::stop_worker(worker& owner) noexcept {
   owner.stopped = true;
   owner.send_gate.close();
   auto ignored = boost::system::error_code{};
   owner.socket.close(ignored);
   _queued -= owner.outbound.size();
   owner.outbound.clear();
   owner.changed.notify();
}

asio::awaitable<void> mdns_service::join_worker(const std::shared_ptr<worker>& owner) {
   while (owner->active != 0) {
      const auto epoch = owner->changed.epoch();
      if (owner->active != 0) { co_await owner->changed.async_wait(epoch); }
   }
}

void mdns_service::spawn(const std::shared_ptr<worker>& owner, bool reader) {
   auto self = shared_from_this();
   ++owner->active;
   ++_tasks;
   try {
      asio::co_spawn(_strand, reader ? receive(owner) : send(owner),
          [self, owner](std::exception_ptr error) {
             if (error && !owner->stopped && !self->_stopping.load()) { self->failed(error); }
             --owner->active;
             --self->_tasks;
             owner->changed.notify();
             self->_changed.notify();
          });
   } catch (...) {
      --owner->active;
      --_tasks;
      throw;
   }
}

void mdns_service::enqueue(worker& owner, codec::message value, ip::udp::endpoint destination,
                           ip::address source, time_point due, packet_kind kind, time_point now) {
   if (!registered(owner) || _queued >= _policy.max_pending_packets) { return; }
   // One pending multicast response per interface/family; flood cannot allocate
   // one delayed task or one queue entry per query.
   auto bytes = codec::encode(value, mdns_wire::bounds(_policy));
   if (std::ranges::any_of(owner.outbound, [&](const auto& item) {
          return item.kind == kind && (kind != packet_kind::legacy ||
                 (item.destination == destination && item.source == source && item.bytes == bytes));
       })) { return; }
   if (kind == packet_kind::legacy) {
      // Service-wide admission budget persists across queue drains/interfaces.
      // Duplicates do not spend it; unique destinations/transactions do.
      if (now >= _unicast_window + 1s) {
         _unicast_window = now;
         _unicast_admitted = 0;
      }
      if (_unicast_admitted >= std::min(_policy.max_pending_packets, std::size_t{32})) { return; }
   }
   owner.outbound.push_back({std::move(bytes), std::move(destination), std::move(source), due, kind});
   if (kind == packet_kind::legacy) { ++_unicast_admitted; }
   ++_queued;
   owner.changed.notify();
}

asio::awaitable<void> mdns_service::send(std::shared_ptr<worker> owner) {
   using namespace asio::experimental::awaitable_operators;
   while (registered(*owner)) {
      const auto epoch = owner->changed.epoch();
      if (owner->outbound.empty()) {
         co_await owner->changed.async_wait(epoch);
         continue;
      }
      const auto now = std::chrono::steady_clock::now();
      const auto next = std::min_element(owner->outbound.begin(), owner->outbound.end(),
                                        [](const auto& a, const auto& b) { return a.due < b.due; });
      if (next->due > now) {
         co_await owner->changed.async_wait_until(epoch, next->due);
         continue;
      }
      auto packet = std::move(*next);
      owner->outbound.erase(next);
      auto error = std::exception_ptr{};
      try {
         auto ticket = co_await owner->send_gate.acquire();
         if (registered(*owner)) {
            auto timer = asio::steady_timer{_strand};
            timer.expires_after(1s);
            const auto result = co_await (
                datagram::async_send(owner->socket, asio::buffer(packet.bytes), packet.destination,
                                     datagram::source{packet.source, owner->interface.index}) ||
                timer.async_wait(asio::use_awaitable));
            if (result.index() != 0) { throw boost::system::system_error{asio::error::timed_out}; }
         }
      } catch (...) { error = std::current_exception(); }
      --_queued;
      if (error && registered(*owner)) { std::rethrow_exception(error); }
      // Send and receive hot paths both yield even when the kernel stays ready.
      co_await asio::post(_strand, asio::use_awaitable);
   }
}

asio::awaitable<void> mdns_service::receive(std::shared_ptr<worker> owner) {
   auto batch = 0U;
   while (registered(*owner)) {
      if (++batch >= 8) {
         batch = 0;
         co_await asio::post(_strand, asio::use_awaitable);
         if (!registered(*owner)) { break; }
      }
      auto route = datagram::received{};
      try {
         route = co_await datagram::async_receive(owner->socket, asio::buffer(owner->buffer));
      } catch (const boost::system::system_error& error) {
         if (!registered(*owner)) { break; }
         if (error.code() == asio::error::message_size || error.code() == asio::error::invalid_argument) { continue; }
         throw;
      }
      process_packet(*owner, std::span{owner->buffer}.first(route.size), route, std::chrono::steady_clock::now());
   }
}

void mdns_service::process_packet(worker& owner, std::span<const std::uint8_t> bytes,
                                  const datagram::received& route, time_point now) {
   // A canceled/replaced receiving socket cannot forward late completions into
   // a current generation, even if pktinfo names another live interface.
   if (!registered(owner)) { return; }
   // Multicast is already fanned out to member sockets: do not duplicate its
   // processing through cross-interface dispatch. Unicast reuseport selection,
   // however, need not agree with the ingress interface reported by the kernel.
   if (route.local.address().is_multicast() && route.interface_index != owner.interface.index) { return; }
   const auto found = _workers.find({route.interface_index, owner.ipv6});
   if (found == _workers.end() || !registered(*found->second)) { return; }
   auto& target = *found->second;
   if (!mdns_wire::admit(target.interface, target.ipv6, route)) { return; }
   try {
      const auto packet = codec::decode(bytes, mdns_wire::bounds(_policy));
      if ((packet.head.flags & 0x780f) != 0) { return; }
      if ((packet.head.flags & 0x8000) != 0) {
         // Source port is intentionally unrestricted: pinned Rust emits
         // multicast answers from an ephemeral send socket.
         _registry->apply(target.interface.index, target.interface.generation, packet, now);
         _changed.notify();
         publish(now);
         return;
      }
      respond(target, packet, route, now);
   } catch (const exceptions::codec_error&) {
      // Packet-local malformed/oversized data cannot tear down the service.
   }
}

void mdns_service::respond(worker& owner, const codec::message& request,
                            const datagram::received& route, time_point now) {
   auto response = mdns_wire::answer(request, owner.advertisement, false);
   if (!response) { return; }
   const auto random = forge::crypto::core::random_array<1>();
   const auto due = now + std::chrono::milliseconds{20 + random[0] % 101};
   if (now >= owner.response_at) {
      owner.response_at = now + 1s;
      enqueue(owner, std::move(*response), mdns_wire::group(owner.ipv6, owner.interface.index),
              owner.source, due, packet_kind::response, now);
   }
   // Rust ephemeral queries still receive a multicast answer. Its cooldown must
   // not suppress distinct legacy/QU requesters within the global unicast budget.
   const auto qu = std::ranges::any_of(request.questions, [](const auto& question) {
      return (question.class_code & codec::class_high_bit) != 0;
   });
   if ((route.remote.port() != 5353 || qu) && mdns_wire::on_link(owner.interface, route.remote.address())) {
      auto unicast = mdns_wire::answer(request, owner.advertisement, route.remote.port() != 5353);
      if (unicast) {
         const auto source = route.local.address().is_multicast() ? owner.source : route.local.address();
         enqueue(owner, std::move(*unicast), route.remote, source, due, packet_kind::legacy, now);
      }
   }
}

void mdns_service::publish(time_point now) {
   if (_stopping.load()) { return; }
   _callbacks.replace(_registry->snapshot(now));
}

void mdns_service::refresh() {
   const auto listeners = _callbacks.listeners();
   if (listeners.size() > 256) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "mDNS listener snapshot exceeds bound");
   }
   const auto due = std::chrono::steady_clock::now() + 100ms;
   for (auto& [key, owner] : _workers) {
      auto next = mdns_wire::advertisement(_policy, _service, _instance, _local, owner->interface, listeners);
      if (next == owner->advertisement) { continue; }
      // Remove stale unsent advertisements/responses before publishing new data.
      const auto old_size = owner->outbound.size();
      std::erase_if(owner->outbound, [](const auto& packet) { return packet.kind != packet_kind::query; });
      _queued -= old_size - owner->outbound.size();
      if (!owner->advertisement.answers.empty()) {
         auto goodbye = owner->advertisement;
         for (auto& record : goodbye.answers) { record.ttl = 0; }
         for (auto& record : goodbye.additionals) { record.ttl = 0; }
         enqueue(*owner, std::move(goodbye), mdns_wire::group(owner->ipv6, owner->interface.index),
                 owner->source, due, packet_kind::response);
      }
      owner->advertisement = std::move(next);
      if (!owner->advertisement.answers.empty()) {
         enqueue(*owner, owner->advertisement, mdns_wire::group(owner->ipv6, owner->interface.index),
                 owner->source, due + 100ms, packet_kind::advertisement);
      }
   }
}

asio::awaitable<void> mdns_service::reconcile(std::vector<interface_state::interface> interfaces) {
   std::erase_if(interfaces, [](const auto& value) { return !mdns_wire::usable(value); });
   // Registry withdrawal precedes any suspension, so old callbacks cannot revive
   // a removed generation while the socket owners are being joined.
   _registry->replace_interfaces(interfaces);
   publish();
   // Invalidate every removed generation before joining any one worker.
   for (auto& [key, owner] : _workers) {
      const auto present = std::ranges::any_of(interfaces, [&](const auto& value) {
         return value.index == owner->interface.index && value.generation == owner->interface.generation;
      });
      if (!present) { stop_worker(*owner); }
   }
   for (auto it = _workers.begin(); it != _workers.end();) {
      const auto owner = it->second;
      if (!owner->stopped) { ++it; continue; }
      co_await join_worker(owner);
      it = _workers.erase(it);
   }
   if (_stopping.load()) { co_return; }
   for (const auto& interface : interfaces) {
      for (bool ipv6 : {false, true}) {
         if ((ipv6 && !_policy.ipv6_enabled) || (!ipv6 && !_policy.ipv4_enabled) ||
             _workers.contains({interface.index, ipv6})) { continue; }
         const auto source = std::find_if(interface.addresses.begin(), interface.addresses.end(), [&](const auto& value) {
            return mdns_wire::usable(value) && value.value.is_v6() == ipv6;
         });
         if (source == interface.addresses.end()) { continue; }
         auto address = source->value;
         if (ipv6 && address.to_v6().is_link_local()) {
            auto scoped = address.to_v6();
            scoped.scope_id(interface.index);
            address = scoped;
         }
         auto owner = std::make_shared<worker>(_strand, interface, ipv6, address, _policy.max_packet_size);
         open(*owner);
         _workers.emplace(std::pair{interface.index, ipv6}, owner);
         spawn(owner, true);
         spawn(owner, false);
         enqueue(*owner, mdns_wire::query(_service), mdns_wire::group(ipv6, interface.index),
                 owner->source, std::chrono::steady_clock::now(), packet_kind::query);
      }
   }
   refresh();
}

asio::awaitable<void> mdns_service::watch() {
   while (!_stopping.load()) {
      auto update = co_await _watcher->async_next();
      if (!_stopping.load()) { co_await reconcile(std::move(update.interfaces)); }
   }
}

asio::awaitable<void> mdns_service::run_bridged(std::shared_ptr<mdns_service> self,
                                              std::shared_ptr<worker_terminal_owner> terminal) {
   static_cast<void>(terminal->publish([self]() noexcept { self->request_stop(); }));
   co_await run(std::move(self));
}

asio::awaitable<void> mdns_service::run_lifecycle(std::shared_ptr<mdns_service> self,
                                                std::shared_ptr<lifecycle_stop_source> stop) {
   auto failure = std::exception_ptr{};
   try {
      co_await async_run_with_stop_bridge(std::make_shared<worker_stop_bridge>(),
          [self](std::shared_ptr<worker_terminal_owner> terminal) {
             return run_bridged(self, std::move(terminal));
          }, {.lifecycle_stop = std::move(stop)});
   } catch (...) { failure = std::current_exception(); }
   // Both bridge branches are joined here. Pre-stop/setup failure can skip work;
   // still publish the empty replacement, exactly once, under the tracked owner.
   if (!self->_run_entered) {
      self->request_stop();
      co_await run(self);
   }
   if (failure) { std::rethrow_exception(failure); }
}

asio::awaitable<void> mdns_service::maintain() {
   auto next_query = std::chrono::steady_clock::now() + _policy.query_interval;
   while (!_stopping.load()) {
      const auto epoch = _changed.epoch();
      const auto now = std::chrono::steady_clock::now();
      if (_addresses_changed.exchange(false)) { refresh(); }
      _registry->expire(now);
      publish(now);
      if (now >= next_query) {
         for (auto& [key, owner] : _workers) {
            enqueue(*owner, mdns_wire::query(_service), mdns_wire::group(owner->ipv6, owner->interface.index),
                    owner->source, now, packet_kind::query);
         }
         next_query = now + _policy.query_interval;
      }
      auto deadline = next_query;
      if (const auto expiry = _registry->next_expiry()) { deadline = std::min(deadline, *expiry); }
      co_await _changed.async_wait_until(epoch, deadline);
   }
}

asio::awaitable<void> mdns_service::run(std::shared_ptr<mdns_service> self) {
   self->_run_entered = true;
   auto failure = std::exception_ptr{};
   try {
      if (!self->_stopping.load()) {
         auto admission = self->_resources.reserve_lifecycle();
         if (!admission) { FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "mDNS lifecycle admission denied"); }
         self->_reservation.emplace(std::move(*admission));
         // Conservative envelope: registry + replacement snapshots, watcher
         // snapshots, decode/encode scratch, all worker buffers and bounded queues.
         const auto memory = std::uint64_t{16 * 1024 * 1024} + self->_policy.max_interfaces * 128 * 1024 +
                             self->_policy.max_pending_packets * (4 * self->_policy.max_packet_size + 4096);
         self->_memory = self->_reservation->reserve_memory(memory);
         self->_descriptors = self->_reservation->reserve_file_descriptors(2 + 2 * self->_policy.max_interfaces);
         if (!self->_memory || !self->_descriptors) {
            FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "mDNS memory/descriptor admission denied");
         }
         self->_instance = instance_name();
         self->_registry = std::make_unique<mdns_registry>(self->_policy, mdns_registry::limits{}, self->_service, self->_local);
         auto options = interface_watcher::options{};
         options.state.interfaces = self->_policy.max_interfaces;
         self->_watcher = std::make_unique<interface_watcher>(self->_strand, options);
         ++self->_tasks;
         try {
            asio::co_spawn(self->_strand, self->watch(), [self](std::exception_ptr error) {
               if (error && !self->_stopping.load()) { self->failed(error); }
               --self->_tasks;
               self->_changed.notify();
            });
         } catch (...) { --self->_tasks; throw; }
         co_await self->maintain();
      }
   } catch (...) { failure = std::current_exception(); }
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
   self->request_stop();
   if (self->_watcher) { self->_watcher->request_stop(); }
   for (auto& [key, owner] : self->_workers) { self->stop_worker(*owner); }
   while (self->_tasks != 0) {
      const auto epoch = self->_changed.epoch();
      if (self->_tasks != 0) { co_await self->_changed.async_wait(epoch); }
   }
   if (self->_watcher) { co_await self->_watcher->async_stop(); }
   self->_workers.clear();
   self->_watcher.reset();
   self->_registry.reset();
   try { self->_callbacks.replace({}); } catch (...) { if (!failure) { failure = std::current_exception(); } }
   self->_descriptors.reset();
   self->_memory.reset();
   self->_reservation.reset();
   {
      const auto lock = std::scoped_lock{self->_mutex};
      if (!failure) { failure = self->_failure; }
   }
   if (failure) {
      if (self->_callbacks.error) { try { self->_callbacks.error(failure); } catch (...) {} }
      std::rethrow_exception(failure);
   }
}

} // namespace forge::net::p2p::detail
