module;

#include <forge/exceptions/macros.hpp>
#include <forge/log/macros.hpp>

#include <boost/asio/awaitable.hpp>

#include <coroutine>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

module forge.api.chain.binding;

import forge.log.logger;

namespace forge::api::chain {

const read_snapshot& read_session::for_contract(const runtime& service, const destination& target,
                                                const core::api_ref& contract) const {
   if (&service != &_service || target != _target || !service.same_read_profile(_contract, contract)) {
      FORGE_THROW_EXCEPTION(core::exceptions::protocol_error,
                            "Chain read session belongs to another runtime, destination or verification profile");
   }
   return _snapshot;
}

boost::asio::awaitable<void> read(core::view apis, destination target, std::optional<block_reference> block,
                                  read_session::handler handler, std::optional<core::api_ref> contract) {
   const auto service = apis.get<runtime>(runtime::ref());
   co_await service->read(target, block, [&](const read_snapshot& snapshot) -> boost::asio::awaitable<void> {
      if (block) {
         const auto* id = std::get_if<::forge::chain::protocol::block_id>(&*block);
         if (id ? snapshot.anchor().block != *id : snapshot.anchor().block_num != std::get<std::uint32_t>(*block)) {
            FORGE_THROW_EXCEPTION(core::exceptions::protocol_error, "Chain read session opened at another block");
         }
      }
      const read_session session{*service.operator->(), target, contract, snapshot};
      co_await handler(session);
   }, contract);
}

boost::asio::awaitable<::forge::chain::protocol::block_id> finalized(core::view apis, destination target) {
   const auto service = apis.get<runtime>(runtime::ref());
   co_return co_await service->finalized(std::move(target));
}

boost::asio::awaitable<::forge::chain::protocol::transaction_status_response>
status(core::view apis, destination target, ::forge::chain::protocol::transaction_id id) {
   const auto service = apis.get<runtime>(runtime::ref());
   co_return co_await service->status(std::move(target), std::move(id));
}

} // namespace forge::api::chain

namespace forge::api::chain {

boost::asio::awaitable<::forge::chain::protocol::time_point_sec>
block_time(core::view apis, destination target, ::forge::chain::protocol::block_id block) {
   auto time = ::forge::chain::protocol::time_point_sec{};
   co_await apis.get<runtime>(runtime::ref())->read(
       std::move(target), block, [&](const read_snapshot& state) -> boost::asio::awaitable<void> {
          time = state.time();
          co_return;
       });
   co_return time;
}

} // namespace forge::api::chain

namespace forge::api::chain::detail {

core::api_id contract_id(std::string_view type) {
   if (type.starts_with("::")) {
      type.remove_prefix(2);
   }
   auto id = std::string{type};
   for (auto pos = id.find("::"); pos != std::string::npos; pos = id.find("::", pos + 1)) {
      id.replace(pos, 2, ".");
   }
   return {.value = std::move(id)};
}

void report_interceptor_failure(const core::api_ref& contract, std::string_view method) noexcept {
   wlog("forge.api.chain", "Chain API terminal interceptor failed", ("api", contract.id.value)("method", method));
}

client::client(core::handle<runtime> service, destination target,
               std::vector<core::interceptor_step> steps)
    : service{std::move(service)}, target{std::move(target)} {
   auto binding = core::binding();
   for (auto& step : steps) {
      binding.interceptor(std::move(step));
   }
   interceptors = binding.build().interceptors;
}

boost::asio::awaitable<void> client::intercept(core::call_context& context) const {
   const auto kind = context.kind;
   for (const auto& step : interceptors) {
      const auto matches = kind == core::frame_kind::request ? step.phase <= core::interceptor_phase::before_call
                         : kind == core::frame_kind::response ? step.phase == core::interceptor_phase::after_call
                                                              : step.phase == core::interceptor_phase::error;
      if (matches) {
         co_await step.handler(context);
         if (!context.payload.empty()) {
            FORGE_THROW_EXCEPTION(core::exceptions::protocol_error,
                                  "Chain API interceptors cannot supply a serialized payload");
         }
      }
   }
}

} // namespace forge::api::chain::detail
