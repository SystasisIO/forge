module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

export module forge.asio.affine;

export import forge.asio.exceptions;

export namespace forge::asio::affine {

struct operation_options {
   std::string name;
};

struct metrics {
   std::uint64_t submitted = 0;
   std::uint64_t completed = 0;
   std::uint64_t canceled = 0;
   std::uint64_t rejected = 0;
   std::uint64_t failed = 0;
   std::size_t waiting = 0;
   std::size_t pending = 0;
   std::size_t running = 0;
   bool stopped = false;
   std::chrono::nanoseconds total_queue_time{};
   std::chrono::nanoseconds total_execution_time{};
};

} // namespace forge::asio::affine

namespace forge::asio::affine::detail {

class lane_state;

enum class operation_phase : std::uint8_t {
   waiting,
   pending,
   dispatched,
   running,
   canceled,
   rejected,
   completed,
};

class operation_state : public std::enable_shared_from_this<operation_state> {
 public:
   operation_state(std::shared_ptr<lane_state> owner, std::string name);
   virtual ~operation_state();

   boost::asio::awaitable<void> wait();
   bool cancel_before_start() noexcept;
   bool complete_value() noexcept;
   bool complete_exception(std::exception_ptr error) noexcept;

   virtual void run() = 0;

   std::weak_ptr<lane_state> owner;
   std::string name;
   std::chrono::steady_clock::time_point queued_at;
   operation_phase phase = operation_phase::waiting;

 private:
   mutable std::mutex completion_mutex_;
   bool completed_ = false;
   bool wait_started_ = false;
   std::exception_ptr error_;
   std::function<void()> wake_;
};

struct operation_cancellation_filter {
   std::weak_ptr<operation_state> operation;

   boost::asio::cancellation_type_t
   operator()(boost::asio::cancellation_type_t type) const noexcept;
};

template <typename Work, typename Result> class result_state final : public operation_state {
 public:
   result_state(std::shared_ptr<lane_state> owner, std::string name, std::shared_ptr<Work> callable)
       : operation_state{std::move(owner), std::move(name)}, callable_{std::move(callable)} {}

   void run() override {
      result_.emplace(std::invoke(*callable_));
   }

   Result take_result() {
      return std::move(result_).value();
   }

 private:
   std::shared_ptr<Work> callable_;
   std::optional<Result> result_;
};

template <typename Work> class result_state<Work, void> final : public operation_state {
 public:
   result_state(std::shared_ptr<lane_state> owner, std::string name, std::shared_ptr<Work> callable)
       : operation_state{std::move(owner), std::move(name)}, callable_{std::move(callable)} {}

   void run() override {
      std::invoke(*callable_);
   }

 private:
   std::shared_ptr<Work> callable_;
};

template <typename T> inline constexpr bool is_boost_awaitable = false;
template <typename T, typename Executor>
inline constexpr bool is_boost_awaitable<boost::asio::awaitable<T, Executor>> = true;

template <typename Work> using work_result_t = std::invoke_result_t<Work&>;

template <typename Work>
concept supported_work = std::invocable<Work&> &&
                         !is_boost_awaitable<std::remove_cvref_t<work_result_t<Work>>> &&
                         !std::is_reference_v<work_result_t<Work>> &&
                         (std::is_void_v<work_result_t<Work>> ||
                          std::move_constructible<work_result_t<Work>>);

class lane_state : public std::enable_shared_from_this<lane_state> {
 public:
   struct configuration {
      std::size_t max_pending_operations = 1024;
      std::size_t max_waiting_submissions = 1024;
      std::string thread_name = "forge-affine";
   };

   explicit lane_state(configuration configuration);
   ~lane_state();

   boost::asio::awaitable<void> execute(std::shared_ptr<operation_state> operation);
   bool cancel_before_start(const std::shared_ptr<operation_state>& operation) noexcept;
   void request_stop() noexcept;
   boost::asio::awaitable<void> shutdown();
   void shutdown_sync() noexcept;
   [[nodiscard]] forge::asio::affine::metrics snapshot() const;

 private:
   struct impl;

   void submit(const std::shared_ptr<operation_state>& operation);
   void post_operation(const std::shared_ptr<operation_state>& operation);
   void run_operation(const std::shared_ptr<operation_state>& operation) noexcept;
   void finish_operation(const std::shared_ptr<operation_state>& operation,
                         std::exception_ptr error,
                         bool invoked,
                         std::chrono::nanoseconds execution_time) noexcept;
   std::shared_ptr<operation_state> select_next_locked();
   void promote_waiters_locked();
   bool drained_locked() const noexcept;
   boost::asio::awaitable<void> wait_for_drain();
   void finalize();

   std::unique_ptr<impl> impl_;

   friend class operation_state;
};

} // namespace forge::asio::affine::detail

export namespace forge::asio::affine {

class executor {
 public:
   executor() = default;

   [[nodiscard]] bool valid() const noexcept {
      return state_ != nullptr;
   }

   template <typename Work>
      requires detail::supported_work<std::remove_cvref_t<Work>>
   boost::asio::awaitable<detail::work_result_t<std::remove_cvref_t<Work>>>
   execute(operation_options options, Work&& work) const {
      require_valid();
      using work_type = std::remove_cvref_t<Work>;
      using result_type = detail::work_result_t<work_type>;
      auto callable = std::make_shared<work_type>(std::forward<Work>(work));
      auto operation = std::make_shared<detail::result_state<work_type, result_type>>(
         state_, std::move(options.name), std::move(callable));
      return execute_owned<work_type, result_type>(state_, std::move(operation));
   }

 private:
   explicit executor(std::shared_ptr<detail::lane_state> state) : state_{std::move(state)} {}

   void require_valid() const {
      if (state_ == nullptr) {
         throw exceptions::invalid_state{"affine executor is empty"};
      }
   }

   template <typename Work, typename Result>
   static boost::asio::awaitable<Result>
   execute_owned(std::shared_ptr<detail::lane_state> state,
                 std::shared_ptr<detail::result_state<Work, Result>> operation) {
      const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
      if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
         static_cast<void>(operation->cancel_before_start());
         FORGE_THROW_EXCEPTION(exceptions::canceled, "affine operation was canceled before execution");
      }
      const auto executor = co_await boost::asio::this_coro::executor;
      // The operation's completion-wins filter belongs to its own awaitable thread.
      // Keep the result outside co_spawn so non-default-constructible results remain supported.
      co_await boost::asio::co_spawn(executor, state->execute(operation), boost::asio::use_awaitable);
      if constexpr (!std::is_void_v<Result>) {
         co_return operation->take_result();
      }
   }

   std::shared_ptr<detail::lane_state> state_;

   friend class lane;
};

class lane {
 public:
   struct options {
      std::size_t max_pending_operations = 1024;
      std::size_t max_waiting_submissions = 1024;
      std::string thread_name = "forge-affine";
   };

   lane();
   explicit lane(options options);
   ~lane();

   lane(const lane&) = delete;
   lane& operator=(const lane&) = delete;
   lane(lane&&) = delete;
   lane& operator=(lane&&) = delete;

   [[nodiscard]] executor get_executor() const noexcept;
   [[nodiscard]] metrics snapshot() const;
   void request_stop() noexcept;
   boost::asio::awaitable<void> shutdown();

 private:
   std::shared_ptr<detail::lane_state> state_;
};

} // namespace forge::asio::affine
