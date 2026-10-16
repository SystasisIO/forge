module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/system/error_code.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

export module forge.asio.gate;

export import forge.asio.exceptions;

namespace forge::asio::detail {

class gate_state;

} // namespace forge::asio::detail

export namespace forge::asio {

class gate {
 public:
   class ticket;

   gate();
   ~gate();

   gate(const gate&) = delete;
   gate& operator=(const gate&) = delete;
   gate(gate&&) = delete;
   gate& operator=(gate&&) = delete;

   boost::asio::awaitable<ticket> acquire();
   void close() noexcept;
   [[nodiscard]] bool closed() const noexcept;

 private:
   std::shared_ptr<detail::gate_state> state_;
};

class gate::ticket {
 public:
   ticket() = default;
   ~ticket();

   ticket(const ticket&) = delete;
   ticket& operator=(const ticket&) = delete;
   ticket(ticket&& other) noexcept;
   ticket& operator=(ticket&& other) noexcept;

   [[nodiscard]] bool active() const noexcept;
   void release() noexcept;

 private:
   explicit ticket(std::shared_ptr<detail::gate_state> state);

   std::shared_ptr<detail::gate_state> state_;

   friend class detail::gate_state;
};

} // namespace forge::asio

namespace forge::asio::detail {

enum class gate_wait_state : std::uint8_t {
   queued,
   granted,
   canceled,
   rejected,
   completed,
};

struct gate_waiter {
   std::function<void()> wake;
   gate_wait_state state = gate_wait_state::queued;
};

struct gate_cancellation_handler {
   std::weak_ptr<gate_state> owner;
   std::weak_ptr<gate_waiter> waiter;

   void operator()(boost::asio::cancellation_type_t type) const noexcept;
};

class gate_state : public std::enable_shared_from_this<gate_state> {
 public:
   boost::asio::awaitable<forge::asio::gate::ticket>
   acquire(std::shared_ptr<gate_waiter> waiter, boost::asio::awaitable<boost::system::error_code> wait);
   void close() noexcept;
   [[nodiscard]] bool closed() const noexcept;
   void release_one() noexcept;

 private:
   void cancel(const std::shared_ptr<gate_waiter>& waiter) noexcept;

   mutable std::mutex mutex_;
   bool held_ = false;
   bool closed_ = false;
   std::shared_ptr<gate_waiter> granted_;
   std::deque<std::shared_ptr<gate_waiter>> waiters_;

   friend struct gate_cancellation_handler;
};

} // namespace forge::asio::detail
