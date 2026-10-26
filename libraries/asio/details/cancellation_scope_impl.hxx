#pragma once

#include <boost/asio/strand.hpp>

#include <functional>
#include <span>
#include <vector>

namespace forge::asio {

struct cancellation_scope::impl {
   impl(boost::asio::any_io_executor executor, std::span<const std::stop_token> sources,
        boost::asio::cancellation_state parent);
   ~impl();

   boost::asio::strand<boost::asio::any_io_executor> executor;
   std::shared_ptr<std::stop_source> stopping;
   boost::asio::cancellation_slot parent_slot;
   std::vector<std::unique_ptr<std::stop_callback<std::function<void()>>>> callbacks;
   std::atomic_bool claimed{false};
};

} // namespace forge::asio
