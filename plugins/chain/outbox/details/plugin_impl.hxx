#pragma once

namespace forge::plugins::chain::outbox {

struct plugin::impl : std::enable_shared_from_this<impl> {
   explicit impl(plugin_options options);

   void require_running() const;
   void begin_call();
   void end_call() noexcept;
   boost::asio::awaitable<void> drain();
   void configure_http(std::vector<http_profile> profiles);
   forge::api::chain::outbox::transport connect();
   void resolve_signer();
   boost::asio::awaitable<void> recover(forge::asio::task::context& task);
   boost::asio::awaitable<void> open(forge::api::chain::outbox::transport transport);

   plugin_options options;
   std::vector<std::pair<std::string, forge::net::http::base_url>> http_endpoints;
   std::optional<forge::api::core::view> apis;
   std::function<boost::asio::awaitable<void>()> flush;
   std::function<boost::asio::awaitable<forge::chain::protocol::info_response>(forge::api::chain::destination)> info;
   std::function<boost::asio::awaitable<forge::chain::transaction::prepared_transaction>(
       forge::chain::transaction::unsigned_transaction)> sign;
   std::shared_ptr<forge::api::chain::outbox> queue;
   forge::asio::task::scheduler* scheduler = nullptr;
   forge::asio::task::handle worker;
   std::stop_source stop;
   std::atomic_bool running = false;
   std::mutex calls_mutex;
   std::size_t calls = 0;
   forge::asio::notification calls_changed;
};

} // namespace forge::plugins::chain::outbox
