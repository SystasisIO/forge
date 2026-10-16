#pragma once

namespace forge::plugins::crypto::secrets {

struct loaded_secret;
struct version_catalog;

class version_store : public std::enable_shared_from_this<version_store> {
 public:
   version_store(version_store_config settings, forge::crypto::keystore::decrypt_limits limits,
                 forge::app::plugin_lifecycle lifecycle);
   ~version_store();

   boost::asio::awaitable<void> initialize(forge::asio::compute::executor compute);
   void request_stop() noexcept;
   boost::asio::awaitable<void> shutdown();

   boost::asio::awaitable<version_mutation_result> create(create_version_request value);
   boost::asio::awaitable<version_mutation_result> revoke(revoke_version_request value);
   boost::asio::awaitable<version_get_result> get(version_get_request value);
   boost::asio::awaitable<version_derive_result> derive(version_derive_request value);
   boost::asio::awaitable<version_aead_encrypt_result> encrypt(version_aead_encrypt_request value);
   boost::asio::awaitable<version_aead_decrypt_result> decrypt(version_aead_decrypt_request value);
   boost::asio::awaitable<version_metadata> describe(version_query value);
   boost::asio::awaitable<version_page> list(version_list_request value);
   boost::asio::awaitable<version_snapshot> status_async();
   version_snapshot status() const;

 private:
   class reservation {
    public:
      reservation(std::shared_ptr<version_store> owner, std::uint64_t bytes, bool crypto);
      ~reservation();
      reservation(const reservation&) = delete;
      reservation& operator=(const reservation&) = delete;

      void activate() noexcept;

    private:
      std::shared_ptr<version_store> _owner;
      std::uint64_t _bytes;
      bool _crypto;
      bool _active = false;
   };

   template <typename Result>
   static boost::asio::awaitable<Result> isolated(boost::asio::awaitable<Result> work, bool admit = true);

   template <typename Work> boost::asio::awaitable<std::invoke_result_t<Work>> blocking(Work work);

   static boost::asio::awaitable<void> initialize_owned(std::shared_ptr<version_store> owner,
                                                        forge::asio::compute::executor compute);
   static boost::asio::awaitable<void> shutdown_owned(std::shared_ptr<version_store> owner);
   static boost::asio::awaitable<version_mutation_result> create_owned(std::shared_ptr<version_store> owner,
                                                                       create_version_request value);
   static boost::asio::awaitable<version_mutation_result> revoke_owned(std::shared_ptr<version_store> owner,
                                                                       revoke_version_request value);
   static boost::asio::awaitable<version_get_result> get_owned(std::shared_ptr<version_store> owner,
                                                               version_get_request value);
   static boost::asio::awaitable<version_derive_result> derive_owned(std::shared_ptr<version_store> owner,
                                                                     version_derive_request value);
   static boost::asio::awaitable<version_aead_encrypt_result> encrypt_owned(std::shared_ptr<version_store> owner,
                                                                            version_aead_encrypt_request value);
   static boost::asio::awaitable<version_aead_decrypt_result> decrypt_owned(std::shared_ptr<version_store> owner,
                                                                            version_aead_decrypt_request value);
   static boost::asio::awaitable<version_metadata> describe_owned(std::shared_ptr<version_store> owner,
                                                                  version_query value);
   static boost::asio::awaitable<version_page> list_owned(std::shared_ptr<version_store> owner,
                                                          version_list_request value);
   static boost::asio::awaitable<version_snapshot> status_owned(std::shared_ptr<version_store> owner);

   std::unique_ptr<reservation> reserve(std::uint64_t bytes, bool crypto = false);
   void check_process() const;
   void check_start() const;
   void release(std::uint64_t bytes, bool crypto) noexcept;
   void open();
   void clear() noexcept;
   void validate_config();
   void validate_catalog(const version_catalog& value) const;
   void require_capacity(const version_catalog& value, std::uint64_t encoded_size) const;
   version_policy resolve_policy(version_policy value) const;
   static void validate_id(std::string_view value, std::uint64_t maximum);
   static void validate_ref(const version_ref& value);
   static void validate_policy_shape(const version_policy& value);
   static version_policy policy_of(const loaded_secret& value);
   std::shared_ptr<const version_catalog> current();
   std::shared_ptr<const loaded_secret> select(const version_ref& ref);
   version_metadata describe_sync(version_query value);
   version_page list_sync(version_list_request value);
   version_mutation_result create_sync(create_version_request value);
   version_mutation_result revoke_sync(revoke_version_request value);
   std::array<std::uint8_t, 32> fingerprint(const create_version_request& value, const version_catalog& catalog) const;
   std::array<std::uint8_t, 32> fingerprint(const revoke_version_request& value, const version_catalog& catalog) const;
   void persist(std::shared_ptr<version_catalog> candidate, bool replace = true);
   void publish(std::shared_ptr<const version_catalog> candidate, bool fault) noexcept;
   void fence() noexcept;
   std::uint64_t input_size(const create_version_request& value) const;

   const version_store_config _settings;
   const forge::crypto::keystore::decrypt_limits _limits;
   const forge::app::plugin_lifecycle _lifecycle;
   const std::uint64_t _process;
   mutable std::mutex _mutex;
   forge::asio::gate _write_gate;
   forge::asio::notification _drained;
   forge::asio::compute::executor _compute;
   std::shared_ptr<forge::crypto::keystore::ownership> _owner;
   std::shared_ptr<forge::crypto::keystore::ownership::access> _access;
   forge::crypto::core::secret_string _password;
   std::shared_ptr<const version_catalog> _catalog;
   std::uint64_t _pending = 0;
   std::uint64_t _pending_bytes = 0;
   std::uint64_t _crypto_calls = 0;
   bool _ready = false;
   bool _fault = false;
   bool _stopping = false;
};

} // namespace forge::plugins::crypto::secrets
