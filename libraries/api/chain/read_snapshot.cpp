module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <coroutine>
#include <memory>
#include <utility>

module forge.api.chain.read_snapshot;

import forge.chain.api.exceptions;
import forge.chain.protocol.block;

namespace forge::api::chain {
namespace protocol = ::forge::chain::protocol;

read_snapshot::check::check() : _id(std::make_shared<const identity>()) {}

read_snapshot::read_snapshot(protocol::state_anchor anchor, protocol::time_point_sec time,
                             rows_reader rows, code_reader code, contract_caller trusted)
    : _anchor(std::move(anchor)), _time(time), _rows(std::move(rows)), _code(std::move(code)),
      _trusted(std::move(trusted)) {}

bool read_snapshot::verified(const check& rule) const noexcept {
   return std::atomic_load_explicit(&_verified, std::memory_order_acquire) == rule._id;
}

void read_snapshot::confirm(const check& rule) const noexcept {
   std::atomic_store_explicit(&_verified, rule._id, std::memory_order_release);
}

namespace {
template <typename Request> void pin(Request& request, const protocol::state_anchor& anchor) {
   if (request.anchor && request.anchor != anchor.block) {
      FORGE_THROW_EXCEPTION(::forge::chain::api::exceptions::invalid_request, "Read belongs to another block");
   }
   request.anchor = anchor.block;
}
void require_anchor(const protocol::audited_response& result, const protocol::state_anchor& expected) {
   if (result.context.anchor != expected) {
      FORGE_THROW_EXCEPTION(::forge::chain::api::exceptions::invalid_finality,
                            "Read changed its verified state anchor");
   }
}
} // namespace

boost::asio::awaitable<protocol::table_rows_response>
read_snapshot::get_table_rows(protocol::table_rows_request request) const {
   pin(request, _anchor);
   auto result = co_await _rows(std::move(request));
   require_anchor(result, _anchor);
   co_return result;
}

boost::asio::awaitable<protocol::code_response> read_snapshot::get_code(protocol::code_request request) const {
   pin(request, _anchor);
   auto result = co_await _code(std::move(request));
   require_anchor(result, _anchor);
   co_return result;
}

boost::asio::awaitable<std::optional<protocol::bytes>>
read_snapshot::call(protocol::account_name contract, protocol::bytes data) const {
   if (!_trusted) {
      FORGE_THROW_EXCEPTION(::forge::chain::api::exceptions::unavailable,
                            "This chain read profile does not trust contract execution");
   }
   auto result = co_await _trusted({.block = _anchor.block, .contract = contract, .data = std::move(data)});
   if (result.anchor != _anchor) {
      FORGE_THROW_EXCEPTION(::forge::chain::api::exceptions::invalid_finality,
                            "Contract execution returned a different chain state anchor");
   }
   co_return std::move(result.value);
}

} // namespace forge::api::chain
