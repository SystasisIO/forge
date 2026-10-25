#pragma once

#include "compiler_builtins.hxx"

namespace forge::tooling::testing {

class memory_driver;

struct host::impl : compiler_builtins {
   using wasm = forge::vm::wasm::interpret::interpreter;

   struct copy_arguments {
      void* destination;
      const void* source;
      std::uint32_t size;
   };

   struct compare_arguments {
      const void* left;
      const void* right;
      std::uint32_t size;
   };

   struct fill_arguments {
      void* destination;
      std::int32_t value;
      std::uint32_t size;
   };

   struct type_converter : forge::vm::wasm::interpret::type_converter<impl> {
      using base = forge::vm::wasm::interpret::type_converter<impl>;
      using base::base;
      using base::from_wasm;

      FORGE_VM_WASM_INTERPRET_FROM_WASM(copy_arguments, (forge::vm::wasm::interpret::wasm_ptr_t destination,
                                                         forge::vm::wasm::interpret::wasm_ptr_t source,
                                                         forge::vm::wasm::interpret::wasm_size_t size)) {
         auto* destination_ptr = this->template validate_pointer<char>(destination, size);
         const auto* source_ptr = this->template validate_pointer<const char>(source, size);
         this->template validate_pointer<char>(destination, 1);
         return {destination_ptr, source_ptr, size};
      }

      FORGE_VM_WASM_INTERPRET_FROM_WASM(compare_arguments, (forge::vm::wasm::interpret::wasm_ptr_t left,
                                                            forge::vm::wasm::interpret::wasm_ptr_t right,
                                                            forge::vm::wasm::interpret::wasm_size_t size)) {
         const auto* left_ptr = this->template validate_pointer<const char>(left, size);
         const auto* right_ptr = this->template validate_pointer<const char>(right, size);
         return {left_ptr, right_ptr, size};
      }

      FORGE_VM_WASM_INTERPRET_FROM_WASM(fill_arguments,
                                        (forge::vm::wasm::interpret::wasm_ptr_t destination, std::int32_t value,
                                         forge::vm::wasm::interpret::wasm_size_t size)) {
         auto* destination_ptr = this->template validate_pointer<char>(destination, size);
         this->template validate_pointer<char>(destination, 1);
         return {destination_ptr, value, size};
      }
   };

   using functions =
       forge::vm::wasm::interpret::registered_host_functions<impl, forge::vm::wasm::interpret::execution_interface,
                                                             type_converter>;
   using backend =
       forge::vm::wasm::interpret::backend<functions, wasm, forge::vm::wasm::interpret::compatibility_options>;
   template <typename T, std::size_t Alignment = alignof(T)>
   using input = forge::vm::wasm::interpret::argument_proxy<const T*, Alignment>;
   template <typename T, std::size_t Alignment = alignof(T)>
   using output = forge::vm::wasm::interpret::argument_proxy<T*, Alignment>;
   template <typename T, std::size_t Alignment = alignof(T)>
   using input_span = forge::vm::wasm::interpret::argument_proxy<forge::vm::wasm::interpret::span<const T>, Alignment>;
   template <typename T, std::size_t Alignment = alignof(T)>
   using output_span = forge::vm::wasm::interpret::argument_proxy<forge::vm::wasm::interpret::span<T>, Alignment>;
   using uint128_input = input<unsigned __int128, 16>;
   using uint128_output = output<unsigned __int128, 16>;
   using float128_input = input<float128, 16>;
   using float128_output = output<float128, 16>;
   using checksum160_input = input<capi_checksum160>;
   using checksum160_output = output<capi_checksum160>;
   using checksum256_input = input<capi_checksum256>;
   using checksum256_output = output<capi_checksum256>;
   using checksum512_input = input<capi_checksum512>;
   using checksum512_output = output<capi_checksum512>;

   explicit impl(execution_limits limits);
#if defined(FORGE_TOOLING_TESTING_EXPRESSION)
   impl(forge::expression::runtime& expressions, execution_limits limits);
#endif
   ~impl();

   invocation_result invoke(std::span<const std::uint8_t> code, std::uint64_t receiver, std::uint64_t first_receiver,
                            std::uint64_t action, std::vector<std::uint8_t> data, invocation_options options = {});

   std::uint32_t expression_compile_v1(std::span<const char> request, std::span<char> response);
   std::uint32_t expression_import_v1(std::span<const char> request, std::span<char> response);
   std::uint32_t expression_evaluate_v1(std::span<const char> request, std::span<char> response);
   std::optional<table> find_table(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name);
   std::optional<key_value> find_primary(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                         std::uint64_t primary);
   [[nodiscard]] std::vector<std::uint8_t> snapshot() const;
   template <typename Row, typename Index>
   std::optional<Row> find_secondary(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                     std::uint64_t primary);

   void transact(const std::function<void()>& body);
   void configure(oracle_state state);
   [[nodiscard]] oracle_state state() const;
   void register_contract(std::uint64_t account, std::vector<std::uint8_t> code);

   void require_recipient(std::uint64_t account);
   void require_auth(std::uint64_t account) const;
   [[nodiscard]] bool has_auth(std::uint64_t account) const;
   void require_auth2(std::uint64_t account, std::uint64_t permission) const;
   [[nodiscard]] bool is_account(std::uint64_t account) const;
   void send_inline(std::span<const char> action);
   void send_context_free_inline(std::span<const char> action);
   [[nodiscard]] std::uint64_t publication_time() const;
   std::uint32_t get_code_hash(std::uint64_t account, std::uint32_t version, output_span<char, 1> result) const;

   std::int64_t call(std::uint64_t receiver, std::uint64_t flags, std::span<const char> data);
   std::uint32_t get_call_return_value(std::span<char> destination) const;
   std::uint32_t get_call_data(std::span<char> destination) const;
   void set_call_return_value(std::span<const char> value);

   std::uint32_t get_active_producers(output_span<std::uint64_t> producers) const;

   void assert_sha256(std::span<const char> data, checksum256_input expected) const;
   void assert_sha1(std::span<const char> data, checksum160_input expected) const;
   void assert_sha512(std::span<const char> data, checksum512_input expected) const;
   void assert_ripemd160(std::span<const char> data, checksum160_input expected) const;
   void sha256(std::span<const char> data, checksum256_output result) const;
   void sha1(std::span<const char> data, checksum160_output result) const;
   void sha512(std::span<const char> data, checksum512_output result) const;
   void ripemd160(std::span<const char> data, checksum160_output result) const;
   std::int32_t recover_key(checksum256_input digest, std::span<const char> signature,
                            std::span<char> public_key) const;
   void assert_recover_key(checksum256_input digest, std::span<const char> signature,
                           std::span<const char> public_key) const;

   std::int32_t bls_g1_add(std::span<const char> left, std::span<const char> right, std::span<char> result) const;
   std::int32_t bls_g2_add(std::span<const char> left, std::span<const char> right, std::span<char> result) const;
   std::int32_t bls_g1_weighted_sum(std::span<const char> points, std::span<const char> scalars, std::uint32_t count,
                                    std::span<char> result) const;
   std::int32_t bls_g2_weighted_sum(std::span<const char> points, std::span<const char> scalars, std::uint32_t count,
                                    std::span<char> result) const;
   std::int32_t bls_pairing(std::span<const char> g1_points, std::span<const char> g2_points, std::uint32_t count,
                            std::span<char> result) const;
   std::int32_t bls_g1_map(std::span<const char> element, std::span<char> result) const;
   std::int32_t bls_g2_map(std::span<const char> element, std::span<char> result) const;
   std::int32_t bls_fp_mod(std::span<const char> scalar, std::span<char> result) const;
   std::int32_t bls_fp_mul(std::span<const char> left, std::span<const char> right, std::span<char> result) const;
   std::int32_t bls_fp_exp(std::span<const char> base, std::span<const char> exponent, std::span<char> result) const;
   void sha3(std::span<const char> data, std::span<char> hash, std::int32_t keccak) const;
   std::int32_t blake2_f(std::uint32_t rounds, std::span<const char> state, std::span<const char> message,
                         std::span<const char> offset0, std::span<const char> offset1, std::int32_t final,
                         std::span<char> result) const;
   std::int32_t k1_recover(std::span<const char> signature, std::span<const char> digest,
                           std::span<char> public_key) const;
   std::int32_t alt_bn128_add(std::span<const char> left, std::span<const char> right, std::span<char> result) const;
   std::int32_t alt_bn128_mul(std::span<const char> point, std::span<const char> scalar, std::span<char> result) const;
   std::int32_t alt_bn128_pair(std::span<const char> pairs) const;
   std::int32_t mod_exp(std::span<const char> base, std::span<const char> exponent, std::span<const char> modulus,
                        std::span<char> result) const;

   std::int32_t check_transaction_authorization(std::span<const char> transaction, std::span<const char> public_keys,
                                                std::span<const char> permissions) const;
   std::int32_t check_permission_authorization(std::uint64_t account, std::uint64_t permission,
                                               std::span<const char> public_keys, std::span<const char> permissions,
                                               std::uint64_t delay_us) const;
   std::int64_t get_permission_last_used(std::uint64_t account, std::uint64_t permission) const;
   std::int64_t get_account_creation_time(std::uint64_t account) const;

   void prints(input<const char, 1> value);
   void prints_l(std::span<const char> value);
   void printi(std::int64_t value);
   void printui(std::uint64_t value);
   void printi128(input<__int128, 16> value);
   void printui128(uint128_input value);
   void printsf(float value);
   void printdf(double value);
   void printqf(float128_input value);
   void printn(std::uint64_t value);
   void printhex(std::span<const char> value);

   void get_resource_limits(std::uint64_t account, output<std::int64_t> ram_bytes, output<std::int64_t> net_weight,
                            output<std::int64_t> cpu_weight) const;
   void set_resource_limits(std::uint64_t account, std::int64_t ram_bytes, std::int64_t net_weight,
                            std::int64_t cpu_weight);
   std::int64_t set_proposed_producers(std::span<const char> producer_data);
   std::int64_t set_proposed_producers_ex(std::uint64_t format, std::span<const char> producer_data);
   [[nodiscard]] bool is_privileged(std::uint64_t account) const;
   void set_privileged(std::uint64_t account, bool value);
   void set_blockchain_parameters_packed(std::span<const char> data);
   std::uint32_t get_blockchain_parameters_packed(std::span<char> data) const;
   void set_kv_parameters_packed(std::span<const char> data);
   std::uint32_t get_wasm_parameters_packed(std::span<char> data, std::uint32_t max_version) const;
   void set_wasm_parameters_packed(std::span<const char> data);
   std::uint32_t get_parameters_packed(std::span<const char> ids, std::span<char> data) const;
   void set_parameters_packed(std::span<const char> data);
   void preactivate_feature(checksum256_input digest);
   void set_finalizers(std::uint64_t format, std::span<const char> data);

   [[noreturn]] void abort();
   void* memcpy(copy_arguments arguments);
   void* memmove(copy_arguments arguments);
   std::int32_t memcmp(compare_arguments arguments) const;
   void* memset(fill_arguments arguments);

   void eosio_assert(std::uint32_t test, input<const char, 1> message);
   void eosio_assert_message(std::uint32_t test, forge::vm::wasm::interpret::span<const char> message);
   void eosio_assert_code(std::uint32_t test, std::uint64_t code);
   [[noreturn]] void eosio_exit(std::int32_t code);
   std::uint32_t action_data_size() const;
   std::uint32_t read_action_data(forge::vm::wasm::interpret::span<char> destination) const;
   void set_action_return_value(forge::vm::wasm::interpret::span<const char> value);
   std::uint64_t current_receiver() const;
   [[nodiscard]] std::uint64_t current_time() const;
   [[nodiscard]] std::uint32_t get_block_num() const;
   [[nodiscard]] bool is_feature_activated(checksum256_input digest) const;
   [[nodiscard]] std::uint64_t get_sender() const;

   void send_deferred(input<unsigned __int128, 16> sender_id, std::uint64_t payer, std::span<const char> transaction,
                      std::uint32_t replace_existing);
   std::int32_t cancel_deferred(input<unsigned __int128, 16> sender_id);
   std::uint32_t read_transaction(std::span<char> destination) const;
   [[nodiscard]] std::uint32_t transaction_size() const;
   [[nodiscard]] std::int32_t tapos_block_num() const;
   [[nodiscard]] std::int32_t tapos_block_prefix() const;
   [[nodiscard]] std::uint32_t expiration() const;
   std::int32_t get_action(std::uint32_t type, std::uint32_t index, std::span<char> destination) const;
   std::int32_t get_context_free_data(std::uint32_t index, std::span<char> destination) const;

   std::int32_t db_store_i64(std::uint64_t scope, std::uint64_t table_name, std::uint64_t payer, std::uint64_t primary,
                             forge::vm::wasm::interpret::span<const char> value);
   void db_update_i64(std::int32_t iterator, std::uint64_t payer, forge::vm::wasm::interpret::span<const char> value);
   void db_remove_i64(std::int32_t iterator);
   std::int32_t db_get_i64(std::int32_t iterator, output_span<char, 1> value);
   std::int32_t db_next_i64(std::int32_t iterator, output<std::uint64_t> primary);
   std::int32_t db_previous_i64(std::int32_t iterator, output<std::uint64_t> primary);
   std::int32_t db_find_i64(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name, std::uint64_t primary);
   std::int32_t db_lowerbound_i64(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                  std::uint64_t primary);
   std::int32_t db_upperbound_i64(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                  std::uint64_t primary);
   std::int32_t db_end_i64(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name);

#define FORGE_CONTRACT_TEST_DECLARE_SECONDARY(prefix, type, input_type, output_type)                                   \
   std::int32_t prefix##_store(std::uint64_t scope, std::uint64_t table_name, std::uint64_t payer,                     \
                               std::uint64_t primary, input_type secondary);                                           \
   void prefix##_update(std::int32_t iterator, std::uint64_t payer, input_type secondary);                             \
   void prefix##_remove(std::int32_t iterator);                                                                        \
   std::int32_t prefix##_next(std::int32_t iterator, output<std::uint64_t> primary);                                   \
   std::int32_t prefix##_previous(std::int32_t iterator, output<std::uint64_t> primary);                               \
   std::int32_t prefix##_find_primary(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,               \
                                      output_type secondary, std::uint64_t primary);                                   \
   std::int32_t prefix##_find_secondary(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,             \
                                        input_type secondary, output<std::uint64_t> primary);                          \
   std::int32_t prefix##_lowerbound(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,                 \
                                    output_type secondary, output<std::uint64_t> primary);                             \
   std::int32_t prefix##_upperbound(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,                 \
                                    output_type secondary, output<std::uint64_t> primary);                             \
   std::int32_t prefix##_end(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name)

   FORGE_CONTRACT_TEST_DECLARE_SECONDARY(db_idx64, std::uint64_t, input<std::uint64_t>, output<std::uint64_t>);
   FORGE_CONTRACT_TEST_DECLARE_SECONDARY(db_idx128, unsigned __int128, uint128_input, uint128_output);

   std::int32_t db_idx256_store(std::uint64_t scope, std::uint64_t table_name, std::uint64_t payer,
                                std::uint64_t primary, input_span<unsigned __int128, 16> secondary);
   void db_idx256_update(std::int32_t iterator, std::uint64_t payer, input_span<unsigned __int128, 16> secondary);
   void db_idx256_remove(std::int32_t iterator);
   std::int32_t db_idx256_next(std::int32_t iterator, output<std::uint64_t> primary);
   std::int32_t db_idx256_previous(std::int32_t iterator, output<std::uint64_t> primary);
   std::int32_t db_idx256_find_primary(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                       output_span<unsigned __int128, 16> secondary, std::uint64_t primary);
   std::int32_t db_idx256_find_secondary(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                         input_span<unsigned __int128, 16> secondary, output<std::uint64_t> primary);
   std::int32_t db_idx256_lowerbound(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                     output_span<unsigned __int128, 16> secondary, output<std::uint64_t> primary);
   std::int32_t db_idx256_upperbound(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                     output_span<unsigned __int128, 16> secondary, output<std::uint64_t> primary);
   std::int32_t db_idx256_end(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name);

   FORGE_CONTRACT_TEST_DECLARE_SECONDARY(db_idx_double, double, input<double>, output<double>);
   FORGE_CONTRACT_TEST_DECLARE_SECONDARY(db_idx_long_double, float128, float128_input, float128_output);

#undef FORGE_CONTRACT_TEST_DECLARE_SECONDARY

   enum class row_kind : std::uint8_t {
      primary,
      index64,
      index128,
      index256,
      index_double,
      index_long_double,
   };

   struct iterator_entry {
      row_kind kind = row_kind::primary;
      forge::db::ids::object_id id;
      table::id_t table_id;
      bool erased = false;
   };

   template <typename T> T run(boost::asio::awaitable<T> operation);
   void run(boost::asio::awaitable<void> operation);
   void begin_invocation(std::uint64_t receiver, std::uint64_t first_receiver, std::vector<std::uint8_t> data,
                         invocation_options options);
   void commit_invocation();
   void rollback_invocation();
   [[nodiscard]] std::chrono::steady_clock::duration remaining_execution_time() const;
   template <typename... Args> void execute(backend& vm, std::string_view function, Args&&... args) {
      vm.with_invocation(this, forge::vm::wasm::interpret::compatibility_options{}, remaining_execution_time(),
                         invocation_cancel_, [&] {
                            static_cast<void>(remaining_execution_time());
                            vm(*this, "env", function, std::forward<Args>(args)...);
                         });
   }
   void register_intrinsics();
   void require_writable() const;
   std::int32_t cache(row_kind kind, forge::db::ids::object_id id, table::id_t table_id);
   std::int32_t end(row_kind kind, table::id_t table_id);
   iterator_entry& require_iterator(std::int32_t iterator, row_kind kind);
   table::id_t require_end(std::int32_t iterator, row_kind kind) const;
   void erase_iterator(std::int32_t iterator);

   template <typename Row, typename Index, row_kind Kind, typename Secondary>
   std::int32_t secondary_store(std::uint64_t scope, std::uint64_t table_name, std::uint64_t payer,
                                std::uint64_t primary, Secondary secondary);
   template <typename Row, typename Index, row_kind Kind, typename Secondary>
   void secondary_update(std::int32_t iterator, std::uint64_t payer, Secondary secondary);
   template <typename Row, typename Index, row_kind Kind> void secondary_remove(std::int32_t iterator);
   template <typename Row, typename Index, row_kind Kind>
   std::int32_t secondary_next(std::int32_t iterator, std::uint64_t& primary);
   template <typename Row, typename Index, row_kind Kind>
   std::int32_t secondary_previous(std::int32_t iterator, std::uint64_t& primary);
   template <typename Row, typename Index, row_kind Kind, typename Secondary>
   std::int32_t secondary_find_primary(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                       Secondary& secondary, std::uint64_t primary);
   template <typename Row, typename Index, row_kind Kind, typename Secondary>
   std::int32_t secondary_find_secondary(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                         const Secondary& secondary, std::uint64_t& primary);
   template <typename Row, typename Index, row_kind Kind, typename Secondary>
   std::int32_t secondary_bound(bool upper, std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                Secondary& secondary, std::uint64_t& primary);
   template <typename Row, typename Index, row_kind Kind>
   std::int32_t secondary_end(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name);

   forge::asio::runtime runtime_;
   execution_limits limits_;
#if defined(FORGE_TOOLING_TESTING_EXPRESSION)
   forge::expression::runtime* expressions_ = nullptr;
#endif
   std::shared_ptr<memory_driver> driver_;
   std::optional<forge::db::object::store> store_;
   std::optional<forge::db::object::transaction> transaction_;
   bool group_active_ = false;
   bool invocation_active_ = false;
   bool group_failed_ = false;
   oracle_state state_;
   std::optional<oracle_state> state_before_invocation_;
   std::optional<std::chrono::steady_clock::time_point> invocation_deadline_;
   std::stop_token invocation_cancel_;
   std::map<std::uint64_t, std::vector<std::uint8_t>> contracts_;
   std::uint64_t receiver_ = 0;
   std::uint64_t first_receiver_ = 0;
   bool read_only_ = false;
   std::vector<std::uint8_t> action_data_;
   std::vector<std::uint8_t> call_data_;
   std::vector<std::uint8_t> call_return_value_;
   std::vector<std::uint8_t> last_call_return_value_;
   invocation_result result_;
   std::vector<iterator_entry> iterators_;
   std::map<std::tuple<row_kind, std::uint64_t>, std::int32_t> object_iterators_;
   std::map<std::tuple<row_kind, std::uint64_t>, std::int32_t> end_iterators_;
   std::int32_t next_end_ = -2;
};

} // namespace forge::tooling::testing
