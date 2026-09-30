#pragma once

namespace forge::crypto::keystore {

class store::impl {
 public:
   impl(std::shared_ptr<ownership> owner, core::secret_string password, store_options options);

   static std::unique_ptr<impl> create(std::filesystem::path path, core::secret_string password, store_options options);
   static std::unique_ptr<impl> open(std::filesystem::path path, core::secret_string password, store_options options);
   static std::unique_ptr<impl> create(std::shared_ptr<ownership> owner, core::secret_string password,
                                       store_options options);
   static std::unique_ptr<impl> open(std::shared_ptr<ownership> owner, core::secret_string password,
                                     store_options options);

   void put(signer::key_id id, asymmetric::private_key key, bool replace);
   void erase(const signer::key_id& id);
   void save();
   std::vector<signer::key_info> keys() const;
   signer::key_info describe(const signer::key_id& id) const;
   signer::sign_digest_response sign_digest(const signer::sign_digest_request& request) const;

   const std::filesystem::path& path() const noexcept;
   std::shared_ptr<ownership> owner() const noexcept;

 private:
   struct entry {
      core::secret_string encoded;
      asymmetric::public_key public_key;
   };

   using entries = std::map<std::string, entry, std::less<>>;

   static void validate_options(const store_options& options);
   void validate_id(const signer::key_id& id) const;
   void load();
   void write_entries(const entries& value, bool replace = true) const;
   void save_locked() const;

   std::filesystem::path path_;
   std::shared_ptr<ownership> owner_;
   std::shared_ptr<ownership::access> access_;
   core::secret_string password_;
   store_options options_;
   entries entries_;
   mutable std::mutex mutex_;
};

} // namespace forge::crypto::keystore
