#pragma once

namespace forge::plugins::net::p2p::diagnostics {

struct plugin::impl : public std::enable_shared_from_this<plugin::impl> {
   config settings;
   std::shared_ptr<forge::plugins::net::p2p::node::diagnostics_source> source;
   bool initialized = false;
   bool stopping = false;

   [[nodiscard]] forge::plugins::net::p2p::node::diagnostics_source& require_source() const;
   [[nodiscard]] forge::net::p2p::diagnostics::snapshot snapshot() const;
};

} // namespace forge::plugins::net::p2p::diagnostics
