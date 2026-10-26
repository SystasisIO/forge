module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stop_token>
#include <type_traits>
#include <utility>

export module forge.asio.cancellation_scope;

export import forge.asio.exceptions;

export namespace forge::asio {

// One joined asynchronous invocation, with standard/native cancellation.
// Sources are registered eagerly; neither the span nor its storage is retained.
// The parent's output slot is borrowed EXCLUSIVELY until this scope and its
// returned awaitable are destroyed. Do not install another operation on that
// slot concurrently. Parent emit/assign/clear must obey Asio serialization;
// request_stop() and the standard stop tokens may be used from any thread.
// The parent cancellation state/executor must outlive the scope and all work.
// This owner never resets the parent's filters, pending state or throw policy.
class cancellation_scope {
 public:
   cancellation_scope(boost::asio::any_io_executor executor, std::span<const std::stop_token> sources,
                      boost::asio::cancellation_state parent);
   ~cancellation_scope();
   cancellation_scope(const cancellation_scope&) = delete;
   cancellation_scope& operator=(const cancellation_scope&) = delete;
   cancellation_scope(cancellation_scope&&) = delete;
   cancellation_scope& operator=(cancellation_scope&&) = delete;

   [[nodiscard]] std::stop_token stop_token() const noexcept;
   void request_stop() noexcept;

   // Claims the single invocation eagerly and retains its state even when the
   // returned coroutine has not been polled. External links remain active AFTER
   // run returns, until scope destruction (for example, an opened stream).
   // Operation receives the sticky token and must check it before external I/O.
   // Result/exception policy belongs to the operation/caller: stopping does not
   // automatically discard a successful result. Timeout merely requests stop.
   template <typename Result, typename Action>
   boost::asio::awaitable<Result> run(Action operation, std::optional<std::chrono::milliseconds> timeout = {}) {
      return execute<Result>(claim(), std::make_shared<Action>(std::move(operation)), timeout);
   }

 private:
   struct impl;
   std::shared_ptr<impl> claim();
   static boost::asio::any_io_executor executor(const std::shared_ptr<impl>& state);
   static std::shared_ptr<std::stop_source> source(const std::shared_ptr<impl>& state);

   template <typename Result, typename Action>
   static boost::asio::awaitable<Result> execute(std::shared_ptr<impl> state, std::shared_ptr<Action> operation,
                                                 std::optional<std::chrono::milliseconds> timeout) {
      const auto target = executor(state);
      const auto stopping = source(state);
      auto invoke = [target, stopping, operation = std::move(operation),
                     timeout]() mutable -> boost::asio::awaitable<Result> {
         const auto signal = std::make_shared<boost::asio::cancellation_signal>();
         const auto propagate = std::stop_callback{
             stopping->get_token(), [target, signal] {
                boost::asio::post(target, [signal] { signal->emit(boost::asio::cancellation_type::all); });
             }};
         auto deadline = boost::asio::steady_timer{target};
         const auto armed = std::make_shared<std::atomic_bool>(false);
         const auto disarm = boost::scope::scope_exit([armed] {
            armed->store(false, std::memory_order_release);
            // The timer's native destructor cancels its wait immediately after
            // this guard. An already-queued handler can retain only stop/armed.
         });
         if (timeout) {
            deadline.expires_after(*timeout);
            armed->store(true, std::memory_order_release);
            deadline.async_wait([stopping, armed](const boost::system::error_code& error) {
               if (!error && armed->exchange(false, std::memory_order_acq_rel)) {
                  stopping->request_stop();
               }
            });
         }

         auto child = [stopping, operation = std::move(operation)]() mutable -> boost::asio::awaitable<Result> {
            // co_spawn may dispatch completion before destroying its function
            // object. Move the owned callable into this coroutine frame, so its
            // captures retire BEFORE the child's result/exception is published.
            const auto callable = std::move(operation);
            co_return co_await (*callable)(stopping->get_token());
         };
         if constexpr (std::is_void_v<Result>) {
            co_await boost::asio::co_spawn(
                target, std::move(child),
                boost::asio::bind_cancellation_slot(signal->slot(), boost::asio::use_awaitable));
         } else {
            co_return co_await boost::asio::co_spawn(
                target, std::move(child),
                boost::asio::bind_cancellation_slot(signal->slot(), boost::asio::use_awaitable));
         }
      };
      // Registration/emission share target's strand. The caller cannot leave
      // this joined operation while child code still borrows its arguments.
      if constexpr (std::is_void_v<Result>) {
         co_await boost::asio::co_spawn(
             target, std::move(invoke),
             boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
      } else {
         co_return co_await boost::asio::co_spawn(
             target, std::move(invoke),
             boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
      }
   }

   std::shared_ptr<impl> _impl;
};

} // namespace forge::asio
