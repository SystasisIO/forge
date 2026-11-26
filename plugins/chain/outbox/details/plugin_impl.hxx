#pragma once

namespace forge::plugins::chain::outbox {

struct plugin::impl : std::enable_shared_from_this<impl> {
   struct read_profile {
      forge::api::core::api_ref contract;
      forge::api::chain::destination target;
      forge::chain::api::verified_client_options verification;
      std::size_t max_calls;
      std::chrono::milliseconds timeout;
      bool trust_execution;
      mutable std::size_t active = 0;
   };

   explicit impl(plugin_options options);

   void require_running() const;
   const read_profile* begin_call(const std::optional<forge::api::core::api_ref>& read_contract = {});
   void end_call(const read_profile* profile = nullptr) noexcept;
   boost::asio::awaitable<void> drain();
   void configure_http(std::vector<http_profile> profiles);
   forge::api::chain::outbox::transport connect();
   void resolve_signer();
   boost::asio::awaitable<void> recover(forge::asio::task::context& task);
   boost::asio::awaitable<void> open(forge::api::chain::outbox::transport transport);
   const read_profile* profile_for(const forge::api::core::api_ref& contract) const;

   plugin_options options;
   std::vector<read_profile> read_profiles;
   std::vector<std::pair<std::string, forge::net::http::base_url>> http_endpoints;
   std::optional<forge::api::core::view> apis;
   std::function<boost::asio::awaitable<void>()> flush;
   std::function<boost::asio::awaitable<forge::chain::api::verified_client>(
       forge::api::chain::destination, const forge::chain::api::verified_client_options&)> read_client;
   std::function<boost::asio::awaitable<forge::chain::protocol::info_response>(
       forge::api::chain::destination, const forge::chain::api::verified_client_options&)> info;
   std::function<boost::asio::awaitable<forge::chain::transaction::prepared_transaction>(
       forge::chain::transaction::unsigned_transaction)> sign;
   std::shared_ptr<forge::api::chain::outbox> queue;
   forge::asio::task::scheduler* scheduler = nullptr;
   forge::asio::task::handle worker;
   std::stop_source stop;
   std::atomic_bool running = false;
   forge::api::chain::outbox::admission admit;
   bool initialized = false;
   std::mutex calls_mutex;
   std::size_t calls = 0;
   forge::asio::notification calls_changed;
};

} // namespace forge::plugins::chain::outbox
