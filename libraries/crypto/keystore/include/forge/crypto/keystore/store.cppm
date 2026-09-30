module;

#include <boost/asio/awaitable.hpp>
#include <filesystem>
#include <memory>

export module forge.crypto.keystore.store;

export import forge.crypto.keystore.encrypted_file;
export import forge.crypto.keystore.ownership;
export import forge.crypto.signer.provider;

export namespace forge::crypto::keystore {

class store final : public signer::provider {
 public:
   [[nodiscard]] static store create(std::filesystem::path path, core::secret_string password,
                                     store_options options = {});
   [[nodiscard]] static store open(std::filesystem::path path, core::secret_string password,
                                   store_options options = {});
   [[nodiscard]] static store create(std::shared_ptr<ownership> owner, core::secret_string password,
                                     store_options options = {});
   [[nodiscard]] static store open(std::shared_ptr<ownership> owner, core::secret_string password,
                                   store_options options = {});

   ~store() override;
   store(store&&) noexcept;
   store& operator=(store&&) noexcept;

   store(const store&) = delete;
   store& operator=(const store&) = delete;

   void put(signer::key_id id, asymmetric::private_key key, bool replace = false);
   void erase(const signer::key_id& id);
   void save();

   [[nodiscard]] const std::filesystem::path& path() const noexcept;
   [[nodiscard]] std::shared_ptr<ownership> owner() const noexcept;
   [[nodiscard]] boost::asio::awaitable<std::vector<signer::key_info>> keys() override;
   [[nodiscard]] boost::asio::awaitable<signer::key_info> describe(const signer::key_id& id) override;
   [[nodiscard]] boost::asio::awaitable<signer::sign_digest_response>
   sign_digest(signer::sign_digest_request request) override;

   // Explicit synchronous entry points for bounded caller-owned blocking pools.
   // The async provider methods delegate to exactly these implementations.
   [[nodiscard]] std::vector<signer::key_info> keys_sync();
   [[nodiscard]] signer::key_info describe_sync(const signer::key_id& id);
   [[nodiscard]] signer::sign_digest_response sign_digest_sync(const signer::sign_digest_request& request);

 private:
   class impl;
   explicit store(std::unique_ptr<impl> implementation);

   std::unique_ptr<impl> impl_;
};

} // namespace forge::crypto::keystore
