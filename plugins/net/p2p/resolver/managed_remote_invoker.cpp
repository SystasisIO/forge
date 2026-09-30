module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.resolver.plugin;

import forge.api.core.connection;
import forge.api.core.descriptor;
import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.transport.connection;
import forge.asio.notification;
import forge.exceptions;
import forge.net.p2p.identity;
import forge.plugins.net.p2p.resolver.exceptions;
import forge.plugins.net.p2p.resolver.types;
import forge.plugins.net.p2p.node.api;

#include "details/plugin_impl.hxx"
#include "details/managed_remote_invoker.hxx"

namespace forge::plugins::net::p2p::resolver::detail {
namespace {

[[nodiscard]] std::exception_ptr remote_stopped_failure() noexcept {
   try {
      FORGE_THROW_EXCEPTION(exceptions::remote_stopped, "managed remote is stopped");
   } catch (...) {
      return std::current_exception();
   }
}

void cancel_generation(const std::shared_ptr<managed_remote_generation>& generation) noexcept {
   if (generation && generation->connection) {
      try {
         generation->connection->cancel();
      } catch (...) {
      }
   }
}

} // namespace

} // namespace forge::plugins::net::p2p::resolver::detail

extern "C++" {
namespace forge::plugins::net::p2p::resolver::detail {

managed_remote_invoker::managed_remote_invoker(std::weak_ptr<plugin::impl> owner,
                                               std::vector<forge::net::p2p::peer_id> ordered_peers,
                                               forge::api::core::api_ref requested,
                                               forge::api::core::descriptor descriptor, managed_remote_options options,
                                               std::size_t max_waiters)
    : owner_{std::move(owner)}, peers_{std::move(ordered_peers)}, requested_{std::move(requested)},
      descriptor_{std::move(descriptor)}, options_{options},
      state_{std::make_unique<managed_remote_state>(max_waiters)} {
   if (peers_.empty() || options_.max_connect_rounds == 0 || options_.max_connect_rounds > 64 ||
       options_.initial_backoff.count() <= 0 || options_.max_backoff < options_.initial_backoff ||
       options_.max_backoff > std::chrono::hours{1} || max_waiters == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_remote, "managed remote options are invalid");
   }

   auto unique = std::set<std::string>{};
   for (const auto& peer : peers_) {
      if (!forge::net::p2p::valid_peer_id(peer) || !unique.insert(peer.value).second) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_remote, "managed remote peer list is invalid");
      }
   }
}

managed_remote_invoker::~managed_remote_invoker() {
   request_stop();
}

boost::asio::awaitable<void> managed_remote_invoker::connect_initial() {
   static_cast<void>(co_await require_generation());
}

void managed_remote_invoker::request_stop() noexcept {
   auto effects = state_->request_stop();
   if (!effects.initiated) {
      return;
   }
   cancel_generation(effects.current);
}

boost::asio::awaitable<void> managed_remote_invoker::async_stop() {
   request_stop();
   const auto active = state_->observe_active_flight();
   if (!active.flight) {
      co_return;
   }
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   if (!active.done) {
      static_cast<void>(co_await active.flight->completed().async_wait(active.observed));
   }
   const auto cleanup = state_->observe_cleanup(active.flight);
   if (!cleanup.done) {
      static_cast<void>(co_await active.flight->cleanup_completed().async_wait(cleanup.observed));
   }
}

bool managed_remote_invoker::stopped() const noexcept {
   return state_->stopped();
}

std::chrono::milliseconds managed_remote_invoker::backoff_for(std::uint32_t round) const noexcept {
   auto value = options_.initial_backoff;
   for (auto index = std::uint32_t{0}; index < round && value < options_.max_backoff; ++index) {
      value = std::min(options_.max_backoff, value * 2);
   }
   return value;
}

boost::asio::awaitable<std::shared_ptr<managed_remote_generation>> managed_remote_invoker::require_generation() {
   const auto executor = co_await boost::asio::this_coro::executor;
   while (true) {
      auto acquired = state_->acquire_or_join(executor);
      if (acquired.status == managed_remote_state::acquire_status::stopped) {
         FORGE_THROW_EXCEPTION(exceptions::remote_stopped, "managed remote is stopped");
      }
      if (acquired.status == managed_remote_state::acquire_status::backpressure) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted,
                               "managed remote reconnect waiter limit exceeded");
      }
      if (acquired.status == managed_remote_state::acquire_status::current) {
         if (acquired.current->connection && acquired.current->connection->valid()) {
            co_return acquired.current;
         }
         if (auto stale = state_->invalidate(acquired.current, peers_.size()); stale && stale->connection) {
            stale->connection->cancel();
         }
         continue;
      }
      if (acquired.status == managed_remote_state::acquire_status::draining) {
         try {
            static_cast<void>(co_await acquired.flight->cleanup_completed().async_wait(acquired.observed));
         } catch (const boost::system::system_error& error) {
            leave_flight(acquired.flight);
            if (error.code() == boost::asio::error::operation_aborted) {
               FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled,
                                     "managed remote reconnect wait was cancelled");
            }
            throw;
         } catch (...) {
            leave_flight(acquired.flight);
            throw;
         }
         leave_flight(acquired.flight);
         continue;
      }

      if (acquired.start) {
         try {
            auto self = shared_from_this();
            boost::asio::co_spawn(acquired.flight->executor(), self->run_flight(acquired.flight),
                                  [self, flight = acquired.flight](std::exception_ptr error) noexcept {
                                     self->finish_flight(flight, std::move(error));
                                  });
         } catch (...) {
            static_cast<void>(state_->complete(acquired.flight, {}, std::current_exception(), {}));
            state_->finish_child(acquired.flight, {});
            state_->finish_watcher(acquired.flight);
         }
      }

      try {
         static_cast<void>(co_await acquired.flight->completed().async_wait(acquired.observed));
      } catch (const boost::system::system_error& error) {
         leave_flight(acquired.flight);
         if (error.code() == boost::asio::error::operation_aborted) {
            FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled,
                                  "managed remote reconnect wait was cancelled");
         }
         throw;
      } catch (...) {
         leave_flight(acquired.flight);
         throw;
      }

      const auto completed = state_->read_completion(acquired.flight);
      leave_flight(acquired.flight);
      if (completed.stopped) {
         FORGE_THROW_EXCEPTION(exceptions::remote_stopped, "managed remote is stopped");
      }
      if (completed.error) {
         std::rethrow_exception(completed.error);
      }
      if (!completed.result) {
         FORGE_THROW_EXCEPTION(exceptions::remote_unavailable,
                               "managed remote reconnect completed without a generation");
      }
      co_return completed.result;
   }
}

boost::asio::awaitable<void>
managed_remote_invoker::run_flight(std::shared_ptr<managed_remote_reconnect_flight> flight) {
   try {
      auto self = shared_from_this();
      boost::asio::co_spawn(flight->executor(), self->run_connect(flight),
                            boost::asio::bind_cancellation_slot(flight->cancellation().slot(),
                                                                [self, flight](std::exception_ptr error) noexcept {
                                                                   self->state_->finish_child(flight, std::move(error));
                                                                }));
   } catch (...) {
      static_cast<void>(state_->complete(flight, {}, std::current_exception(), {}));
      state_->finish_child(flight, {});
      co_return;
   }
   co_await watch_stop(std::move(flight));
}

boost::asio::awaitable<void>
managed_remote_invoker::run_connect(std::shared_ptr<managed_remote_reconnect_flight> flight) {
   auto result = std::shared_ptr<managed_remote_generation>{};
   auto error = std::exception_ptr{};
   try {
      result = co_await connect_generation();
   } catch (...) {
      error = std::current_exception();
   }
   const auto stopped_error = result ? remote_stopped_failure() : std::exception_ptr{};
   auto effects = state_->complete(flight, std::move(result), std::move(error), stopped_error);
   if (effects.canceled && effects.canceled->connection) {
      effects.canceled->connection->cancel();
   }
}

boost::asio::awaitable<void>
managed_remote_invoker::watch_stop(std::shared_ptr<managed_remote_reconnect_flight> flight) {
   auto cancel = false;
   auto timer = std::shared_ptr<managed_remote_timer_state>{};
   try {
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
      while (true) {
         const auto observed = state_->observe_stop(flight);
         if (observed.done) {
            break;
         }
         if (observed.requested) {
            cancel = true;
            timer = observed.timer;
            break;
         }
         static_cast<void>(co_await flight->stop_changed().async_wait(observed.observed));
      }
   } catch (...) {
      cancel = true;
      timer = state_->observe_stop(flight).timer;
   }
   if (cancel) {
      cancel_connect(flight, timer);
   }
}

void managed_remote_invoker::finish_flight(const std::shared_ptr<managed_remote_reconnect_flight>& flight,
                                           std::exception_ptr error) noexcept {
   if (error) {
      cancel_connect(flight, state_->observe_stop(flight).timer);
      static_cast<void>(state_->complete(flight, {}, std::move(error), {}));
   }
   state_->finish_watcher(flight);
}

void managed_remote_invoker::cancel_connect(const std::shared_ptr<managed_remote_reconnect_flight>& flight,
                                            const std::shared_ptr<managed_remote_timer_state>& timer) noexcept {
   try {
      flight->cancellation().emit(boost::asio::cancellation_type::all);
   } catch (...) {
   }
   if (timer) {
      try {
         static_cast<void>(timer->timer().cancel());
      } catch (...) {
      }
   }
}

boost::asio::awaitable<std::shared_ptr<managed_remote_generation>> managed_remote_invoker::connect_generation() {
   const auto executor = co_await boost::asio::this_coro::executor;
   const auto start = state_->next_peer();

   for (auto round = std::uint32_t{0}; round < options_.max_connect_rounds; ++round) {
      for (auto offset = std::size_t{0}; offset < peers_.size(); ++offset) {
         const auto index = (start + offset) % peers_.size();
         const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
         if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
            FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "managed remote connect was cancelled");
         }
         if (stopped()) {
            FORGE_THROW_EXCEPTION(exceptions::remote_stopped, "managed remote is stopped");
         }

         auto owner = owner_.lock();
         if (!owner) {
            FORGE_THROW_EXCEPTION(exceptions::remote_stopped, "managed remote owner is unavailable");
         }
         try {
            auto opened =
                co_await owner->open_resolved_connection(peers_[index], requested_, descriptor_, options_.resolution);
            auto connection = std::make_shared<forge::api::transport::connection>(std::move(opened.connection));
            auto invoker = co_await connection->get_remote_invoker(opened.selected, descriptor_);
            auto result = std::make_shared<managed_remote_generation>(managed_remote_generation{
                .connection = std::move(connection),
                .invoker = std::move(invoker),
                .selected = std::move(opened.selected),
                .peer_index = index,
            });
            co_return result;
         } catch (const exceptions::remote_stopped&) {
            throw;
         } catch (const forge::api::core::exceptions::cancelled&) {
            if (stopped()) {
               FORGE_THROW_EXCEPTION(exceptions::remote_stopped, "managed remote is stopped");
            }
            if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
               throw;
            }
         } catch (const forge::exceptions::base&) {
            if (stopped()) {
               FORGE_THROW_EXCEPTION(exceptions::remote_stopped, "managed remote is stopped");
            }
         }
      }

      if (round + 1U < options_.max_connect_rounds) {
         auto timer = std::make_shared<managed_remote_timer_state>(executor, backoff_for(round));
         if (!state_->install_timer(timer)) {
            FORGE_THROW_EXCEPTION(exceptions::remote_stopped, "managed remote is stopped");
         }
         auto error = boost::system::error_code{};
         co_await timer->timer().async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
         state_->clear_timer(timer);
         if (error) {
            if (stopped()) {
               FORGE_THROW_EXCEPTION(exceptions::remote_stopped, "managed remote is stopped");
            }
            FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "managed remote backoff was cancelled");
         }
      }
   }

   FORGE_THROW_EXCEPTION(exceptions::remote_unavailable, "managed remote could not connect to an ordered peer");
}

void managed_remote_invoker::invalidate(const std::shared_ptr<managed_remote_generation>& value) noexcept {
   if (auto current = state_->invalidate(value, peers_.size()); current && current->connection) {
      current->connection->cancel();
   }
}

void managed_remote_invoker::leave_flight(const std::shared_ptr<managed_remote_reconnect_flight>& value) noexcept {
   state_->leave(value);
}

boost::asio::awaitable<forge::api::core::response> managed_remote_invoker::async_call(forge::api::core::request value) {
   auto generation = co_await require_generation();
   value.api = generation->selected;
   try {
      co_return co_await generation->invoker->async_call(std::move(value));
   } catch (const forge::api::core::exceptions::deadline_exceeded&) {
      throw;
   } catch (const forge::api::core::exceptions::cancelled&) {
      throw;
   } catch (const forge::api::core::exceptions::resource_exhausted&) {
      throw;
   } catch (...) {
      invalidate(generation);
      throw;
   }
}

boost::asio::awaitable<forge::api::core::response>
managed_remote_invoker::async_stream_call(forge::api::core::request value, forge::api::core::method_kind kind,
                                          std::shared_ptr<forge::api::core::detail::stream_endpoint> input,
                                          std::shared_ptr<forge::api::core::detail::stream_endpoint> output) {
   auto generation = co_await require_generation();
   value.api = generation->selected;
   try {
      co_return co_await generation->invoker->async_stream_call(std::move(value), kind, std::move(input),
                                                                std::move(output));
   } catch (const forge::api::core::exceptions::deadline_exceeded&) {
      throw;
   } catch (const forge::api::core::exceptions::cancelled&) {
      throw;
   } catch (const forge::api::core::exceptions::resource_exhausted&) {
      throw;
   } catch (...) {
      invalidate(generation);
      throw;
   }
}

} // namespace forge::plugins::net::p2p::resolver::detail
}
