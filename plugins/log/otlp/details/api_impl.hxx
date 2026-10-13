#pragma once

#include <boost/asio/awaitable.hpp>

#include <memory>

namespace forge::plugins::log::otlp {

class plugin::api_impl final : public api {
 public:
   explicit api_impl(std::shared_ptr<impl> state);

   boost::asio::awaitable<flush_result> flush(flush_request value) override;
   boost::asio::awaitable<::forge::plugins::log::otlp::metrics> metrics(metrics_request value) override;

 private:
   static boost::asio::awaitable<flush_result> flush_owned(std::shared_ptr<impl> state);
   static boost::asio::awaitable<::forge::plugins::log::otlp::metrics>
   metrics_owned(std::shared_ptr<impl> state);
   std::shared_ptr<impl> state_;
};

} // namespace forge::plugins::log::otlp
