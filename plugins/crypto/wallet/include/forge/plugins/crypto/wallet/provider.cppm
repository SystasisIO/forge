module;

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

export module forge.plugins.crypto.wallet.provider;

export import forge.crypto.signer.provider;
export import forge.crypto.wallet.protocol;
export import forge.crypto.core.secret_string;
import forge.asio.task;
import forge.asio.compute;

export namespace forge::plugins::crypto::wallet {

// Stable composition-owned provider. Its existence does not imply an unlocked
// wallet. Management calls are local implementation methods; remote access is
// exclusively through the separately authorized Wallet API.
class provider final : public forge::crypto::signer::provider {
 public:
   explicit provider(std::string name);
   ~provider() override;
   provider(const provider&) = delete;
   provider& operator=(const provider&) = delete;

   [[nodiscard]] const std::string& name() const noexcept;
   void initialize(std::filesystem::path directory, forge::asio::task::scheduler& scheduler,
                   forge::asio::compute::executor compute, std::uint32_t timeout_seconds = 300,
                   std::uint32_t max_pending = 64);
   void startup();
   void request_stop() noexcept;
   boost::asio::awaitable<void> shutdown();

   boost::asio::awaitable<forge::crypto::wallet::wallet_status> create(forge::crypto::core::secret_string password);
   boost::asio::awaitable<forge::crypto::wallet::wallet_status> open();
   boost::asio::awaitable<forge::crypto::wallet::wallet_status> status();
   boost::asio::awaitable<forge::crypto::wallet::wallet_status> unlock(forge::crypto::core::secret_string password);
   boost::asio::awaitable<forge::crypto::wallet::wallet_status> lock();
   boost::asio::awaitable<forge::crypto::wallet::wallet_status> set_timeout(std::uint32_t seconds);
   boost::asio::awaitable<forge::crypto::wallet::key_info> create_key(std::string id);
   boost::asio::awaitable<forge::crypto::wallet::key_info> import_key(std::string id,
                                                                      forge::crypto::core::secret_string key);
   boost::asio::awaitable<forge::crypto::wallet::wallet_status> remove_key(std::string id);

   boost::asio::awaitable<std::vector<forge::crypto::signer::key_info>> keys() override;
   boost::asio::awaitable<forge::crypto::signer::key_info> describe(const forge::crypto::signer::key_id& id) override;
   boost::asio::awaitable<forge::crypto::signer::sign_digest_response>
   sign_digest(forge::crypto::signer::sign_digest_request request) override;

 private:
   struct impl;
   std::shared_ptr<impl> impl_;
};

} // namespace forge::plugins::crypto::wallet
