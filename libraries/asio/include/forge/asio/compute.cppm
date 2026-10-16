module;

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>

export module forge.asio.compute;

export import forge.asio.exceptions;

namespace forge::asio::compute {

export class context;

namespace detail {

class pool_state;
class reservation;

class operation_state : public std::enable_shared_from_this<operation_state> {
 public:
   operation_state(std::shared_ptr<pool_state> owner, std::uint64_t id);
   virtual ~operation_state();

   [[nodiscard]] std::uint64_t id() const noexcept;
   [[nodiscard]] bool cancel() noexcept;
   [[nodiscard]] bool stop_requested() const noexcept;
   [[nodiscard]] std::stop_token stop_token() const noexcept;
   boost::asio::awaitable<void> wait();
   boost::asio::awaitable<void> execute(reservation reservation, std::string name, std::function<void(context&)>& work);

   void link_parent(std::stop_token parent);
   void request_stop_only() noexcept;
   bool complete_value();
   bool complete_exception(std::exception_ptr error);
   [[nodiscard]] bool completed() const noexcept;
   [[nodiscard]] std::exception_ptr cancellation_error() const noexcept;
   void install_completion(boost::asio::any_completion_handler<void(std::exception_ptr)>&& completion);

 private:
   struct impl;
   std::unique_ptr<impl> impl_;
};

template <typename T> class result_state final : public operation_state {
 public:
   using operation_state::operation_state;

   template <typename U> void store_result(U&& value) {
      const auto lock = std::scoped_lock{mutex_};
      result_.emplace(std::forward<U>(value));
   }

   T take_result() {
      const auto lock = std::scoped_lock{mutex_};
      return std::move(result_).value();
   }

 private:
   std::mutex mutex_;
   std::optional<T> result_;
};

template <> class result_state<void> final : public operation_state {
 public:
   using operation_state::operation_state;
};

template <typename T> inline constexpr bool is_boost_awaitable = false;
template <typename T, typename Executor>
inline constexpr bool is_boost_awaitable<boost::asio::awaitable<T, Executor>> = true;

template <typename Work, bool WithContext = std::invocable<Work&, context&>> struct work_traits;

template <typename Work> struct work_traits<Work, true> {
   using result_type = std::invoke_result_t<Work&, context&>;
   static constexpr bool with_context = true;
};

template <typename Work> struct work_traits<Work, false> {
   static_assert(std::invocable<Work&>, "compute work must accept compute::context& or no arguments");
   using result_type = std::invoke_result_t<Work&>;
   static constexpr bool with_context = false;
};

template <typename Work> using work_result_t = typename work_traits<Work>::result_type;

template <typename Work>
concept supported_work =
    (std::invocable<Work&, context&> || std::invocable<Work&>) &&
    !is_boost_awaitable<std::remove_cvref_t<work_result_t<Work>>> && !std::is_reference_v<work_result_t<Work>> &&
    (std::is_void_v<work_result_t<Work>> || std::move_constructible<work_result_t<Work>>);

class reservation {
 public:
   reservation() = default;
   reservation(std::shared_ptr<pool_state> owner, std::uint64_t id) noexcept;
   ~reservation();

   reservation(const reservation&) = delete;
   reservation& operator=(const reservation&) = delete;
   reservation(reservation&& other) noexcept;
   reservation& operator=(reservation&& other) noexcept;

   [[nodiscard]] std::uint64_t id() const noexcept;
   void consume() noexcept;

 private:
   std::shared_ptr<pool_state> owner_;
   std::uint64_t id_ = 0;
};

class pool_state : public std::enable_shared_from_this<pool_state> {
 public:
   struct configuration;

   explicit pool_state(configuration configuration);
   ~pool_state();

   boost::asio::awaitable<reservation> reserve(std::stop_token parent);
   std::optional<reservation> try_reserve(std::stop_token parent);
   void release_reservation(std::uint64_t id) noexcept;
   void submit(reservation reservation, std::string name, std::shared_ptr<operation_state> operation,
               std::function<void(context&)>&& work);
   bool cancel(std::uint64_t id) noexcept;
   void request_stop() noexcept;
   boost::asio::awaitable<void> shutdown();
   void shutdown_sync() noexcept;

   struct metrics_snapshot;
   [[nodiscard]] metrics_snapshot snapshot() const;

 private:
   struct impl;
   std::unique_ptr<impl> impl_;
};

} // namespace detail

class context {
 public:
   [[nodiscard]] std::stop_token stop_token() const noexcept;
   [[nodiscard]] bool stop_requested() const noexcept;
   void throw_if_stop_requested() const;

 private:
   explicit context(std::stop_token stop_token) noexcept;

   std::stop_token stop_token_;

   friend class detail::pool_state;
};

export struct task_options {
   std::string name;
   std::stop_token parent_stop_token;
};

export struct metrics {
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

namespace detail {

struct pool_state::metrics_snapshot : metrics {};

struct pool_state::configuration {
   std::size_t worker_threads = 0;
   std::size_t max_pending_tasks = 0;
   std::size_t max_waiting_submissions = 0;
   std::string thread_name;
   std::function<void(std::size_t)> on_worker_start;
   std::function<void(std::size_t)> on_worker_stop;
};

} // namespace detail

export template <typename T> class operation {
 public:
   operation() = default;

   operation(const operation&) = delete;
   operation& operator=(const operation&) = delete;
   operation(operation&&) noexcept = default;
   operation& operator=(operation&&) noexcept = default;

   [[nodiscard]] bool valid() const noexcept {
      return state_ != nullptr;
   }

   [[nodiscard]] std::uint64_t id() const noexcept {
      return state_ == nullptr ? 0 : state_->id();
   }

   bool cancel() noexcept {
      return state_ != nullptr && state_->cancel();
   }

   [[nodiscard]] bool stop_requested() const noexcept {
      return state_ != nullptr && state_->stop_requested();
   }

   boost::asio::awaitable<T> wait() && {
      return wait_owned(state_);
   }

 private:
   static boost::asio::awaitable<T> wait_owned(std::shared_ptr<detail::result_state<T>> state) {
      if (state == nullptr) {
         throw exceptions::invalid_state{"compute operation is empty"};
      }
      co_await state->wait();
      co_return state->take_result();
   }

   explicit operation(std::shared_ptr<detail::result_state<T>> state) : state_{std::move(state)} {}

   std::shared_ptr<detail::result_state<T>> state_;

   friend class executor;
};

export template <> class operation<void> {
 public:
   operation() = default;

   operation(const operation&) = delete;
   operation& operator=(const operation&) = delete;
   operation(operation&&) noexcept = default;
   operation& operator=(operation&&) noexcept = default;

   [[nodiscard]] bool valid() const noexcept {
      return state_ != nullptr;
   }

   [[nodiscard]] std::uint64_t id() const noexcept {
      return state_ == nullptr ? 0 : state_->id();
   }

   bool cancel() noexcept {
      return state_ != nullptr && state_->cancel();
   }

   [[nodiscard]] bool stop_requested() const noexcept {
      return state_ != nullptr && state_->stop_requested();
   }

   boost::asio::awaitable<void> wait() && {
      return wait_owned(state_);
   }

 private:
   static boost::asio::awaitable<void> wait_owned(std::shared_ptr<detail::result_state<void>> state) {
      if (state == nullptr) {
         throw exceptions::invalid_state{"compute operation is empty"};
      }
      co_await state->wait();
   }

   explicit operation(std::shared_ptr<detail::result_state<void>> state) : state_{std::move(state)} {}

   std::shared_ptr<detail::result_state<void>> state_;

   friend class executor;
};

export class executor {
 public:
   executor() = default;

   [[nodiscard]] bool valid() const noexcept {
      return state_ != nullptr;
   }

   template <typename Work>
      requires detail::supported_work<std::remove_cvref_t<Work>>
   boost::asio::awaitable<operation<detail::work_result_t<std::remove_cvref_t<Work>>>> submit(task_options options,
                                                                                              Work&& work) const {
      require_valid();
      using work_type = std::remove_cvref_t<Work>;
      auto callable = std::make_shared<work_type>(std::forward<Work>(work));
      return submit_owned(state_, std::move(options), std::move(callable));
   }

   template <typename Work>
      requires detail::supported_work<std::remove_cvref_t<Work>>
   std::optional<operation<detail::work_result_t<std::remove_cvref_t<Work>>>> try_submit(task_options options,
                                                                                         Work&& work) const {
      require_valid();
      auto state = state_;
      auto reservation = state->try_reserve(options.parent_stop_token);
      if (!reservation.has_value()) {
         return std::nullopt;
      }
      return submit_reserved_materialized(std::move(state), std::move(*reservation), std::move(options),
                                          std::forward<Work>(work));
   }

   template <typename Work>
      requires detail::supported_work<std::remove_cvref_t<Work>>
   boost::asio::awaitable<detail::work_result_t<std::remove_cvref_t<Work>>> execute(task_options options,
                                                                                    Work&& work) const {
      require_valid();
      using work_type = std::remove_cvref_t<Work>;
      auto callable = std::make_shared<work_type>(std::forward<Work>(work));
      return execute_owned(state_, std::move(options), std::move(callable));
   }

 private:
   explicit executor(std::shared_ptr<detail::pool_state> state) : state_{std::move(state)} {}

   void require_valid() const {
      if (state_ == nullptr) {
         throw exceptions::invalid_state{"compute executor is empty"};
      }
   }

   template <typename Work>
   static boost::asio::awaitable<detail::work_result_t<Work>>
   execute_owned(std::shared_ptr<detail::pool_state> state, task_options options, std::shared_ptr<Work> callable) {
      using result_type = detail::work_result_t<Work>;
      auto reservation = co_await state->reserve(options.parent_stop_token);
      auto result = std::make_shared<detail::result_state<result_type>>(state, reservation.id());
      result->link_parent(options.parent_stop_token);
      auto invoke = make_invocation(result, std::move(callable));
      co_await result->execute(std::move(reservation), std::move(options.name), invoke);
      if constexpr (!std::is_void_v<result_type>) {
         co_return result->take_result();
      }
   }

   template <typename Work>
   static boost::asio::awaitable<operation<detail::work_result_t<Work>>>
   submit_owned(std::shared_ptr<detail::pool_state> state, task_options options, std::shared_ptr<Work> callable) {
      auto reservation = co_await state->reserve(options.parent_stop_token);
      co_return submit_reserved_owned(std::move(state), std::move(reservation), std::move(options),
                                      std::move(callable));
   }

   template <typename Work>
   static operation<detail::work_result_t<std::remove_cvref_t<Work>>>
   submit_reserved_materialized(std::shared_ptr<detail::pool_state> state, detail::reservation reservation,
                                task_options options, Work&& work) {
      using work_type = std::remove_cvref_t<Work>;
      using result_type = detail::work_result_t<work_type>;

      auto result = std::make_shared<detail::result_state<result_type>>(state, reservation.id());
      result->link_parent(options.parent_stop_token);
      auto callable = std::make_shared<work_type>(std::forward<Work>(work));
      return submit_linked(std::move(state), std::move(reservation), std::move(options), std::move(result),
                           std::move(callable));
   }

   template <typename Work>
   static operation<detail::work_result_t<Work>>
   submit_reserved_owned(std::shared_ptr<detail::pool_state> state, detail::reservation reservation,
                         task_options options, std::shared_ptr<Work> callable) {
      using result_type = detail::work_result_t<Work>;

      auto result = std::make_shared<detail::result_state<result_type>>(state, reservation.id());
      result->link_parent(options.parent_stop_token);
      return submit_linked(std::move(state), std::move(reservation), std::move(options), std::move(result),
                           std::move(callable));
   }

   template <typename Work>
   static operation<detail::work_result_t<Work>>
   submit_linked(std::shared_ptr<detail::pool_state> state, detail::reservation reservation, task_options options,
                 std::shared_ptr<detail::result_state<detail::work_result_t<Work>>> result,
                 std::shared_ptr<Work> callable) {
      using result_type = detail::work_result_t<Work>;
      auto invoke = make_invocation(result, std::move(callable));
      state->submit(std::move(reservation), std::move(options.name), result, std::move(invoke));
      return operation<result_type>{std::move(result)};
   }

   template <typename Work>
   static std::function<void(context&)>
   make_invocation(std::shared_ptr<detail::result_state<detail::work_result_t<Work>>> result,
                   std::shared_ptr<Work> callable) {
      using result_type = detail::work_result_t<Work>;
      return [callable = std::move(callable), result = std::move(result)](context& work_context) mutable {
         if constexpr (detail::work_traits<Work>::with_context) {
            if constexpr (std::is_void_v<result_type>) {
               std::invoke(*callable, work_context);
            } else {
               result->store_result(std::invoke(*callable, work_context));
            }
         } else {
            if constexpr (std::is_void_v<result_type>) {
               std::invoke(*callable);
            } else {
               result->store_result(std::invoke(*callable));
            }
         }
      };
   }

   std::shared_ptr<detail::pool_state> state_;

   friend class pool;
};

export class pool {
 public:
   struct options {
      std::size_t worker_threads = 0;
      std::size_t max_pending_tasks = 1024;
      std::size_t max_waiting_submissions = 1024;
      std::string thread_name = "forge-compute";
      std::function<void(std::size_t)> on_worker_start;
      std::function<void(std::size_t)> on_worker_stop;
   };

   pool();
   explicit pool(options options);
   ~pool();

   pool(const pool&) = delete;
   pool& operator=(const pool&) = delete;
   pool(pool&&) = delete;
   pool& operator=(pool&&) = delete;

   [[nodiscard]] executor get_executor() const noexcept;
   [[nodiscard]] metrics snapshot() const;
   void request_stop() noexcept;
   boost::asio::awaitable<void> shutdown();

 private:
   std::shared_ptr<detail::pool_state> state_;
};

} // namespace forge::asio::compute
