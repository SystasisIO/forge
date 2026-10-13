#include "controlled_collector.hxx"
#include "controlled_collector_state.hxx"

#include <utility>

namespace otlp_concurrency_fixtures {

controlled_collector::controlled_collector(forge::asio::runtime& runtime, forge::net::http::status status)
    : state_{std::make_shared<state>()},
      server_{runtime, {.bind_address = "127.0.0.1", .port = 0},
              [owner = state_](forge::net::http::route_context& context) { return handle(owner, context); }} {
   state_->status = status;
   server_.start();
}

controlled_collector::~controlled_collector() {
   release();
   server_.stop();
}

std::string controlled_collector::endpoint() const {
   return "http://127.0.0.1:" + std::to_string(server_.port());
}

bool controlled_collector::wait_for_requests(std::size_t count, std::chrono::milliseconds timeout) const {
   auto lock = std::unique_lock{state_->mutex};
   return state_->requests_changed.wait_for(lock, timeout, [&] { return state_->bodies.size() >= count; });
}

std::vector<std::string> controlled_collector::bodies() const {
   const auto lock = std::scoped_lock{state_->mutex};
   return state_->bodies;
}

void controlled_collector::release() noexcept {
   {
      const auto lock = std::scoped_lock{state_->mutex};
      state_->released = true;
   }
   state_->responses_released.notify();
}

boost::asio::awaitable<forge::net::http::response> controlled_collector::handle(
    std::shared_ptr<state> owner, forge::net::http::route_context& context) {
   {
      const auto lock = std::scoped_lock{owner->mutex};
      owner->bodies.push_back(context.request.body());
   }
   owner->requests_changed.notify_all();
   for (;;) {
      const auto epoch = owner->responses_released.epoch();
      {
         const auto lock = std::scoped_lock{owner->mutex};
         if (owner->released) {
            break;
         }
      }
      co_await owner->responses_released.async_wait(epoch);
   }
   co_return forge::net::http::make_text_response(context.request, owner->status, "{}", "application/json");
}

} // namespace otlp_concurrency_fixtures
