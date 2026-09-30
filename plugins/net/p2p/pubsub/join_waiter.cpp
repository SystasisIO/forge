module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.pubsub.plugin;

import forge.net.p2p.pubsub;
import forge.plugins.net.p2p.pubsub.api;
import forge.plugins.net.p2p.pubsub.types;

#include "details/join_waiter.hxx"

namespace forge::plugins::net::p2p::pubsub {

join_waiter::join_waiter(boost::asio::any_io_executor executor) : timer(std::move(executor)) {
   timer.expires_at(boost::asio::steady_timer::time_point::max());
}

void join_waiter::complete(std::exception_ptr failure) {
   auto self = shared_from_this();
   boost::asio::post(timer.get_executor(), [self = std::move(self), failure = std::move(failure)]() mutable {
      self->error = std::move(failure);
      self->ready = true;
      self->timer.cancel(); // waiter_executor
   });
}

} // namespace forge::plugins::net::p2p::pubsub
