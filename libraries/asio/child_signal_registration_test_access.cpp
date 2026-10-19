#include <utility>
#include <boost/asio/any_io_executor.hpp>

import forge.asio.child_signal_registration;

#include "details/child_signal_registration_test_access.hxx"

namespace forge::asio::detail {

void child_signal_registration_test_access::acquire(child_signal_registration& owner,
                                                    boost::asio::any_io_executor executor, void (*after_install)()) {
   owner.acquire(std::move(executor), after_install);
}

} // namespace forge::asio::detail
