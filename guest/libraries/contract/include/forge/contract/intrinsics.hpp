#pragma once

// Canonical union of CDT 69599db and Spring e6a99f6 contract imports.
// db_get_i64 keeps CDT's historical const-qualified output buffer for source compatibility.
#define FORGE_CONTRACT_BASE_INTRINSICS(FORGE_CONTRACT_INTRINSIC)                                                       \
   FORGE_CONTRACT_INTRINSIC(1, core, action, none, read_action_data, env, read_action_data, std::uint32_t,             \
                            (void* msg, std::uint32_t len))                                                            \
   FORGE_CONTRACT_INTRINSIC(1, core, action, none, action_data_size, env, action_data_size, std::uint32_t, ())         \
   FORGE_CONTRACT_INTRINSIC(1, core, action, none, require_recipient, env, require_recipient, void,                    \
                            (std::uint64_t name))                                                                      \
   FORGE_CONTRACT_INTRINSIC(1, core, action, none, require_auth, env, require_auth, void, (std::uint64_t name))        \
   FORGE_CONTRACT_INTRINSIC(1, core, action, none, has_auth, env, has_auth, bool, (std::uint64_t name))                \
   FORGE_CONTRACT_INTRINSIC(1, core, action, none, require_auth2, env, require_auth2, void,                            \
                            (std::uint64_t name, std::uint64_t permission))                                            \
   FORGE_CONTRACT_INTRINSIC(1, core, action, none, is_account, env, is_account, bool, (std::uint64_t name))            \
   FORGE_CONTRACT_INTRINSIC(1, core, action, none, send_inline, env, send_inline, void,                                \
                            (char* serialized_action, forge_contract_size_t size))                                     \
   FORGE_CONTRACT_INTRINSIC(1, core, action, none, send_context_free_inline, env, send_context_free_inline, void,      \
                            (char* serialized_action, forge_contract_size_t size))                                     \
   FORGE_CONTRACT_INTRINSIC(1, core, action, none, publication_time, env, publication_time, std::uint64_t, ())         \
   FORGE_CONTRACT_INTRINSIC(1, core, action, none, current_receiver, env, current_receiver, std::uint64_t, ())         \
   FORGE_CONTRACT_INTRINSIC(                                                                                           \
       1, core, action, get_code_hash, get_code_hash, env, get_code_hash, std::uint32_t,                               \
       (std::uint64_t account, std::uint32_t struct_version, char* result_buffer, forge_contract_size_t buffer_size))  \
   FORGE_CONTRACT_INTRINSIC(1, core, action, action_return_value, set_action_return_value, env,                        \
                            set_action_return_value, void, (const char* return_value, forge_contract_size_t size))     \
   FORGE_CONTRACT_INTRINSIC(                                                                                           \
       1, call, call, none, call, env, call, std::int64_t,                                                             \
       (std::uint64_t receiver, std::uint64_t flags, const char* data, forge_contract_size_t data_size))               \
   FORGE_CONTRACT_INTRINSIC(1, call, call, none, get_call_return_value, env, get_call_return_value, std::uint32_t,     \
                            (void* mem, std::uint32_t len))                                                            \
   FORGE_CONTRACT_INTRINSIC(1, call, call, none, get_call_data, env, get_call_data, std::uint32_t,                     \
                            (void* mem, std::uint32_t len))                                                            \
   FORGE_CONTRACT_INTRINSIC(1, call, call, none, set_call_return_value, env, set_call_return_value, void,              \
                            (void* mem, std::uint32_t len))                                                            \
   FORGE_CONTRACT_INTRINSIC(1, core, chain, none, get_active_producers, env, get_active_producers, std::uint32_t,      \
                            (std::uint64_t* producers, std::uint32_t datalen))                                         \
   FORGE_CONTRACT_INTRINSIC(1, core, crypto, none, assert_sha256, env, assert_sha256, void,                            \
                            (const char* data, std::uint32_t length, const capi_checksum256* hash))                    \
   FORGE_CONTRACT_INTRINSIC(1, core, crypto, none, assert_sha1, env, assert_sha1, void,                                \
                            (const char* data, std::uint32_t length, const capi_checksum160* hash))                    \
   FORGE_CONTRACT_INTRINSIC(1, core, crypto, none, assert_sha512, env, assert_sha512, void,                            \
                            (const char* data, std::uint32_t length, const capi_checksum512* hash))                    \
   FORGE_CONTRACT_INTRINSIC(1, core, crypto, none, assert_ripemd160, env, assert_ripemd160, void,                      \
                            (const char* data, std::uint32_t length, const capi_checksum160* hash))                    \
   FORGE_CONTRACT_INTRINSIC(1, core, crypto, none, sha256, env, sha256, void,                                          \
                            (const char* data, std::uint32_t length, capi_checksum256* hash))                          \
   FORGE_CONTRACT_INTRINSIC(1, core, crypto, none, sha1, env, sha1, void,                                              \
                            (const char* data, std::uint32_t length, capi_checksum160* hash))                          \
   FORGE_CONTRACT_INTRINSIC(1, core, crypto, none, sha512, env, sha512, void,                                          \
                            (const char* data, std::uint32_t length, capi_checksum512* hash))                          \
   FORGE_CONTRACT_INTRINSIC(1, core, crypto, none, ripemd160, env, ripemd160, void,                                    \
                            (const char* data, std::uint32_t length, capi_checksum160* hash))                          \
   FORGE_CONTRACT_INTRINSIC(1, core, crypto, none, recover_key, env, recover_key, std::int32_t,                        \
                            (const capi_checksum256* digest, const char* sig, forge_contract_size_t siglen, char* pub, \
                             forge_contract_size_t publen))                                                            \
   FORGE_CONTRACT_INTRINSIC(1, core, crypto, none, assert_recover_key, env, assert_recover_key, void,                  \
                            (const capi_checksum256* digest, const char* sig, forge_contract_size_t siglen,            \
                             const char* pub, forge_contract_size_t publen))                                           \
   FORGE_CONTRACT_INTRINSIC(1, bls, crypto_bls_ext, bls_primitives, bls_g1_add, env, bls_g1_add, std::int32_t,         \
                            (const char* op1, std::uint32_t op1_len, const char* op2, std::uint32_t op2_len,           \
                             char* res, std::uint32_t res_len))                                                        \
   FORGE_CONTRACT_INTRINSIC(1, bls, crypto_bls_ext, bls_primitives, bls_g2_add, env, bls_g2_add, std::int32_t,         \
                            (const char* op1, std::uint32_t op1_len, const char* op2, std::uint32_t op2_len,           \
                             char* res, std::uint32_t res_len))                                                        \
   FORGE_CONTRACT_INTRINSIC(1, bls, crypto_bls_ext, bls_primitives, bls_g1_weighted_sum, env, bls_g1_weighted_sum,     \
                            std::int32_t,                                                                              \
                            (const char* points, std::uint32_t points_len, const char* scalars,                        \
                             std::uint32_t scalars_len, std::uint32_t n, char* res, std::uint32_t res_len))            \
   FORGE_CONTRACT_INTRINSIC(1, bls, crypto_bls_ext, bls_primitives, bls_g2_weighted_sum, env, bls_g2_weighted_sum,     \
                            std::int32_t,                                                                              \
                            (const char* points, std::uint32_t points_len, const char* scalars,                        \
                             std::uint32_t scalars_len, std::uint32_t n, char* res, std::uint32_t res_len))            \
   FORGE_CONTRACT_INTRINSIC(1, bls, crypto_bls_ext, bls_primitives, bls_pairing, env, bls_pairing, std::int32_t,       \
                            (const char* g1_points, std::uint32_t g1_points_len, const char* g2_points,                \
                             std::uint32_t g2_points_len, std::uint32_t n, char* res, std::uint32_t res_len))          \
   FORGE_CONTRACT_INTRINSIC(1, bls, crypto_bls_ext, bls_primitives, bls_g1_map, env, bls_g1_map, std::int32_t,         \
                            (const char* e, std::uint32_t e_len, char* res, std::uint32_t res_len))                    \
   FORGE_CONTRACT_INTRINSIC(1, bls, crypto_bls_ext, bls_primitives, bls_g2_map, env, bls_g2_map, std::int32_t,         \
                            (const char* e, std::uint32_t e_len, char* res, std::uint32_t res_len))                    \
   FORGE_CONTRACT_INTRINSIC(1, bls, crypto_bls_ext, bls_primitives, bls_fp_mod, env, bls_fp_mod, std::int32_t,         \
                            (const char* s, std::uint32_t s_len, char* res, std::uint32_t res_len))                    \
   FORGE_CONTRACT_INTRINSIC(1, bls, crypto_bls_ext, bls_primitives, bls_fp_mul, env, bls_fp_mul, std::int32_t,         \
                            (const char* op1, std::uint32_t op1_len, const char* op2, std::uint32_t op2_len,           \
                             char* res, std::uint32_t res_len))                                                        \
   FORGE_CONTRACT_INTRINSIC(1, bls, crypto_bls_ext, bls_primitives, bls_fp_exp, env, bls_fp_exp, std::int32_t,         \
                            (const char* base, std::uint32_t base_len, const char* exp, std::uint32_t exp_len,         \
                             char* res, std::uint32_t res_len))                                                        \
   FORGE_CONTRACT_INTRINSIC(                                                                                           \
       1, crypto_ext, crypto_ext, crypto_primitives, sha3, env, sha3, void,                                            \
       (const char* data, std::uint32_t data_len, char* hash, std::uint32_t hash_len, std::int32_t keccak))            \
   FORGE_CONTRACT_INTRINSIC(1, crypto_ext, crypto_ext, crypto_primitives, blake2_f, env, blake2_f, std::int32_t,       \
                            (std::uint32_t rounds, const char* state, std::uint32_t state_len, const char* msg,        \
                             std::uint32_t msg_len, const char* t0_offset, std::uint32_t t0_len,                       \
                             const char* t1_offset, std::uint32_t t1_len, std::int32_t final, char* result,            \
                             std::uint32_t result_len))                                                                \
   FORGE_CONTRACT_INTRINSIC(1, crypto_ext, crypto_ext, crypto_primitives, k1_recover, env, k1_recover, std::int32_t,   \
                            (const char* sig, std::uint32_t sig_len, const char* dig, std::uint32_t dig_len,           \
                             char* pub, std::uint32_t pub_len))                                                        \
   FORGE_CONTRACT_INTRINSIC(1, crypto_ext, crypto_ext, crypto_primitives, alt_bn128_add, env, alt_bn128_add,           \
                            std::int32_t,                                                                              \
                            (const char* op1, std::uint32_t op1_len, const char* op2, std::uint32_t op2_len,           \
                             char* result, std::uint32_t result_len))                                                  \
   FORGE_CONTRACT_INTRINSIC(1, crypto_ext, crypto_ext, crypto_primitives, alt_bn128_mul, env, alt_bn128_mul,           \
                            std::int32_t,                                                                              \
                            (const char* g1, std::uint32_t g1_len, const char* scalar, std::uint32_t scalar_len,       \
                             char* result, std::uint32_t result_len))                                                  \
   FORGE_CONTRACT_INTRINSIC(1, crypto_ext, crypto_ext, crypto_primitives, alt_bn128_pair, env, alt_bn128_pair,         \
                            std::int32_t, (const char* pairs, std::uint32_t pairs_len))                                \
   FORGE_CONTRACT_INTRINSIC(1, crypto_ext, crypto_ext, crypto_primitives, mod_exp, env, mod_exp, std::int32_t,         \
                            (const char* base, std::uint32_t base_len, const char* exp, std::uint32_t exp_len,         \
                             const char* mod, std::uint32_t mod_len, char* result, std::uint32_t result_len))          \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_store_i64, env, db_store_i64, std::int32_t,                      \
                            (std::uint64_t scope, std::uint64_t table, std::uint64_t payer, std::uint64_t id,          \
                             const void* data, std::uint32_t len))                                                     \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_update_i64, env, db_update_i64, void,                            \
                            (std::int32_t iterator, std::uint64_t payer, const void* data, std::uint32_t len))         \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_remove_i64, env, db_remove_i64, void, (std::int32_t iterator))   \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_get_i64, env, db_get_i64, std::int32_t,                          \
                            (std::int32_t iterator, const void* data, std::uint32_t len))                              \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_next_i64, env, db_next_i64, std::int32_t,                        \
                            (std::int32_t iterator, std::uint64_t* primary))                                           \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_previous_i64, env, db_previous_i64, std::int32_t,                \
                            (std::int32_t iterator, std::uint64_t* primary))                                           \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_find_i64, env, db_find_i64, std::int32_t,                        \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table, std::uint64_t id))          \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_lowerbound_i64, env, db_lowerbound_i64, std::int32_t,            \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table, std::uint64_t id))          \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_upperbound_i64, env, db_upperbound_i64, std::int32_t,            \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table, std::uint64_t id))          \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_end_i64, env, db_end_i64, std::int32_t,                          \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table))                            \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx64_store, env, db_idx64_store, std::int32_t,                  \
                            (std::uint64_t scope, std::uint64_t table, std::uint64_t payer, std::uint64_t id,          \
                             const std::uint64_t* secondary))                                                          \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx64_update, env, db_idx64_update, void,                        \
                            (std::int32_t iterator, std::uint64_t payer, const std::uint64_t* secondary))              \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx64_remove, env, db_idx64_remove, void,                        \
                            (std::int32_t iterator))                                                                   \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx64_next, env, db_idx64_next, std::int32_t,                    \
                            (std::int32_t iterator, std::uint64_t* primary))                                           \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx64_previous, env, db_idx64_previous, std::int32_t,            \
                            (std::int32_t iterator, std::uint64_t* primary))                                           \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx64_find_primary, env, db_idx64_find_primary, std::int32_t,    \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table, std::uint64_t* secondary,   \
                             std::uint64_t primary))                                                                   \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx64_find_secondary, env, db_idx64_find_secondary,              \
                            std::int32_t,                                                                              \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table,                             \
                             const std::uint64_t* secondary, std::uint64_t* primary))                                  \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx64_lowerbound, env, db_idx64_lowerbound, std::int32_t,        \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table, std::uint64_t* secondary,   \
                             std::uint64_t* primary))                                                                  \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx64_upperbound, env, db_idx64_upperbound, std::int32_t,        \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table, std::uint64_t* secondary,   \
                             std::uint64_t* primary))                                                                  \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx64_end, env, db_idx64_end, std::int32_t,                      \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table))                            \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx128_store, env, db_idx128_store, std::int32_t,                \
                            (std::uint64_t scope, std::uint64_t table, std::uint64_t payer, std::uint64_t id,          \
                             const unsigned __int128* secondary))                                                      \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx128_update, env, db_idx128_update, void,                      \
                            (std::int32_t iterator, std::uint64_t payer, const unsigned __int128* secondary))          \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx128_remove, env, db_idx128_remove, void,                      \
                            (std::int32_t iterator))                                                                   \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx128_next, env, db_idx128_next, std::int32_t,                  \
                            (std::int32_t iterator, std::uint64_t* primary))                                           \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx128_previous, env, db_idx128_previous, std::int32_t,          \
                            (std::int32_t iterator, std::uint64_t* primary))                                           \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx128_find_primary, env, db_idx128_find_primary, std::int32_t,  \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table,                             \
                             unsigned __int128* secondary, std::uint64_t primary))                                     \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx128_find_secondary, env, db_idx128_find_secondary,            \
                            std::int32_t,                                                                              \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table,                             \
                             const unsigned __int128* secondary, std::uint64_t* primary))                              \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx128_lowerbound, env, db_idx128_lowerbound, std::int32_t,      \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table,                             \
                             unsigned __int128* secondary, std::uint64_t* primary))                                    \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx128_upperbound, env, db_idx128_upperbound, std::int32_t,      \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table,                             \
                             unsigned __int128* secondary, std::uint64_t* primary))                                    \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx128_end, env, db_idx128_end, std::int32_t,                    \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table))                            \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx256_store, env, db_idx256_store, std::int32_t,                \
                            (std::uint64_t scope, std::uint64_t table, std::uint64_t payer, std::uint64_t id,          \
                             const unsigned __int128* data, std::uint32_t data_len))                                   \
   FORGE_CONTRACT_INTRINSIC(                                                                                           \
       1, database, db, none, db_idx256_update, env, db_idx256_update, void,                                           \
       (std::int32_t iterator, std::uint64_t payer, const unsigned __int128* data, std::uint32_t data_len))            \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx256_remove, env, db_idx256_remove, void,                      \
                            (std::int32_t iterator))                                                                   \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx256_next, env, db_idx256_next, std::int32_t,                  \
                            (std::int32_t iterator, std::uint64_t* primary))                                           \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx256_previous, env, db_idx256_previous, std::int32_t,          \
                            (std::int32_t iterator, std::uint64_t* primary))                                           \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx256_find_primary, env, db_idx256_find_primary, std::int32_t,  \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table, unsigned __int128* data,    \
                             std::uint32_t data_len, std::uint64_t primary))                                           \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx256_find_secondary, env, db_idx256_find_secondary,            \
                            std::int32_t,                                                                              \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table,                             \
                             const unsigned __int128* data, std::uint32_t data_len, std::uint64_t* primary))           \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx256_lowerbound, env, db_idx256_lowerbound, std::int32_t,      \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table, unsigned __int128* data,    \
                             std::uint32_t data_len, std::uint64_t* primary))                                          \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx256_upperbound, env, db_idx256_upperbound, std::int32_t,      \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table, unsigned __int128* data,    \
                             std::uint32_t data_len, std::uint64_t* primary))                                          \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx256_end, env, db_idx256_end, std::int32_t,                    \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table))                            \
   FORGE_CONTRACT_INTRINSIC(                                                                                           \
       1, database, db, none, db_idx_double_store, env, db_idx_double_store, std::int32_t,                             \
       (std::uint64_t scope, std::uint64_t table, std::uint64_t payer, std::uint64_t id, const double* secondary))     \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx_double_update, env, db_idx_double_update, void,              \
                            (std::int32_t iterator, std::uint64_t payer, const double* secondary))                     \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx_double_remove, env, db_idx_double_remove, void,              \
                            (std::int32_t iterator))                                                                   \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx_double_next, env, db_idx_double_next, std::int32_t,          \
                            (std::int32_t iterator, std::uint64_t* primary))                                           \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx_double_previous, env, db_idx_double_previous, std::int32_t,  \
                            (std::int32_t iterator, std::uint64_t* primary))                                           \
   FORGE_CONTRACT_INTRINSIC(                                                                                           \
       1, database, db, none, db_idx_double_find_primary, env, db_idx_double_find_primary, std::int32_t,               \
       (std::uint64_t code, std::uint64_t scope, std::uint64_t table, double* secondary, std::uint64_t primary))       \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx_double_find_secondary, env, db_idx_double_find_secondary,    \
                            std::int32_t,                                                                              \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table, const double* secondary,    \
                             std::uint64_t* primary))                                                                  \
   FORGE_CONTRACT_INTRINSIC(                                                                                           \
       1, database, db, none, db_idx_double_lowerbound, env, db_idx_double_lowerbound, std::int32_t,                   \
       (std::uint64_t code, std::uint64_t scope, std::uint64_t table, double* secondary, std::uint64_t* primary))      \
   FORGE_CONTRACT_INTRINSIC(                                                                                           \
       1, database, db, none, db_idx_double_upperbound, env, db_idx_double_upperbound, std::int32_t,                   \
       (std::uint64_t code, std::uint64_t scope, std::uint64_t table, double* secondary, std::uint64_t* primary))      \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx_double_end, env, db_idx_double_end, std::int32_t,            \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table))                            \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx_long_double_store, env, db_idx_long_double_store,            \
                            std::int32_t,                                                                              \
                            (std::uint64_t scope, std::uint64_t table, std::uint64_t payer, std::uint64_t id,          \
                             const long double* secondary))                                                            \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx_long_double_update, env, db_idx_long_double_update, void,    \
                            (std::int32_t iterator, std::uint64_t payer, const long double* secondary))                \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx_long_double_remove, env, db_idx_long_double_remove, void,    \
                            (std::int32_t iterator))                                                                   \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx_long_double_next, env, db_idx_long_double_next,              \
                            std::int32_t, (std::int32_t iterator, std::uint64_t* primary))                             \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx_long_double_previous, env, db_idx_long_double_previous,      \
                            std::int32_t, (std::int32_t iterator, std::uint64_t* primary))                             \
   FORGE_CONTRACT_INTRINSIC(                                                                                           \
       1, database, db, none, db_idx_long_double_find_primary, env, db_idx_long_double_find_primary, std::int32_t,     \
       (std::uint64_t code, std::uint64_t scope, std::uint64_t table, long double* secondary, std::uint64_t primary))  \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx_long_double_find_secondary, env,                             \
                            db_idx_long_double_find_secondary, std::int32_t,                                           \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table,                             \
                             const long double* secondary, std::uint64_t* primary))                                    \
   FORGE_CONTRACT_INTRINSIC(                                                                                           \
       1, database, db, none, db_idx_long_double_lowerbound, env, db_idx_long_double_lowerbound, std::int32_t,         \
       (std::uint64_t code, std::uint64_t scope, std::uint64_t table, long double* secondary, std::uint64_t* primary)) \
   FORGE_CONTRACT_INTRINSIC(                                                                                           \
       1, database, db, none, db_idx_long_double_upperbound, env, db_idx_long_double_upperbound, std::int32_t,         \
       (std::uint64_t code, std::uint64_t scope, std::uint64_t table, long double* secondary, std::uint64_t* primary)) \
   FORGE_CONTRACT_INTRINSIC(1, database, db, none, db_idx_long_double_end, env, db_idx_long_double_end, std::int32_t,  \
                            (std::uint64_t code, std::uint64_t scope, std::uint64_t table))                            \
   FORGE_CONTRACT_INTRINSIC(1, instant_finality, instant_finality, savanna, set_finalizers, env, set_finalizers, void, \
                            (std::uint64_t packed_finalizer_format, const char* data, std::uint32_t len))              \
   FORGE_CONTRACT_INTRINSIC(1, core, permission, none, check_transaction_authorization, env,                           \
                            check_transaction_authorization, std::int32_t,                                             \
                            (const char* trx_data, std::uint32_t trx_size, const char* pubkeys_data,                   \
                             std::uint32_t pubkeys_size, const char* perms_data, std::uint32_t perms_size))            \
   FORGE_CONTRACT_INTRINSIC(                                                                                           \
       1, core, permission, none, check_permission_authorization, env, check_permission_authorization, std::int32_t,   \
       (std::uint64_t account, std::uint64_t permission, const char* pubkeys_data, std::uint32_t pubkeys_size,         \
        const char* perms_data, std::uint32_t perms_size, std::uint64_t delay_us))                                     \
   FORGE_CONTRACT_INTRINSIC(1, core, permission, none, get_permission_last_used, env, get_permission_last_used,        \
                            std::int64_t, (std::uint64_t account, std::uint64_t permission))                           \
   FORGE_CONTRACT_INTRINSIC(1, core, permission, none, get_account_creation_time, env, get_account_creation_time,      \
                            std::int64_t, (std::uint64_t account))                                                     \
   FORGE_CONTRACT_INTRINSIC(1, core, print, none, prints, env, prints, void, (const char* cstr))                       \
   FORGE_CONTRACT_INTRINSIC(1, core, print, none, prints_l, env, prints_l, void,                                       \
                            (const char* cstr, std::uint32_t len))                                                     \
   FORGE_CONTRACT_INTRINSIC(1, core, print, none, printi, env, printi, void, (std::int64_t value))                     \
   FORGE_CONTRACT_INTRINSIC(1, core, print, none, printui, env, printui, void, (std::uint64_t value))                  \
   FORGE_CONTRACT_INTRINSIC(1, core, print, none, printi128, env, printi128, void, (const __int128* value))            \
   FORGE_CONTRACT_INTRINSIC(1, core, print, none, printui128, env, printui128, void, (const unsigned __int128* value)) \
   FORGE_CONTRACT_INTRINSIC(1, core, print, none, printsf, env, printsf, void, (float value))                          \
   FORGE_CONTRACT_INTRINSIC(1, core, print, none, printdf, env, printdf, void, (double value))                         \
   FORGE_CONTRACT_INTRINSIC(1, core, print, none, printqf, env, printqf, void, (const long double* value))             \
   FORGE_CONTRACT_INTRINSIC(1, core, print, none, printn, env, printn, void, (std::uint64_t name))                     \
   FORGE_CONTRACT_INTRINSIC(1, core, print, none, printhex, env, printhex, void,                                       \
                            (const void* data, std::uint32_t datalen))                                                 \
   FORGE_CONTRACT_INTRINSIC(                                                                                           \
       1, privileged, privileged, none, get_resource_limits, env, get_resource_limits, void,                           \
       (std::uint64_t account, std::int64_t* ram_bytes, std::int64_t* net_weight, std::int64_t* cpu_weight))           \
   FORGE_CONTRACT_INTRINSIC(                                                                                           \
       1, privileged, privileged, none, set_resource_limits, env, set_resource_limits, void,                           \
       (std::uint64_t account, std::int64_t ram_bytes, std::int64_t net_weight, std::int64_t cpu_weight))              \
   FORGE_CONTRACT_INTRINSIC(1, privileged, privileged, none, set_proposed_producers, env, set_proposed_producers,      \
                            std::int64_t, (char* producer_data, std::uint32_t producer_data_size))                     \
   FORGE_CONTRACT_INTRINSIC(                                                                                           \
       1, privileged, privileged, wtmsig_block_signatures, set_proposed_producers_ex, env, set_proposed_producers_ex,  \
       std::int64_t, (std::uint64_t producer_data_format, char* producer_data, std::uint32_t producer_data_size))      \
   FORGE_CONTRACT_INTRINSIC(1, privileged, privileged, none, is_privileged, env, is_privileged, bool,                  \
                            (std::uint64_t account))                                                                   \
   FORGE_CONTRACT_INTRINSIC(1, privileged, privileged, none, set_privileged, env, set_privileged, void,                \
                            (std::uint64_t account, bool is_priv))                                                     \
   FORGE_CONTRACT_INTRINSIC(1, privileged, privileged, none, set_blockchain_parameters_packed, env,                    \
                            set_blockchain_parameters_packed, void, (char* data, std::uint32_t datalen))               \
   FORGE_CONTRACT_INTRINSIC(1, privileged, privileged, none, get_blockchain_parameters_packed, env,                    \
                            get_blockchain_parameters_packed, std::uint32_t, (char* data, std::uint32_t datalen))      \
   FORGE_CONTRACT_INTRINSIC(1, privileged, privileged, none, set_kv_parameters_packed, env, set_kv_parameters_packed,  \
                            void, (const char* data, std::uint32_t datalen))                                           \
   FORGE_CONTRACT_INTRINSIC(1, privileged, privileged, configurable_wasm_limits, get_wasm_parameters_packed, env,      \
                            get_wasm_parameters_packed, std::uint32_t,                                                 \
                            (char* data, std::uint32_t datalen, std::uint32_t max_version))                            \
   FORGE_CONTRACT_INTRINSIC(1, privileged, privileged, configurable_wasm_limits, set_wasm_parameters_packed, env,      \
                            set_wasm_parameters_packed, void, (const char* data, std::uint32_t datalen))               \
   FORGE_CONTRACT_INTRINSIC(1, privileged, privileged, blockchain_parameters, get_parameters_packed, env,              \
                            get_parameters_packed, std::uint32_t,                                                      \
                            (const char* ids, std::uint32_t ids_size, char* data, std::uint32_t datalen))              \
   FORGE_CONTRACT_INTRINSIC(1, privileged, privileged, blockchain_parameters, set_parameters_packed, env,              \
                            set_parameters_packed, void, (const char* data, forge_contract_size_t datalen))            \
   FORGE_CONTRACT_INTRINSIC(1, privileged, privileged, preactivate_feature, preactivate_feature, env,                  \
                            preactivate_feature, void, (const capi_checksum256* feature_digest))                       \
   FORGE_CONTRACT_INTRINSIC(1, core, system, none, eosio_assert, env, eosio_assert, void,                              \
                            (std::uint32_t test, const char* msg))                                                     \
   FORGE_CONTRACT_INTRINSIC(1, core, system, none, eosio_assert_message, env, eosio_assert_message, void,              \
                            (std::uint32_t test, const char* msg, std::uint32_t msg_len))                              \
   FORGE_CONTRACT_INTRINSIC(1, core, system, none, eosio_assert_code, env, eosio_assert_code, void,                    \
                            (std::uint32_t test, std::uint64_t code))                                                  \
   FORGE_CONTRACT_INTRINSIC(1, core, system, none, eosio_exit, env, eosio_exit, void, (std::int32_t code))             \
   FORGE_CONTRACT_INTRINSIC(1, core, system, none, current_time, env, current_time, std::uint64_t, ())                 \
   FORGE_CONTRACT_INTRINSIC(1, core, system, get_block_num, get_block_num, env, get_block_num, std::uint32_t, ())      \
   FORGE_CONTRACT_INTRINSIC(1, core, system, preactivate_feature, is_feature_activated, env, is_feature_activated,     \
                            bool, (const capi_checksum256* feature_digest))                                            \
   FORGE_CONTRACT_INTRINSIC(1, core, system, get_sender, get_sender, env, get_sender, std::uint64_t, ())               \
   FORGE_CONTRACT_INTRINSIC(1, core, transaction, none, send_deferred, env, send_deferred, void,                       \
                            (const unsigned __int128* sender_id, std::uint64_t payer,                                  \
                             const char* serialized_transaction, forge_contract_size_t size,                           \
                             std::uint32_t replace_existing))                                                          \
   FORGE_CONTRACT_INTRINSIC(1, core, transaction, none, cancel_deferred, env, cancel_deferred, std::int32_t,           \
                            (const unsigned __int128* sender_id))                                                      \
   FORGE_CONTRACT_INTRINSIC(1, core, transaction, none, read_transaction, env, read_transaction,                       \
                            forge_contract_size_t, (char* buffer, forge_contract_size_t size))                         \
   FORGE_CONTRACT_INTRINSIC(1, core, transaction, none, transaction_size, env, transaction_size,                       \
                            forge_contract_size_t, ())                                                                 \
   FORGE_CONTRACT_INTRINSIC(1, core, transaction, none, tapos_block_num, env, tapos_block_num, std::int32_t, ())       \
   FORGE_CONTRACT_INTRINSIC(1, core, transaction, none, tapos_block_prefix, env, tapos_block_prefix, std::int32_t, ()) \
   FORGE_CONTRACT_INTRINSIC(1, core, transaction, none, expiration, env, expiration, std::uint32_t, ())                \
   FORGE_CONTRACT_INTRINSIC(1, core, transaction, none, get_action, env, get_action, std::int32_t,                     \
                            (std::uint32_t type, std::uint32_t index, char* buff, forge_contract_size_t size))         \
   FORGE_CONTRACT_INTRINSIC(1, core, transaction, none, get_context_free_data, env, get_context_free_data,             \
                            std::int32_t, (std::uint32_t index, char* buff, forge_contract_size_t size))               \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, abort, env, abort, void, ())                                    \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, memcpy, env, memcpy, void*,                                     \
                            (void* destination, const void* source, std::uint32_t size))                               \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, memmove, env, memmove, void*,                                   \
                            (void* destination, const void* source, std::uint32_t size))                               \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, memcmp, env, memcmp, std::int32_t,                              \
                            (const void* left, const void* right, std::uint32_t size))                                 \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, memset, env, memset, void*,                                     \
                            (void* destination, std::int32_t value, std::uint32_t size))                               \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __ashlti3, env, __ashlti3, void,                                \
                            (__int128* result, std::uint64_t low, std::uint64_t high, std::uint32_t shift))            \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __ashrti3, env, __ashrti3, void,                                \
                            (__int128* result, std::uint64_t low, std::uint64_t high, std::uint32_t shift))            \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __lshlti3, env, __lshlti3, void,                                \
                            (__int128* result, std::uint64_t low, std::uint64_t high, std::uint32_t shift))            \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __lshrti3, env, __lshrti3, void,                                \
                            (__int128* result, std::uint64_t low, std::uint64_t high, std::uint32_t shift))            \
   FORGE_CONTRACT_INTRINSIC(                                                                                           \
       1, runtime, runtime, none, __divti3, env, __divti3, void,                                                       \
       (__int128* result, std::uint64_t low_a, std::uint64_t high_a, std::uint64_t low_b, std::uint64_t high_b))       \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __udivti3, env, __udivti3, void,                                \
                            (unsigned __int128* result, std::uint64_t low_a, std::uint64_t high_a,                     \
                             std::uint64_t low_b, std::uint64_t high_b))                                               \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __multi3, env, __multi3, void,                                  \
                            (unsigned __int128* result, std::uint64_t low_a, std::uint64_t high_a,                     \
                             std::uint64_t low_b, std::uint64_t high_b))                                               \
   FORGE_CONTRACT_INTRINSIC(                                                                                           \
       1, runtime, runtime, none, __modti3, env, __modti3, void,                                                       \
       (__int128* result, std::uint64_t low_a, std::uint64_t high_a, std::uint64_t low_b, std::uint64_t high_b))       \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __umodti3, env, __umodti3, void,                                \
                            (unsigned __int128* result, std::uint64_t low_a, std::uint64_t high_a,                     \
                             std::uint64_t low_b, std::uint64_t high_b))                                               \
   FORGE_CONTRACT_INTRINSIC(                                                                                           \
       1, runtime, runtime, none, __addtf3, env, __addtf3, void,                                                       \
       (long double* result, std::uint64_t low_a, std::uint64_t high_a, std::uint64_t low_b, std::uint64_t high_b))    \
   FORGE_CONTRACT_INTRINSIC(                                                                                           \
       1, runtime, runtime, none, __subtf3, env, __subtf3, void,                                                       \
       (long double* result, std::uint64_t low_a, std::uint64_t high_a, std::uint64_t low_b, std::uint64_t high_b))    \
   FORGE_CONTRACT_INTRINSIC(                                                                                           \
       1, runtime, runtime, none, __multf3, env, __multf3, void,                                                       \
       (long double* result, std::uint64_t low_a, std::uint64_t high_a, std::uint64_t low_b, std::uint64_t high_b))    \
   FORGE_CONTRACT_INTRINSIC(                                                                                           \
       1, runtime, runtime, none, __divtf3, env, __divtf3, void,                                                       \
       (long double* result, std::uint64_t low_a, std::uint64_t high_a, std::uint64_t low_b, std::uint64_t high_b))    \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __negtf2, env, __negtf2, void,                                  \
                            (long double* result, std::uint64_t low, std::uint64_t high))                              \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __extendsftf2, env, __extendsftf2, void,                        \
                            (long double* result, float value))                                                        \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __extenddftf2, env, __extenddftf2, void,                        \
                            (long double* result, double value))                                                       \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __trunctfdf2, env, __trunctfdf2, double,                        \
                            (std::uint64_t low, std::uint64_t high))                                                   \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __trunctfsf2, env, __trunctfsf2, float,                         \
                            (std::uint64_t low, std::uint64_t high))                                                   \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __fixtfsi, env, __fixtfsi, std::int32_t,                        \
                            (std::uint64_t low, std::uint64_t high))                                                   \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __fixtfdi, env, __fixtfdi, std::int64_t,                        \
                            (std::uint64_t low, std::uint64_t high))                                                   \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __fixtfti, env, __fixtfti, void,                                \
                            (__int128* result, std::uint64_t low, std::uint64_t high))                                 \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __fixunstfsi, env, __fixunstfsi, std::uint32_t,                 \
                            (std::uint64_t low, std::uint64_t high))                                                   \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __fixunstfdi, env, __fixunstfdi, std::uint64_t,                 \
                            (std::uint64_t low, std::uint64_t high))                                                   \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __fixunstfti, env, __fixunstfti, void,                          \
                            (unsigned __int128* result, std::uint64_t low, std::uint64_t high))                        \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __fixsfti, env, __fixsfti, void,                                \
                            (__int128* result, float value))                                                           \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __fixdfti, env, __fixdfti, void,                                \
                            (__int128* result, double value))                                                          \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __fixunssfti, env, __fixunssfti, void,                          \
                            (unsigned __int128* result, float value))                                                  \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __fixunsdfti, env, __fixunsdfti, void,                          \
                            (unsigned __int128* result, double value))                                                 \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __floatsidf, env, __floatsidf, double, (std::int32_t value))    \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __floatsitf, env, __floatsitf, void,                            \
                            (long double* result, std::int32_t value))                                                 \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __floatditf, env, __floatditf, void,                            \
                            (long double* result, std::uint64_t value))                                                \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __floatunsitf, env, __floatunsitf, void,                        \
                            (long double* result, std::uint32_t value))                                                \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __floatunditf, env, __floatunditf, void,                        \
                            (long double* result, std::uint64_t value))                                                \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __floattidf, env, __floattidf, double,                          \
                            (std::uint64_t low, std::uint64_t high))                                                   \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __floatuntidf, env, __floatuntidf, double,                      \
                            (std::uint64_t low, std::uint64_t high))                                                   \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __cmptf2, env, __cmptf2, std::int32_t,                          \
                            (std::uint64_t low_a, std::uint64_t high_a, std::uint64_t low_b, std::uint64_t high_b))    \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __eqtf2, env, __eqtf2, std::int32_t,                            \
                            (std::uint64_t low_a, std::uint64_t high_a, std::uint64_t low_b, std::uint64_t high_b))    \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __netf2, env, __netf2, std::int32_t,                            \
                            (std::uint64_t low_a, std::uint64_t high_a, std::uint64_t low_b, std::uint64_t high_b))    \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __getf2, env, __getf2, std::int32_t,                            \
                            (std::uint64_t low_a, std::uint64_t high_a, std::uint64_t low_b, std::uint64_t high_b))    \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __gttf2, env, __gttf2, std::int32_t,                            \
                            (std::uint64_t low_a, std::uint64_t high_a, std::uint64_t low_b, std::uint64_t high_b))    \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __letf2, env, __letf2, std::int32_t,                            \
                            (std::uint64_t low_a, std::uint64_t high_a, std::uint64_t low_b, std::uint64_t high_b))    \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __lttf2, env, __lttf2, std::int32_t,                            \
                            (std::uint64_t low_a, std::uint64_t high_a, std::uint64_t low_b, std::uint64_t high_b))    \
   FORGE_CONTRACT_INTRINSIC(1, runtime, runtime, none, __unordtf2, env, __unordtf2, std::int32_t,                      \
                            (std::uint64_t low_a, std::uint64_t high_a, std::uint64_t low_b, std::uint64_t high_b))

// Optional Forge capability. It does not extend the required CDT/Spring host interface.
#define FORGE_CONTRACT_EXPRESSION_V1_INTRINSICS(FORGE_CONTRACT_INTRINSIC)                                              \
   FORGE_CONTRACT_INTRINSIC(1, expression_v1, expression_v1, none, expression_compile_v1, env,                       \
                            expression_compile_v1, std::uint32_t,                                                    \
                            (const char* request, std::uint32_t request_size, char* response,                         \
                             std::uint32_t response_capacity))                                                      \
   FORGE_CONTRACT_INTRINSIC(1, expression_v1, expression_v1, none, expression_import_v1, env,                        \
                            expression_import_v1, std::uint32_t,                                                     \
                            (const char* request, std::uint32_t request_size, char* response,                         \
                             std::uint32_t response_capacity))                                                      \
   FORGE_CONTRACT_INTRINSIC(1, expression_v1, expression_v1, none, expression_evaluate_v1, env,                      \
                            expression_evaluate_v1, std::uint32_t,                                                   \
                            (const char* request, std::uint32_t request_size, char* response,                         \
                             std::uint32_t response_capacity))

#define FORGE_CONTRACT_INTRINSICS(FORGE_CONTRACT_INTRINSIC)                                                            \
   FORGE_CONTRACT_BASE_INTRINSICS(FORGE_CONTRACT_INTRINSIC)                                                            \
   FORGE_CONTRACT_EXPRESSION_V1_INTRINSICS(FORGE_CONTRACT_INTRINSIC)
