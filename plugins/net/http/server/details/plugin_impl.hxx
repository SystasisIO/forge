#pragma once

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "pending_binding.hxx"
#include "startup_snapshot.hxx"

namespace forge::plugins::net::http::server {
struct plugin::impl {
   mutable std::mutex mutex;
   config settings;
   forge::asio::runtime* runtime = nullptr;
   forge::asio::compute::executor file_read_executor;
   const forge::api::core::registry* apis = nullptr;
   std::vector<pending_binding> bindings;
   std::vector<middleware_descriptor> middleware;
   std::vector<forge::net::http::asset_bundle> asset_mounts;
   std::unique_ptr<forge::net::http::server> server;
   std::shared_ptr<forge::plugins::crypto::secrets::api> secrets;
   std::shared_ptr<forge::net::tls::context_provider> tls_context_provider;
   std::uint64_t lifecycle_generation = 0;
   bool publication_closed = false;
   bool stopping = false;

   void add(pending_binding value);
   void add(middleware_descriptor value);
   void add(forge::net::http::asset_mount value);
   [[nodiscard]] startup_snapshot close_publication();
   boost::asio::awaitable<std::shared_ptr<forge::net::tls::context_provider>> make_tls_context_provider();
   boost::asio::awaitable<void> reload_tls();
   void reset_runtime() noexcept;
};

} // namespace forge::plugins::net::http::server
