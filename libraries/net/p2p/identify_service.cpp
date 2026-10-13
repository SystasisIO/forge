module;

#include <forge/log/macros.hpp>

#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

module forge.net.p2p.node;

import forge.exceptions;
import forge.log.logger;
import forge.net.p2p.exceptions;
import forge.net.p2p.identify;

#include "details/identify_service.hxx"

namespace forge::net::p2p::detail {

identify_service::identify_service(boost::asio::any_io_executor executor) : executor_(std::move(executor)) {}

void identify_service::complete(const std::shared_ptr<attempt>& current, outcome result) noexcept {
   auto waiters = std::vector<std::weak_ptr<completion_channel>>{};
   try {
      {
         auto lock = std::scoped_lock{mutex_};
         if (current->completed) {
            return;
         }
         current->running = false;
         current->completed = true;
         current->result = std::move(result);
         waiters.swap(current->waiters);
      }
      for (auto& pending : waiters) {
         if (auto waiter = pending.lock()) {
            try {
               static_cast<void>(waiter->try_send(boost::system::error_code{}, current->result));
            } catch (...) {
               // A failed waiter must not prevent completion of the remaining callers.
            }
         }
      }
   } catch (...) {
      // Identify shutdown and completion are best effort and noexcept.
   }
}

boost::asio::awaitable<identify_service::outcome> identify_service::async_identify(std::uint64_t session_id,
                                                                                   operation run) {
   auto current = std::shared_ptr<attempt>{};
   auto leader = false;
   auto completion = std::make_shared<completion_channel>(executor_, 1);
   {
      auto lock = std::scoped_lock{mutex_};
      if (closed_) {
         co_return outcome{.state = identify::state::failed, .error = "P2P Identify service is closed"};
      }
      auto& entry = attempts_[session_id];
      if (!entry) {
         entry = std::make_shared<attempt>();
         leader = true;
      }
      current = entry;
      current->waiters.push_back(completion);
      if (current->completed) {
         static_cast<void>(completion->try_send(boost::system::error_code{}, current->result));
      }
   }

   if (leader) {
      auto result = outcome{};
      try {
         result.document = co_await run();
         result.state = identify::state::identified;
      } catch (const forge::exceptions::base& error) {
         result.state = identify::state::failed;
         result.error = error.what();
      } catch (const std::exception& error) {
         result.state = identify::state::failed;
         result.error = error.what();
      } catch (...) {
         result.state = identify::state::failed;
         result.error = "unknown P2P Identify failure";
      }
      // Only the coalesced leader emits; cached results and waiters do not duplicate it.
      dlog("forge.net.p2p.connection", "P2P Identify attempt completed",
           ("event", "identify.completed")("session_id", session_id)("state", static_cast<int>(result.state))
           ("protocols", result.document ? result.document->protocols.size() : 0U)
           ("addresses", result.document ? result.document->listen_endpoints.size() : 0U));
      complete(current, std::move(result));
   }

   co_return co_await completion->async_receive(boost::asio::use_awaitable);
}

std::size_t identify_service::retained() const noexcept {
   try {
      auto lock = std::scoped_lock{mutex_};
      return attempts_.size();
   } catch (...) {
      return 0;
   }
}

void identify_service::forget(std::uint64_t session_id) noexcept {
   try {
      auto lock = std::scoped_lock{mutex_};
      attempts_.erase(session_id);
   } catch (...) {
      // Session teardown must remain noexcept.
   }
}

void identify_service::close() noexcept {
   auto attempts = std::map<std::uint64_t, std::shared_ptr<attempt>>{};
   try {
      {
         auto lock = std::scoped_lock{mutex_};
         if (closed_) {
            return;
         }
         closed_ = true;
         attempts.swap(attempts_);
      }
      for (const auto& [_, current] : attempts) {
         try {
            complete(current, outcome{.state = identify::state::failed, .error = "P2P node stopped during Identify"});
         } catch (...) {
            // Continue closing every coalesced Identify attempt.
         }
      }
   } catch (...) {
      // Node teardown must not terminate on an allocation or executor failure.
   }
}

} // namespace forge::net::p2p::detail
