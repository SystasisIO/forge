module;

#include <boost/asio/awaitable.hpp>
#include <forge/contract/types.h>
#include <forge/vm/wasm/interpret/host_function.hpp>

#include <concepts>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

module forge.tooling.testing.host;

import forge.asio.runtime;
import forge.db.core.driver;
import forge.db.object.store;
import forge.db.object.transaction;
import forge.db.ids.object_id;
import forge.vm.wasm.interpret.backend;

#include "details/host_impl.hxx"

namespace forge::tooling::testing {

host::host(execution_limits limits) : impl_{std::make_unique<impl>(limits)} {}

#if defined(FORGE_TOOLING_TESTING_EXPRESSION)
host::host(forge::expression::runtime& expressions, execution_limits limits)
    : impl_{std::make_unique<impl>(expressions, limits)} {}
#endif

host::~host() = default;

invocation_result host::invoke(std::span<const std::uint8_t> code, std::uint64_t receiver, std::uint64_t first_receiver,
                               std::uint64_t action, std::vector<std::uint8_t> data) {
   return impl_->invoke(code, receiver, first_receiver, action, std::move(data));
}

invocation_result host::invoke(std::span<const std::uint8_t> code, std::uint64_t receiver, std::uint64_t first_receiver,
                               std::uint64_t action, std::vector<std::uint8_t> data, invocation_options options) {
   return impl_->invoke(code, receiver, first_receiver, action, std::move(data), std::move(options));
}

void host::transact(const std::function<void()>& body) {
   impl_->transact(body);
}

void host::configure(oracle_state state) {
   impl_->configure(std::move(state));
}

oracle_state host::state() const {
   return impl_->state();
}

void host::register_contract(std::uint64_t account, std::vector<std::uint8_t> code) {
   impl_->register_contract(account, std::move(code));
}

std::optional<table> host::find_table(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name) {
   return impl_->find_table(code, scope, table_name);
}

std::optional<key_value> host::find_primary(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                            std::uint64_t primary) {
   return impl_->find_primary(code, scope, table_name, primary);
}

std::optional<index64> host::find_index64(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                          std::uint64_t primary) {
   return impl_->find_secondary<index64, index64_index>(code, scope, table_name, primary);
}

std::optional<index128> host::find_index128(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                            std::uint64_t primary) {
   return impl_->find_secondary<index128, index128_index>(code, scope, table_name, primary);
}

std::optional<index256> host::find_index256(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                            std::uint64_t primary) {
   return impl_->find_secondary<index256, index256_index>(code, scope, table_name, primary);
}

std::optional<index_double> host::find_index_double(std::uint64_t code, std::uint64_t scope, std::uint64_t table_name,
                                                    std::uint64_t primary) {
   return impl_->find_secondary<index_double, index_double_index>(code, scope, table_name, primary);
}

std::optional<index_long_double> host::find_index_long_double(std::uint64_t code, std::uint64_t scope,
                                                              std::uint64_t table_name, std::uint64_t primary) {
   return impl_->find_secondary<index_long_double, index_long_double_index>(code, scope, table_name, primary);
}

std::vector<std::uint8_t> host::snapshot() const {
   return impl_->snapshot();
}

} // namespace forge::tooling::testing
