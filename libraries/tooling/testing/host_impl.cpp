module;

#include <boost/asio/awaitable.hpp>
#include <forge/contract/intrinsics.hpp>
#include <forge/contract/types.h>
#include <forge/exceptions/macros.hpp>
#include <forge/vm/wasm/interpret/host_function.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

module forge.tooling.testing.host;

import forge.asio.blocking;
import forge.asio.runtime;
import forge.db.core.driver;
import forge.db.object.index;
import forge.db.object.store;
import forge.db.object.transaction;
import forge.db.ids.object_id;
import forge.chain.protocol.values;
import forge.crypto.asymmetric;
import forge.crypto.digest.blake2;
import forge.crypto.bls.primitives;
import forge.crypto.bn256;
import forge.codec.hex;
import forge.crypto.math.modular_arithmetic;
import forge.crypto.digest.ripemd160;
import forge.crypto.asymmetric.secp256k1;
import forge.crypto.digest.sha1;
import forge.crypto.digest.sha256;
import forge.crypto.digest.sha3;
import forge.crypto.digest.sha512;
import forge.raw.raw;
import forge.vm.wasm.interpret.backend;

#if defined(FORGE_TOOLING_TESTING_EXPRESSION)
import forge.expression.artifact;
import forge.expression.exceptions;
import forge.expression.program;
import forge.tooling.testing.conversion;
import forge.raw.exceptions;
#endif

#include "details/memory_driver.hxx"
#include "details/compiler_builtins.hxx"
#include "details/host_impl.hxx"
#include "details/softfloat.hxx"

namespace forge::tooling::testing {

namespace {

constexpr auto max_assert_message_size = std::size_t{1024};

class exit_signal final {
 public:
   explicit exit_signal(std::int32_t value) : code{value} {}

   std::int32_t code = 0;
};

struct packed_code_hash_result {
   forge::unsigned_int version;
   std::uint64_t sequence = 0;
   forge::crypto::digest::sha256 digest;
   std::uint8_t vm_type = 0;
   std::uint8_t vm_version = 0;
};

template <typename Stream> void raw_pack(Stream& stream, const packed_code_hash_result& value) {
   forge::raw::pack(stream, value.version);
   forge::raw::pack(stream, value.sequence);
   forge::raw::pack(stream, value.digest);
   forge::raw::pack(stream, value.vm_type);
   forge::raw::pack(stream, value.vm_version);
}

[[noreturn]] void fail_database(std::string_view message) {
   FORGE_THROW_EXCEPTION(exceptions::database_error, std::string{message});
}

[[noreturn]] void fail_iterator(std::string_view message) {
   FORGE_THROW_EXCEPTION(exceptions::invalid_iterator, std::string{message});
}

void require_payer(std::uint64_t payer) {
   if (payer == 0U) {
      fail_database("database payer must not be zero");
   }
}

void require_owner(const table& value, std::uint64_t receiver) {
   if (value.code != receiver) {
      fail_database("database row belongs to a different contract");
   }
}

void require_ordered(float64 value) {
   if (is_nan(value)) {
      fail_database("NaN is not a valid secondary key");
   }
}

void require_ordered(float128 value) {
   if (is_nan(value)) {
      fail_database("NaN is not a valid secondary key");
   }
}

void require_ordered(std::uint64_t) {}
void require_ordered(const uint128&) {}
void require_ordered(const uint256&) {}

template <typename Byte> [[nodiscard]] std::span<const std::uint8_t> as_bytes(std::span<Byte> value) {
   return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size_bytes()};
}

template <typename Byte> [[nodiscard]] std::span<std::uint8_t> as_writable_bytes(std::span<Byte> value) {
   return {reinterpret_cast<std::uint8_t*>(value.data()), value.size_bytes()};
}

template <typename Source, typename Destination> void copy_digest(const Source& source, Destination& destination) {
   if (source.data_size() != sizeof(destination.hash)) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "crypto digest size mismatch");
   }
   std::memcpy(destination.hash, source.data(), sizeof(destination.hash));
}

template <typename Digest, typename Source> [[nodiscard]] Digest read_digest(const Source& source) {
   static_assert(sizeof(source.hash) == Digest::byte_size);
   return Digest{reinterpret_cast<const char*>(source.hash), sizeof(source.hash)};
}

template <typename Byte>
[[nodiscard]] std::uint32_t copy_sized(std::span<const Byte> source, std::span<Byte> destination) {
   if (destination.empty()) {
      return static_cast<std::uint32_t>(source.size());
   }
   const auto size = std::min(source.size(), destination.size());
   std::copy_n(source.begin(), size, destination.begin());
   return static_cast<std::uint32_t>(size);
}

[[nodiscard]] std::uint32_t copy_bytes(std::span<const std::uint8_t> source, std::span<char> destination) {
   if (destination.empty()) {
      return static_cast<std::uint32_t>(source.size());
   }
   const auto size = std::min(source.size(), destination.size());
   std::memcpy(destination.data(), source.data(), size);
   return static_cast<std::uint32_t>(size);
}

[[nodiscard]] std::string unsigned_128_to_string(unsigned __int128 value) {
   if (value == 0U) {
      return "0";
   }
   auto result = std::string{};
   while (value != 0U) {
      result.push_back(static_cast<char>('0' + value % 10U));
      value /= 10U;
   }
   std::reverse(result.begin(), result.end());
   return result;
}

[[nodiscard]] std::uint32_t read_varuint(std::span<const std::uint8_t> data, std::size_t& offset) {
   auto result = std::uint32_t{0};
   for (auto shift = std::uint32_t{0}; shift < 35U; shift += 7U) {
      if (offset == data.size()) {
         FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "unexpected end of packed parameters");
      }
      const auto byte = data[offset++];
      if (shift == 28U && (byte & 0xf0U) != 0U) {
         FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "packed parameter varuint32 overflow");
      }
      result |= static_cast<std::uint32_t>(byte & 0x7fU) << shift;
      if ((byte & 0x80U) == 0U) {
         return result;
      }
   }
   FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "packed parameter varuint32 overflow");
}

void write_varuint(std::vector<std::uint8_t>& output, std::uint32_t value) {
   do {
      auto byte = static_cast<std::uint8_t>(value & 0x7fU);
      value >>= 7U;
      if (value != 0U) {
         byte |= 0x80U;
      }
      output.push_back(byte);
   } while (value != 0U);
}

[[nodiscard]] std::size_t parameter_size(std::uint32_t id) {
   if (id == 0U) {
      return sizeof(std::uint64_t);
   }
   if ((id >= 1U && id <= 14U) || id == 17U) {
      return sizeof(std::uint32_t);
   }
   if (id == 15U || id == 16U) {
      return sizeof(std::uint16_t);
   }
   FORGE_THROW_EXCEPTION(exceptions::assertion_failure,
                         "provided parameter id " + std::to_string(id) + " should be less than 18");
}

[[nodiscard]] std::vector<std::uint32_t> unpack_parameter_ids(std::span<const std::uint8_t> data, bool includes_values,
                                                              std::array<std::vector<std::uint8_t>, 18>* values) {
   auto offset = std::size_t{0};
   const auto count = read_varuint(data, offset);
   auto result = std::vector<std::uint32_t>{};
   result.reserve(count);
   auto visited = std::array<bool, 18>{};
   for (auto index = std::uint32_t{0}; index < count; ++index) {
      const auto id = read_varuint(data, offset);
      const auto size = parameter_size(id);
      if (visited[id]) {
         FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "duplicate parameter id provided: " + std::to_string(id));
      }
      visited[id] = true;
      result.push_back(id);
      if (includes_values) {
         if (data.size() - offset < size) {
            FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "unexpected end of packed parameters");
         }
         (*values)[id].assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
                              data.begin() + static_cast<std::ptrdiff_t>(offset + size));
         offset += size;
      }
   }
   if (offset != data.size()) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "trailing bytes in packed parameters");
   }
   return result;
}

void require_privileged(const oracle_state& state, std::uint64_t receiver) {
   if (!state.privileged_accounts.contains(receiver)) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "contract is not privileged");
   }
}

boost::asio::awaitable<std::optional<table>> lookup_table(forge::db::object::transaction& transaction,
                                                          std::uint64_t code, std::uint64_t scope,
                                                          std::uint64_t table_name) {
   co_return co_await transaction.index<table_index, by_code_scope_table>().find(code, scope, table_name);
}

boost::asio::awaitable<table> ensure_table(forge::db::object::transaction& transaction, std::uint64_t code,
                                           std::uint64_t scope, std::uint64_t table_name, std::uint64_t payer) {
   if (auto found = co_await lookup_table(transaction, code, scope, table_name)) {
      co_return *found;
   }
   require_payer(payer);
   co_return co_await transaction.create<table>([&](table& value) {
      value.code = code;
      value.scope = scope;
      value.table_name = table_name;
      value.payer = payer;
   });
}

boost::asio::awaitable<void> increment_table(forge::db::object::transaction& transaction, table::id_t id) {
   co_await transaction.modify(id, [](table& value) {
      if (value.count == std::numeric_limits<std::uint32_t>::max()) {
         fail_database("database table row count overflow");
      }
      ++value.count;
   });
}

boost::asio::awaitable<void> decrement_table(forge::db::object::transaction& transaction, table::id_t id) {
   const auto value = co_await transaction.get(id);
   if (value.count == 0U) {
      fail_database("database table row count underflow");
   }
   if (value.count == 1U) {
      co_await transaction.erase(id);
   } else {
      co_await transaction.modify(id, [](table& current) { --current.count; });
   }
}

template <typename Row> [[nodiscard]] bool same_table(const std::optional<Row>& row, table::id_t id) {
   return row && row->table_id == id;
}

} // namespace

template <typename T> T host::impl::run(boost::asio::awaitable<T> operation) {
   return forge::asio::blocking::run(runtime_, std::move(operation));
}

void host::impl::run(boost::asio::awaitable<void> operation) {
   forge::asio::blocking::run(runtime_, std::move(operation));
}

#if defined(FORGE_TOOLING_TESTING_EXPRESSION)
host::impl::impl(forge::expression::runtime& expressions, execution_limits limits) : impl(limits) {
   expressions_ = &expressions;
}
#endif

host::impl::impl(execution_limits limits)
    : runtime_{forge::asio::runtime_options{.worker_threads = 1, .thread_name = "contract-db-test"}}, limits_{limits},
      driver_{std::make_shared<memory_driver>()} {
   if (limits_.timeout <= std::chrono::milliseconds::zero()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "contract execution timeout must be positive");
   }
   store_.emplace(run(forge::db::object::store::open(driver_)));
   store_->register_object<table_index>();
   store_->register_object<key_value_index>();
   store_->register_object<index64_index>();
   store_->register_object<index128_index>();
   store_->register_object<index256_index>();
   store_->register_object<index_double_index>();
   store_->register_object<index_long_double_index>();
   register_intrinsics();
}

host::impl::~impl() {
   if (transaction_) {
      try {
         rollback_invocation();
      } catch (...) {
      }
   }
}

void host::impl::register_intrinsics() {
   static const auto registered = [] {
#define FORGE_CONTRACT_TEST_REGISTER(version, capability, header, feature, identifier, module_name, import_name,       \
                                     result, arguments)                                                                \
   functions::template add<&impl::identifier>(#module_name, #import_name);
      FORGE_CONTRACT_INTRINSICS(FORGE_CONTRACT_TEST_REGISTER)
#undef FORGE_CONTRACT_TEST_REGISTER
      return true;
   }();
   static_cast<void>(registered);
}

void host::impl::begin_invocation(std::uint64_t receiver, std::uint64_t first_receiver, std::vector<std::uint8_t> data,
                                  invocation_options options) {
   if (transaction_) {
      fail_database("contract database invocation is already active");
   }
   if (options.cancel.stop_requested())
      FORGE_THROW_EXCEPTION(exceptions::execution_cancelled, "contract execution cancelled");
   const auto now = std::chrono::steady_clock::now();
   const auto timeout = std::chrono::duration_cast<std::chrono::steady_clock::duration>(limits_.timeout);
   auto deadline = timeout >= std::chrono::steady_clock::time_point::max() - now
                       ? std::chrono::steady_clock::time_point::max()
                       : now + timeout;
   if (options.deadline && *options.deadline < deadline)
      deadline = *options.deadline;
   if (deadline <= now)
      FORGE_THROW_EXCEPTION(exceptions::execution_timeout, "contract execution timed out");
   // Finish the only allocating state copy before a transaction exists.
   // Publishing the prepared snapshot and invocation inputs cannot then throw.
   static_assert(std::is_nothrow_move_constructible_v<forge::db::object::transaction>);
   static_assert(std::is_nothrow_move_constructible_v<oracle_state>);
   static_assert(std::is_nothrow_move_assignable_v<oracle_state>);
   static_assert(std::is_nothrow_move_assignable_v<decltype(state_before_invocation_)>);
   static_assert(std::is_nothrow_move_assignable_v<decltype(action_data_)>);
   static_assert(std::is_nothrow_move_assignable_v<invocation_result>);
   auto snapshot = std::optional<oracle_state>{std::in_place, state_};
   transaction_.emplace(run(store_->begin_transaction()));
   state_before_invocation_ = std::move(snapshot);
   invocation_deadline_ = deadline;
   invocation_cancel_ = std::move(options.cancel);
   receiver_ = receiver;
   first_receiver_ = first_receiver;
   read_only_ = false;
   action_data_ = std::move(data);
   result_ = {};
   last_call_return_value_.clear();
   iterators_.clear();
   object_iterators_.clear();
   end_iterators_.clear();
   next_end_ = -2;
}

void host::impl::commit_invocation() {
   run(transaction_->commit());
   transaction_.reset();
   state_before_invocation_.reset();
   invocation_deadline_.reset();
   invocation_cancel_ = {};
}

void host::impl::rollback_invocation() {
   run(transaction_->rollback());
   transaction_.reset();
   if (state_before_invocation_) {
      state_ = std::move(*state_before_invocation_);
      state_before_invocation_.reset();
   }
   invocation_deadline_.reset();
   invocation_cancel_ = {};
}

std::chrono::steady_clock::duration host::impl::remaining_execution_time() const {
   if (!invocation_deadline_) {
      FORGE_THROW_EXCEPTION(exceptions::database_error, "contract execution is not active");
   }
   if (invocation_cancel_.stop_requested())
      FORGE_THROW_EXCEPTION(exceptions::execution_cancelled, "contract execution cancelled");
   const auto now = std::chrono::steady_clock::now();
   if (now >= *invocation_deadline_) {
      FORGE_THROW_EXCEPTION(exceptions::execution_timeout, "contract execution timed out");
   }
   return *invocation_deadline_ - now;
}

invocation_result host::impl::invoke(std::span<const std::uint8_t> code, std::uint64_t receiver,
                                     std::uint64_t first_receiver, std::uint64_t action, std::vector<std::uint8_t> data,
                                     invocation_options options) {
   begin_invocation(receiver, first_receiver, std::move(data), std::move(options));
   try {
      auto bytes = forge::vm::wasm::interpret::wasm_code{code.begin(), code.end()};
      auto pointer = forge::vm::wasm::interpret::wasm_code_ptr{bytes.data(), bytes.size()};
      auto vm =
          backend{pointer, bytes.size(), nullptr, forge::vm::wasm::interpret::compatibility_options{}, false, false};
      vm.validate();
      execute(vm, "apply", receiver, first_receiver, action);
      commit_invocation();
   } catch (const exit_signal& signal) {
      result_.exit_code = signal.code;
      commit_invocation();
   } catch (const forge::vm::wasm::interpret::exceptions::timeout&) {
      rollback_invocation();
      FORGE_THROW_EXCEPTION(exceptions::execution_timeout, "contract execution timed out");
   } catch (const forge::vm::wasm::interpret::exceptions::canceled&) {
      rollback_invocation();
      FORGE_THROW_EXCEPTION(exceptions::execution_cancelled, "contract execution cancelled");
   } catch (...) {
      const auto failure = std::current_exception();
      rollback_invocation();
      std::rethrow_exception(failure);
   }
   return result_;
}

#if defined(FORGE_TOOLING_TESTING_EXPRESSION)
namespace {

template <typename T> T expression_request(std::span<const char> bytes) {
   namespace expression = forge::expression;
   if (bytes.size() > expression::max_wire_bytes)
      FORGE_THROW_EXCEPTION(expression::exceptions::resource_exhausted, "expression request exceeds the wire limit");
   try {
      return forge::raw::unpack_exact<T>(
          std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()},
          forge::raw::unpack_limits{.max_container_elements = expression::limits{}.collection_elements,
                                    .max_total_container_elements = expression::max_wire_bytes,
                                    .max_bytes = expression::max_wire_bytes});
   } catch (const forge::raw::exceptions::range_error&) {
      FORGE_THROW_EXCEPTION(expression::exceptions::invalid_input, "invalid expression request encoding");
   } catch (const forge::raw::exceptions::codec_error&) {
      FORGE_THROW_EXCEPTION(expression::exceptions::invalid_input, "invalid expression request encoding");
   } catch (const forge::raw::exceptions::allocation_limit&) {
      FORGE_THROW_EXCEPTION(expression::exceptions::resource_exhausted, "expression request allocation limit exceeded");
   }
}

template <typename T> std::uint32_t expression_response(const T& value, std::span<char> destination) {
   auto sizing = forge::datastream<std::size_t>{};
   forge::raw::pack(sizing, value);
   const auto size = sizing.tellp();
   if (size > forge::expression::max_wire_bytes || size > destination.size())
      FORGE_THROW_EXCEPTION(forge::expression::exceptions::resource_exhausted,
                            "expression response exceeds the output capacity");
   auto output = forge::datastream<char*>{destination.data(), size};
   forge::raw::pack(output, value);
   return static_cast<std::uint32_t>(output.tellp());
}

} // namespace
#endif

std::uint32_t host::impl::expression_compile_v1(std::span<const char> request, std::span<char> response) {
#if defined(FORGE_TOOLING_TESTING_EXPRESSION)
   if (expressions_) {
      static_cast<void>(remaining_execution_time());
      const auto input = expression_request<forge::expression::compilation>(request);
      if (input.profile != forge::expression::profile_version ||
          input.semantics != forge::expression::semantics_version)
         FORGE_THROW_EXCEPTION(forge::expression::exceptions::invalid_profile, "unsupported expression profile");
      const auto operation = forge::expression::operation_options{invocation_deadline_, invocation_cancel_};
      const auto program =
          forge::expression::program{*expressions_, input.source, input.variables, input.budget, operation};
      return expression_response(program.artifact(), response);
   }
#endif
   FORGE_THROW_EXCEPTION(exceptions::expression_unavailable, "expression capability is unavailable");
}

std::uint32_t host::impl::expression_import_v1(std::span<const char> request, std::span<char> response) {
#if defined(FORGE_TOOLING_TESTING_EXPRESSION)
   if (expressions_) {
      static_cast<void>(remaining_execution_time());
      const auto input = expression_request<forge::expression::artifact>(request);
      const auto operation = forge::expression::operation_options{invocation_deadline_, invocation_cancel_};
      const auto program = forge::expression::program{*expressions_, input, operation};
      return expression_response(program.artifact(), response);
   }
#endif
   FORGE_THROW_EXCEPTION(exceptions::expression_unavailable, "expression capability is unavailable");
}

std::uint32_t host::impl::expression_evaluate_v1(std::span<const char> request, std::span<char> response) {
#if defined(FORGE_TOOLING_TESTING_EXPRESSION)
   if (expressions_) {
      static_cast<void>(remaining_execution_time());
      const auto input = expression_request<forge::expression::calculation>(request);
      const auto operation = forge::expression::operation_options{invocation_deadline_, invocation_cancel_};
      const auto program = forge::expression::program{*expressions_, input.program, operation};
      const auto values = forge::core::conversion<forge::variant_object>{input.inputs}.finish();
      const auto evaluated = program.evaluate(values, operation);
      return expression_response(forge::core::conversion<forge::expression::result>{evaluated}.finish(), response);
   }
#endif
   FORGE_THROW_EXCEPTION(exceptions::expression_unavailable, "expression capability is unavailable");
}

void host::impl::configure(oracle_state state) {
   if (transaction_) {
      fail_database("cannot configure contract test host during an invocation");
   }
   state_ = std::move(state);
}

oracle_state host::impl::state() const {
   return state_;
}

void host::impl::register_contract(std::uint64_t account, std::vector<std::uint8_t> code) {
   if (transaction_) {
      fail_database("cannot register a contract during an invocation");
   }
   contracts_.insert_or_assign(account, std::move(code));
}

std::optional<table> host::impl::find_table(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name) {
   return run(store_->index<table_index, by_code_scope_table>().find(code, scope, table_name));
}

std::optional<key_value> host::impl::find_primary(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                                  std::uint64_t primary) {
   const auto owner = find_table(code, scope, table_name);
   if (!owner) {
      return std::nullopt;
   }
   return run(store_->index<key_value_index, by_scope_primary>().find(owner->id, primary));
}

template <typename Row, typename Index>
std::optional<Row> host::impl::find_secondary(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                              std::uint64_t primary) {
   const auto owner = find_table(code, scope, table_name);
   if (!owner) {
      return std::nullopt;
   }
   return run(store_->index<Index, by_primary>().find(owner->id, primary));
}

template std::optional<index64> host::impl::find_secondary<index64, index64_index>(std::uint64_t, std::uint64_t,
                                                                                   std::uint64_t, std::uint64_t);
template std::optional<index128> host::impl::find_secondary<index128, index128_index>(std::uint64_t, std::uint64_t,
                                                                                      std::uint64_t, std::uint64_t);
template std::optional<index256> host::impl::find_secondary<index256, index256_index>(std::uint64_t, std::uint64_t,
                                                                                      std::uint64_t, std::uint64_t);
template std::optional<index_double> host::impl::find_secondary<index_double, index_double_index>(std::uint64_t,
                                                                                                  std::uint64_t,
                                                                                                  std::uint64_t,
                                                                                                  std::uint64_t);
template std::optional<index_long_double>
host::impl::find_secondary<index_long_double, index_long_double_index>(std::uint64_t, std::uint64_t, std::uint64_t,
                                                                       std::uint64_t);

std::int32_t host::impl::cache(row_kind kind, forge::db::ids::object_id id, table::id_t table_id) {
   const auto key = std::tuple{kind, id.instance};
   if (const auto found = object_iterators_.find(key); found != object_iterators_.end()) {
      return found->second;
   }
   if (iterators_.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
      fail_iterator("contract database iterator cache overflow");
   }
   const auto result = static_cast<std::int32_t>(iterators_.size());
   iterators_.push_back(iterator_entry{.kind = kind, .id = id, .table_id = table_id});
   object_iterators_.emplace(key, result);
   return result;
}

std::int32_t host::impl::end(row_kind kind, table::id_t table_id) {
   const auto key = std::tuple{kind, table_id.instance};
   if (const auto found = end_iterators_.find(key); found != end_iterators_.end()) {
      return found->second;
   }
   if (next_end_ == std::numeric_limits<std::int32_t>::min()) {
      fail_iterator("contract database end iterator cache overflow");
   }
   const auto result = next_end_--;
   end_iterators_.emplace(key, result);
   return result;
}

host::impl::iterator_entry& host::impl::require_iterator(std::int32_t iterator, row_kind kind) {
   if (iterator < 0 || static_cast<std::size_t>(iterator) >= iterators_.size()) {
      fail_iterator("contract database iterator is invalid");
   }
   auto& entry = iterators_[static_cast<std::size_t>(iterator)];
   if (entry.erased || entry.kind != kind) {
      fail_iterator("contract database iterator has the wrong row type");
   }
   return entry;
}

table::id_t host::impl::require_end(std::int32_t iterator, row_kind kind) const {
   for (const auto& [key, value] : end_iterators_) {
      if (value == iterator && std::get<0>(key) == kind) {
         return table::id_t{std::get<1>(key)};
      }
   }
   fail_iterator("contract database end iterator is invalid");
}

void host::impl::erase_iterator(std::int32_t iterator) {
   auto& entry = iterators_[static_cast<std::size_t>(iterator)];
   object_iterators_.erase(std::tuple{entry.kind, entry.id.instance});
   entry.erased = true;
}

void host::impl::require_recipient(std::uint64_t account) {
   if (!is_account(account)) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "require_recipient account does not exist");
   }
   if (std::find(state_.recipients.begin(), state_.recipients.end(), account) == state_.recipients.end()) {
      state_.recipients.push_back(account);
   }
}

void host::impl::require_auth(std::uint64_t account) const {
   if (!has_auth(account)) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "missing required authority");
   }
}

bool host::impl::has_auth(std::uint64_t account) const {
   return state_.authorized_accounts.contains(account);
}

void host::impl::require_auth2(std::uint64_t account, std::uint64_t permission) const {
   if (!state_.authorized_permissions.contains({account, permission})) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "missing required authority");
   }
}

bool host::impl::is_account(std::uint64_t account) const {
   return account == receiver_ || state_.accounts.contains(account);
}

void host::impl::send_inline(std::span<const char> action) {
   state_.inline_actions.emplace_back(as_bytes(action).begin(), as_bytes(action).end());
}

void host::impl::send_context_free_inline(std::span<const char> action) {
   state_.context_free_inline_actions.emplace_back(as_bytes(action).begin(), as_bytes(action).end());
}

std::uint64_t host::impl::publication_time() const {
   return state_.publication_time;
}

std::uint32_t host::impl::get_code_hash(std::uint64_t account, std::uint32_t version,
                                        output_span<char, 1> result) const {
   const auto found = state_.code_hashes.find(account);
   const auto value = found == state_.code_hashes.end() ? code_hash{} : found->second;
   const auto packed = forge::raw::pack(packed_code_hash_result{
       .version = forge::unsigned_int{std::min(version, std::uint32_t{0})},
       .sequence = value.sequence,
       .digest = value.digest,
       .vm_type = value.vm_type,
       .vm_version = value.vm_version,
   });
   if (result.size() >= packed.size()) {
      std::memcpy(result.data(), packed.data(), packed.size());
   }
   return static_cast<std::uint32_t>(packed.size());
}

std::int64_t host::impl::call(std::uint64_t receiver, std::uint64_t flags, std::span<const char> data) {
   const auto target = contracts_.find(receiver);
   if (target == contracts_.end()) {
      return -1;
   }

   auto code = forge::vm::wasm::interpret::wasm_code{target->second.begin(), target->second.end()};
   auto pointer = forge::vm::wasm::interpret::wasm_code_ptr{code.data(), code.size()};
   auto vm = backend{pointer, code.size(), nullptr, forge::vm::wasm::interpret::compatibility_options{}, false, false};
   vm.validate();
   if (vm.get_module().get_exported_function("__forge_call") == std::numeric_limits<std::uint32_t>::max()) {
      return -1;
   }

   const auto previous_receiver = receiver_;
   const auto previous_read_only = read_only_;
   auto previous_call_data = std::move(call_data_);
   auto previous_call_return = std::move(call_return_value_);
   try {
      receiver_ = receiver;
      read_only_ = previous_read_only || (flags & std::uint64_t{1}) != 0U;
      call_data_.assign(as_bytes(data).begin(), as_bytes(data).end());
      call_return_value_.clear();
      execute(vm, "__forge_call", previous_receiver, receiver);
      auto result = std::move(call_return_value_);
      receiver_ = previous_receiver;
      read_only_ = previous_read_only;
      call_data_ = std::move(previous_call_data);
      call_return_value_ = std::move(previous_call_return);
      last_call_return_value_ = std::move(result);
      return static_cast<std::int64_t>(last_call_return_value_.size());
   } catch (...) {
      receiver_ = previous_receiver;
      read_only_ = previous_read_only;
      call_data_ = std::move(previous_call_data);
      call_return_value_ = std::move(previous_call_return);
      throw;
   }
}

std::uint32_t host::impl::get_call_return_value(std::span<char> destination) const {
   return copy_bytes(last_call_return_value_, destination);
}

std::uint32_t host::impl::get_call_data(std::span<char> destination) const {
   return copy_bytes(call_data_, destination);
}

void host::impl::set_call_return_value(std::span<const char> value) {
   call_return_value_.assign(as_bytes(value).begin(), as_bytes(value).end());
}

std::uint32_t host::impl::get_active_producers(output_span<std::uint64_t> producers) const {
   const auto required = state_.active_producers.size() * sizeof(std::uint64_t);
   if (!producers.empty()) {
      const auto count = std::min(producers.size(), state_.active_producers.size());
      std::copy_n(state_.active_producers.begin(), count, producers.begin());
      return static_cast<std::uint32_t>(count * sizeof(std::uint64_t));
   }
   return static_cast<std::uint32_t>(required);
}

void host::impl::assert_sha256(std::span<const char> data, checksum256_input expected) const {
   const auto actual = forge::crypto::digest::sha256::hash(data.data(), static_cast<std::uint32_t>(data.size()));
   if (std::memcmp(actual.data(), expected->hash, sizeof(expected->hash)) != 0) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "hash mismatch");
   }
}

void host::impl::assert_sha1(std::span<const char> data, checksum160_input expected) const {
   const auto actual = forge::crypto::digest::sha1::hash(data.data(), static_cast<std::uint32_t>(data.size()));
   if (std::memcmp(actual.data(), expected->hash, sizeof(expected->hash)) != 0) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "hash mismatch");
   }
}

void host::impl::assert_sha512(std::span<const char> data, checksum512_input expected) const {
   const auto actual = forge::crypto::digest::sha512::hash(data.data(), static_cast<std::uint32_t>(data.size()));
   if (std::memcmp(actual.data(), expected->hash, sizeof(expected->hash)) != 0) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "hash mismatch");
   }
}

void host::impl::assert_ripemd160(std::span<const char> data, checksum160_input expected) const {
   const auto actual = forge::crypto::digest::ripemd160::hash(data.data(), static_cast<std::uint32_t>(data.size()));
   if (std::memcmp(actual.data(), expected->hash, sizeof(expected->hash)) != 0) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "hash mismatch");
   }
}

void host::impl::sha256(std::span<const char> data, checksum256_output result) const {
   copy_digest(forge::crypto::digest::sha256::hash(data.data(), static_cast<std::uint32_t>(data.size())),
               *result.get());
}

void host::impl::sha1(std::span<const char> data, checksum160_output result) const {
   copy_digest(forge::crypto::digest::sha1::hash(data.data(), static_cast<std::uint32_t>(data.size())), *result.get());
}

void host::impl::sha512(std::span<const char> data, checksum512_output result) const {
   copy_digest(forge::crypto::digest::sha512::hash(data.data(), static_cast<std::uint32_t>(data.size())),
               *result.get());
}

void host::impl::ripemd160(std::span<const char> data, checksum160_output result) const {
   copy_digest(forge::crypto::digest::ripemd160::hash(data.data(), static_cast<std::uint32_t>(data.size())),
               *result.get());
}

std::int32_t host::impl::recover_key(checksum256_input digest, std::span<const char> signature,
                                     std::span<char> public_key) const {
   const auto packed_signature = as_bytes(signature);
   const auto value = forge::raw::unpack_exact<forge::crypto::asymmetric::signature>(packed_signature);
   const auto recovered =
       forge::crypto::asymmetric::recover(value, read_digest<forge::crypto::digest::sha256>(*digest.get()), false);
   const auto packed = forge::raw::pack(recovered);
   if (forge::crypto::asymmetric::index(value) >= 2U) {
      if (public_key.size() < 33U) {
         FORGE_THROW_EXCEPTION(exceptions::assertion_failure,
                               "destination buffer must at least be able to hold an ECC public key");
      }
      std::memcpy(public_key.data(), packed.data(), std::min(public_key.size(), packed.size()));
      return static_cast<std::int32_t>(packed.size());
   }

   if (public_key.size() < packed.size()) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure,
                            "destination buffer must at least be able to hold an ECC public key");
   }
   std::memcpy(public_key.data(), packed.data(), packed.size());
   return static_cast<std::int32_t>(packed.size());
}

void host::impl::assert_recover_key(checksum256_input digest, std::span<const char> signature,
                                    std::span<const char> public_key) const {
   auto buffer = std::vector<char>(std::max(public_key.size(), std::size_t{33}));
   const auto size = recover_key(std::move(digest), signature, buffer);
   if (public_key.size() != static_cast<std::size_t>(size) ||
       std::memcmp(public_key.data(), buffer.data(), public_key.size()) != 0) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "Error expected key different than recovered key");
   }
}

std::int32_t host::impl::bls_g1_add(std::span<const char> left, std::span<const char> right,
                                    std::span<char> result) const {
   return forge::crypto::bls::primitives::g1_add(as_bytes(left), as_bytes(right), as_writable_bytes(result));
}

std::int32_t host::impl::bls_g2_add(std::span<const char> left, std::span<const char> right,
                                    std::span<char> result) const {
   return forge::crypto::bls::primitives::g2_add(as_bytes(left), as_bytes(right), as_writable_bytes(result));
}

std::int32_t host::impl::bls_g1_weighted_sum(std::span<const char> points, std::span<const char> scalars,
                                             std::uint32_t count, std::span<char> result) const {
   return forge::crypto::bls::primitives::g1_weighted_sum(as_bytes(points), as_bytes(scalars), count,
                                                          as_writable_bytes(result));
}

std::int32_t host::impl::bls_g2_weighted_sum(std::span<const char> points, std::span<const char> scalars,
                                             std::uint32_t count, std::span<char> result) const {
   return forge::crypto::bls::primitives::g2_weighted_sum(as_bytes(points), as_bytes(scalars), count,
                                                          as_writable_bytes(result));
}

std::int32_t host::impl::bls_pairing(std::span<const char> g1_points, std::span<const char> g2_points,
                                     std::uint32_t count, std::span<char> result) const {
   return forge::crypto::bls::primitives::pairing(as_bytes(g1_points), as_bytes(g2_points), count,
                                                  as_writable_bytes(result));
}

std::int32_t host::impl::bls_g1_map(std::span<const char> element, std::span<char> result) const {
   return forge::crypto::bls::primitives::g1_map(as_bytes(element), as_writable_bytes(result));
}

std::int32_t host::impl::bls_g2_map(std::span<const char> element, std::span<char> result) const {
   return forge::crypto::bls::primitives::g2_map(as_bytes(element), as_writable_bytes(result));
}

std::int32_t host::impl::bls_fp_mod(std::span<const char> scalar, std::span<char> result) const {
   return forge::crypto::bls::primitives::field_mod(as_bytes(scalar), as_writable_bytes(result));
}

std::int32_t host::impl::bls_fp_mul(std::span<const char> left, std::span<const char> right,
                                    std::span<char> result) const {
   return forge::crypto::bls::primitives::field_multiply(as_bytes(left), as_bytes(right), as_writable_bytes(result));
}

std::int32_t host::impl::bls_fp_exp(std::span<const char> base, std::span<const char> exponent,
                                    std::span<char> result) const {
   return forge::crypto::bls::primitives::field_exponentiate(as_bytes(base), as_bytes(exponent),
                                                             as_writable_bytes(result));
}

void host::impl::sha3(std::span<const char> data, std::span<char> hash, std::int32_t keccak) const {
   const auto value =
       forge::crypto::digest::sha3::hash(data.data(), static_cast<std::uint32_t>(data.size()), keccak != 1);
   const auto size = std::min(hash.size(), value.data_size());
   std::memcpy(hash.data(), value.data(), size);
}

std::int32_t host::impl::blake2_f(std::uint32_t rounds, std::span<const char> state, std::span<const char> message,
                                  std::span<const char> offset0, std::span<const char> offset1, std::int32_t final,
                                  std::span<char> result) const {
   try {
      const auto bytes = [](std::span<const char> value) {
         return forge::crypto::core::bytes{reinterpret_cast<const std::uint8_t*>(value.data()),
                                           reinterpret_cast<const std::uint8_t*>(value.data() + value.size())};
      };
      const auto value = forge::crypto::digest::blake2b(rounds, bytes(state), bytes(message), bytes(offset0),
                                                        bytes(offset1), final == 1, [] {});
      if (result.size() < value.size()) {
         return -1;
      }
      std::memcpy(result.data(), value.data(), value.size());
      return 0;
   } catch (const forge::crypto::digest::blake2::exceptions::invalid_input&) {
      return -1;
   }
}

std::int32_t host::impl::k1_recover(std::span<const char> signature, std::span<const char> digest,
                                    std::span<char> public_key) const {
   try {
      const auto to_bytes = [](std::span<const char> value) {
         return forge::crypto::asymmetric::secp256k1::recover_bytes(value.begin(), value.end());
      };
      const auto value = forge::crypto::asymmetric::secp256k1::recover(to_bytes(signature), to_bytes(digest));
      if (public_key.size() < value.size()) {
         return -1;
      }
      std::memcpy(public_key.data(), value.data(), value.size());
      return 0;
   } catch (const std::exception&) {
      return -1;
   }
}

std::int32_t host::impl::alt_bn128_add(std::span<const char> left, std::span<const char> right,
                                       std::span<char> result) const {
   return forge::crypto::bn256::add(as_bytes(left), as_bytes(right), as_writable_bytes(result));
}

std::int32_t host::impl::alt_bn128_mul(std::span<const char> point, std::span<const char> scalar,
                                       std::span<char> result) const {
   return forge::crypto::bn256::multiply(as_bytes(point), as_bytes(scalar), as_writable_bytes(result));
}

std::int32_t host::impl::alt_bn128_pair(std::span<const char> pairs) const {
   const auto value = forge::crypto::bn256::pairing_check(as_bytes(pairs));
   return value < 0 ? -1 : (value != 0 ? 0 : 1);
}

std::int32_t host::impl::mod_exp(std::span<const char> base, std::span<const char> exponent,
                                 std::span<const char> modulus, std::span<char> result) const {
   try {
      const auto to_bytes = [](std::span<const char> value) {
         return forge::crypto::core::bytes{reinterpret_cast<const std::uint8_t*>(value.data()),
                                           reinterpret_cast<const std::uint8_t*>(value.data() + value.size())};
      };
      const auto value = forge::crypto::math::modexp(to_bytes(base), to_bytes(exponent), to_bytes(modulus));
      if (result.size() < value.size()) {
         return -1;
      }
      std::memcpy(result.data(), value.data(), value.size());
      return 0;
   } catch (const forge::crypto::math::modular_arithmetic::exceptions::invalid_modulus&) {
      return -1;
   }
}

std::int32_t host::impl::check_transaction_authorization(std::span<const char> transaction,
                                                         std::span<const char> public_keys,
                                                         std::span<const char> permissions) const {
   static_cast<void>(transaction);
   static_cast<void>(public_keys);
   static_cast<void>(permissions);
   return state_.transaction_authorized ? 1 : 0;
}

std::int32_t host::impl::check_permission_authorization(std::uint64_t account, std::uint64_t permission,
                                                        std::span<const char> public_keys,
                                                        std::span<const char> permissions,
                                                        std::uint64_t delay_us) const {
   static_cast<void>(public_keys);
   static_cast<void>(permissions);
   static_cast<void>(delay_us);
   return state_.permission_authorized || state_.authorized_permissions.contains({account, permission}) ? 1 : 0;
}

std::int64_t host::impl::get_permission_last_used(std::uint64_t account, std::uint64_t permission) const {
   const auto found = state_.permission_last_used.find({account, permission});
   if (found == state_.permission_last_used.end()) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "permission does not exist");
   }
   return found->second;
}

std::int64_t host::impl::get_account_creation_time(std::uint64_t account) const {
   const auto found = state_.account_creation_time.find(account);
   if (found == state_.account_creation_time.end()) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "account does not exist");
   }
   return found->second;
}

void host::impl::prints(input<const char, 1> value) {
   state_.console.append(value.get());
}

void host::impl::prints_l(std::span<const char> value) {
   state_.console.append(value.data(), value.size());
}

void host::impl::printi(std::int64_t value) {
   state_.console.append(std::to_string(value));
}

void host::impl::printui(std::uint64_t value) {
   state_.console.append(std::to_string(value));
}

void host::impl::printi128(input<__int128, 16> value) {
   const auto number = *value.get();
   if (number < 0) {
      state_.console.push_back('-');
      const auto magnitude = ~static_cast<unsigned __int128>(number) + 1U;
      state_.console.append(unsigned_128_to_string(magnitude));
   } else {
      state_.console.append(unsigned_128_to_string(static_cast<unsigned __int128>(number)));
   }
}

void host::impl::printui128(uint128_input value) {
   state_.console.append(unsigned_128_to_string(*value.get()));
}

void host::impl::printsf(float value) {
   auto stream = std::ostringstream{};
   stream.setf(std::ios::scientific, std::ios::floatfield);
   stream.precision(std::numeric_limits<float>::digits10);
   stream << value;
   state_.console.append(stream.str());
}

void host::impl::printdf(double value) {
   auto stream = std::ostringstream{};
   stream.setf(std::ios::scientific, std::ios::floatfield);
   stream.precision(std::numeric_limits<double>::digits10);
   stream << value;
   state_.console.append(stream.str());
}

void host::impl::printqf(float128_input value) {
   state_.console.append(format(*value.get()));
}

void host::impl::printn(std::uint64_t value) {
   state_.console.append(forge::chain::protocol::name{value}.to_string());
}

void host::impl::printhex(std::span<const char> value) {
   state_.console.append(forge::codec::hex::encode(
       std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(value.data()), value.size()}));
}

void host::impl::get_resource_limits(std::uint64_t account, output<std::int64_t> ram_bytes,
                                     output<std::int64_t> net_weight, output<std::int64_t> cpu_weight) const {
   const auto found = state_.limits.find(account);
   const auto value = found == state_.limits.end() ? resource_limits{} : found->second;
   *ram_bytes.get() = value.ram_bytes;
   *net_weight.get() = value.net_weight;
   *cpu_weight.get() = value.cpu_weight;
}

void host::impl::set_resource_limits(std::uint64_t account, std::int64_t ram_bytes, std::int64_t net_weight,
                                     std::int64_t cpu_weight) {
   require_privileged(state_, receiver_);
   if (ram_bytes < -1 || net_weight < -1 || cpu_weight < -1) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "invalid resource limit; expected [-1, INT64_MAX]");
   }
   state_.limits.insert_or_assign(account, resource_limits{ram_bytes, net_weight, cpu_weight});
}

std::int64_t host::impl::set_proposed_producers(std::span<const char> producer_data) {
   return set_proposed_producers_ex(0U, producer_data);
}

std::int64_t host::impl::set_proposed_producers_ex(std::uint64_t format, std::span<const char> producer_data) {
   require_privileged(state_, receiver_);
   if (format > 1U) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "Producer schedule is in an unknown format!");
   }
   state_.proposed_producers.assign(as_bytes(producer_data).begin(), as_bytes(producer_data).end());
   return static_cast<std::int64_t>(++state_.proposed_producer_version);
}

bool host::impl::is_privileged(std::uint64_t account) const {
   return state_.privileged_accounts.contains(account);
}

void host::impl::set_privileged(std::uint64_t account, bool value) {
   require_privileged(state_, receiver_);
   if (value) {
      state_.privileged_accounts.insert(account);
   } else {
      state_.privileged_accounts.erase(account);
   }
}

void host::impl::set_blockchain_parameters_packed(std::span<const char> data) {
   require_privileged(state_, receiver_);
   state_.blockchain_parameters.assign(as_bytes(data).begin(), as_bytes(data).end());
}

std::uint32_t host::impl::get_blockchain_parameters_packed(std::span<char> data) const {
   if (data.empty()) {
      return static_cast<std::uint32_t>(state_.blockchain_parameters.size());
   }
   if (data.size() < state_.blockchain_parameters.size()) {
      return 0U;
   }
   std::memcpy(data.data(), state_.blockchain_parameters.data(), state_.blockchain_parameters.size());
   return static_cast<std::uint32_t>(state_.blockchain_parameters.size());
}

void host::impl::set_kv_parameters_packed(std::span<const char> data) {
   require_privileged(state_, receiver_);
   state_.kv_parameters.assign(as_bytes(data).begin(), as_bytes(data).end());
}

std::uint32_t host::impl::get_wasm_parameters_packed(std::span<char> data, std::uint32_t) const {
   const auto size = static_cast<std::uint32_t>(state_.wasm_parameters.size());
   if (!state_.wasm_parameters.empty() && data.size() >= state_.wasm_parameters.size()) {
      std::memcpy(data.data(), state_.wasm_parameters.data(), state_.wasm_parameters.size());
   }
   return size;
}

void host::impl::set_wasm_parameters_packed(std::span<const char> data) {
   require_privileged(state_, receiver_);
   state_.wasm_parameters.assign(as_bytes(data).begin(), as_bytes(data).end());
}

std::uint32_t host::impl::get_parameters_packed(std::span<const char> ids, std::span<char> data) const {
   const auto selected = unpack_parameter_ids(as_bytes(ids), false, nullptr);
   auto packed = std::vector<std::uint8_t>{};
   write_varuint(packed, static_cast<std::uint32_t>(selected.size()));
   for (const auto id : selected) {
      write_varuint(packed, id);
      if (state_.parameters[id].empty()) {
         packed.resize(packed.size() + parameter_size(id), 0U);
      } else {
         packed.insert(packed.end(), state_.parameters[id].begin(), state_.parameters[id].end());
      }
   }
   if (data.empty()) {
      return static_cast<std::uint32_t>(packed.size());
   }
   if (data.size() < packed.size()) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure,
                            "get_parameters_packed: buffer size is smaller than " + std::to_string(packed.size()));
   }
   std::memcpy(data.data(), packed.data(), packed.size());
   return static_cast<std::uint32_t>(packed.size());
}

void host::impl::set_parameters_packed(std::span<const char> data) {
   require_privileged(state_, receiver_);
   auto updated = state_.parameters;
   static_cast<void>(unpack_parameter_ids(as_bytes(data), true, &updated));
   state_.parameters = std::move(updated);
}

void host::impl::preactivate_feature(checksum256_input digest) {
   require_privileged(state_, receiver_);
   const auto value = read_digest<forge::crypto::digest::sha256>(*digest.get());
   if (!state_.available_features.empty() && !state_.available_features.contains(value)) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "protocol feature is not recognized");
   }
   state_.activated_features.insert(value);
}

void host::impl::set_finalizers(std::uint64_t format, std::span<const char> data) {
   require_privileged(state_, receiver_);
   if (format != 0U) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "Finalizer policy is in an unknown format!");
   }
   state_.finalizers.assign(as_bytes(data).begin(), as_bytes(data).end());
}

[[noreturn]] void host::impl::abort() {
   FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "abort() called");
}

void* host::impl::memcpy(copy_arguments arguments) {
   const auto destination = reinterpret_cast<std::uintptr_t>(arguments.destination);
   const auto source = reinterpret_cast<std::uintptr_t>(arguments.source);
   const auto distance = destination < source ? source - destination : destination - source;
   if (distance < arguments.size) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "memcpy can only accept non-aliasing pointers");
   }
   return std::memcpy(arguments.destination, arguments.source, arguments.size);
}

void* host::impl::memmove(copy_arguments arguments) {
   return std::memmove(arguments.destination, arguments.source, arguments.size);
}

std::int32_t host::impl::memcmp(compare_arguments arguments) const {
   const auto result = std::memcmp(arguments.left, arguments.right, arguments.size);
   return result < 0 ? -1 : result > 0 ? 1 : 0;
}

void* host::impl::memset(fill_arguments arguments) {
   return std::memset(arguments.destination, arguments.value, arguments.size);
}

void host::impl::eosio_assert(std::uint32_t test, input<const char, 1> message) {
   if (test == 0U) {
      const auto size = strnlen(message.get(), max_assert_message_size);
      const auto text = std::string{message.get(), size};
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, text);
   }
}

void host::impl::eosio_assert_message(std::uint32_t test, forge::vm::wasm::interpret::span<const char> message) {
   if (test == 0U) {
      const auto size = std::min(message.size(), max_assert_message_size);
      const auto text = std::string{message.data(), size};
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, text);
   }
}

void host::impl::eosio_assert_code(std::uint32_t test, std::uint64_t code) {
   if (test == 0U) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "contract assertion code",
                            forge::exceptions::ctx("assertion_code", code));
   }
}

[[noreturn]] void host::impl::eosio_exit(std::int32_t code) {
   throw exit_signal{code};
}

std::uint32_t host::impl::action_data_size() const {
   return static_cast<std::uint32_t>(action_data_.size());
}

std::uint32_t host::impl::read_action_data(forge::vm::wasm::interpret::span<char> destination) const {
   if (destination.empty()) {
      return static_cast<std::uint32_t>(action_data_.size());
   }
   const auto size = std::min(destination.size(), action_data_.size());
   std::copy_n(action_data_.begin(), size, destination.begin());
   return static_cast<std::uint32_t>(size);
}

void host::impl::set_action_return_value(forge::vm::wasm::interpret::span<const char> value) {
   result_.return_value.assign(reinterpret_cast<const std::uint8_t*>(value.data()),
                               reinterpret_cast<const std::uint8_t*>(value.data() + value.size()));
}

std::uint64_t host::impl::current_receiver() const {
   return receiver_;
}

std::uint64_t host::impl::current_time() const {
   return state_.current_time;
}

std::uint32_t host::impl::get_block_num() const {
   return state_.block_num;
}

bool host::impl::is_feature_activated(checksum256_input digest) const {
   return state_.activated_features.contains(read_digest<forge::crypto::digest::sha256>(*digest.get()));
}

std::uint64_t host::impl::get_sender() const {
   return state_.sender;
}

void host::impl::send_deferred(input<unsigned __int128, 16> sender_id, std::uint64_t payer,
                               std::span<const char> transaction, std::uint32_t replace_existing) {
   const auto key = *sender_id.get();
   if (payer == 0U) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "deferred transaction payer must not be zero");
   }
   if (state_.deferred.contains(key) && replace_existing == 0U) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure,
                            "deferred transaction with the same sender_id already exists");
   }
   state_.deferred.insert_or_assign(
       key, deferred_transaction{.sender_id = key,
                                 .payer = payer,
                                 .packed = {as_bytes(transaction).begin(), as_bytes(transaction).end()}});
}

std::int32_t host::impl::cancel_deferred(input<unsigned __int128, 16> sender_id) {
   return state_.deferred.erase(*sender_id.get()) != 0U ? 1 : 0;
}

std::uint32_t host::impl::read_transaction(std::span<char> destination) const {
   return copy_bytes(state_.transaction, destination);
}

std::uint32_t host::impl::transaction_size() const {
   return static_cast<std::uint32_t>(state_.transaction.size());
}

std::int32_t host::impl::tapos_block_num() const {
   return state_.tapos_block_num;
}

std::int32_t host::impl::tapos_block_prefix() const {
   return state_.tapos_block_prefix;
}

std::uint32_t host::impl::expiration() const {
   return state_.expiration;
}

std::int32_t host::impl::get_action(std::uint32_t type, std::uint32_t index, std::span<char> destination) const {
   if (type > 1U) {
      FORGE_THROW_EXCEPTION(exceptions::assertion_failure, "action is not found");
   }
   const auto& actions = state_.actions[type];
   if (index >= actions.size()) {
      return -1;
   }
   const auto& action = actions[index];
   if (destination.size() >= action.size()) {
      std::memcpy(destination.data(), action.data(), action.size());
   }
   return static_cast<std::int32_t>(action.size());
}

std::int32_t host::impl::get_context_free_data(std::uint32_t index, std::span<char> destination) const {
   if (index >= state_.context_free_data.size()) {
      return -1;
   }
   return static_cast<std::int32_t>(copy_bytes(state_.context_free_data[index], destination));
}

std::vector<std::uint8_t> host::impl::snapshot() const {
   return driver_->snapshot();
}

void host::impl::require_writable() const {
   if (read_only_) {
      fail_database("this API is not allowed in read only action/call");
   }
}

std::int32_t host::impl::db_store_i64(std::uint64_t scope, std::uint64_t table_name, std::uint64_t payer,
                                      std::uint64_t primary, forge::vm::wasm::interpret::span<const char> value) {
   require_writable();
   require_payer(payer);
   const auto owner = run(ensure_table(*transaction_, receiver_, scope, table_name, payer));
   if (run(transaction_->index<key_value_index, by_scope_primary>().find(owner.id, primary))) {
      fail_database("database primary key already exists");
   }
   const auto created = run(transaction_->create<key_value>([&](key_value& row) {
      row.table_id = owner.id;
      row.primary = primary;
      row.payer = payer;
      row.value.assign(reinterpret_cast<const std::uint8_t*>(value.data()),
                       reinterpret_cast<const std::uint8_t*>(value.data() + value.size()));
   }));
   run(increment_table(*transaction_, owner.id));
   return cache(row_kind::primary, forge::db::ids::to_object_id(created.id), owner.id);
}

void host::impl::db_update_i64(std::int32_t iterator, std::uint64_t payer,
                               forge::vm::wasm::interpret::span<const char> value) {
   require_writable();
   auto& entry = require_iterator(iterator, row_kind::primary);
   const auto owner = run(transaction_->get(entry.table_id));
   require_owner(owner, receiver_);
   run(transaction_->modify(key_value::id_t{entry.id.instance}, [&](key_value& row) {
      if (payer != 0U) {
         row.payer = payer;
      }
      row.value.assign(reinterpret_cast<const std::uint8_t*>(value.data()),
                       reinterpret_cast<const std::uint8_t*>(value.data() + value.size()));
   }));
}

void host::impl::db_remove_i64(std::int32_t iterator) {
   require_writable();
   auto& entry = require_iterator(iterator, row_kind::primary);
   const auto owner = run(transaction_->get(entry.table_id));
   require_owner(owner, receiver_);
   run(transaction_->erase(key_value::id_t{entry.id.instance}));
   run(decrement_table(*transaction_, entry.table_id));
   erase_iterator(iterator);
}

std::int32_t host::impl::db_get_i64(std::int32_t iterator, output_span<char, 1> value) {
   const auto& entry = require_iterator(iterator, row_kind::primary);
   const auto row = run(transaction_->get(key_value::id_t{entry.id.instance}));
   if (value.empty()) {
      return static_cast<std::int32_t>(row.value.size());
   }
   const auto size = std::min(value.size(), row.value.size());
   std::copy_n(row.value.begin(), size, value.begin());
   return static_cast<std::int32_t>(size);
}

std::int32_t host::impl::db_next_i64(std::int32_t iterator, output<std::uint64_t> primary) {
   const auto& entry = require_iterator(iterator, row_kind::primary);
   const auto current = run(transaction_->get(key_value::id_t{entry.id.instance}));
   auto index = transaction_->index<key_value_index, by_scope_primary>();
   const auto rank = run(index.rank(current));
   const auto next = run(index.nth(rank + 1U));
   if (!same_table(next, entry.table_id)) {
      return end(row_kind::primary, entry.table_id);
   }
   *primary = next->primary;
   return cache(row_kind::primary, forge::db::ids::to_object_id(next->id), next->table_id);
}

std::int32_t host::impl::db_previous_i64(std::int32_t iterator, output<std::uint64_t> primary) {
   auto index = transaction_->index<key_value_index, by_scope_primary>();
   auto table_id = table::id_t{};
   auto rank = std::uint64_t{};
   if (iterator < -1) {
      table_id = require_end(iterator, row_kind::primary);
      rank = run(index.equal_range_rank(table_id)).second;
   } else {
      const auto& entry = require_iterator(iterator, row_kind::primary);
      table_id = entry.table_id;
      const auto current = run(transaction_->get(key_value::id_t{entry.id.instance}));
      rank = run(index.rank(current));
   }
   if (rank == 0U) {
      return -1;
   }
   const auto previous = run(index.nth(rank - 1U));
   if (!same_table(previous, table_id)) {
      return -1;
   }
   *primary = previous->primary;
   return cache(row_kind::primary, forge::db::ids::to_object_id(previous->id), previous->table_id);
}

std::int32_t host::impl::db_find_i64(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                     std::uint64_t primary) {
   const auto owner = run(lookup_table(*transaction_, code, scope, table_name));
   if (!owner) {
      return -1;
   }
   const auto row = run(transaction_->index<key_value_index, by_scope_primary>().find(owner->id, primary));
   if (!row) {
      return -1;
   }
   return cache(row_kind::primary, forge::db::ids::to_object_id(row->id), row->table_id);
}

std::int32_t host::impl::db_lowerbound_i64(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                           std::uint64_t primary) {
   const auto owner = run(lookup_table(*transaction_, code, scope, table_name));
   if (!owner) {
      return -1;
   }
   auto index = transaction_->index<key_value_index, by_scope_primary>();
   const auto row = run(index.nth(run(index.lower_bound_rank(owner->id, primary))));
   if (!same_table(row, owner->id)) {
      return end(row_kind::primary, owner->id);
   }
   return cache(row_kind::primary, forge::db::ids::to_object_id(row->id), row->table_id);
}

std::int32_t host::impl::db_upperbound_i64(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                           std::uint64_t primary) {
   const auto owner = run(lookup_table(*transaction_, code, scope, table_name));
   if (!owner) {
      return -1;
   }
   auto index = transaction_->index<key_value_index, by_scope_primary>();
   const auto row = run(index.nth(run(index.upper_bound_rank(owner->id, primary))));
   if (!same_table(row, owner->id)) {
      return end(row_kind::primary, owner->id);
   }
   return cache(row_kind::primary, forge::db::ids::to_object_id(row->id), row->table_id);
}

std::int32_t host::impl::db_end_i64(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name) {
   const auto owner = run(lookup_table(*transaction_, code, scope, table_name));
   return owner ? end(row_kind::primary, owner->id) : -1;
}

template <typename Row, typename Index, host::impl::row_kind Kind, typename Secondary>
std::int32_t host::impl::secondary_store(std::uint64_t scope, std::uint64_t table_name, std::uint64_t payer,
                                         std::uint64_t primary, Secondary secondary) {
   require_writable();
   require_payer(payer);
   require_ordered(secondary);
   const auto owner = run(ensure_table(*transaction_, receiver_, scope, table_name, payer));
   if (run(transaction_->index<Index, by_primary>().find(owner.id, primary))) {
      fail_database("database secondary primary key already exists");
   }
   const auto created = run(transaction_->create<Row>([&](Row& row) {
      row.table_id = owner.id;
      row.primary = primary;
      row.payer = payer;
      row.secondary = secondary;
   }));
   run(increment_table(*transaction_, owner.id));
   return cache(Kind, forge::db::ids::to_object_id(created.id), owner.id);
}

template <typename Row, typename Index, host::impl::row_kind Kind, typename Secondary>
void host::impl::secondary_update(std::int32_t iterator, std::uint64_t payer, Secondary secondary) {
   require_writable();
   require_ordered(secondary);
   const auto& entry = require_iterator(iterator, Kind);
   const auto owner = run(transaction_->get(entry.table_id));
   require_owner(owner, receiver_);
   run(transaction_->modify(typename Row::id_t{entry.id.instance}, [&](Row& row) {
      if (payer != 0U) {
         row.payer = payer;
      }
      row.secondary = secondary;
   }));
}

template <typename Row, typename Index, host::impl::row_kind Kind>
void host::impl::secondary_remove(std::int32_t iterator) {
   require_writable();
   const auto& entry = require_iterator(iterator, Kind);
   const auto owner = run(transaction_->get(entry.table_id));
   require_owner(owner, receiver_);
   run(transaction_->erase(typename Row::id_t{entry.id.instance}));
   run(decrement_table(*transaction_, entry.table_id));
   erase_iterator(iterator);
}

template <typename Row, typename Index, host::impl::row_kind Kind>
std::int32_t host::impl::secondary_next(std::int32_t iterator, std::uint64_t& primary) {
   const auto& entry = require_iterator(iterator, Kind);
   const auto current = run(transaction_->get(typename Row::id_t{entry.id.instance}));
   auto index = transaction_->index<Index, by_secondary>();
   const auto next = run(index.nth(run(index.rank(current)) + 1U));
   if (!same_table(next, entry.table_id)) {
      return end(Kind, entry.table_id);
   }
   primary = next->primary;
   return cache(Kind, forge::db::ids::to_object_id(next->id), next->table_id);
}

template <typename Row, typename Index, host::impl::row_kind Kind>
std::int32_t host::impl::secondary_previous(std::int32_t iterator, std::uint64_t& primary) {
   auto index = transaction_->index<Index, by_secondary>();
   auto table_id = table::id_t{};
   auto rank = std::uint64_t{};
   if (iterator < -1) {
      table_id = require_end(iterator, Kind);
      rank = run(index.equal_range_rank(table_id)).second;
   } else {
      const auto& entry = require_iterator(iterator, Kind);
      table_id = entry.table_id;
      const auto current = run(transaction_->get(typename Row::id_t{entry.id.instance}));
      rank = run(index.rank(current));
   }
   if (rank == 0U) {
      return -1;
   }
   const auto previous = run(index.nth(rank - 1U));
   if (!same_table(previous, table_id)) {
      return -1;
   }
   primary = previous->primary;
   return cache(Kind, forge::db::ids::to_object_id(previous->id), previous->table_id);
}

template <typename Row, typename Index, host::impl::row_kind Kind, typename Secondary>
std::int32_t host::impl::secondary_find_primary(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                                Secondary& secondary, std::uint64_t primary) {
   const auto owner = run(lookup_table(*transaction_, code, scope, table_name));
   if (!owner) {
      return -1;
   }
   const auto row = run(transaction_->index<Index, by_primary>().find(owner->id, primary));
   if (!row) {
      return end(Kind, owner->id);
   }
   secondary = row->secondary;
   return cache(Kind, forge::db::ids::to_object_id(row->id), row->table_id);
}

namespace {

template <typename T> bool equivalent(const T& left, const T& right) {
   return left == right;
}

bool equivalent(float64 left, float64 right) {
   return equal(left, right);
}

bool equivalent(float128 left, float128 right) {
   return equal(left, right);
}

std::uint64_t to_key(std::uint64_t value) {
   return value;
}

uint128 to_key(unsigned __int128 value) {
   return uint128{std::array<uint128::word_type, uint128::num_words()>{value}};
}

float64 to_key(double value) {
   return float64{.bits = std::bit_cast<std::uint64_t>(value)};
}

float128 to_key(float128 value) {
   return value;
}

void from_key(std::uint64_t& output, std::uint64_t value) {
   output = value;
}

void from_key(unsigned __int128& output, const uint128& value) {
   output = value.get_array()[0];
}

void from_key(double& output, float64 value) {
   output = std::bit_cast<double>(value.bits);
}

void from_key(float128& output, float128 value) {
   output = value;
}

uint256 read_uint256(forge::vm::wasm::interpret::span<const unsigned __int128> value) {
   if (value.size() != 2U) {
      fail_database("idx256 data_len must equal two");
   }
   auto words = std::array<unsigned __int128, 2>{};
   std::memcpy(words.data(), value.data(), sizeof(words));
   return uint256::make_from_word_sequence(words[0], words[1]);
}

uint256 read_uint256(forge::vm::wasm::interpret::span<unsigned __int128> value) {
   return read_uint256(forge::vm::wasm::interpret::span<const unsigned __int128>{value.data(), value.size()});
}

void write_uint256(forge::vm::wasm::interpret::span<unsigned __int128> output, const uint256& value) {
   if (output.size() != 2U) {
      fail_database("idx256 data_len must equal two");
   }
   const auto words = value.get_array();
   std::memcpy(output.data(), words.data(), sizeof(words));
}

} // namespace

template <typename Row, typename Index, host::impl::row_kind Kind, typename Secondary>
std::int32_t host::impl::secondary_find_secondary(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                                  const Secondary& secondary, std::uint64_t& primary) {
   require_ordered(secondary);
   const auto owner = run(lookup_table(*transaction_, code, scope, table_name));
   if (!owner) {
      return -1;
   }
   auto index = transaction_->index<Index, by_secondary>();
   const auto row = run(index.nth(run(index.lower_bound_rank(owner->id, secondary))));
   if (!same_table(row, owner->id) || !equivalent(row->secondary, secondary)) {
      return end(Kind, owner->id);
   }
   primary = row->primary;
   return cache(Kind, forge::db::ids::to_object_id(row->id), row->table_id);
}

template <typename Row, typename Index, host::impl::row_kind Kind, typename Secondary>
std::int32_t host::impl::secondary_bound(bool upper, std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                         Secondary& secondary, std::uint64_t& primary) {
   require_ordered(secondary);
   const auto owner = run(lookup_table(*transaction_, code, scope, table_name));
   if (!owner) {
      return -1;
   }
   auto index = transaction_->index<Index, by_secondary>();
   const auto rank =
       upper ? run(index.upper_bound_rank(owner->id, secondary)) : run(index.lower_bound_rank(owner->id, secondary));
   const auto row = run(index.nth(rank));
   if (!same_table(row, owner->id)) {
      return end(Kind, owner->id);
   }
   primary = row->primary;
   secondary = row->secondary;
   return cache(Kind, forge::db::ids::to_object_id(row->id), row->table_id);
}

template <typename Row, typename Index, host::impl::row_kind Kind>
std::int32_t host::impl::secondary_end(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name) {
   const auto owner = run(lookup_table(*transaction_, code, scope, table_name));
   return owner ? end(Kind, owner->id) : -1;
}

#define FORGE_CONTRACT_TEST_DEFINE_SECONDARY(prefix, row_type, index_type, kind_name, input_type, output_type)         \
   std::int32_t host::impl::prefix##_store(std::uint64_t scope, std::uint64_t table_name, std::uint64_t payer,         \
                                           std::uint64_t primary, input_type secondary) {                              \
      return secondary_store<row_type, index_type, row_kind::kind_name>(scope, table_name, payer, primary,             \
                                                                        to_key(*secondary.get()));                     \
   }                                                                                                                   \
   void host::impl::prefix##_update(std::int32_t iterator, std::uint64_t payer, input_type secondary) {                \
      secondary_update<row_type, index_type, row_kind::kind_name>(iterator, payer, to_key(*secondary.get()));          \
   }                                                                                                                   \
   void host::impl::prefix##_remove(std::int32_t iterator) {                                                           \
      secondary_remove<row_type, index_type, row_kind::kind_name>(iterator);                                           \
   }                                                                                                                   \
   std::int32_t host::impl::prefix##_next(std::int32_t iterator, output<std::uint64_t> primary) {                      \
      return secondary_next<row_type, index_type, row_kind::kind_name>(iterator, *primary.get());                      \
   }                                                                                                                   \
   std::int32_t host::impl::prefix##_previous(std::int32_t iterator, output<std::uint64_t> primary) {                  \
      return secondary_previous<row_type, index_type, row_kind::kind_name>(iterator, *primary.get());                  \
   }                                                                                                                   \
   std::int32_t host::impl::prefix##_find_primary(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,   \
                                                  output_type secondary, std::uint64_t primary) {                      \
      auto key = decltype(to_key(*secondary.get())){};                                                                 \
      const auto result =                                                                                              \
          secondary_find_primary<row_type, index_type, row_kind::kind_name>(code, scope, table_name, key, primary);    \
      if (result >= 0)                                                                                                 \
         from_key(*secondary.get(), key);                                                                              \
      return result;                                                                                                   \
   }                                                                                                                   \
   std::int32_t host::impl::prefix##_find_secondary(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name, \
                                                    input_type secondary, output<std::uint64_t> primary) {             \
      return secondary_find_secondary<row_type, index_type, row_kind::kind_name>(                                      \
          code, scope, table_name, to_key(*secondary.get()), *primary.get());                                          \
   }                                                                                                                   \
   std::int32_t host::impl::prefix##_lowerbound(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,     \
                                                output_type secondary, output<std::uint64_t> primary) {                \
      auto key = to_key(*secondary.get());                                                                             \
      const auto result = secondary_bound<row_type, index_type, row_kind::kind_name>(false, code, scope, table_name,   \
                                                                                     key, *primary.get());             \
      if (result >= 0)                                                                                                 \
         from_key(*secondary.get(), key);                                                                              \
      (void)output_type(std::move(secondary));                                                                         \
      (void)output<std::uint64_t>(std::move(primary));                                                                 \
      return result;                                                                                                   \
   }                                                                                                                   \
   std::int32_t host::impl::prefix##_upperbound(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,     \
                                                output_type secondary, output<std::uint64_t> primary) {                \
      auto key = to_key(*secondary.get());                                                                             \
      const auto result = secondary_bound<row_type, index_type, row_kind::kind_name>(true, code, scope, table_name,    \
                                                                                     key, *primary.get());             \
      if (result >= 0)                                                                                                 \
         from_key(*secondary.get(), key);                                                                              \
      (void)output_type(std::move(secondary));                                                                         \
      (void)output<std::uint64_t>(std::move(primary));                                                                 \
      return result;                                                                                                   \
   }                                                                                                                   \
   std::int32_t host::impl::prefix##_end(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name) {          \
      return secondary_end<row_type, index_type, row_kind::kind_name>(code, scope, table_name);                        \
   }

FORGE_CONTRACT_TEST_DEFINE_SECONDARY(db_idx64, index64, index64_index, index64, input<std::uint64_t>,
                                     output<std::uint64_t>)
FORGE_CONTRACT_TEST_DEFINE_SECONDARY(db_idx128, index128, index128_index, index128, uint128_input, uint128_output)
FORGE_CONTRACT_TEST_DEFINE_SECONDARY(db_idx_double, index_double, index_double_index, index_double, input<double>,
                                     output<double>)
FORGE_CONTRACT_TEST_DEFINE_SECONDARY(db_idx_long_double, index_long_double, index_long_double_index, index_long_double,
                                     float128_input, float128_output)

#undef FORGE_CONTRACT_TEST_DEFINE_SECONDARY

std::int32_t host::impl::db_idx256_store(std::uint64_t scope, std::uint64_t table_name, std::uint64_t payer,
                                         std::uint64_t primary, input_span<unsigned __int128, 16> secondary) {
   return secondary_store<index256, index256_index, row_kind::index256>(scope, table_name, payer, primary,
                                                                        read_uint256(secondary));
}

void host::impl::db_idx256_update(std::int32_t iterator, std::uint64_t payer,
                                  input_span<unsigned __int128, 16> secondary) {
   secondary_update<index256, index256_index, row_kind::index256>(iterator, payer, read_uint256(secondary));
}

void host::impl::db_idx256_remove(std::int32_t iterator) {
   secondary_remove<index256, index256_index, row_kind::index256>(iterator);
}

std::int32_t host::impl::db_idx256_next(std::int32_t iterator, output<std::uint64_t> primary) {
   return secondary_next<index256, index256_index, row_kind::index256>(iterator, *primary.get());
}

std::int32_t host::impl::db_idx256_previous(std::int32_t iterator, output<std::uint64_t> primary) {
   return secondary_previous<index256, index256_index, row_kind::index256>(iterator, *primary.get());
}

std::int32_t host::impl::db_idx256_find_primary(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                                output_span<unsigned __int128, 16> secondary, std::uint64_t primary) {
   auto key = uint256{};
   const auto result =
       secondary_find_primary<index256, index256_index, row_kind::index256>(code, scope, table_name, key, primary);
   if (result >= 0) {
      write_uint256(secondary, key);
   }
   return result;
}

std::int32_t host::impl::db_idx256_find_secondary(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                                  input_span<unsigned __int128, 16> secondary,
                                                  output<std::uint64_t> primary) {
   return secondary_find_secondary<index256, index256_index, row_kind::index256>(
       code, scope, table_name, read_uint256(secondary), *primary.get());
}

std::int32_t host::impl::db_idx256_lowerbound(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                              output_span<unsigned __int128, 16> secondary,
                                              output<std::uint64_t> primary) {
   auto key = read_uint256(secondary);
   const auto result = secondary_bound<index256, index256_index, row_kind::index256>(false, code, scope, table_name,
                                                                                     key, *primary.get());
   if (result >= 0) {
      write_uint256(secondary, key);
   }
   (void)output_span<unsigned __int128, 16>(std::move(secondary));
   (void)output<std::uint64_t>(std::move(primary));
   return result;
}

std::int32_t host::impl::db_idx256_upperbound(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                              output_span<unsigned __int128, 16> secondary,
                                              output<std::uint64_t> primary) {
   auto key = read_uint256(secondary);
   const auto result = secondary_bound<index256, index256_index, row_kind::index256>(true, code, scope, table_name, key,
                                                                                     *primary.get());
   if (result >= 0) {
      write_uint256(secondary, key);
   }
   (void)output_span<unsigned __int128, 16>(std::move(secondary));
   (void)output<std::uint64_t>(std::move(primary));
   return result;
}

std::int32_t host::impl::db_idx256_end(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name) {
   return secondary_end<index256, index256_index, row_kind::index256>(code, scope, table_name);
}

} // namespace forge::tooling::testing
