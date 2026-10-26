module;

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

export module forge.asio.task;

export import forge.asio.exceptions;
import forge.asio.runtime;

export namespace forge::asio::task {

class priority {
 public:
   explicit constexpr priority(int value = 0) noexcept : value_(value) {}

   [[nodiscard]] constexpr int value() const noexcept {
      return value_;
   }

   [[nodiscard]] static constexpr priority max() noexcept {
      return priority{10'000};
   }

   [[nodiscard]] static constexpr priority min() noexcept {
      return priority{-10'000};
   }

   [[nodiscard]] friend constexpr bool operator==(priority, priority) noexcept = default;
   [[nodiscard]] friend constexpr auto operator<=>(priority left, priority right) noexcept {
      return left.value_ <=> right.value_;
   }

 private:
   int value_ = 0;
};

struct task {
   priority priority{};
   std::string name;
   std::function<void()> work;
};

class context {
 public:
   [[nodiscard]] bool cancel_requested() const noexcept;
   [[nodiscard]] std::stop_token stop_token() const noexcept;
   void throw_if_cancel_requested() const;

 private:
   explicit context(std::stop_token stop_token) noexcept;

   std::stop_token stop_token_;

   friend class scheduler;
};

struct awaitable {
   priority priority{};
   std::string name;
   std::function<boost::asio::awaitable<void>(context&)> work;
};

class handle {
 public:
   handle();
   ~handle();

   handle(handle&&) noexcept;
   handle& operator=(handle&&) noexcept;

   handle(const handle&) = delete;
   handle& operator=(const handle&) = delete;

   [[nodiscard]] bool valid() const noexcept;
   [[nodiscard]] bool accepted() const noexcept;
   // Latched terminal state, including failure, cancellation and rejection.
   // Empty or moved-from handles are not completed. Use wait() for the outcome.
   [[nodiscard]] bool completed() const noexcept;
   [[nodiscard]] std::uint64_t id() const noexcept;
   [[nodiscard]] bool cancel_requested() const noexcept;
   bool cancel() noexcept;
   boost::asio::awaitable<void> wait() const;

 private:
   struct state;
   std::shared_ptr<state> state_;

   static boost::asio::awaitable<void> wait_owned(std::shared_ptr<state> state);
   explicit handle(std::shared_ptr<state> state);

   friend class scheduler;
};

class scheduler {
 public:
   struct options {
      std::size_t max_blocking_tasks = 2;
      std::size_t max_awaitable_tasks = 4096;
      std::size_t max_pending_tasks = 4096;
   };

   struct metrics {
      std::uint64_t submitted = 0;
      std::uint64_t started = 0;
      std::uint64_t completed = 0;
      std::uint64_t canceled = 0;
      std::uint64_t rejected = 0;
      std::uint64_t failed = 0;
      std::size_t pending = 0;
      std::size_t running_blocking = 0;
      std::size_t running_awaitable = 0;
      bool stopped = false;
   };

   explicit scheduler(runtime& runtime);
   scheduler(runtime& runtime, options options);
   ~scheduler();

   scheduler(const scheduler&) = delete;
   scheduler& operator=(const scheduler&) = delete;

   scheduler(scheduler&&) = delete;
   scheduler& operator=(scheduler&&) = delete;

   handle submit(task value);
   handle submit_after(task value, std::chrono::milliseconds delay);
   handle submit(awaitable value);
   handle submit_after(awaitable value, std::chrono::milliseconds delay);

   [[nodiscard]] std::size_t pending_count() const;
   [[nodiscard]] std::size_t pending_count(priority priority) const;
   [[nodiscard]] metrics snapshot() const;
   [[nodiscard]] runtime& runtime_context() noexcept;

   void request_stop();
   boost::asio::awaitable<void> shutdown();
   void stop();

 private:
   struct impl;
   std::shared_ptr<impl> impl_;
};

} // namespace forge::asio::task
