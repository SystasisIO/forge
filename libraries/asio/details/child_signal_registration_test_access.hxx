#pragma once

namespace forge::asio::detail {

struct child_signal_registration_test_access {
   static void acquire(child_signal_registration&, boost::asio::any_io_executor executor, void (*after_install)());
};

} // namespace forge::asio::detail
