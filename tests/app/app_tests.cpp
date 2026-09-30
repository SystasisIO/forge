#include <boost/test/unit_test.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/describe.hpp>
#include <forge/api/core/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

import forge.app.exceptions;
import forge.app.application;
import forge.app.events;
import forge.app.diagnostics;
import forge.app.signals;
import forge.app.service_registry;
import forge.app.connect_context;
import forge.app.plugin_context;
import forge.app.plugin;
import forge.app.plugin_registry;
import forge.app.application_shell;
import forge.app.application_builder;
import forge.app.runner;
import forge.app.daemon;
import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.asio.blocking;
import forge.asio.compute;
import forge.asio.runtime;
import forge.asio.task;
import forge.config.core.key_path;
import forge.config.core.value;
import forge.config.core.document;
import forge.config.core.component;
import forge.config.core.decode;
import forge.config.core.migration;
import forge.schema.diagnostic;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.enums;
import forge.schema.exceptions;

namespace app_test_contract {

class sample_api : public forge::api::core::contract<sample_api> {
 public:
   virtual ~sample_api() = default;
   virtual boost::asio::awaitable<int> value(int input) = 0;
};

} // namespace app_test_contract

FORGE_API(::app_test_contract::sample_api, FORGE_API_CONTRACT("sample", 1, 0), FORGE_API_METHOD(value))

namespace {

using app_test_contract::sample_api;

template <typename Context>
concept exposes_api_installer = requires(Context& context) {
   context.apis();
};

static_assert(!exposes_api_installer<const forge::app::application_context>);

struct lifecycle_log {
   std::vector<std::string> entries;
};

struct connected_service {
   connected_service() = default;
   connected_service(int input, lifecycle_log* lifecycle = nullptr) : value{input}, log{lifecycle} {}

   int value = 0;
   lifecycle_log* log = nullptr;

   ~connected_service() {
      if (log != nullptr) {
         log->entries.push_back("service.destroy");
      }
   }
};

struct auxiliary_service {
   int value = 0;
};

struct reentrant_service {
   reentrant_service(forge::app::service_view value, std::atomic_bool* signal)
       : services{std::move(value)}, destroyed{signal} {}

   forge::app::service_view services;
   std::atomic_bool* destroyed = nullptr;

   ~reentrant_service() {
      static_cast<void>(services.try_get<auxiliary_service>());
      destroyed->store(true, std::memory_order_release);
   }
};

class sample_api_impl final : public sample_api {
 public:
   explicit sample_api_impl(int offset = 1) : offset_{offset} {}

   boost::asio::awaitable<int> value(int input) override {
      co_return input + offset_;
   }

 private:
   int offset_ = 1;
};

class sample_api_consumer_plugin final : public forge::app::plugin {
 public:
   sample_api_consumer_plugin(lifecycle_log& log) : log_{&log} {}

   forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "api-consumer"};
   }

   std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      api_ = context.apis().get<sample_api>({.id = {"sample"}, .major = 1});
      log_->entries.push_back("api.initialize:" + std::to_string(static_cast<bool>(api_)));
      co_return;
   }

   boost::asio::awaitable<void> startup() override {
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      co_return;
   }

 private:
   lifecycle_log* log_ = nullptr;
   forge::api::core::handle<sample_api> api_;
};

struct slow_shutdown_state {
   mutable std::mutex mutex;
   std::condition_variable ready;
   bool shutdown_done = false;
   bool destroyed = false;

   void mark_shutdown_done() {
      {
         const auto lock = std::scoped_lock{mutex};
         shutdown_done = true;
      }
      ready.notify_all();
   }

   void mark_destroyed() {
      {
         const auto lock = std::scoped_lock{mutex};
         destroyed = true;
      }
      ready.notify_all();
   }

   bool shutdown_finished() const {
      const auto lock = std::scoped_lock{mutex};
      return shutdown_done;
   }

   bool shell_destroyed() const {
      const auto lock = std::scoped_lock{mutex};
      return destroyed;
   }

   bool wait_for_shutdown(std::chrono::milliseconds timeout) {
      auto lock = std::unique_lock{mutex};
      return ready.wait_for(lock, timeout, [&] {
         return shutdown_done;
      });
   }

   bool wait_for_destroyed(std::chrono::milliseconds timeout) {
      auto lock = std::unique_lock{mutex};
      return ready.wait_for(lock, timeout, [&] {
         return destroyed;
      });
   }
};

struct shell_service_config {
   std::uint16_t workers = 2;
};

BOOST_DESCRIBE_STRUCT(shell_service_config, (), (workers))

struct shell_plugin_config {
   std::uint16_t port = 9000;
};

BOOST_DESCRIBE_STRUCT(shell_plugin_config, (), (port))

struct daemon_service_config {
   std::uint16_t workers = 2;
   std::string token;
};

BOOST_DESCRIBE_STRUCT(daemon_service_config, (), (workers, token))

class test_plugin final : public forge::app::plugin {
 public:
   test_plugin(std::string id, lifecycle_log& log, bool fail_startup = false)
       : id_{std::move(id)}, log_{&log}, fail_startup_{fail_startup} {}

   forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = id_};
   }
   std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context&) override {
      log_->entries.push_back("initialize:" + id_);
      co_return;
   }

   boost::asio::awaitable<void> startup() override {
      log_->entries.push_back("startup:" + id_);
      if (fail_startup_) {
         throw forge::app::exceptions::startup_failed{"startup failed"};
      }
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      log_->entries.push_back("shutdown:" + id_);
      co_return;
   }

 private:
   std::string id_;
   lifecycle_log* log_ = nullptr;
   bool fail_startup_ = false;
};

class configurable_plugin final : public forge::app::plugin {
 public:
   configurable_plugin(lifecycle_log& log) : log_{&log} {}

   forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "configurable"};
   }
   std::string version() const override {
      return "1";
   }

   std::optional<forge::config::core::component_descriptor> describe_config() const override {
      return forge::config::core::component_descriptor{
          .section = "http",
          .fields = {forge::config::core::field_descriptor{
              .name = "bind-port",
              .kind = forge::schema::value_kind::unsigned_integer,
              .required = true,
          }},
      };
   }

   boost::asio::awaitable<void> configure(forge::config::core::component_view view) override {
      bind_port_ = view.get_or<std::uint16_t>("bind-port", 0);
      log_->entries.push_back("configure:" + std::to_string(bind_port_));
      co_return;
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context&) override {
      if (bind_port_ == 0) {
         throw forge::app::exceptions::config_failed{"not configured"};
      }
      log_->entries.push_back("initialize:configurable");
      co_return;
   }

   boost::asio::awaitable<void> startup() override {
      log_->entries.push_back("startup:configurable");
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      log_->entries.push_back("shutdown:configurable");
      co_return;
   }

 private:
   lifecycle_log* log_ = nullptr;
   std::uint16_t bind_port_ = 0;
};

forge::app::plugin_descriptor descriptor(std::string id, lifecycle_log& log,
                                       std::vector<forge::app::plugin_id> dependencies = {},
                                       bool enabled_by_default = true, bool fail_startup = false) {
   auto id_value = forge::app::plugin_id{.value = std::move(id)};
   auto id_copy = id_value;
   return forge::app::plugin_descriptor{
       .id = std::move(id_value),
       .dependencies = std::move(dependencies),
       .enabled_by_default = enabled_by_default,
       .factory = [id_copy, &log,
                   fail_startup] { return std::make_unique<test_plugin>(id_copy.value, log, fail_startup); },
   };
}

} // namespace

template <>
struct forge::schema::rules<shell_service_config> {
   static forge::schema::object_schema<shell_service_config> define() {
      auto schema = forge::schema::object<shell_service_config>();
      schema.field<&shell_service_config::workers>("workers").default_value(2).range(1, 32);
      return schema;
   }
};

template <>
struct forge::schema::rules<shell_plugin_config> {
   static forge::schema::object_schema<shell_plugin_config> define() {
      auto schema = forge::schema::object<shell_plugin_config>();
      schema.field<&shell_plugin_config::port>("port").default_value(9000).range(1, 65'535);
      return schema;
   }
};

template <>
struct forge::schema::rules<daemon_service_config> {
   static forge::schema::object_schema<daemon_service_config> define() {
      auto schema = forge::schema::object<daemon_service_config>();
      schema.field<&daemon_service_config::workers>("workers").default_value(2).range(1, 32);
      schema.field<&daemon_service_config::token>("token").default_value(std::string{}).secret();
      return schema;
   }
};

namespace {

struct stream_capture {
   explicit stream_capture(std::ostream& stream) : stream_{stream}, old_{stream.rdbuf(buffer_.rdbuf())} {}

   ~stream_capture() {
      stream_.rdbuf(old_);
   }

   std::string text() const {
      return buffer_.str();
   }

 private:
   std::ostream& stream_;
   std::ostringstream buffer_;
   std::streambuf* old_ = nullptr;
};

std::filesystem::path make_temp_dir(std::string name) {
   const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
   auto path = std::filesystem::temp_directory_path() / (std::move(name) + "-" + std::to_string(stamp));
   std::filesystem::create_directories(path);
   return path;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
   std::filesystem::create_directories(path.parent_path());
   auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
   output << text;
}

std::string read_text(const std::filesystem::path& path) {
   auto input = std::ifstream{path, std::ios::binary};
   return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

class env_var_guard {
 public:
   env_var_guard(std::string name, std::string value) : name_{std::move(name)} {
      if (const auto* current = std::getenv(name_.c_str()); current != nullptr) {
         old_value_ = std::string{current};
      }
      set(value);
   }

   ~env_var_guard() {
      if (old_value_) {
         set(*old_value_);
      } else {
         unset();
      }
   }

 private:
   void set(const std::string& value) const {
#if defined(_WIN32)
      _putenv_s(name_.c_str(), value.c_str());
#else
      setenv(name_.c_str(), value.c_str(), 1);
#endif
   }

   void unset() const {
#if defined(_WIN32)
      _putenv_s(name_.c_str(), "");
#else
      unsetenv(name_.c_str());
#endif
   }

   std::string name_;
   std::optional<std::string> old_value_;
};

class shell_config_plugin final : public forge::app::plugin {
 public:
   shell_config_plugin(lifecycle_log& log) : log_{&log} {}

   forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "http"};
   }

   std::string version() const override {
      return "1";
   }

   std::optional<forge::config::core::component_descriptor> describe_config() const override {
      auto descriptor = forge::config::core::describe_component<shell_plugin_config>("http");
      descriptor.fields.push_back(forge::config::core::field_descriptor{
         .name = "removed-output",
         .kind = forge::schema::value_kind::string,
         .deprecated = true,
         .deprecated_message = "http removed-output was removed",
         .ingestion_only = true,
      });
      return descriptor;
   }

   boost::asio::awaitable<void> configure(forge::config::core::component_view view) override {
      port_ = view.get_or<std::uint16_t>("port", 0);
      log_->entries.push_back("plugin.configure:" + std::to_string(port_));
      co_return;
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      log_->entries.push_back("plugin.initialize:" + std::to_string(port_));
      context.events().publish(forge::app::event_severity::info, "shell.plugin", "initialized");
      co_return;
   }

   boost::asio::awaitable<void> after_initialize() override {
      log_->entries.push_back("plugin.after_initialize:" + std::to_string(port_));
      co_return;
   }

   boost::asio::awaitable<void> startup() override {
      log_->entries.push_back("plugin.startup");
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      log_->entries.push_back("plugin.shutdown");
      co_return;
   }

 private:
   lifecycle_log* log_ = nullptr;
   std::uint16_t port_ = 0;
};

class shell_dependency_plugin final : public forge::app::plugin {
 public:
   shell_dependency_plugin(std::string id, lifecycle_log& log, bool fail_shutdown_once = false)
       : id_{std::move(id)}, log_{&log}, fail_shutdown_once_{fail_shutdown_once} {}

   forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = id_};
   }

   std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context&) override {
      log_->entries.push_back("initialize:" + id_);
      co_return;
   }

   boost::asio::awaitable<void> after_initialize() override {
      log_->entries.push_back("after_initialize:" + id_);
      co_return;
   }

   boost::asio::awaitable<void> startup() override {
      log_->entries.push_back("startup:" + id_);
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      log_->entries.push_back("shutdown:" + id_);
      if (fail_shutdown_once_) {
         fail_shutdown_once_ = false;
         throw std::runtime_error{"dependent shutdown failed"};
      }
      co_return;
   }

 private:
   std::string id_;
   lifecycle_log* log_ = nullptr;
   bool fail_shutdown_once_ = false;
};

class connected_service_plugin final : public forge::app::plugin {
 public:
   explicit connected_service_plugin(lifecycle_log& log) : log_{&log} {}

   forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "connected-service"};
   }

   std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      services_ = context.services();
      initialized_ = true;
      log_->entries.push_back("plugin.initialize");
      co_return;
   }

   boost::asio::awaitable<void> after_initialize() override {
      log_->entries.push_back("plugin.after_initialize");
      co_return;
   }

   boost::asio::awaitable<void> startup() override {
      const auto service = services_.get<connected_service>();
      log_->entries.push_back("plugin.startup:" + std::to_string(service->value));
      co_return;
   }

   void request_stop() noexcept override {
      if (initialized_) {
         log_->entries.push_back("plugin.request_stop");
      }
   }

   boost::asio::awaitable<void> shutdown() override {
      const auto service = services_.get<connected_service>();
      log_->entries.push_back("plugin.shutdown:" + std::to_string(service->value));
      co_return;
   }

 private:
   lifecycle_log* log_ = nullptr;
   forge::app::service_view services_;
   bool initialized_ = false;
};

class scheduler_cleanup_plugin final : public forge::app::plugin {
 public:
   explicit scheduler_cleanup_plugin(lifecycle_log& log) : log_{&log} {}

   forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "cleanup"};
   }

   std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      scheduler_ = &context.scheduler();
      log_->entries.push_back("initialize:cleanup");
      co_return;
   }

   boost::asio::awaitable<void> startup() override {
      log_->entries.push_back("startup:cleanup");
      co_return;
   }

   void request_stop() noexcept override {
      log_->entries.push_back("request_stop:cleanup");
   }

   boost::asio::awaitable<void> shutdown() override {
      auto handle = scheduler_->submit(forge::asio::task::task{
         .priority = forge::asio::task::priority{1},
         .name = "cleanup-flush",
         .work = [this] {
            log_->entries.push_back("cleanup.scheduler.work");
         },
      });
      co_await handle.wait();
      log_->entries.push_back("shutdown:cleanup");
   }

 private:
   lifecycle_log* log_ = nullptr;
   forge::asio::task::scheduler* scheduler_ = nullptr;
};

class compute_cleanup_plugin final : public forge::app::plugin {
 public:
   explicit compute_cleanup_plugin(lifecycle_log& log) : log_{&log} {}

   forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "compute-cleanup"};
   }

   std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      BOOST_REQUIRE(context.has_compute());
      compute_ = context.compute();
      log_->entries.push_back("initialize:compute-cleanup");
      co_return;
   }

   boost::asio::awaitable<void> startup() override {
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      co_await compute_.execute({.name = "plugin-finalize"},
                                [this] { log_->entries.push_back("cleanup.compute.work"); });
      log_->entries.push_back("shutdown:compute-cleanup");
   }

 private:
   lifecycle_log* log_ = nullptr;
   forge::asio::compute::executor compute_;
};

class failing_initialize_plugin final : public forge::app::plugin {
 public:
   explicit failing_initialize_plugin(lifecycle_log& log) : log_{&log} {}

   forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "init-fail"};
   }

   std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context&) override {
      log_->entries.push_back("initialize:init-fail");
      throw forge::app::exceptions::initialize_failed{"initialize failed"};
   }

   boost::asio::awaitable<void> startup() override {
      log_->entries.push_back("startup:init-fail");
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      log_->entries.push_back("shutdown:init-fail");
      co_return;
   }

 private:
   lifecycle_log* log_ = nullptr;
};

class failing_after_initialize_plugin final : public forge::app::plugin {
 public:
   explicit failing_after_initialize_plugin(lifecycle_log& log) : log_{&log} {}

   forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "after-init-fail"};
   }

   std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context&) override {
      initialized_ = true;
      log_->entries.push_back("initialize:after-init-fail");
      co_return;
   }

   boost::asio::awaitable<void> after_initialize() override {
      log_->entries.push_back("after_initialize:after-init-fail");
      throw forge::app::exceptions::initialize_failed{"after initialize failed"};
   }

   boost::asio::awaitable<void> startup() override {
      log_->entries.push_back("startup:after-init-fail");
      co_return;
   }

   void request_stop() noexcept override {
      if (initialized_) {
         log_->entries.push_back("request_stop:after-init-fail");
      }
   }

   boost::asio::awaitable<void> shutdown() override {
      log_->entries.push_back("shutdown:after-init-fail");
      co_return;
   }

 private:
   lifecycle_log* log_ = nullptr;
   bool initialized_ = false;
};

class slow_shutdown_plugin final : public forge::app::plugin {
 public:
   slow_shutdown_plugin(lifecycle_log& log, std::shared_ptr<slow_shutdown_state> state)
       : log_{&log}, state_{std::move(state)} {}

   forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "slow"};
   }

   std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      context_ = &context;
      log_->entries.push_back("initialize:slow");
      co_return;
   }

   boost::asio::awaitable<void> startup() override {
      log_->entries.push_back("startup:slow");
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      auto timer = boost::asio::steady_timer{context_->scheduler().runtime_context().context()};
      timer.expires_after(std::chrono::milliseconds{100});
      co_await timer.async_wait(boost::asio::use_awaitable);
      log_->entries.push_back("shutdown:slow");
      state_->mark_shutdown_done();
   }

 private:
   lifecycle_log* log_ = nullptr;
   std::shared_ptr<slow_shutdown_state> state_;
   forge::app::plugin_context* context_ = nullptr;
};

class shell_test_application final : public forge::app::application_shell {
 public:
   explicit shell_test_application(lifecycle_log& log)
       : forge::app::application_shell{forge::app::application_shell_options{
            .name = "shell-test",
            .runtime = {.worker_threads = 1, .thread_name = "shell-test"},
         }},
         log_{&log} {}

   int run_count() {
      return run();
   }

   std::uint16_t workers() const noexcept {
      return workers_;
   }

 protected:
   void on_describe_config(forge::config::core::component_registry& registry) const override {
      registry.add(forge::config::core::describe_component<shell_service_config>("service"));
   }

   boost::asio::awaitable<void> on_configure(forge::app::configure_context& context) override {
      workers_ = context.view("service").get_or<std::uint16_t>("workers", 0);
      log_->entries.push_back("app.configure:" + std::to_string(workers_));
      co_return;
   }

   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(forge::app::plugin_descriptor{
         .id = forge::app::plugin_id{.value = "http"},
         .factory = [this] {
            return std::make_unique<shell_config_plugin>(*log_);
         },
      });
   }

   boost::asio::awaitable<void> on_provide(forge::app::application_context& context) override {
      context.apis().install<sample_api>(sample_api::describe(), std::make_shared<sample_api_impl>(workers_));
      log_->entries.push_back("app.provide");
      co_return;
   }

   int on_run_foreground() override {
      log_->entries.push_back("app.run");
      request_stop();
      return 7;
   }

 private:
   lifecycle_log* log_ = nullptr;
   std::uint16_t workers_ = 0;
};

class shell_api_application final : public forge::app::application_shell {
 public:
   explicit shell_api_application(lifecycle_log& log) : log_{&log} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(forge::app::plugin_descriptor{
         .id = forge::app::plugin_id{.value = "api-consumer"},
         .factory = [this] {
            return std::make_unique<sample_api_consumer_plugin>(*log_);
         },
      });
   }

   boost::asio::awaitable<void> on_provide(forge::app::application_context& context) override {
      context.apis().install<sample_api>(sample_api::describe(), std::make_shared<sample_api_impl>());
      log_->entries.push_back("app.provide");
      co_return;
   }

 private:
   lifecycle_log* log_ = nullptr;
};

class shell_order_application final : public forge::app::application_shell {
 public:
   explicit shell_order_application(lifecycle_log& log) : log_{&log} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(forge::app::plugin_descriptor{
         .id = forge::app::plugin_id{.value = "store"},
         .factory = [this] {
            return std::make_unique<shell_dependency_plugin>("store", *log_);
         },
      });
      registry.register_plugin(forge::app::plugin_descriptor{
         .id = forge::app::plugin_id{.value = "api"},
         .dependencies = {forge::app::plugin_id{.value = "store"}},
         .factory = [this] {
            return std::make_unique<shell_dependency_plugin>("api", *log_);
         },
      });
   }

 private:
   lifecycle_log* log_ = nullptr;
};

class shell_shutdown_retry_application final : public forge::app::application_shell {
 public:
   explicit shell_shutdown_retry_application(lifecycle_log& log) : log_{&log} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(forge::app::plugin_descriptor{
         .id = forge::app::plugin_id{.value = "store"},
         .factory = [this] {
            return std::make_unique<shell_dependency_plugin>("store", *log_);
         },
      });
      registry.register_plugin(forge::app::plugin_descriptor{
         .id = forge::app::plugin_id{.value = "api"},
         .dependencies = {forge::app::plugin_id{.value = "store"}},
         .factory = [this] {
            return std::make_unique<shell_dependency_plugin>("api", *log_, true);
         },
      });
   }

 private:
   lifecycle_log* log_ = nullptr;
};

class shell_failure_application final : public forge::app::application_shell {
 public:
   explicit shell_failure_application(lifecycle_log& log) : log_{&log} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(descriptor("store", *log_));
      registry.register_plugin(descriptor(
         "api",
         *log_,
         {forge::app::plugin_id{.value = "store"}},
         true,
         true));
   }

 private:
   lifecycle_log* log_ = nullptr;
};

class shell_initialize_failure_application final : public forge::app::application_shell {
 public:
   explicit shell_initialize_failure_application(lifecycle_log& log) : log_{&log} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(forge::app::plugin_descriptor{
         .id = forge::app::plugin_id{.value = "init-fail"},
         .factory = [this] {
            return std::make_unique<failing_initialize_plugin>(*log_);
         },
      });
   }

 private:
   lifecycle_log* log_ = nullptr;
};

class shell_after_initialize_failure_application final : public forge::app::application_shell {
 public:
   explicit shell_after_initialize_failure_application(lifecycle_log& log) : log_{&log} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(forge::app::plugin_descriptor{
         .id = forge::app::plugin_id{.value = "after-init-fail"},
         .factory = [this] {
            return std::make_unique<failing_after_initialize_plugin>(*log_);
         },
      });
   }

 private:
   lifecycle_log* log_ = nullptr;
};

class shell_scheduler_cleanup_application final : public forge::app::application_shell {
 public:
   explicit shell_scheduler_cleanup_application(lifecycle_log& log) : log_{&log} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(forge::app::plugin_descriptor{
         .id = forge::app::plugin_id{.value = "cleanup"},
         .factory = [this] {
            return std::make_unique<scheduler_cleanup_plugin>(*log_);
         },
      });
   }

 private:
   lifecycle_log* log_ = nullptr;
};

class shell_compute_cleanup_application final : public forge::app::application_shell {
 public:
   explicit shell_compute_cleanup_application(lifecycle_log& log)
       : forge::app::application_shell{forge::app::application_shell_options{
            .runtime = {.worker_threads = 1, .thread_name = "compute-app-test"},
            .compute = forge::asio::compute::pool::options{
               .worker_threads = 1,
               .max_pending_tasks = 2,
            },
         }},
         log_{&log} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(forge::app::plugin_descriptor{
         .id = forge::app::plugin_id{.value = "compute-cleanup"},
         .factory = [this] {
            return std::make_unique<compute_cleanup_plugin>(*log_);
         },
      });
   }

 private:
   lifecycle_log* log_ = nullptr;
};

class shell_slow_shutdown_application final : public forge::app::application_shell {
 public:
   shell_slow_shutdown_application(lifecycle_log& log, std::shared_ptr<slow_shutdown_state> state)
       : forge::app::application_shell{forge::app::application_shell_options{
            .name = "slow-shutdown",
            .runtime = {.worker_threads = 1, .thread_name = "slow-shutdown"},
         }},
         log_{&log},
         state_{std::move(state)} {}

   ~shell_slow_shutdown_application() override {
      state_->mark_destroyed();
   }

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(forge::app::plugin_descriptor{
         .id = forge::app::plugin_id{.value = "slow"},
         .factory = [this] {
            return std::make_unique<slow_shutdown_plugin>(*log_, state_);
         },
      });
   }

 private:
   lifecycle_log* log_ = nullptr;
   std::shared_ptr<slow_shutdown_state> state_;
};

class shell_selection_application final : public forge::app::application_shell {
 public:
   explicit shell_selection_application(lifecycle_log& log) : log_{&log} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(descriptor("store", *log_));
      registry.register_plugin(descriptor("api", *log_, {forge::app::plugin_id{.value = "store"}}));
      registry.register_plugin(descriptor("metrics", *log_, {}, false));
   }

 private:
   lifecycle_log* log_ = nullptr;
};

class shell_official_plugin_selection_application final : public forge::app::application_shell {
 public:
   explicit shell_official_plugin_selection_application(lifecycle_log& log) : log_{&log} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(descriptor("forge.plugins.net.p2p.node", *log_));
   }

 private:
   lifecycle_log* log_ = nullptr;
};

struct daemon_test_state {
   lifecycle_log log;
   std::uint16_t workers = 0;
   std::string token;
   std::size_t runtime_threads = 0;
   std::size_t blocking_tasks = 0;
   std::size_t queue_depth = 0;
   std::filesystem::path data_dir;
   std::filesystem::path config_path;
   std::filesystem::path dotenv_path;
   std::string profile;
};

class daemon_test_application final : public forge::app::application_shell {
 public:
   daemon_test_application(forge::app::daemon_context context, daemon_test_state& state)
       : forge::app::application_shell{context.shell}, state_{&state} {
      state_->runtime_threads = context.shell.runtime.worker_threads;
      state_->blocking_tasks = context.shell.scheduler.max_blocking_tasks;
      state_->queue_depth = context.shell.scheduler.max_pending_tasks;
      state_->data_dir = context.data_dir;
      state_->config_path = context.config_path;
      state_->dotenv_path = context.dotenv_path;
      state_->profile = context.profile;
   }

 protected:
   void on_describe_config(forge::config::core::component_registry& registry) const override {
      registry.add(forge::config::core::describe_component<daemon_service_config>("service"));
   }

   boost::asio::awaitable<void> on_configure(forge::app::configure_context& context) override {
      auto decoded = forge::config::core::decode<daemon_service_config>(context.document(), "service");
      if (!decoded.ok()) {
         throw std::invalid_argument{decoded.diagnostics.entries.front().message};
      }
      state_->workers = decoded.value.workers;
      state_->token = decoded.value.token;
      state_->log.entries.push_back("app.configure:" + std::to_string(state_->workers));
      co_return;
   }

   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(forge::app::plugin_descriptor{
         .id = forge::app::plugin_id{.value = "http"},
         .factory = [this] {
            return std::make_unique<shell_config_plugin>(state_->log);
         },
      });
   }

   boost::asio::awaitable<void> on_provide(forge::app::application_context& context) override {
      context.apis().install<sample_api>(sample_api::describe(), std::make_shared<sample_api_impl>(state_->workers));
      state_->log.entries.push_back("app.provide");
      co_return;
   }

   int on_run_foreground() override {
      state_->log.entries.push_back("app.run");
      request_stop();
      return 17;
   }

 private:
   daemon_test_state* state_ = nullptr;
};

int run_test_daemon(std::vector<std::string> args, daemon_test_state& state, forge::app::daemon_options options = {},
                    bool read_process_env = false) {
   auto argv = std::vector<char*>{};
   argv.reserve(args.size());
   for (auto& arg : args) {
      argv.push_back(arg.data());
   }
   if (options.name.empty()) {
      options.name = "testd";
   }
   if (options.display_name.empty()) {
      options.display_name = "Test Daemon";
   }
   if (options.default_data_dir_name.empty()) {
      options.default_data_dir_name = "testd";
   }
   if (read_process_env) {
      options.read_process_env = true;
      if (options.env_prefix.empty()) {
         options.env_prefix = "FORGE_TESTD";
      }
   }
   options.run.handle_sigint = false;
   options.run.handle_sigterm = false;
   if (!options.run.wait_for_stop) {
      options.run.wait_for_stop = [](forge::app::application_shell&) -> boost::asio::awaitable<void> {
         co_return;
      };
   }
   return forge::app::run_daemon(
      [&state](const forge::app::daemon_context& context) {
         return std::make_unique<daemon_test_application>(context, state);
      },
      static_cast<int>(argv.size()),
      argv.data(),
      std::move(options));
}

} // namespace

BOOST_AUTO_TEST_CASE(event_bus_bounds_queues_and_keeps_critical_ring) {
   auto bus = forge::app::event_bus{forge::app::event_bus_options{
       .max_subscription_events = 1,
       .max_critical_events = 2,
       .max_recent_events = 3,
   }};
   auto subscription = bus.subscribe(forge::app::event_filter{.topic = "app"});

   bus.publish(forge::app::event_severity::info, "app.start", "first");
   bus.publish(forge::app::event_severity::info, "app.start", "dropped");
   bus.publish(forge::app::event_severity::critical, "system", "critical-a");
   bus.publish(forge::app::event_severity::critical, "system", "critical-b");
   bus.publish(forge::app::event_severity::critical, "system", "critical-c");

   BOOST_TEST(bus.metrics().dropped == 1);
   auto first = subscription.poll();
   BOOST_REQUIRE(first.has_value());
   BOOST_TEST(first->message == "first");
   BOOST_TEST(!subscription.poll().has_value());

   const auto critical = bus.critical_events();
   BOOST_REQUIRE_EQUAL(critical.size(), 2);
   BOOST_TEST(critical.front().message == "critical-b");
   BOOST_TEST(critical.back().message == "critical-c");

   const auto recent = bus.recent_events();
   BOOST_REQUIRE_EQUAL(recent.size(), 3);
   BOOST_TEST(recent.front().message == "critical-a");
   BOOST_TEST(recent.back().message == "critical-c");
}

BOOST_AUTO_TEST_CASE(plugin_registry_orders_dependencies_and_rejects_bad_graphs) {
   auto log = lifecycle_log{};
   auto registry = forge::app::plugin_registry{};
   registry.register_plugin(descriptor("store", log, {}, false));
   registry.register_plugin(descriptor("provider", log, {forge::app::plugin_id{.value = "store"}}));

   auto plugins = registry.instantiate_enabled({});
   BOOST_REQUIRE_EQUAL(plugins.size(), 2);
   BOOST_TEST(plugins[0]->id().value == "store");
   BOOST_TEST(plugins[1]->id().value == "provider");

   BOOST_CHECK_THROW(registry.register_plugin(descriptor("store", log)), std::invalid_argument);

   auto broken = forge::app::plugin_registry{};
   broken.register_plugin(descriptor("broken", log, {forge::app::plugin_id{.value = "missing"}}));
   BOOST_CHECK_THROW(static_cast<void>(broken.instantiate_enabled({})), std::logic_error);

   BOOST_CHECK_THROW(static_cast<void>(registry.instantiate_enabled({forge::app::plugin_config{
                         .id = forge::app::plugin_id{.value = "store"},
                         .enabled = false,
                     }})),
                     std::logic_error);
}

BOOST_AUTO_TEST_CASE(application_runtime_rolls_back_and_shutdown_is_idempotent) {
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto context = forge::app::plugin_context{scheduler, signals, events};
   auto log = lifecycle_log{};

   auto registry = forge::app::plugin_registry{};
   registry.register_plugin(descriptor("a", log));
   registry.register_plugin(descriptor("b", log, {}, true, true));

   auto app = forge::app::application_runtime{context, registry.instantiate_enabled({})};
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, app.startup()), forge::app::exceptions::startup_failed);
   BOOST_CHECK(app.state() == forge::app::application_state::stopped);
   BOOST_REQUIRE_EQUAL(log.entries.size(), 6);
   BOOST_TEST(log.entries[0] == "initialize:a");
   BOOST_TEST(log.entries[1] == "initialize:b");
   BOOST_TEST(log.entries[2] == "startup:a");
   BOOST_TEST(log.entries[3] == "startup:b");
   BOOST_TEST(log.entries[4] == "shutdown:b");
   BOOST_TEST(log.entries[5] == "shutdown:a");

   forge::asio::blocking::run(runtime, app.shutdown());
   forge::asio::blocking::run(runtime, app.shutdown());
}

BOOST_AUTO_TEST_CASE(application_runtime_rejects_startup_after_shutdown) {
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto context = forge::app::plugin_context{scheduler, signals, events};
   auto log = lifecycle_log{};

   auto registry = forge::app::plugin_registry{};
   registry.register_plugin(descriptor("a", log));

   auto app = forge::app::application_runtime{context, registry.instantiate_enabled({})};
   forge::asio::blocking::run(runtime, app.startup());
   forge::asio::blocking::run(runtime, app.shutdown());

   BOOST_CHECK(app.state() == forge::app::application_state::stopped);
   BOOST_REQUIRE_EQUAL(log.entries.size(), 3);
   BOOST_TEST(log.entries[0] == "initialize:a");
   BOOST_TEST(log.entries[1] == "startup:a");
   BOOST_TEST(log.entries[2] == "shutdown:a");

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, app.startup()), std::logic_error);
   BOOST_CHECK(app.state() == forge::app::application_state::stopped);
   BOOST_REQUIRE_EQUAL(log.entries.size(), 3);
}

BOOST_AUTO_TEST_CASE(application_runtime_records_diagnostics_and_events) {
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto diagnostics = forge::app::diagnostics_store{};
   auto context = forge::app::plugin_context{scheduler, signals, events, &diagnostics};
   auto log = lifecycle_log{};

   auto registry = forge::app::plugin_registry{};
   registry.register_plugin(descriptor("a", log));

   auto app = forge::app::application_runtime{context, registry.instantiate_enabled({}), &diagnostics};
   forge::asio::blocking::run(runtime, app.startup());

   auto snapshot = diagnostics.snapshot(events);
   BOOST_REQUIRE_EQUAL(snapshot.plugins.size(), 1U);
   BOOST_TEST(snapshot.plugins.front().id == "a");
   BOOST_TEST(static_cast<int>(snapshot.plugins.front().state) == static_cast<int>(forge::app::lifecycle_state::started));
   BOOST_TEST(snapshot.events.published >= 4U);
   BOOST_TEST(!snapshot.recent_events.empty());

   forge::asio::blocking::run(runtime, app.shutdown());
   snapshot = diagnostics.snapshot(events);
   BOOST_TEST(static_cast<int>(snapshot.plugins.front().state) == static_cast<int>(forge::app::lifecycle_state::stopped));
}

BOOST_AUTO_TEST_CASE(application_runtime_records_failed_plugin_diagnostics) {
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto diagnostics = forge::app::diagnostics_store{};
   auto context = forge::app::plugin_context{scheduler, signals, events, &diagnostics};
   auto log = lifecycle_log{};

   auto registry = forge::app::plugin_registry{};
   registry.register_plugin(descriptor("a", log, {}, true, true));

   auto app = forge::app::application_runtime{context, registry.instantiate_enabled({}), &diagnostics};
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, app.startup()), forge::app::exceptions::startup_failed);

   const auto snapshot = diagnostics.snapshot(events);
   BOOST_TEST(static_cast<int>(snapshot.state) == static_cast<int>(forge::app::lifecycle_state::failed));
   BOOST_REQUIRE_EQUAL(snapshot.plugins.size(), 1U);
   BOOST_TEST(static_cast<int>(snapshot.plugins.front().state) == static_cast<int>(forge::app::lifecycle_state::failed));
   BOOST_TEST(snapshot.plugins.front().last_error == "startup failed");
}

BOOST_AUTO_TEST_CASE(application_runtime_collects_and_applies_plugin_config_before_initialize) {
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto context = forge::app::plugin_context{scheduler, signals, events};
   auto log = lifecycle_log{};

   auto plugins = std::vector<std::unique_ptr<forge::app::plugin>>{};
   plugins.push_back(std::make_unique<configurable_plugin>(log));

   auto app = forge::app::application_runtime{context, std::move(plugins)};
   const auto registry = app.describe_config();
   BOOST_REQUIRE_EQUAL(registry.components().size(), 1U);
   BOOST_TEST(registry.components().front().section == "http");

   auto document = forge::config::core::document{};
   document.set("http.bind-port", 7777);
   forge::asio::blocking::run(runtime, app.configure(document));
   forge::asio::blocking::run(runtime, app.startup());
   forge::asio::blocking::run(runtime, app.shutdown());

   BOOST_REQUIRE_EQUAL(log.entries.size(), 4U);
   BOOST_TEST(log.entries[0] == "configure:7777");
   BOOST_TEST(log.entries[1] == "initialize:configurable");
   BOOST_TEST(log.entries[2] == "startup:configurable");
   BOOST_TEST(log.entries[3] == "shutdown:configurable");
}

BOOST_AUTO_TEST_CASE(application_shell_owns_config_plugin_lifecycle_and_context) {
   auto log = lifecycle_log{};
   auto app = shell_test_application{log};

   const auto registry = app.describe_config();
   BOOST_REQUIRE_EQUAL(registry.components().size(), 3U);
   BOOST_TEST(registry.components()[0].section == "service");
   BOOST_TEST(registry.components()[1].section == "plugins");
   BOOST_TEST(registry.components()[2].section == "http");

   auto document = forge::config::core::document{};
   document.set("service.workers", 4);
   app.configure(document);

   forge::asio::blocking::run(app.runtime(), app.initialize());
   forge::asio::blocking::run(app.runtime(), app.startup());

   BOOST_TEST(app.workers() == 4U);
   auto api = app.apis().get<sample_api>({.id = {"sample"}, .major = 1});
   BOOST_TEST(forge::asio::blocking::run(app.runtime(), api->value(0)) == 4);

   const auto exit_code = app.run_count();
   BOOST_TEST(exit_code == 7);
   forge::asio::blocking::run(app.runtime(), app.shutdown());

   const auto expected = std::vector<std::string>{
      "app.configure:4",
      "plugin.configure:9000",
      "app.provide",
      "plugin.initialize:9000",
      "plugin.after_initialize:9000",
      "plugin.startup",
      "app.run",
      "plugin.shutdown",
   };
   BOOST_TEST(log.entries == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(application_shell_rejects_ingestion_only_fields_before_configure_callbacks) {
   auto log = lifecycle_log{};
   auto app = shell_test_application{log};
   auto document = forge::config::core::document{};
   document.set("http.removed-output", "legacy-value");

   BOOST_CHECK_EXCEPTION(
      app.configure(document),
      std::invalid_argument,
      [](const std::invalid_argument& error) {
         const auto message = std::string_view{error.what()};
         return message.find("http.removed-output") != std::string_view::npos &&
                message.find("http removed-output was removed") != std::string_view::npos;
      });
   BOOST_TEST(log.entries.empty());
}

BOOST_AUTO_TEST_CASE(application_shell_publishes_api_before_plugin_initialize) {
   auto log = lifecycle_log{};
   auto app = shell_api_application{log};

   forge::asio::blocking::run(app.runtime(), app.initialize());

   const auto expected = std::vector<std::string>{
      "app.provide",
      "api.initialize:1",
   };
   BOOST_TEST(log.entries == expected, boost::test_tools::per_element());
   BOOST_TEST(app.apis().describe({.id = {"sample"}, .major = 1}) != nullptr);

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(application_shell_preserves_dependency_order_and_reverse_shutdown) {
   auto log = lifecycle_log{};
   auto app = shell_order_application{log};

   app.configure(forge::config::core::document{});
   forge::asio::blocking::run(app.runtime(), app.startup());
   forge::asio::blocking::run(app.runtime(), app.shutdown());

   const auto expected = std::vector<std::string>{
      "initialize:store",
      "initialize:api",
      "after_initialize:store",
      "after_initialize:api",
      "startup:store",
      "startup:api",
      "shutdown:api",
      "shutdown:store",
   };
   BOOST_TEST(log.entries == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(application_shell_retries_failed_dependent_shutdown_before_dependencies) {
   auto log = lifecycle_log{};
   auto app = shell_shutdown_retry_application{log};

   forge::asio::blocking::run(app.runtime(), app.startup());
   BOOST_CHECK_EXCEPTION(
      forge::asio::blocking::run(app.runtime(), app.shutdown()),
      std::runtime_error,
      [](const std::runtime_error& error) {
         return std::string{error.what()} == "dependent shutdown failed";
      });

   BOOST_TEST(static_cast<int>(app.state()) == static_cast<int>(forge::app::application_state::started));
   BOOST_CHECK(!app.scheduler().snapshot().stopped);
   const auto first_attempt = std::vector<std::string>{
      "initialize:store",
      "initialize:api",
      "after_initialize:store",
      "after_initialize:api",
      "startup:store",
      "startup:api",
      "shutdown:api",
   };
   BOOST_TEST(log.entries == first_attempt, boost::test_tools::per_element());

   const auto failed_snapshot = app.diagnostics().snapshot(app.events());
   BOOST_TEST(static_cast<int>(failed_snapshot.state) == static_cast<int>(forge::app::lifecycle_state::failed));
   BOOST_TEST(failed_snapshot.last_transition == "shutdown");
   BOOST_TEST(failed_snapshot.last_error == "dependent shutdown failed");

   forge::asio::blocking::run(app.runtime(), app.shutdown());

   BOOST_TEST(static_cast<int>(app.state()) == static_cast<int>(forge::app::application_state::stopped));
   BOOST_CHECK(app.scheduler().snapshot().stopped);
   const auto expected = std::vector<std::string>{
      "initialize:store",
      "initialize:api",
      "after_initialize:store",
      "after_initialize:api",
      "startup:store",
      "startup:api",
      "shutdown:api",
      "shutdown:api",
      "shutdown:store",
   };
   BOOST_TEST(log.entries == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(application_shell_rolls_back_started_plugins_on_startup_failure) {
   auto log = lifecycle_log{};
   auto app = shell_failure_application{log};

   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), app.startup()), forge::app::exceptions::startup_failed);
   BOOST_TEST(static_cast<int>(app.state()) == static_cast<int>(forge::app::application_state::stopped));

   const auto expected = std::vector<std::string>{
      "initialize:store",
      "initialize:api",
      "startup:store",
      "startup:api",
      "shutdown:api",
      "shutdown:store",
   };
   BOOST_TEST(log.entries == expected, boost::test_tools::per_element());

   const auto snapshot = app.diagnostics().snapshot(app.events());
   BOOST_TEST(static_cast<int>(snapshot.state) == static_cast<int>(forge::app::lifecycle_state::failed));
   BOOST_TEST(snapshot.last_error == "startup failed");
}

BOOST_AUTO_TEST_CASE(application_shell_keeps_scheduler_available_until_shutdown_finishes) {
   auto log = lifecycle_log{};
   auto app = shell_scheduler_cleanup_application{log};

   forge::asio::blocking::run(app.runtime(), app.startup());
   app.request_stop();
   forge::asio::blocking::run(app.runtime(), app.shutdown());

   const auto cleanup = std::ranges::find(log.entries, "cleanup.scheduler.work");
   const auto shutdown = std::ranges::find(log.entries, "shutdown:cleanup");
   BOOST_REQUIRE(cleanup != log.entries.end());
   BOOST_REQUIRE(shutdown != log.entries.end());
   BOOST_CHECK(cleanup < shutdown);
}

BOOST_AUTO_TEST_CASE(application_shell_compute_is_opt_in) {
   auto app = forge::app::application_shell{};
   BOOST_CHECK(!app.has_compute());
   BOOST_CHECK_THROW(static_cast<void>(app.compute()), forge::asio::exceptions::invalid_state);
}

BOOST_AUTO_TEST_CASE(application_shell_configures_on_its_single_worker_without_nested_blocking) {
   bool configured = false;
   auto builder = forge::app::application_builder{};
   builder.runtime({.worker_threads = 1}).configure([&](forge::app::configure_context& context) {
      const auto* value = context.document().try_get("test.marker");
      BOOST_REQUIRE(value);
      BOOST_CHECK_EQUAL(std::get<std::string>(value->storage), "original");
      configured = true;
   });
   auto app = std::move(builder).build();
   forge::config::core::document document;
   document.set("test.marker", "original");
   auto configuration = app->async_configure(document);
   document.set("test.marker", "changed");
   auto execute = [&]() -> boost::asio::awaitable<void> {
      co_await std::move(configuration);
      co_await app->startup();
      bool rejected = false;
      try {
         co_await app->async_configure({});
      } catch (const std::logic_error&) {
         rejected = true;
      }
      BOOST_CHECK(rejected);
      co_await app->shutdown();
   };
   forge::asio::blocking::run(app->runtime(), execute());
   BOOST_CHECK(configured);
}

BOOST_AUTO_TEST_CASE(application_shell_keeps_compute_available_through_plugin_shutdown) {
   auto log = lifecycle_log{};
   auto app = shell_compute_cleanup_application{log};

   BOOST_REQUIRE(app.has_compute());
   auto compute = app.compute();
   forge::asio::blocking::run(app.runtime(), app.startup());
   forge::asio::blocking::run(app.runtime(), app.shutdown());

   const auto expected = std::vector<std::string>{
      "initialize:compute-cleanup",
      "cleanup.compute.work",
      "shutdown:compute-cleanup",
   };
   BOOST_TEST(log.entries == expected, boost::test_tools::per_element());
   BOOST_CHECK_THROW(
      forge::asio::blocking::run(app.runtime(), compute.submit({.name = "after-stop"}, [] {})),
      forge::asio::exceptions::rejected);
}

BOOST_AUTO_TEST_CASE(application_shell_stops_compute_before_waiting_for_scheduled_awaitables) {
   auto app = forge::app::application_shell{forge::app::application_shell_options{
      .runtime = {.worker_threads = 1, .thread_name = "shutdown-order-test"},
      .compute = forge::asio::compute::pool::options{.worker_threads = 1},
   }};
   forge::asio::blocking::run(app.runtime(), app.startup());

   auto compute_started = std::atomic_bool{false};
   auto delayed_ran = std::atomic_bool{false};
   auto scheduled = app.scheduler().submit(forge::asio::task::awaitable{
      .priority = forge::asio::task::priority{1},
      .name = "scheduled-compute",
      .work = [&](forge::asio::task::context&) -> boost::asio::awaitable<void> {
         try {
            co_await app.compute().execute(
               {.name = "cooperative-shutdown"},
               [&](forge::asio::compute::context& context) {
                  compute_started.store(true, std::memory_order_release);
                  while (!context.stop_requested()) {
                     std::this_thread::sleep_for(std::chrono::milliseconds{1});
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds{50});
               });
         } catch (const forge::asio::exceptions::canceled&) {
         }

         auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
         timer.expires_after(std::chrono::milliseconds{10});
         co_await timer.async_wait(boost::asio::use_awaitable);
      },
   });
   auto delayed = app.scheduler().submit_after(
      forge::asio::task::task{
         .priority = forge::asio::task::priority{1},
         .name = "must-not-start-during-compute-drain",
         .work = [&] { delayed_ran.store(true, std::memory_order_release); },
      },
      std::chrono::milliseconds{10});

   forge::asio::blocking::run(app.runtime(), [&]() -> boost::asio::awaitable<void> {
      auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
      for (auto attempt = 0;
           attempt < 500 && !compute_started.load(std::memory_order_acquire);
           ++attempt) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }
      co_return;
   }());
   BOOST_REQUIRE(compute_started.load(std::memory_order_acquire));

   auto shutdown = std::async(std::launch::async, [&] {
      forge::asio::blocking::run(app.runtime(), app.shutdown());
   });
   const auto completed_without_fallback =
      shutdown.wait_for(std::chrono::milliseconds{250}) == std::future_status::ready;
   if (!completed_without_fallback) {
      // Keep the regression finite on broken implementations: a second caller
      // can run the continuation that the sole runtime worker is blocking.
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
      while (shutdown.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready &&
             std::chrono::steady_clock::now() < deadline) {
         if (app.runtime().context().poll_one() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
         }
      }
   }
   BOOST_REQUIRE(shutdown.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready);
   shutdown.get();

   BOOST_CHECK(completed_without_fallback);
   BOOST_CHECK(app.scheduler().snapshot().stopped);
   BOOST_CHECK(!delayed_ran.load(std::memory_order_acquire));
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), delayed.wait()),
                     forge::asio::exceptions::canceled);
   forge::asio::blocking::run(app.runtime(), scheduled.wait());
}

BOOST_AUTO_TEST_CASE(application_shell_initialize_failure_transitions_to_stopped) {
   auto log = lifecycle_log{};
   auto app = shell_initialize_failure_application{log};

   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), app.initialize()), forge::app::exceptions::initialize_failed);
   BOOST_TEST(static_cast<int>(app.state()) == static_cast<int>(forge::app::application_state::stopped));
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), app.startup()), std::logic_error);

   const auto expected = std::vector<std::string>{
      "initialize:init-fail",
   };
   BOOST_TEST(log.entries == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(application_shell_after_initialize_failure_cleans_up_and_preserves_error) {
   auto log = lifecycle_log{};
   auto app = shell_after_initialize_failure_application{log};

   BOOST_CHECK_EXCEPTION(
      forge::asio::blocking::run(app.runtime(), app.initialize()),
      forge::app::exceptions::initialize_failed,
      [](const auto& error) {
         return error.message() == "after initialize failed";
      });
   BOOST_TEST(static_cast<int>(app.state()) == static_cast<int>(forge::app::application_state::stopped));
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), app.startup()), std::logic_error);

   const auto expected = std::vector<std::string>{
      "initialize:after-init-fail",
      "after_initialize:after-init-fail",
      "request_stop:after-init-fail",
      "shutdown:after-init-fail",
   };
   BOOST_TEST(log.entries == expected, boost::test_tools::per_element());

   const auto snapshot = app.diagnostics().snapshot(app.events());
   BOOST_TEST(static_cast<int>(snapshot.state) == static_cast<int>(forge::app::lifecycle_state::failed));
   BOOST_REQUIRE_EQUAL(snapshot.plugins.size(), 1U);
   BOOST_TEST(snapshot.plugins.front().id == "after-init-fail");
   BOOST_TEST(static_cast<int>(snapshot.plugins.front().state) ==
              static_cast<int>(forge::app::lifecycle_state::failed));
   BOOST_TEST(snapshot.plugins.front().last_transition == "after_initialize");
   BOOST_TEST(snapshot.plugins.front().last_error == "after initialize failed");
}

BOOST_AUTO_TEST_CASE(application_shell_after_initialize_is_idempotent) {
   auto log = lifecycle_log{};
   auto app = shell_order_application{log};

   forge::asio::blocking::run(app.runtime(), app.initialize());
   forge::asio::blocking::run(app.runtime(), app.initialize());
   forge::asio::blocking::run(app.runtime(), app.startup());
   forge::asio::blocking::run(app.runtime(), app.startup());
   forge::asio::blocking::run(app.runtime(), app.shutdown());

   const auto expected = std::vector<std::string>{
      "initialize:store",
      "initialize:api",
      "after_initialize:store",
      "after_initialize:api",
      "startup:store",
      "startup:api",
      "shutdown:api",
      "shutdown:store",
   };
   BOOST_TEST(log.entries == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(application_shell_applies_plugin_selection_from_config) {
   auto log = lifecycle_log{};
   auto app = shell_selection_application{log};

   const auto registry = app.describe_config();
   auto found_plugins_section = false;
   for (const auto& component : registry.components()) {
      if (component.section == "plugins") {
         found_plugins_section = true;
         BOOST_REQUIRE_EQUAL(component.fields.size(), 3U);
         BOOST_TEST(component.fields[0].name == "store.enabled");
         BOOST_TEST(component.fields[1].name == "api.enabled");
         BOOST_TEST(component.fields[2].name == "metrics.enabled");
         BOOST_TEST(component.fields[0].has_default);
         BOOST_TEST(std::get<bool>(component.fields[0].default_value.storage));
         BOOST_TEST(!std::get<bool>(component.fields[2].default_value.storage));
      }
   }
   BOOST_TEST(found_plugins_section);

   auto document = forge::config::core::document{};
   document.set("plugins.api.enabled", false);
   document.set("plugins.metrics.enabled", true);
   app.configure(document);
   forge::asio::blocking::run(app.runtime(), app.startup());
   forge::asio::blocking::run(app.runtime(), app.shutdown());

   const auto expected = std::vector<std::string>{
      "initialize:store",
      "initialize:metrics",
      "startup:store",
      "startup:metrics",
      "shutdown:metrics",
      "shutdown:store",
   };
   BOOST_TEST(log.entries == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(application_shell_uses_nested_official_plugin_selection_path) {
   auto log = lifecycle_log{};
   auto app = shell_official_plugin_selection_application{log};

   const auto registry = app.describe_config();
   auto found_plugins_section = false;
   for (const auto& component : registry.components()) {
      if (component.section == "plugins") {
         found_plugins_section = true;
         BOOST_REQUIRE_EQUAL(component.fields.size(), 1U);
         BOOST_TEST(component.fields[0].name == "net.p2p.node.enabled");
      }
   }
   BOOST_TEST(found_plugins_section);

   auto document = forge::config::core::document{};
   document.set("plugins.net.p2p.node.enabled", false);
   app.configure(document);
   forge::asio::blocking::run(app.runtime(), app.startup());
   forge::asio::blocking::run(app.runtime(), app.shutdown());

   BOOST_TEST(log.entries.empty());
}

BOOST_AUTO_TEST_CASE(application_shell_accepts_textual_plugin_selection_flags) {
   auto log = lifecycle_log{};
   auto app = shell_selection_application{log};

   auto document = forge::config::core::document{};
   document.set("plugins.api.enabled", "false");
   document.set("plugins.metrics.enabled", "true");
   app.configure(document);
   forge::asio::blocking::run(app.runtime(), app.startup());
   forge::asio::blocking::run(app.runtime(), app.shutdown());

   const auto expected = std::vector<std::string>{
      "initialize:store",
      "initialize:metrics",
      "startup:store",
      "startup:metrics",
      "shutdown:metrics",
      "shutdown:store",
   };
   BOOST_TEST(log.entries == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(application_shell_rejects_invalid_textual_plugin_selection_flag) {
   auto log = lifecycle_log{};
   auto app = shell_selection_application{log};

   auto document = forge::config::core::document{};
   document.set("plugins.api.enabled", "definitely");

   BOOST_CHECK_THROW(app.configure(document), forge::schema::exceptions::invalid_value);
   BOOST_TEST(log.entries.empty());
}

BOOST_AUTO_TEST_CASE(application_shell_rejects_enabled_plugin_with_disabled_dependency) {
   auto log = lifecycle_log{};
   auto app = shell_selection_application{log};

   auto document = forge::config::core::document{};
   document.set("plugins.store.enabled", false);
   document.set("plugins.api.enabled", true);

   BOOST_CHECK_THROW(app.configure(document), std::logic_error);
   BOOST_TEST(log.entries.empty());
}

BOOST_AUTO_TEST_CASE(run_application_executes_lifecycle_and_custom_stop_waiter) {
   auto log = lifecycle_log{};
   auto app = shell_order_application{log};

   auto options = forge::app::run_options{};
   options.handle_sigint = false;
   options.handle_sigterm = false;
   options.wait_for_stop = [](forge::app::application_shell& shell) -> boost::asio::awaitable<void> {
      auto timer = boost::asio::steady_timer{shell.runtime().context()};
      timer.expires_after(std::chrono::milliseconds{5});
      co_await timer.async_wait(boost::asio::use_awaitable);
      co_return;
   };

   const auto exit_code = forge::app::run_application(app, forge::config::core::document{}, options);
   BOOST_TEST(exit_code == 0);
   BOOST_TEST(static_cast<int>(app.state()) == static_cast<int>(forge::app::application_state::stopped));

   const auto expected = std::vector<std::string>{
      "initialize:store",
      "initialize:api",
      "after_initialize:store",
      "after_initialize:api",
      "startup:store",
      "startup:api",
      "shutdown:api",
      "shutdown:store",
   };
   BOOST_TEST(log.entries == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(run_application_unique_ptr_reports_shutdown_timeout_after_cleanup_finishes) {
   auto log = lifecycle_log{};
   auto state = std::make_shared<slow_shutdown_state>();

   auto app = std::make_unique<shell_slow_shutdown_application>(log, state);
   auto options = forge::app::run_options{};
   options.handle_sigint = false;
   options.handle_sigterm = false;
   options.shutdown_timeout = std::chrono::milliseconds{1};

   BOOST_CHECK_THROW(forge::app::run_application(std::move(app), forge::config::core::document{}, options),
                     forge::app::exceptions::shutdown_failed);
   BOOST_TEST(!state->shutdown_finished());
   BOOST_TEST(!state->shell_destroyed());
   BOOST_TEST(state->wait_for_shutdown(std::chrono::seconds{1}));
   BOOST_TEST(state->wait_for_destroyed(std::chrono::seconds{1}));

   const auto expected = std::vector<std::string>{
      "initialize:slow",
      "startup:slow",
      "shutdown:slow",
   };
   BOOST_TEST(log.entries == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(run_application_reference_reports_timeout_only_after_shutdown_finishes) {
   auto log = lifecycle_log{};
   auto state = std::make_shared<slow_shutdown_state>();

   auto app = shell_slow_shutdown_application{log, state};
   auto options = forge::app::run_options{};
   options.handle_sigint = false;
   options.handle_sigterm = false;
   options.shutdown_timeout = std::chrono::milliseconds{1};

   auto threw_timeout = false;
   try {
      static_cast<void>(forge::app::run_application(app, forge::config::core::document{}, options));
   } catch (const forge::app::exceptions::shutdown_failed&) {
      threw_timeout = true;
   }

   const auto finished_before_return = state->shutdown_finished();
   if (!finished_before_return) {
      static_cast<void>(state->wait_for_shutdown(std::chrono::seconds{1}));
   }

   BOOST_TEST(threw_timeout);
   BOOST_TEST(finished_before_return);
   BOOST_TEST(!state->shell_destroyed());

   const auto expected = std::vector<std::string>{
      "initialize:slow",
      "startup:slow",
      "shutdown:slow",
   };
   BOOST_TEST(log.entries == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(run_daemon_builds_shell_options_before_factory) {
   auto state = daemon_test_state{};
   const auto data_dir = make_temp_dir("forge-daemon-shell-options");

   const auto exit_code = run_test_daemon(
      {
         "testd",
         "--data-dir",
         data_dir.string(),
         "--profile=single_node",
         "--runtime-threads=3",
         "--scheduler-queue-depth=123",
         "--check-config",
      },
      state);

   BOOST_TEST(exit_code == 0);
   BOOST_TEST(state.runtime_threads == 3U);
   BOOST_TEST(state.blocking_tasks == 2U);
   BOOST_TEST(state.queue_depth == 123U);
   BOOST_TEST(state.data_dir == data_dir);
   BOOST_TEST(state.config_path == data_dir / "config.yml");
   BOOST_TEST(state.dotenv_path == data_dir / ".env");
   BOOST_TEST(state.profile == "single_node");
   BOOST_REQUIRE_EQUAL(state.log.entries.size(), 2U);
   BOOST_TEST(state.log.entries[0] == "app.configure:2");
   BOOST_TEST(state.log.entries[1] == "plugin.configure:9000");
}

BOOST_AUTO_TEST_CASE(run_daemon_help_prints_daemon_app_and_plugin_options_without_lifecycle) {
   auto state = daemon_test_state{};
   auto output = stream_capture{std::cout};

   const auto exit_code = run_test_daemon({"testd", "--help"}, state);

   BOOST_TEST(exit_code == 0);
   const auto text = output.text();
   BOOST_TEST(text.find("--profile") != std::string::npos);
   BOOST_TEST(text.find("--dotenv") != std::string::npos);
   BOOST_TEST(text.find("--service.workers") != std::string::npos);
   BOOST_TEST(text.find("--http.port") != std::string::npos);
   BOOST_TEST(state.log.entries.empty());
}

BOOST_AUTO_TEST_CASE(run_daemon_merges_yaml_and_cli_before_check_config) {
   auto state = daemon_test_state{};
   const auto dir = make_temp_dir("forge-daemon-merge");
   const auto config = dir / "config.yml";
   write_text(
      config,
      R"(daemon:
  runtime-threads: 2
service:
  workers: 3
http:
  port: 7000
)");

   const auto exit_code = run_test_daemon(
      {
         "testd",
         "--config",
         config.string(),
         "--service.workers=5",
         "--check-config",
      },
      state);

   BOOST_TEST(exit_code == 0);
   BOOST_TEST(state.runtime_threads == 2U);
   BOOST_TEST(state.blocking_tasks == 1U);
   BOOST_TEST(state.workers == 5U);
   BOOST_REQUIRE_EQUAL(state.log.entries.size(), 2U);
   BOOST_TEST(state.log.entries[0] == "app.configure:5");
   BOOST_TEST(state.log.entries[1] == "plugin.configure:7000");
}

BOOST_AUTO_TEST_CASE(run_daemon_merges_yaml_dotenv_process_env_and_cli_in_order) {
   auto service_workers = env_var_guard{"FORGE_TESTD_SERVICE_WORKERS", "6"};
   auto http_port = env_var_guard{"FORGE_TESTD_HTTP_PORT", "8001"};

   auto state = daemon_test_state{};
   const auto dir = make_temp_dir("forge-daemon-source-merge");
   const auto config = dir / "config.yml";
   const auto dotenv = dir / ".env";
   write_text(
      config,
      R"(service:
  workers: 3
http:
  port: 7000
)");
   write_text(
      dotenv,
      R"(FORGE_TESTD_DAEMON_RUNTIME_THREADS=4
FORGE_TESTD_SERVICE_WORKERS=7
FORGE_TESTD_HTTP_PORT=8000
)");

   const auto exit_code = run_test_daemon(
      {
         "testd",
         "--config",
         config.string(),
         "--dotenv",
         dotenv.string(),
         "--service.workers=8",
         "--check-config",
      },
      state,
      {},
      true);

   BOOST_TEST(exit_code == 0);
   BOOST_TEST(state.runtime_threads == 4U);
   BOOST_TEST(state.workers == 8U);
   BOOST_REQUIRE_EQUAL(state.log.entries.size(), 2U);
   BOOST_TEST(state.log.entries[0] == "app.configure:8");
   BOOST_TEST(state.log.entries[1] == "plugin.configure:8001");
}

BOOST_AUTO_TEST_CASE(run_daemon_source_flags_disable_yaml_dotenv_process_env_and_app_cli) {
   auto env_workers = env_var_guard{"FORGE_TESTD_SERVICE_WORKERS", "9"};

   auto state = daemon_test_state{};
   const auto dir = make_temp_dir("forge-daemon-source-flags");
   const auto config = dir / "missing.yml";
   const auto dotenv = dir / ".env";
   write_text(dotenv, "FORGE_TESTD_SERVICE_WORKERS=8\n");

   auto options = forge::app::daemon_options{};
   options.env_prefix = "FORGE_TESTD";
   options.read_yaml = false;
   options.read_dotenv = false;
   options.read_process_env = false;
   options.read_cli = false;

   const auto exit_code = run_test_daemon(
      {
         "testd",
         "--config",
         config.string(),
         "--dotenv",
         dotenv.string(),
         "--service.workers=7",
         "--check-config",
      },
      state,
      options);

   BOOST_TEST(exit_code == 0);
   BOOST_TEST(state.workers == 2U);
   BOOST_REQUIRE_EQUAL(state.log.entries.size(), 2U);
   BOOST_TEST(state.log.entries[0] == "app.configure:2");
   BOOST_TEST(state.log.entries[1] == "plugin.configure:9000");
}

BOOST_AUTO_TEST_CASE(run_daemon_empty_env_prefix_disables_dotenv_and_process_env) {
   auto env_workers = env_var_guard{"FORGE_TESTD_SERVICE_WORKERS", "9"};

   auto state = daemon_test_state{};
   const auto dir = make_temp_dir("forge-daemon-empty-env-prefix");
   const auto dotenv = dir / ".env";
   write_text(dotenv, "FORGE_TESTD_SERVICE_WORKERS=8\n");

   auto options = forge::app::daemon_options{};
   options.env_prefix = "";
   options.read_process_env = true;

   const auto exit_code = run_test_daemon(
      {
         "testd",
         "--dotenv",
         dotenv.string(),
         "--check-config",
      },
      state,
      options);

   BOOST_TEST(exit_code == 0);
   BOOST_TEST(state.workers == 2U);
   BOOST_REQUIRE_EQUAL(state.log.entries.size(), 2U);
   BOOST_TEST(state.log.entries[0] == "app.configure:2");
   BOOST_TEST(state.log.entries[1] == "plugin.configure:9000");
}

BOOST_AUTO_TEST_CASE(run_daemon_help_ignores_missing_explicit_config) {
   auto state = daemon_test_state{};
   auto output = stream_capture{std::cout};

   const auto exit_code = run_test_daemon(
      {
         "testd",
         "--config",
         (make_temp_dir("forge-daemon-help") / "missing.yml").string(),
         "--help",
      },
      state);

   BOOST_TEST(exit_code == 0);
   BOOST_TEST(output.text().find("Daemon options") != std::string::npos);
   BOOST_TEST(state.log.entries.empty());
}

BOOST_AUTO_TEST_CASE(run_daemon_help_ignores_malformed_explicit_config) {
   auto state = daemon_test_state{};
   const auto dir = make_temp_dir("forge-daemon-help-broken-config");
   const auto config = dir / "broken.yml";
   write_text(config, "daemon: [\n");
   auto output = stream_capture{std::cout};

   const auto exit_code = run_test_daemon(
      {
         "testd",
         "--config",
         config.string(),
         "--help",
      },
      state);

   BOOST_TEST(exit_code == 0);
   BOOST_TEST(output.text().find("Daemon options") != std::string::npos);
   BOOST_TEST(output.text().find("--service.workers") != std::string::npos);
   BOOST_TEST(state.log.entries.empty());
}

BOOST_AUTO_TEST_CASE(run_daemon_explicit_missing_config_is_error_for_check_config) {
   auto state = daemon_test_state{};
   auto errors = stream_capture{std::cerr};

   const auto exit_code = run_test_daemon(
      {
         "testd",
         "--config",
         (make_temp_dir("forge-daemon-missing-config") / "missing.yml").string(),
         "--check-config",
      },
      state);

   BOOST_TEST(exit_code == 1);
   BOOST_TEST(errors.text().find("daemon.config_missing") != std::string::npos);
   BOOST_TEST(state.log.entries.empty());
}

BOOST_AUTO_TEST_CASE(run_daemon_explicit_missing_dotenv_is_error_when_dotenv_source_enabled) {
   auto state = daemon_test_state{};
   auto errors = stream_capture{std::cerr};

   auto options = forge::app::daemon_options{};
   options.env_prefix = "FORGE_TESTD";

   const auto exit_code = run_test_daemon(
      {
         "testd",
         "--dotenv",
         (make_temp_dir("forge-daemon-missing-dotenv") / ".env").string(),
         "--check-config",
      },
      state,
      options);

   BOOST_TEST(exit_code == 1);
   BOOST_TEST(errors.text().find("daemon.dotenv_missing") != std::string::npos);
   BOOST_TEST(state.log.entries.empty());
}

BOOST_AUTO_TEST_CASE(run_daemon_reports_bootstrap_type_error_as_diagnostic) {
   auto state = daemon_test_state{};
   const auto dir = make_temp_dir("forge-daemon-bootstrap-type-error");
   const auto config = dir / "config.yml";
   write_text(
      config,
      R"(daemon:
  runtime-threads: four
)");
   auto errors = stream_capture{std::cerr};

   auto exit_code = -1;
   BOOST_CHECK_NO_THROW(exit_code = run_test_daemon(
                           {
                              "testd",
                              "--config",
                              config.string(),
                              "--check-config",
                           },
                           state));

   BOOST_TEST(exit_code == 1);
   BOOST_TEST(errors.text().find("daemon.bootstrap") != std::string::npos);
   BOOST_TEST(errors.text().find("runtime-threads") != std::string::npos);
   BOOST_TEST(state.log.entries.empty());
}

BOOST_AUTO_TEST_CASE(run_daemon_reports_invalid_action_flag_type_as_diagnostic) {
   auto state = daemon_test_state{};
   const auto dir = make_temp_dir("forge-daemon-bootstrap-bool-error");
   const auto config = dir / "config.yml";
   write_text(
      config,
      R"(daemon:
  check-config: maybe
)");
   auto errors = stream_capture{std::cerr};

   auto exit_code = -1;
   BOOST_CHECK_NO_THROW(exit_code = run_test_daemon(
                           {
                              "testd",
                              "--config",
                              config.string(),
                           },
                           state));

   BOOST_TEST(exit_code == 1);
   BOOST_TEST(errors.text().find("daemon.bootstrap") != std::string::npos);
   BOOST_TEST(errors.text().find("boolean has invalid syntax") != std::string::npos);
   BOOST_TEST(state.log.entries.empty());
}

BOOST_AUTO_TEST_CASE(run_daemon_print_effective_config_redacts_secret_fields) {
   auto state = daemon_test_state{};
   const auto dir = make_temp_dir("forge-daemon-print");
   const auto config = dir / "config.yml";
   write_text(
      config,
      R"(service:
  token: super-secret
)");
   auto output = stream_capture{std::cout};

   const auto exit_code = run_test_daemon(
      {
         "testd",
         "--config",
         config.string(),
         "--print-effective-config",
      },
      state);

   BOOST_TEST(exit_code == 0);
   const auto text = output.text();
   BOOST_TEST(text.find("super-secret") == std::string::npos);
   BOOST_TEST(text.find("<redacted>") != std::string::npos);
   BOOST_TEST(state.log.entries.empty());
}

BOOST_AUTO_TEST_CASE(run_daemon_print_effective_config_rejects_ingestion_only_fields) {
   auto state = daemon_test_state{};
   const auto dir = make_temp_dir("forge-daemon-print-removed");
   const auto config = dir / "config.yml";
   write_text(
      config,
      R"(http:
  removed-output: legacy-value
)"
   );
   auto output = stream_capture{std::cout};
   auto errors = stream_capture{std::cerr};

   const auto exit_code = run_test_daemon(
      {
         "testd",
         "--config",
         config.string(),
         "--print-effective-config",
      },
      state);

   BOOST_TEST(exit_code == 1);
   BOOST_TEST(output.text().empty());
   BOOST_TEST(errors.text().find("config.removed") != std::string::npos);
   BOOST_TEST(errors.text().find("http.removed-output") != std::string::npos);
   BOOST_TEST(errors.text().find("http removed-output was removed") != std::string::npos);
   BOOST_TEST(state.log.entries.empty());
}

BOOST_AUTO_TEST_CASE(run_daemon_configure_writes_generated_config_without_product_template) {
   auto state = daemon_test_state{};
   const auto dir = make_temp_dir("forge-daemon-configure");
   const auto config = dir / "generated.yml";

   const auto exit_code = run_test_daemon(
      {
         "testd",
         "--config",
         config.string(),
         "--configure",
      },
      state);

   BOOST_TEST(exit_code == 0);
   const auto text = read_text(config);
   BOOST_TEST(text.find("daemon:") != std::string::npos);
   BOOST_TEST(text.find("service:") != std::string::npos);
   BOOST_TEST(text.find("plugins:") != std::string::npos);
   BOOST_TEST(text.find(config.string()) != std::string::npos);
   BOOST_TEST(text.find("configure: true") == std::string::npos);
   BOOST_TEST(text.find("options:") == std::string::npos);
   BOOST_TEST(state.log.entries.empty());
}

BOOST_AUTO_TEST_CASE(run_daemon_configure_refuses_to_overwrite_existing_config) {
   auto state = daemon_test_state{};
   const auto dir = make_temp_dir("forge-daemon-configure-overwrite");
   const auto config = dir / "generated.yml";
   write_text(config, "service:\n  workers: 3\n");
   auto errors = stream_capture{std::cerr};

   const auto exit_code = run_test_daemon(
      {
         "testd",
         "--config",
         config.string(),
         "--configure",
      },
      state);

   BOOST_TEST(exit_code == 1);
   BOOST_TEST(errors.text().find("refusing to overwrite") != std::string::npos);
   BOOST_TEST(read_text(config).find("workers: 3") != std::string::npos);
   BOOST_TEST(state.log.entries.empty());
}

BOOST_AUTO_TEST_CASE(run_daemon_respects_plugin_selection_before_lifecycle) {
   auto state = daemon_test_state{};

   const auto exit_code = run_test_daemon(
      {
         "testd",
         "--plugins.http.enabled=false",
      },
      state);

   BOOST_TEST(exit_code == 0);
   const auto expected = std::vector<std::string>{
      "app.configure:2",
      "app.provide",
   };
   BOOST_TEST(state.log.entries == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(run_daemon_returns_nonzero_for_invalid_config) {
   auto state = daemon_test_state{};
   auto errors = stream_capture{std::cerr};

   const auto exit_code = run_test_daemon(
      {
         "testd",
         "--service.workers=99",
         "--check-config",
      },
      state);

   BOOST_TEST(exit_code == 1);
   BOOST_TEST(errors.text().find("allowed maximum") != std::string::npos);
   BOOST_TEST(state.log.entries.empty());
}

BOOST_AUTO_TEST_CASE(application_builder_creates_shell_and_applies_config_handlers) {
   auto log = lifecycle_log{};
   auto workers = std::uint16_t{0};
   auto async_config_called = false;

   auto builder = forge::app::application_builder{};
   builder.name("builder-test")
      .runtime(forge::asio::runtime_options{.worker_threads = 1, .thread_name = "builder-test"})
      .compute(forge::asio::compute::pool::options{.worker_threads = 1})
      .config<shell_service_config>("service", [&](forge::app::configure_context& context, const shell_service_config& config)
                                    -> boost::asio::awaitable<void> {
         workers = config.workers;
         log.entries.push_back("typed.configure:" + std::to_string(workers));
         BOOST_TEST(context.view("service").get_or<std::uint16_t>("workers", 0) == 6U);
         co_return;
      })
      .configure([&](forge::app::configure_context&) {
         async_config_called = true;
         log.entries.push_back("configure.extra");
      })
      .provide([&](forge::app::application_context& context) {
         BOOST_REQUIRE(context.has_compute());
         context.apis().install<sample_api>(sample_api::describe(), std::make_shared<sample_api_impl>(workers));
         log.entries.push_back("provide");
      })
      .after_initialize([&](const forge::app::application_context& context)
                           -> boost::asio::awaitable<void> {
         auto api = context.api_view().get<sample_api>({.id = {"sample"}, .major = 1});
         BOOST_TEST(co_await api->value(0) == 6);
         log.entries.push_back("after_initialize.async_context");
         co_return;
      })
      .after_initialize([&](const forge::app::application_context& context) {
         BOOST_TEST(context.api_view().registry_ref().describe({.id = {"sample"}, .major = 1}) != nullptr);
         log.entries.push_back("after_initialize.sync_context");
      })
      .after_initialize([&] {
         log.entries.push_back("after_initialize.sync");
      })
      .after_initialize([&]() -> boost::asio::awaitable<void> {
         log.entries.push_back("after_initialize.async");
         co_return;
      })
      .run_foreground([&](forge::app::application_shell& shell) {
         log.entries.push_back("run");
         auto api = shell.apis().get<sample_api>({.id = {"sample"}, .major = 1});
         return forge::asio::blocking::run(shell.runtime(), api->value(0));
      });

   auto app = std::move(builder).build();
   BOOST_REQUIRE(app->has_compute());
   const auto registry = app->describe_config();
   BOOST_REQUIRE_EQUAL(registry.components().size(), 1U);
   BOOST_TEST(registry.components()[0].section == "service");

   auto document = forge::config::core::document{};
   document.set("service.workers", 6);
   app->configure(document);
   forge::asio::blocking::run(app->runtime(), app->startup());

   BOOST_TEST(async_config_called);
   BOOST_TEST(workers == 6U);
   auto api = app->apis().get<sample_api>({.id = {"sample"}, .major = 1});
   BOOST_TEST(forge::asio::blocking::run(app->runtime(), api->value(0)) == 6);
   BOOST_TEST(app->run() == 6);

   forge::asio::blocking::run(app->runtime(), app->shutdown());

   const auto expected = std::vector<std::string>{
      "typed.configure:6",
      "configure.extra",
      "provide",
      "after_initialize.async_context",
      "after_initialize.sync_context",
      "after_initialize.sync",
      "after_initialize.async",
      "run",
   };
   BOOST_TEST(log.entries == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(application_builder_connects_services_between_plugin_initialize_and_startup) {
   auto log = lifecycle_log{};
   auto connect_count = 0;

   auto builder = forge::app::application_builder{};
   builder.name("connect-test")
      .runtime(forge::asio::runtime_options{.worker_threads = 1, .thread_name = "connect-test"})
      .provide([&](forge::app::application_context& context) {
         context.apis().install<sample_api>(sample_api::describe(), std::make_shared<sample_api_impl>(40));
         log.entries.push_back("app.provide");
      })
      .plugin(forge::app::plugin_descriptor{
         .id = forge::app::plugin_id{.value = "connected-service"},
         .factory = [&log] {
            return std::make_unique<connected_service_plugin>(log);
         },
      })
      .connect([&](forge::app::connect_context& context) -> boost::asio::awaitable<void> {
         const auto api = context.api<sample_api>();
         const auto value = co_await api->value(2);
         context.publish(std::make_shared<connected_service>(value, &log));
         ++connect_count;
         log.entries.push_back("connect.async:" + std::to_string(value));
      })
      .connect([&](forge::app::connect_context& context) {
         const auto service = context.service<connected_service>();
         context.publish(std::make_shared<auxiliary_service>(auxiliary_service{.value = service->value + 1}));
         log.entries.push_back("connect.sync:" + std::to_string(service->value));
      })
      .connect([&] {
         log.entries.push_back("connect.no_context.sync");
      })
      .connect([&]() -> boost::asio::awaitable<void> {
         log.entries.push_back("connect.no_context.async");
         co_return;
      })
      .after_initialize([&](const forge::app::application_context& context) {
         log.entries.push_back("app.after_initialize:" +
                               std::to_string(context.service<auxiliary_service>()->value));
      });

   auto app = std::move(builder).build();
   forge::asio::blocking::run(app->runtime(), app->initialize());
   forge::asio::blocking::run(app->runtime(), app->initialize());
   forge::asio::blocking::run(app->runtime(), app->startup());
   forge::asio::blocking::run(app->runtime(), app->startup());

   BOOST_TEST(connect_count == 1);
   BOOST_TEST(app->services().get<connected_service>()->value == 42);
   BOOST_TEST(app->services().get<auxiliary_service>()->value == 43);

   const auto services = app->services();
   auto read_failed = std::atomic_bool{false};
   auto readers = std::vector<std::thread>{};
   for (auto index = 0; index < 4; ++index) {
      readers.emplace_back([&] {
         for (auto iteration = 0; iteration < 1'000; ++iteration) {
            if (services.get<connected_service>()->value != 42) {
               read_failed.store(true, std::memory_order_relaxed);
            }
         }
      });
   }
   for (auto& reader : readers) {
      reader.join();
   }
   BOOST_TEST(!read_failed.load(std::memory_order_relaxed));

   forge::asio::blocking::run(app->runtime(), app->shutdown());
   forge::asio::blocking::run(app->runtime(), app->shutdown());

   const auto expected = std::vector<std::string>{
      "app.provide",
      "plugin.initialize",
      "plugin.after_initialize",
      "connect.async:42",
      "connect.sync:42",
      "connect.no_context.sync",
      "connect.no_context.async",
      "app.after_initialize:43",
      "plugin.startup:42",
      "plugin.request_stop",
      "plugin.shutdown:42",
      "service.destroy",
   };
   BOOST_TEST(log.entries == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(application_service_registry_reports_exact_typed_errors) {
   static_assert(std::is_constructible_v<forge::app::application_context,
                                         forge::asio::runtime&,
                                         forge::asio::task::scheduler&,
                                         forge::api::core::registry&,
                                         forge::app::signal_bus&,
                                         forge::app::event_bus&,
                                         forge::app::diagnostics_store&>);

   auto apis = forge::api::core::registry{};
   auto services = forge::app::service_registry{};
   auto context = forge::app::connect_context{forge::api::core::view{apis}, services};

   BOOST_TEST(!context.try_service<connected_service>());
   BOOST_CHECK_THROW(static_cast<void>(context.service<connected_service>()),
                     forge::app::exceptions::service_missing);
   BOOST_CHECK_THROW(context.publish(std::shared_ptr<connected_service>{}),
                     forge::app::exceptions::invalid_service);

   context.publish(std::make_shared<connected_service>(7));
   BOOST_TEST(context.try_service<connected_service>() == context.service<connected_service>());
   BOOST_TEST(context.service<connected_service>()->value == 7);
   BOOST_CHECK_THROW(static_cast<void>(context.service<auxiliary_service>()),
                     forge::app::exceptions::service_missing);
   BOOST_CHECK_THROW(context.publish(std::make_shared<connected_service>()),
                     forge::app::exceptions::service_already_published);

   services.close();
   BOOST_CHECK_THROW(context.publish(std::make_shared<auxiliary_service>()),
                     forge::app::exceptions::service_publication_closed);
   BOOST_TEST(!forge::app::service_view{}.try_get<connected_service>());
   BOOST_CHECK_THROW(static_cast<void>(forge::app::service_view{}.get<connected_service>()),
                     forge::app::exceptions::service_registry_detached);
}

BOOST_AUTO_TEST_CASE(application_service_registry_synchronizes_publication_reads_close_and_clear) {
   auto services = forge::app::service_registry{};
   const auto view = services.view();
   auto start = std::atomic_bool{false};
   auto published = std::atomic_bool{false};
   auto failed = std::atomic_bool{false};
   auto readers = std::vector<std::thread>{};

   for (auto index = 0; index < 4; ++index) {
      readers.emplace_back([&] {
         while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
         }
         while (!published.load(std::memory_order_acquire)) {
            const auto current = view.try_get<connected_service>();
            if (current && current->value != 42) {
               failed.store(true, std::memory_order_relaxed);
            }
         }
         for (auto iteration = 0; iteration < 1'000; ++iteration) {
            const auto current = view.try_get<connected_service>();
            if (!current || current->value != 42) {
               failed.store(true, std::memory_order_relaxed);
            }
         }
      });
   }

   start.store(true, std::memory_order_release);
   services.publish(std::make_shared<connected_service>(42));
   published.store(true, std::memory_order_release);
   for (auto& reader : readers) {
      reader.join();
   }
   BOOST_TEST(!failed.load(std::memory_order_relaxed));

   start.store(false, std::memory_order_relaxed);
   failed.store(false, std::memory_order_relaxed);
   readers.clear();
   for (auto index = 0; index < 4; ++index) {
      readers.emplace_back([&] {
         while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
         }
         for (auto iteration = 0; iteration < 1'000; ++iteration) {
            const auto current = view.try_get<connected_service>();
            if (current && current->value != 42) {
               failed.store(true, std::memory_order_relaxed);
            }
         }
      });
   }

   start.store(true, std::memory_order_release);
   services.close();
   services.clear();
   for (auto& reader : readers) {
      reader.join();
   }

   BOOST_TEST(!failed.load(std::memory_order_relaxed));
   BOOST_TEST(!view.try_get<connected_service>());
   BOOST_CHECK_THROW(services.publish(std::make_shared<auxiliary_service>()),
                     forge::app::exceptions::service_publication_closed);
}

BOOST_AUTO_TEST_CASE(application_service_registry_releases_services_after_unlocking) {
   auto services = forge::app::service_registry{};
   auto destroyed = std::atomic_bool{false};
   services.publish(std::make_shared<auxiliary_service>());
   services.publish(std::make_shared<reentrant_service>(services.view(), &destroyed));

   services.clear();

   BOOST_TEST(destroyed.load(std::memory_order_acquire));
}

BOOST_AUTO_TEST_CASE(application_connect_failure_rolls_back_before_destroying_services) {
   auto log = lifecycle_log{};
   auto builder = forge::app::application_builder{};
   builder.runtime(forge::asio::runtime_options{.worker_threads = 1, .thread_name = "connect-fail"})
      .plugin(forge::app::plugin_descriptor{
         .id = forge::app::plugin_id{.value = "connected-service"},
         .factory = [&log] {
            return std::make_unique<connected_service_plugin>(log);
         },
      })
      .connect([&](forge::app::connect_context& context) {
         context.publish(std::make_shared<connected_service>(9, &log));
         log.entries.push_back("connect.publish");
         throw forge::app::exceptions::config_failed{"connect failed"};
      });

   auto app = std::move(builder).build();
   BOOST_CHECK_EXCEPTION(
      forge::asio::blocking::run(app->runtime(), app->initialize()),
      forge::app::exceptions::config_failed,
      [](const auto& error) {
         return error.message() == "connect failed";
      });

   BOOST_TEST(static_cast<int>(app->state()) == static_cast<int>(forge::app::application_state::stopped));
   BOOST_CHECK_THROW(static_cast<void>(app->services().get<connected_service>()),
                     forge::app::exceptions::service_missing);
   BOOST_CHECK_THROW(forge::asio::blocking::run(app->runtime(), app->startup()), std::logic_error);

   const auto expected = std::vector<std::string>{
      "plugin.initialize",
      "plugin.after_initialize",
      "connect.publish",
      "plugin.request_stop",
      "plugin.shutdown:9",
      "service.destroy",
   };
   BOOST_TEST(log.entries == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(application_builder_collects_plugin_config_and_preserves_dependency_order) {
   auto log = lifecycle_log{};
   auto builder = forge::app::application_builder{};
   builder.plugin(forge::app::plugin_descriptor{
      .id = forge::app::plugin_id{.value = "http"},
      .factory = [&log] {
         return std::make_unique<shell_config_plugin>(log);
      },
   });
   builder.plugin(forge::app::plugin_descriptor{
      .id = forge::app::plugin_id{.value = "store"},
      .factory = [&log] {
         return std::make_unique<shell_dependency_plugin>("store", log);
      },
   });
   builder.plugin(forge::app::plugin_descriptor{
      .id = forge::app::plugin_id{.value = "api"},
      .dependencies = {forge::app::plugin_id{.value = "store"}},
      .factory = [&log] {
         return std::make_unique<shell_dependency_plugin>("api", log);
      },
   });

   auto app = std::move(builder).build();
   const auto registry = app->describe_config();
   BOOST_REQUIRE_EQUAL(registry.components().size(), 2U);
   BOOST_TEST(registry.components()[0].section == "plugins");
   BOOST_TEST(registry.components()[1].section == "http");

   app->configure(forge::config::core::document{});
   forge::asio::blocking::run(app->runtime(), app->startup());
   forge::asio::blocking::run(app->runtime(), app->shutdown());

   const auto expected = std::vector<std::string>{
      "plugin.configure:9000",
      "plugin.initialize:9000",
      "initialize:store",
      "initialize:api",
      "plugin.after_initialize:9000",
      "after_initialize:store",
      "after_initialize:api",
      "plugin.startup",
      "startup:store",
      "startup:api",
      "shutdown:api",
      "shutdown:store",
      "plugin.shutdown",
   };
   BOOST_TEST(log.entries == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(application_builder_rejects_invalid_typed_config_before_side_effects) {
   auto configured = false;
   auto provided = false;

   auto builder = forge::app::application_builder{};
   builder.config<shell_service_config>("service", [&](const shell_service_config&) {
      configured = true;
   });
   builder.provide([&](forge::app::application_context& context) {
      provided = true;
      context.apis().install<sample_api>(sample_api::describe(), std::make_shared<sample_api_impl>(1));
   });

   auto app = std::move(builder).build();
   auto document = forge::config::core::document{};
   document.set("service.workers", 99);

   BOOST_CHECK_THROW(app->configure(document), std::invalid_argument);
   BOOST_TEST(!configured);
   BOOST_TEST(!provided);
   BOOST_TEST(app->apis().describe({.id = {"sample"}, .major = 1}) == nullptr);
}
