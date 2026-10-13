module;

#include "details/lifecycle_log.hxx"
#include "details/shutdown_state.hxx"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

module forge.app.runner;

import forge.asio.blocking;
import forge.app.exceptions;
import forge.app.plugin_lifecycle;
import forge.app.application;
import forge.app.application_shell;
import forge.config.core.document;

#include "details/shutdown_owner.hxx"

namespace forge::app {
namespace {

using detail::shutdown_owner;
using detail::shutdown_state;

void log_runner(application_shell& app, const char* event, const char* operation,
                detail::lifecycle_log_level level, std::exception_ptr error = {},
                bool has_exit_code = false, int exit_code = 0) noexcept {
   try {
      const auto state = app.lifecycle().snapshot();
      detail::emit_lifecycle_log({.logger = "forge.app.runner",
                                 .event = event,
                                 .operation = operation,
                                 .level = level,
                                 .phase = static_cast<int>(state.phase),
                                 .generation = state.generation,
                                 .epoch = state.epoch,
                                 .stop_requested = state.stop_requested,
                                 .error = error,
                                 .canceled_event = "runner.canceled",
                                 .exit_code = exit_code,
                                 .has_exit_code = has_exit_code});
   } catch (...) {
   }
}

boost::asio::awaitable<void> wait_for_os_signal(application_shell& app, const run_options& options) {
   auto signals = boost::asio::signal_set{app.runtime().context()};
   if (options.handle_sigint) {
      signals.add(SIGINT);
   }
   if (options.handle_sigterm) {
      signals.add(SIGTERM);
   }

   auto error = boost::system::error_code{};
   co_await signals.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
   if (error) {
      throw exceptions::startup_failed{"application signal wait failed"};
   }
   log_runner(app, "runner.signal", "wait_for_stop", detail::lifecycle_log_level::debug);
}

void keep_owner_until_shutdown_finishes(std::shared_ptr<shutdown_state> state, std::shared_ptr<shutdown_owner> owner) {
   if (!owner) {
      return;
   }
   std::thread{[state = std::move(state), owner = std::move(owner)]() mutable {
      auto lock = std::unique_lock{state->mutex};
      state->ready.wait(lock, [&] {
         return state->done;
      });
      lock.unlock();
      owner.reset();
   }}.detach();
}

bool run_shutdown_until_timeout(application_shell& app, std::chrono::milliseconds timeout,
                                bool allow_async_shutdown_tail,
                                std::shared_ptr<shutdown_owner> owner = {}) {
   auto state = std::make_shared<shutdown_state>();
   auto timer = std::make_shared<boost::asio::steady_timer>(app.runtime().context());
   timer->expires_after(timeout);

   boost::asio::co_spawn(
      app.runtime().context(),
      [state, timer]() -> boost::asio::awaitable<void> {
         auto error = boost::system::error_code{};
         co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
         if (!error) {
            {
               const auto lock = std::scoped_lock{state->mutex};
               if (!state->done) {
                  state->timed_out = true;
               }
            }
            state->ready.notify_all();
         }
      },
      boost::asio::detached);

   boost::asio::co_spawn(
      app.runtime().context(),
      [&app, state, timer]() -> boost::asio::awaitable<void> {
         auto error = std::exception_ptr{};
         try {
            co_await app.shutdown();
         } catch (...) {
            error = std::current_exception();
         }

         {
            const auto lock = std::scoped_lock{state->mutex};
            state->error = std::move(error);
            state->done = true;
         }
         timer->cancel();
         state->ready.notify_all();
      },
      boost::asio::detached);

   auto lock = std::unique_lock{state->mutex};
   state->ready.wait(lock, [&] {
      return state->done || state->timed_out;
   });

   if (state->timed_out) {
      if (!state->done) {
         if (allow_async_shutdown_tail) {
            keep_owner_until_shutdown_finishes(state, std::move(owner));
            return false;
         }
         state->ready.wait(lock, [&] {
            return state->done;
         });
      }
      auto error = state->error;
      lock.unlock();
      if (error) {
         std::rethrow_exception(error);
      }
      return false;
   }
   auto error = state->error;
   lock.unlock();
   if (error) {
      std::rethrow_exception(error);
   }
   return true;
}

void shutdown_with_timeout(application_shell& app, std::chrono::milliseconds timeout,
                           bool allow_async_shutdown_tail, std::shared_ptr<shutdown_owner> owner = {}) {
   app.request_stop();
   if (app.state() == lifecycle_phase::stopped) {
      return;
   }
   if (timeout.count() <= 0) {
      forge::asio::blocking::run(app.runtime(), app.shutdown());
      return;
   }
   if (!run_shutdown_until_timeout(app, timeout, allow_async_shutdown_tail, std::move(owner))) {
      throw exceptions::shutdown_failed{"application shutdown timed out"};
   }
}

int run_application_impl(application_shell& app, const forge::config::core::document& document, run_options options,
                         std::shared_ptr<shutdown_owner> owner) {
   auto exit_code = 0;
   auto failure = std::exception_ptr{};
   auto stage = "configure";
   log_runner(app, "runner.start", "run", detail::lifecycle_log_level::info);

   try {
      app.configure(document);
      stage = "startup";
      forge::asio::blocking::run(app.runtime(), app.startup());
      stage = "wait_for_stop";
      if (options.wait_for_stop) {
         forge::asio::blocking::run(app.runtime(), options.wait_for_stop(app));
      } else if (options.handle_sigint || options.handle_sigterm) {
         forge::asio::blocking::run(app.runtime(), wait_for_os_signal(app, options));
      } else {
         stage = "foreground";
         exit_code = app.run();
      }
   } catch (...) {
      failure = std::current_exception();
      log_runner(app, "runner.failed", stage, detail::lifecycle_log_level::error, failure);
   }

   try {
      const auto allow_async_shutdown_tail = static_cast<bool>(owner);
      shutdown_with_timeout(app, options.shutdown_timeout, allow_async_shutdown_tail, std::move(owner));
   } catch (...) {
      log_runner(app, "runner.failed", "shutdown", detail::lifecycle_log_level::error, std::current_exception());
      if (!failure) {
         throw;
      }
   }

   if (failure) {
      std::rethrow_exception(failure);
   }
   log_runner(app, "runner.exit", "run", detail::lifecycle_log_level::info, {}, true, exit_code);
   return exit_code;
}

} // namespace

int run_application(application_shell& app, const forge::config::core::document& document, run_options options) {
   return run_application_impl(app, document, std::move(options), {});
}

int run_application(std::unique_ptr<application_shell> app, const forge::config::core::document& document,
                    run_options options) {
   if (!app) {
      throw std::invalid_argument{"application pointer must not be null"};
   }
   auto owner = std::make_shared<shutdown_owner>(std::move(app));
   return run_application_impl(*owner->app, document, std::move(options), owner);
}

} // namespace forge::app
