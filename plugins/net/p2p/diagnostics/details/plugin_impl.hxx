#pragma once

#include <memory>
#include <mutex>
#include <optional>

namespace forge::plugins::net::p2p::diagnostics {

struct plugin::impl : public std::enable_shared_from_this<plugin::impl> {
   config settings;
   std::shared_ptr<forge::plugins::net::p2p::node::diagnostics_source> source;
   std::shared_ptr<forge::plugins::net::p2p::node::host_event_source> events_source;
   bool initialized = false;
   bool stopping = false;
   mutable std::mutex mutex;

   [[nodiscard]] std::shared_ptr<forge::plugins::net::p2p::node::host_event_source> require_events_source() const;
   [[nodiscard]] forge::net::p2p::diagnostics::snapshot
   snapshot(std::optional<forge::net::p2p::diagnostics::options> options = std::nullopt) const;
};

} // namespace forge::plugins::net::p2p::diagnostics
