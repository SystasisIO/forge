module;

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

export module forge.vm.wasm.interpret.watchdog;

export namespace forge::vm::wasm::interpret {

/// \brief Triggers a callback after a given time elapses.
///
class watchdog {
   class guard;

 public:
   enum class interruption { deadline, canceled };

   /// \tparam TimeUnits must be a chrono duration type
   /// \pre duration must be a non-negative value.
   template <typename TimeUnits> explicit watchdog(const TimeUnits& duration) : _duration(duration) {}
   template <typename TimeUnits>
   watchdog(const TimeUnits& duration, std::stop_token stop) : _duration(duration), _stop(std::move(stop)) {}
   /// Starts the timer.  If the timer
   /// expires during the lifetime of the returned object, the callback
   /// will be executed.  The callback is executed asynchronously.
   /// If invoking the callback throws an exception, terminate will
   /// be called.
   template <typename F> [[nodiscard]] guard scoped_run(F&& callback) {
      return guard(_duration, _stop, std::forward<F>(callback));
   }

 private:
   class guard {
    public:
      guard(const guard&) = delete;
      guard& operator=(const guard&) = delete;

      template <typename TimeUnits, typename F>
      guard(const TimeUnits& duration, std::stop_token stop, F&& callback)
          : _callback([function = std::forward<F>(callback)](interruption reason) mutable {
               if constexpr (std::is_invocable_v<F&, interruption>)
                  function(reason);
               else
                  function();
            }),
            _run_state(running), _duration(duration), _start(std::chrono::steady_clock::now()),
            _stop(std::move(stop)) {
         // Must be started after all other initialization to avoid races.
         _timer = std::thread(&guard::runner, this);
      }
      ~guard();

    private:
      // worker thread
      // Implementation notes: This implementation creates a thread for
      // each watchdog.  If there are many watchdogs, it might be more
      // efficient to create a single thread for them all.
      void runner();
      std::chrono::steady_clock::time_point deadline() const noexcept;
      enum state_t { running, interrupted, stopped };
      using time_point_type = std::chrono::time_point<std::chrono::steady_clock>;
      using duration_type = std::chrono::steady_clock::duration;
      std::thread _timer;
      std::mutex _mutex;
      std::condition_variable _cond;
      std::function<void(interruption)> _callback;
      state_t _run_state = stopped;
      duration_type _duration;
      time_point_type _start;
      std::stop_token _stop;
   };
   std::chrono::steady_clock::duration _duration;
   std::stop_token _stop;
};

class null_watchdog {
 public:
   template <typename F> null_watchdog scoped_run(F&&) {
      return *this;
   }
   ~null_watchdog() {} // avoid unused variable warnings
};

} // namespace forge::vm::wasm::interpret
