module;

#include <array>
#include <coroutine>
#include <boost/asio/awaitable.hpp>
#include <string>
#include <vector>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <forge/exceptions/macros.hpp>

module forge.expression.program;

import forge.expression.exceptions;
import forge.expression.worker_protocol;

namespace forge::expression {
namespace {

namespace protocol = worker_protocol;

void check(protocol::status status) {
   if (status == protocol::status::ok)
      return;
   switch (status) {
   case protocol::status::invalid_profile:
      FORGE_THROW_EXCEPTION(exceptions::invalid_profile, "invalid expression profile");
   case protocol::status::invalid_program:
      FORGE_THROW_EXCEPTION(exceptions::invalid_program, "expression program rejected");
   case protocol::status::invalid_input:
      FORGE_THROW_EXCEPTION(exceptions::invalid_input, "expression inputs rejected");
   case protocol::status::resource_exhausted:
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression resource budget exceeded");
   case protocol::status::evaluation_failed:
      FORGE_THROW_EXCEPTION(exceptions::evaluation_failed, "expression evaluation failed");
   default:
      FORGE_THROW_EXCEPTION(exceptions::internal, "expression worker failed");
   }
}

void admit_transport(std::string_view source, std::span<const variable> variables) {
   if (source.size() > limits{}.source_bytes || variables.size() > limits{}.variables)
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression transport input limit exceeded");
   for (const auto& item : variables) {
      if (item.name.size() > limits{}.string_bytes)
         FORGE_THROW_EXCEPTION(exceptions::invalid_profile, "expression declaration name limit exceeded");
   }
}

protocol::request preparation(std::string source, std::vector<variable> variables, limits budget) {
   protocol::request request;
   request.op = protocol::operation::prepare;
   request.source = std::move(source);
   request.declarations = std::move(variables);
   request.limits = budget;
   return request;
}

evaluation result(const protocol::response& reply) {
   check(reply.code);
   switch (reply.kind) {
   case protocol::value_kind::int64:
      return {forge::variant{reply.i64}, reply.ast_events};
   case protocol::value_kind::uint64:
      return {forge::variant{reply.u64}, reply.ast_events};
   case protocol::value_kind::boolean:
      return {forge::variant{reply.boolean}, reply.ast_events};
   default:
      FORGE_THROW_EXCEPTION(exceptions::internal, "expression worker returned an invalid result type");
   }
}

} // namespace

struct program::impl {
   std::weak_ptr<runtime::impl> owner;
   std::uint64_t id = 0;
   std::size_t variables = 0;

   ~impl() {
      runtime::release(owner.lock(), id);
   }
};

program::program(runtime& executor, std::string_view source, std::span<const variable> variables, limits budget) try {
   admit_transport(source, variables);
   auto request = preparation(std::string{source}, std::vector<variable>{variables.begin(), variables.end()}, budget);
   // Allocate the parent control record BEFORE accepting a remote program so
   // an allocation failure cannot leak an otherwise successfully prepared ID.
   auto state = std::make_shared<impl>();
   state->owner = executor._impl;
   state->variables = budget.variables;
   const auto reply = runtime::exchange(state->owner.lock(), std::move(request), {}, &state->id);
   check(reply.code);
   if (reply.prepared_id == 0)
      FORGE_THROW_EXCEPTION(exceptions::internal, "expression worker returned an invalid program identity");
   state->id = reply.prepared_id;
   _impl = std::move(state);
} catch (const std::bad_alloc&) {
   FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression control allocation failed");
}

program::~program() = default;
program::program(program&&) noexcept = default;
program& program::operator=(program&&) noexcept = default;

evaluation program::evaluate(const forge::variant_object& values) const {
   if (!_impl)
      FORGE_THROW_EXCEPTION(exceptions::invalid_profile, "expression program has been moved");
   if (values.size() > _impl->variables)
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression input count limit exceeded");
   protocol::request request;
   request.op = protocol::operation::evaluate;
   request.prepared_id = _impl->id;
   const auto reply = runtime::exchange(_impl->owner.lock(), std::move(request), values);
   return result(reply);
}

program::program(std::shared_ptr<impl> state) : _impl(std::move(state)) {}

boost::asio::awaitable<program> program::prepare_async(runtime& executor, std::string source,
                                                       std::vector<variable> variables, limits budget) try {
   // This outer function is deliberately not a coroutine: it owns source and
   // the runtime before returning a lazy awaitable to the caller.
   admit_transport(source, variables);
   auto request = preparation(std::move(source), std::move(variables), budget);
   auto state = std::make_shared<impl>();
   state->owner = executor._impl;
   state->variables = budget.variables;
   return [](std::shared_ptr<impl> state, protocol::request request) -> boost::asio::awaitable<program> {
      const auto reply = co_await runtime::async_exchange(state->owner.lock(), std::move(request), {}, &state->id);
      check(reply.code);
      if (reply.prepared_id == 0)
         FORGE_THROW_EXCEPTION(exceptions::internal, "expression worker returned an invalid program identity");
      state->id = reply.prepared_id;
      co_return program{std::move(state)};
   }(std::move(state), std::move(request));
} catch (const std::bad_alloc&) {
   FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression asynchronous control allocation failed");
}

boost::asio::awaitable<evaluation> program::evaluate_async(forge::variant_object values) const try {
   if (!_impl)
      FORGE_THROW_EXCEPTION(exceptions::invalid_profile, "expression program has been moved");
   if (values.size() > _impl->variables)
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression input count limit exceeded");
   return [](std::shared_ptr<impl> state, forge::variant_object values) -> boost::asio::awaitable<evaluation> {
      protocol::request request;
      request.op = protocol::operation::evaluate;
      request.prepared_id = state->id;
      const auto reply = co_await runtime::async_exchange(state->owner.lock(), std::move(request), std::move(values));
      co_return result(reply);
   }(_impl, std::move(values));
} catch (const std::bad_alloc&) {
   FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression asynchronous control allocation failed");
}

} // namespace forge::expression
