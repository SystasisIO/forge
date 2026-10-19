module;

#include <memory>
#include <boost/asio/any_io_executor.hpp>

namespace forge::asio::detail {
struct child_signal_registration_test_access;
}

export module forge.asio.child_signal_registration;

export import forge.asio.exceptions;

export namespace forge::asio {

// Owns a validated native Asio SIGCHLD registration only. The caller owns all
// children, process handles, runtimes and their shutdown/join order.
class child_signal_registration {
 public:
   child_signal_registration();
   // Explicit release() is required before destruction, while the executor's
   // context remains alive and after every child/process handle has drained.
   ~child_signal_registration();
   child_signal_registration(const child_signal_registration&) = delete;
   child_signal_registration& operator=(const child_signal_registration&) = delete;
   child_signal_registration(child_signal_registration&&) = delete;
   child_signal_registration& operator=(child_signal_registration&&) = delete;

   // A failure after native registration commits retains that SAME owner for
   // explicit release. Never put acquire inside an owner's throwing constructor.
   void acquire(boost::asio::any_io_executor executor);
   void release();
   [[nodiscard]] bool registered() const noexcept;

 private:
   friend struct detail::child_signal_registration_test_access;
   void acquire(boost::asio::any_io_executor executor, void (*after_install)());
   struct impl;
   std::unique_ptr<impl> _impl;
};

} // namespace forge::asio
