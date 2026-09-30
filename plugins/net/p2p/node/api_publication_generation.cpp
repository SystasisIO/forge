module;

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <utility>

module forge.plugins.net.p2p.node.plugin;

import forge.api.p2p.binding;
import forge.api.stream.session;
import forge.asio.notification;
import forge.net.p2p.node;

#include "details/api_publication_generation.hxx"

namespace forge::plugins::net::p2p::node::detail {

api_publication_generation::api_publication_generation(forge::api::p2p::api_binding binding)
    : binding_{std::move(binding)}, drained_{std::make_shared<forge::asio::notification>()} {}

api_publication_generation::~api_publication_generation() {
   request_close();
   try {
      if (retirement_) {
         retirement_(this);
      }
   } catch (...) {
      // Publication retirement must not escape the no-throw destruction path.
   }
}

bool api_publication_generation::active() const noexcept {
   const auto lock = std::scoped_lock{mutex_};
   return admission_open_;
}

void api_publication_generation::set_retirement_handler(
   std::function<void(const api_publication_generation*)> handler) {
   retirement_ = std::move(handler);
}

boost::asio::awaitable<void>
api_publication_generation::accept(forge::net::p2p::node::incoming_protocol_stream stream) {
   auto entry = std::make_shared<session_entry>();
   entry->value = std::make_shared<forge::api::stream::session>(binding_.make_session(std::move(stream)));

   auto reject = false;
   {
      const auto lock = std::scoped_lock{mutex_};
      if (!admission_open_) {
         reject = true;
      } else {
         active_sessions_.push_back(entry);
         entry->position = std::prev(active_sessions_.end());
         ++active_count_;
      }
   }
   if (reject) {
      entry->value->cancel();
      co_return;
   }

   try {
      co_await entry->value->async_serve();
   } catch (...) {
      release(entry);
      throw;
   }
   release(entry);
}

void api_publication_generation::request_close() noexcept {
   auto cancelling = std::list<std::shared_ptr<session_entry>>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      if (!admission_open_) {
         return;
      }
      admission_open_ = false;
      cancellation_complete_ = false;
      cancelling.splice(cancelling.end(), active_sessions_);
   }

   for (const auto& entry : cancelling) {
      entry->value->cancel();
   }

   auto notify = false;
   {
      const auto lock = std::scoped_lock{mutex_};
      closing_sessions_.splice(closing_sessions_.end(), cancelling);
      cancellation_complete_ = true;
      notify = active_count_ == 0;
   }
   if (notify) {
      drained_->notify();
   }
}

boost::asio::awaitable<void> api_publication_generation::async_close() {
   request_close();
   co_await wait_for_drain();
   co_return;
}

void api_publication_generation::release(const std::shared_ptr<session_entry>& entry) noexcept {
   auto notify = false;
   {
      const auto lock = std::scoped_lock{mutex_};
      if (admission_open_) {
         active_sessions_.erase(entry->position);
      }
      --active_count_;
      notify = !admission_open_ && cancellation_complete_ && active_count_ == 0;
   }
   if (notify) {
      drained_->notify();
   }
}

boost::asio::awaitable<void> api_publication_generation::wait_for_drain() {
   for (;;) {
      const auto observed = drained_->epoch();
      auto completed = std::list<std::shared_ptr<session_entry>>{};
      {
         const auto lock = std::scoped_lock{mutex_};
         if (!admission_open_ && cancellation_complete_ && active_count_ == 0) {
            completed.splice(completed.end(), closing_sessions_);
         }
      }
      if (!completed.empty()) {
         co_return;
      }
      {
         const auto lock = std::scoped_lock{mutex_};
         if (!admission_open_ && cancellation_complete_ && active_count_ == 0) {
            co_return;
         }
      }
      co_await drained_->async_wait(observed);
   }
}

std::shared_ptr<api_publication_generation>
make_api_publication_generation(forge::api::p2p::api_binding binding) {
   return std::make_shared<api_publication_generation>(std::move(binding));
}

bool api_publication_generation_active(const std::shared_ptr<api_publication_generation>& generation) noexcept {
   return generation && generation->active();
}

void request_close_api_publication_generation(const std::shared_ptr<api_publication_generation>& generation) noexcept {
   if (generation) {
      generation->request_close();
   }
}

void set_api_publication_generation_retirement(
   const std::shared_ptr<api_publication_generation>& generation,
   std::function<void(const api_publication_generation*)> handler) {
   generation->set_retirement_handler(std::move(handler));
}

boost::asio::awaitable<void>
async_close_api_publication_generation(std::shared_ptr<api_publication_generation> generation) {
   if (generation) {
      co_await generation->async_close();
   }
}

boost::asio::awaitable<void>
accept_api_publication_generation(const std::shared_ptr<api_publication_generation>& generation,
                                  forge::net::p2p::node::incoming_protocol_stream stream) {
   co_await generation->accept(std::move(stream));
}

} // namespace forge::plugins::net::p2p::node::detail
