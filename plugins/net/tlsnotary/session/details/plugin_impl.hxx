#pragma once

namespace forge::plugins::net::tlsnotary::session {

struct plugin::impl {
   explicit impl(forge::app::plugin_lifecycle lifecycle);
   std::shared_ptr<forge::net::tlsnotary::engine> require_engine() const;
   const forge::app::plugin_lifecycle lifecycle;
   mutable std::mutex mutex;
   config settings;
   forge::asio::runtime* runtime = nullptr;
   forge::asio::compute::executor compute;
   std::shared_ptr<forge::net::tlsnotary::engine> engine;
};
} // namespace forge::plugins::net::tlsnotary::session
