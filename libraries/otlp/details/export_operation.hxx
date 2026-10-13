#pragma once

#include <boost/asio/cancellation_signal.hpp>

#include <exception>

namespace forge::otlp {

struct export_operation {
   forge::asio::notification changed;
   boost::asio::cancellation_signal cancellation;
   export_result result;
   std::exception_ptr error;
   bool done = false;
};

} // namespace forge::otlp
