#pragma once

namespace forge::net::tlsnotary {

struct engine::impl : public std::enable_shared_from_this<engine::impl> {
   impl(forge::asio::runtime& runtime, forge::asio::compute::executor compute, engine_options options);
   struct entry;
   static boost::asio::awaitable<result>
   execute(std::shared_ptr<impl> self,
           boost::compat::move_only_function<std::shared_ptr<detail::native_exchange>()> make_exchange,
           session_options options, std::chrono::steady_clock::time_point deadline);
   static boost::asio::awaitable<void> drain(std::shared_ptr<impl> self);
   bool cancel(std::uint64_t id) noexcept;
   void request_stop() noexcept;
   metrics snapshot() const;
   forge::asio::runtime& runtime;
   forge::asio::compute::executor compute;
   const engine_options options;
   mutable std::mutex mutex;
   metrics counters;
   std::deque<std::shared_ptr<entry>> queue;
   std::vector<std::shared_ptr<entry>> active;
   forge::asio::notification changed;
   std::mutex signal_mutex;
   forge::asio::child_signal_registration signal_owner;
};
} // namespace forge::net::tlsnotary
