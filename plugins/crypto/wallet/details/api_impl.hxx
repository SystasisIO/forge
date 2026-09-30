#pragma once

namespace forge::plugins::crypto::wallet {

class plugin::api_impl final : public forge::crypto::wallet::api {
 public:
   explicit api_impl(std::shared_ptr<impl> state);
   boost::asio::awaitable<forge::crypto::wallet::wallet_status> create(forge::crypto::wallet::password_request,
                                                                       forge::api::auth::authenticated_caller) override;
   boost::asio::awaitable<forge::crypto::wallet::wallet_status> open(forge::crypto::wallet::wallet_request,
                                                                     forge::api::auth::authenticated_caller) override;
   boost::asio::awaitable<std::vector<forge::crypto::wallet::wallet_status>>
       list(forge::api::auth::authenticated_caller) override;
   boost::asio::awaitable<forge::crypto::wallet::wallet_status> status(forge::crypto::wallet::wallet_request,
                                                                       forge::api::auth::authenticated_caller) override;
   boost::asio::awaitable<std::vector<forge::crypto::wallet::key_info>>
       list_public_keys(forge::crypto::wallet::wallet_request, forge::api::auth::authenticated_caller) override;
   boost::asio::awaitable<forge::crypto::wallet::wallet_status> unlock(forge::crypto::wallet::password_request,
                                                                       forge::api::auth::authenticated_caller) override;
   boost::asio::awaitable<forge::crypto::wallet::wallet_status> lock(forge::crypto::wallet::wallet_request,
                                                                     forge::api::auth::authenticated_caller) override;
   boost::asio::awaitable<std::vector<forge::crypto::wallet::wallet_status>>
       lock_all(forge::api::auth::authenticated_caller) override;
   boost::asio::awaitable<forge::crypto::wallet::wallet_status>
       set_timeout(forge::crypto::wallet::timeout_request, forge::api::auth::authenticated_caller) override;
   boost::asio::awaitable<forge::crypto::wallet::key_info> create_key(forge::crypto::wallet::key_request,
                                                                      forge::api::auth::authenticated_caller) override;
   boost::asio::awaitable<forge::crypto::wallet::key_info> import_key(forge::crypto::wallet::import_request,
                                                                      forge::api::auth::authenticated_caller) override;
   boost::asio::awaitable<forge::crypto::wallet::wallet_status>
       remove_key(forge::crypto::wallet::key_request, forge::api::auth::authenticated_caller) override;

 private:
   // Never project dependency diagnostics: parsing failures may contain secret
   // input, while filesystem errors may disclose server paths.
   template <typename T> static boost::asio::awaitable<T> sanitized(boost::asio::awaitable<T> work) {
      try {
         co_return co_await std::move(work);
      } catch (const forge::crypto::keystore::exceptions::duplicate_key&) {
         throw forge::crypto::wallet::exceptions::already_exists{"Key already exists"};
      } catch (const forge::crypto::keystore::exceptions::unknown_key&) {
         throw forge::crypto::wallet::exceptions::not_found{"Key does not exist"};
      } catch (const forge::crypto::keystore::exceptions::invalid_file&) {
         throw forge::crypto::wallet::exceptions::invalid_password{"Unable to unlock wallet"};
      } catch (const forge::exceptions::base& error) {
         if (error.code().category() == forge::crypto::wallet::exceptions::make_error_code(
                                            forge::crypto::wallet::exceptions::code::invalid_request)
                                            .category()) {
            throw;
         }
         if (error.code() ==
             forge::api::core::exceptions::make_error_code(forge::api::core::exceptions::code::cancelled)) {
            throw forge::api::core::exceptions::cancelled{"Wallet operation was canceled"};
         }
         throw forge::crypto::wallet::exceptions::storage_error{"Wallet operation failed"};
      } catch (...) {
         throw forge::crypto::wallet::exceptions::storage_error{"Wallet operation failed"};
      }
   }
   std::shared_ptr<impl> state_;
};

} // namespace forge::plugins::crypto::wallet
