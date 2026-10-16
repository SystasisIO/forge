#pragma once

#include "managed_store.hxx"
#include "opened_store.hxx"
#include "pending_open.hxx"
#include "phase.hxx"

namespace forge::plugins::db::store {
struct plugin::impl : public std::enable_shared_from_this<plugin::impl> {
   explicit impl(forge::app::plugin_lifecycle lifecycle, plugin::options options);
   const forge::app::plugin_lifecycle lifecycle;
   mutable std::mutex mutex;
   config settings;
   const plugin::options opening;
   std::exception_ptr opening_failure;
   std::exception_ptr prerequisite_failure;
   std::unordered_map<std::string, std::shared_ptr<managed_store>> stores;
   std::atomic<phase> current = phase::collecting;
   bool enabled = true;
   bool shutdown_completed = false;
   forge::asio::compute::executor compute;

   void configure(config value);
   void initialize(forge::asio::compute::executor executor);
   boost::asio::awaitable<void> close_for_activation(std::string name);
   void claim_activation(const std::string& name);
   boost::asio::awaitable<void> activate_closed(std::string name, std::optional<std::filesystem::path> destination);
   [[nodiscard]] activation_status closed_state(const std::string& name) const;
   [[nodiscard]] bool activation_inflight(const std::string& name) const noexcept;
   boost::asio::awaitable<void> open();
   void start();
   void request_stop() noexcept;
   boost::asio::awaitable<void> close();

   void add_store(std::string name,
                  std::shared_ptr<forge::db::core::driver> driver,
                  store_options options);
   [[nodiscard]] std::shared_ptr<managed_store> find_store(const std::string& name) const;
   [[nodiscard]] std::shared_ptr<managed_store> require_store(const std::string& name) const;
   [[nodiscard]] ::forge::plugins::db::store::opened_store require_setup_store(const std::string& name) const;
   [[nodiscard]] ::forge::plugins::db::store::opened_store require_started_store(const std::string& name) const;
   [[nodiscard]] status current_status() const;

 private:
   [[nodiscard]] static boost::asio::awaitable<std::shared_ptr<forge::db::core::driver>>
   make_configured_driver(const store_config& value);
   void reject_started_setup() const;
   void reject_duplicate_name(const std::string& name) const;
};

} // namespace forge::plugins::db::store
