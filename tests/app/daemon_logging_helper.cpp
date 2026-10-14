#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>
#include <forge/log/macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#if !defined(_WIN32)
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

import forge.app.application_shell;
import forge.app.daemon;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.app.runner;
import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.config.core.component;
import forge.config.core.document;
import forge.log.logger;
import forge.log.logger_config;
import forge.variant.value;

namespace {
using namespace std::chrono_literals;

void require(bool condition, const char* reason) {
   if (!condition) {
      throw std::runtime_error{reason};
   }
}

struct nonstandard_failure {
   int value = 73;
};

struct test_state {
   std::string mode;
   std::string id = "main";
   std::atomic_int configured{0};
   std::atomic_int started{0};
   std::atomic_int cleaned{0};
   std::atomic_int destroyed{0};
   std::atomic_int factory_destroyed{0};
   std::atomic_int waiter_destroyed{0};
   std::atomic_bool premature_close{false};
   std::atomic_bool released{false};
   forge::asio::notification release;
   std::promise<void> held_signal;
   std::shared_future<void> held = held_signal.get_future().share();
   std::promise<void> foreground_signal;
   std::shared_future<void> foreground = foreground_signal.get_future().share();
   std::promise<void> destroyed_signal;
   std::shared_future<void> destruction = destroyed_signal.get_future().share();

   void unblock() {
      released.store(true);
      release.notify();
   }

   boost::asio::awaitable<void> wait_release() {
      const auto epoch = release.epoch();
      if (!released.load()) {
         const auto changed = co_await release.async_wait_until(epoch, std::chrono::steady_clock::now() + 10s);
         require(changed != epoch && released.load(), "fixture release barrier expired");
      }
   }
};

void event(const std::shared_ptr<test_state>& state, const char* phase) {
   const auto closed = forge::logging_delivery_metrics().closed;
   if (closed) {
      state->premature_close.store(true);
   }
   ilog("fixture", "${phase}/${id}", ("phase", phase)("id", state->id)("closed", closed));
}

std::mutex control_mutex;

void report(const char* phase, const std::shared_ptr<test_state>& state, int result = 0,
            bool threw = false, double elapsed_ms = 0) {
   const auto metrics = forge::logging_delivery_metrics();
   const auto lock = std::scoped_lock{control_mutex};
   std::cout << "@@DAEMON_LOG {\"phase\":\"" << phase << "\",\"code\":" << result
             << ",\"threw\":" << threw << ",\"closed\":" << metrics.closed
             << ",\"queued\":" << metrics.queued_records << ",\"bytes\":" << metrics.queued_bytes
             << ",\"accepted\":" << metrics.accepted << ",\"delivered\":" << metrics.delivered
             << ",\"failed\":" << metrics.failed << ",\"dropped\":" << metrics.dropped
             << ",\"log_failures\":" << forge::log_failure_count()
             << ",\"configured\":" << state->configured.load() << ",\"started\":" << state->started.load()
             << ",\"cleaned\":" << state->cleaned.load() << ",\"destroyed\":" << state->destroyed.load()
             << ",\"factory\":" << state->factory_destroyed.load()
             << ",\"waiter\":" << state->waiter_destroyed.load()
             << ",\"premature\":" << state->premature_close.load()
             << ",\"elapsed_ms\":" << elapsed_ms << "}\n" << std::flush;
}

std::string read_command(std::stop_token stop = {}) {
#if defined(_WIN32)
   throw std::runtime_error{"interactive logging fixture requires POSIX pipes"};
#else
   const auto deadline = std::chrono::steady_clock::now() + 12s;
   auto command = std::string{};
   while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
      auto descriptor = pollfd{STDIN_FILENO, POLLIN, 0};
      const auto ready = ::poll(&descriptor, 1, 100);
      if (ready < 0 && errno == EINTR) {
         continue;
      }
      require(ready >= 0, "fixture command poll failed");
      if (ready == 0) {
         continue;
      }
      char value = 0;
      const auto count = ::read(STDIN_FILENO, &value, 1);
      if (count < 0 && errno == EINTR) {
         continue;
      }
      require(count == 1, "fixture command channel closed");
      if (value == '\n') {
         return command;
      }
      command += value;
      require(command.size() < 32, "fixture command exceeded bound");
   }
   require(stop.stop_requested(), "fixture command timed out");
   return {};
#endif
}

struct capture_marker {
   std::shared_ptr<test_state> state;
   bool factory;
   ~capture_marker() {
      event(state, factory ? "factory.destroy" : "waiter.destroy");
      if (factory) {
         ++state->factory_destroyed;
      } else {
         ++state->waiter_destroyed;
      }
   }
};

class test_plugin final : public forge::app::plugin {
 public:
   explicit test_plugin(std::shared_ptr<test_state> state) : _state{std::move(state)} {}
   forge::app::plugin_id id() const override { return {"fixture"}; }
   std::string version() const override { return "1"; }

 protected:
   boost::asio::awaitable<void> on_initialize(forge::app::plugin_context&) override { co_return; }
   boost::asio::awaitable<void> on_startup() override {
      ++_state->started;
      event(_state, "startup");
      if (_state->mode == "startup-failure") {
         throw std::runtime_error{"synthetic startup failure"};
      }
      co_return;
   }
   boost::asio::awaitable<void> on_shutdown() override {
      if (_state->mode == "delayed" || _state->mode == "external-delayed") {
         // The runner's 20 ms timer starts before this hook. This timer proves
         // the deadline elapsed; the separate release barrier still holds cleanup.
         auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor, 60ms};
         co_await timer.async_wait(boost::asio::use_awaitable);
         report("CLEANUP_HELD", _state);
         _state->held_signal.set_value();
         co_await _state->wait_release();
      }
      event(_state, "cleanup.final");
      ++_state->cleaned;
      if (_state->mode == "shutdown-failure") {
         throw std::runtime_error{"synthetic shutdown failure"};
      }
   }

 private:
   std::shared_ptr<test_state> _state;
};

class test_application final : public forge::app::application_shell {
 public:
   test_application(forge::app::application_shell_options options, std::shared_ptr<test_state> state)
       : application_shell{std::move(options)}, _state{std::move(state)} {}
   ~test_application() override {
      event(_state, "app.destroy");
      ++_state->destroyed;
      _state->destroyed_signal.set_value();
   }

 protected:
   boost::asio::awaitable<void> on_configure(forge::app::configure_context&) override {
      ++_state->configured;
      event(_state, "configure");
      if (_state->mode == "config-failure") {
         throw std::runtime_error{"synthetic configuration failure"};
      }
      co_return;
   }
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin({.id = {"fixture"}, .factory = [state = _state] {
         return std::make_unique<test_plugin>(state);
      }});
   }
   int on_run_foreground() override {
      event(_state, "foreground");
      return _state->mode == "nonzero" || _state->mode == "timeout-nonzero" ? 17 : 0;
   }

 private:
   std::shared_ptr<test_state> _state;
};

forge::app::daemon_factory factory(const std::shared_ptr<test_state>& state) {
   // One shared_ptr keeps this callable small enough to exercise std::function
   // implementations that retain small callable captures after moving.
   return [marker = std::make_shared<capture_marker>(state, true)](const forge::app::daemon_context& context) {
      return std::make_unique<test_application>(context.shell, marker->state);
   };
}

forge::app::daemon_options settings(const std::shared_ptr<test_state>& state, bool external) {
   auto options = forge::app::daemon_options{.name = "fixture", .read_yaml = false,
                                             .read_dotenv = false, .read_process_env = false};
   options.shutdown_logging = !external;
   options.logging_shutdown_timeout = state->mode.starts_with("timeout") ? 40ms : 5s;
   if (state->mode == "timeout-zero") {
      options.logging_shutdown_timeout = 0ms;
   }
   options.run.handle_sigint = false;
   options.run.handle_sigterm = false;
   options.run.shutdown_timeout = state->mode == "delayed" || state->mode == "external-delayed" ? 20ms : 2s;
   if (state->mode != "nonzero" && state->mode != "timeout-nonzero") {
      options.run.wait_for_stop = [marker = std::make_shared<capture_marker>(state, false)](
                                      forge::app::application_shell&) -> boost::asio::awaitable<void> {
         event(marker->state, "waiter.run");
         if (marker->state->mode == "nonstd" || marker->state->mode == "timeout-nonstd") {
            throw nonstandard_failure{};
         }
         co_return;
      };
   }
   return options;
}

int invoke(const std::shared_ptr<test_state>& state, const std::filesystem::path& directory, bool external = false) {
   auto args = std::vector<std::string>{"fixture", "--data-dir", directory.string(), "--runtime-threads", "1"};
   if (state->mode == "help") { args.push_back("--help"); }
   if (state->mode == "check") { args.push_back("--check-config"); }
   if (state->mode == "print") { args.push_back("--print-effective-config"); }
   if (state->mode == "generate") { args.push_back("--configure"); }
   if (state->mode == "invalid-cli") { args.push_back("--runtime-threads=invalid"); }
   if (state->mode == "delayed" || state->mode == "external-delayed") {
      args.push_back("--shutdown-timeout-ms=20");
   }
   auto argv = std::vector<char*>{};
   for (auto& arg : args) { argv.push_back(arg.data()); }
   // Pass prvalues: no test-owned moved-from factory/options may retain markers.
   return forge::app::run_daemon(factory(state), static_cast<int>(argv.size()), argv.data(), settings(state, external));
}

void configure_output(const std::filesystem::path& path, bool blocked) {
   auto config = forge::logging_config{};
   config.sinks.push_back({.name = "output", .type = blocked ? "console" : "jsonl",
       .args = blocked ? forge::variant{forge::mutable_variant_object{}("stream", "std_error")}
                       : forge::variant{forge::mutable_variant_object{}("path", path.string())("append", false)}});
   auto route = forge::logger_config{"default"};
   route.level = forge::log_level::debug;
   route.sinks = {"output"};
   config.loggers.push_back(route);
   require(forge::configure_logging(config), "fixture logging configuration failed");
   ilog("fixture", "seed");
}

void drain_logging() {
   auto runtime = forge::asio::runtime{};
   require(forge::asio::blocking::run_for(runtime, []() -> boost::asio::awaitable<void> {
      require(co_await forge::async_shutdown_logging(5s), "explicit fixture drain failed");
   }(), 6s), "explicit fixture drain did not return");
   const auto metrics = forge::logging_delivery_metrics();
   require(metrics.closed && metrics.queued_records == 0 && metrics.queued_bytes == 0,
           "explicit fixture drain left pending records");
}

int owning_case(const std::shared_ptr<test_state>& state, const std::filesystem::path& directory) {
   const auto blocked = state->mode.starts_with("timeout");
   if (blocked) {
      for (int item = 0; item < 20000; ++item) {
         ilog("fixture", std::string(1024, 'x'));
      }
      auto runtime = forge::asio::runtime{};
      require(!forge::asio::blocking::run(runtime, forge::async_flush_logging(20ms)), "OS output did not block");
      require(forge::logging_delivery_metrics().queued_records > 0, "blocked output has no pending records");
   }
   const auto start = std::chrono::steady_clock::now();
   int result = -1;
   bool threw = false;
   try {
      result = invoke(state, directory);
   } catch (const nonstandard_failure& error) {
      require(error.value == 73, "original nonstandard exception changed");
      threw = true;
   }
   const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
   report("DAEMON_RETURN", state, result, threw, elapsed);
   require(!state->premature_close.load(), "logging closed before a final producer");
   require(state->factory_destroyed == 1, "factory capture outlived daemon drain");
   require(state->waiter_destroyed == (state->mode == "nonzero" || state->mode == "timeout-nonzero" ? 0 : 1),
           "waiter capture outlived daemon drain");
   const auto metrics = forge::logging_delivery_metrics();
   require(metrics.closed, "owning daemon did not close configured logging");
   if (blocked) {
      require(metrics.queued_records > 0 && metrics.accepted > metrics.delivered + metrics.failed,
              "logging timeout falsely reported completed work");
      require(elapsed < 3000, "logging timeout did not bound normal-body return");
      require(threw || result == (state->mode == "timeout-nonzero" ? 17 : 1), "logging timeout lost failure result");
      require(read_command() == "drain", "expected parent to release OS output");
      drain_logging();
      report("RECOVERED", state, result, threw);
   } else {
      require(metrics.queued_records == 0 && metrics.queued_bytes == 0 && metrics.failed == 0 &&
              metrics.dropped == 0 && metrics.accepted == metrics.delivered,
              "owning daemon returned before successful delivery");
   }
   return threw ? 0 : result;
}

std::shared_ptr<test_state> make_state(std::string mode, std::string id = "main") {
   auto state = std::make_shared<test_state>();
   state->mode = std::move(mode);
   state->id = std::move(id);
   return state;
}

int interactive_case(const std::shared_ptr<test_state>& state, const std::filesystem::path& directory) {
   auto command = std::packaged_task<void(std::stop_token)>{[state](std::stop_token stop) {
      auto release = boost::scope::scope_exit{[state] { state->unblock(); }};
      const auto value = read_command(stop);
      require(value == "release" || (value.empty() && stop.stop_requested()), "expected release command");
   }};
   auto command_done = command.get_future();
   auto reader = std::jthread{std::move(command)};
   auto cleanup = boost::scope::scope_exit{[&] { state->unblock(); reader.request_stop(); }};
   int result = 0;
   if (state->mode == "delayed") {
      result = owning_case(state, directory);
      require(result == 1 && state->cleaned == 1 && state->destroyed == 1,
              "owning app timeout left an asynchronous cleanup tail");
   } else {
      result = invoke(state, directory, true);
      require(state->held.wait_for(3s) == std::future_status::ready, "external cleanup never reached its barrier");
      require(result == 1 && state->cleaned == 0 && state->destroyed == 0,
              "external daemon lost its existing cleanup tail behavior");
      auto second = make_state("normal", "second");
      require(invoke(second, directory, true) == 0, "second external daemon could not run");
      require(!forge::logging_delivery_metrics().closed && second->destroyed == 1,
              "one external daemon closed logging owned by another host");
      event(second, "external.after.second");
      report("EXTERNAL_RETURN", state, result);
      require(state->destruction.wait_for(3s) == std::future_status::ready, "external cleanup did not finish after release");
      require(state->cleaned == 1 && !state->premature_close && !second->premature_close,
              "external cleanup logged after premature global shutdown");
      event(state, "external.after.tail");
      drain_logging();
      report("FINAL", state, result);
      result = 0;
   }
   reader.request_stop();
   reader.join();
   command_done.get();
   return result;
}

std::unique_ptr<test_application> embedded_app(const std::shared_ptr<test_state>& state) {
   return std::make_unique<test_application>(forge::app::application_shell_options{
       .name = state->id, .runtime = {.worker_threads = 1}}, state);
}

forge::app::run_options embedded_options() {
   return {.handle_sigint = false, .handle_sigterm = false, .shutdown_timeout = 2s,
           .wait_for_stop = [](forge::app::application_shell&) -> boost::asio::awaitable<void> { co_return; }};
}

int embedded_case(const std::shared_ptr<test_state>& state) {
   {
      auto app = embedded_app(state);
      const auto result = state->mode == "runner-owned"
          ? forge::app::run_application(std::move(app), {}, embedded_options())
          : forge::app::run_application(*app, {}, embedded_options());
      require(result == 0 && !forge::logging_delivery_metrics().closed, "embedded runner closed global logging");
   }
   require(!forge::logging_delivery_metrics().closed && state->destroyed == 1,
           "shell destruction closed global logging");
   event(state, "embedded.after.runner");
   report("EMBEDDED_OPEN", state);
   drain_logging();
   report("FINAL", state);
   return 0;
}

int concurrent_case(const std::shared_ptr<test_state>& first_state) {
   auto second_state = make_state("normal", "second");
   auto first_app = embedded_app(first_state);
   auto second_app = embedded_app(second_state);
   const auto run = [](test_application& app, const std::shared_ptr<test_state>& state) {
      auto options = embedded_options();
      options.wait_for_stop = [state](forge::app::application_shell&) -> boost::asio::awaitable<void> {
         state->foreground_signal.set_value();
         co_await state->wait_release();
      };
      return forge::app::run_application(app, {}, std::move(options));
   };
   auto first_task = std::packaged_task<int()>{[&] { return run(*first_app, first_state); }};
   auto second_task = std::packaged_task<int()>{[&] { return run(*second_app, second_state); }};
   auto first = first_task.get_future();
   auto second = second_task.get_future();
   auto first_thread = std::jthread{};
   auto second_thread = std::jthread{};
   auto join_release = boost::scope::scope_exit{[&] { first_state->unblock(); second_state->unblock(); }};
   // Even partial thread construction releases both barriers before joining.
   first_thread = std::jthread{std::move(first_task)};
   second_thread = std::jthread{std::move(second_task)};
   require(first_state->foreground.wait_for(3s) == std::future_status::ready &&
           second_state->foreground.wait_for(3s) == std::future_status::ready, "concurrent shells did not both start");
   first_state->unblock();
   require(first.wait_for(3s) == std::future_status::ready && first.get() == 0, "first shell failed to stop");
   first_thread.join();
   first_app.reset();
   require(!forge::logging_delivery_metrics().closed && second_state->cleaned == 0 &&
           second_app->state() == forge::app::lifecycle_phase::started, "first shell stopped another process producer");
   event(second_state, "concurrent.still.running");
   second_state->unblock();
   require(second.wait_for(3s) == std::future_status::ready && second.get() == 0, "second shell failed to stop");
   second_thread.join();
   second_app.reset();
   auto direct_state = make_state("normal", "direct");
   {
      auto direct = embedded_app(direct_state);
      direct->configure({});
      forge::asio::blocking::run(direct->runtime(), direct->startup());
      forge::asio::blocking::run(direct->runtime(), direct->shutdown());
      require(!forge::logging_delivery_metrics().closed, "direct shell shutdown closed global logging");
   }
   require(!first_state->premature_close && !second_state->premature_close && !direct_state->premature_close,
           "an embedded producer outlived process logging");
   report("EMBEDDED_OPEN", first_state);
   drain_logging();
   report("FINAL", first_state);
   return 0;
}

} // namespace

int main(int argc, char** argv) {
   try {
      require(argc == 3, "expected mode and log path");
      const auto state = make_state(argv[1]);
      const auto path = std::filesystem::path{argv[2]};
      configure_output(path, state->mode.starts_with("timeout"));
      if (state->mode == "delayed" || state->mode == "external-delayed") {
         return interactive_case(state, path.parent_path());
      }
      if (state->mode == "runner-reference" || state->mode == "runner-owned") {
         return embedded_case(state);
      }
      if (state->mode == "concurrent") {
         return concurrent_case(state);
      }
      return owning_case(state, path.parent_path());
   } catch (const std::exception& error) {
      std::cout << "@@DAEMON_LOG ERROR: " << error.what() << '\n' << std::flush;
      return 90;
   } catch (...) {
      std::cout << "@@DAEMON_LOG ERROR: unexpected exception\n" << std::flush;
      return 91;
   }
}
