#pragma once

namespace forge::plugins::crypto::wallet {

struct plugin::impl {
   explicit impl(plugin_options options);
   void authorize(const forge::api::auth::authenticated_caller& caller, std::string_view wallet,
                  forge::crypto::wallet::operation operation) const;
   bool allowed(const forge::api::auth::authenticated_caller& caller, std::string_view wallet,
                forge::crypto::wallet::operation operation) const;
   std::shared_ptr<provider> find(std::string name, bool create);
   std::vector<std::shared_ptr<provider>> snapshot() const;
   void validate() const;

   template <typename Request> void check_size(const Request& request) const {
      if (forge::raw::pack_size(request) > settings.max_request_bytes) {
         throw forge::crypto::wallet::exceptions::invalid_request{"Wallet request exceeds its size limit"};
      }
   }

   config settings;
   const bool allow_http_management;
   mutable std::mutex mutex;
   std::map<std::string, std::shared_ptr<provider>, std::less<>> wallets;
   forge::asio::task::scheduler* scheduler = nullptr;
   forge::asio::compute::executor compute;
   forge::api::core::handle<forge::plugins::net::http::server::api> http;
   std::atomic_bool stopping = false;
   bool started = false;
};

} // namespace forge::plugins::crypto::wallet
