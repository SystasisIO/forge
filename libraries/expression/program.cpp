module;

#include <array>
#include <algorithm>
#include <chrono>
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
import forge.expression.artifact_codec;
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

void check_operation(const operation_options& operation) {
   if (operation.cancel.stop_requested())
      FORGE_THROW_EXCEPTION(exceptions::cancelled, "expression operation cancelled");
   if (operation.deadline && std::chrono::steady_clock::now() >= *operation.deadline)
      FORGE_THROW_EXCEPTION(exceptions::deadline_exceeded, "expression operation deadline exceeded");
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

protocol::request restoration(const forge::expression::artifact& value) {
   validate(value);
   protocol::request request;
   request.op = protocol::operation::load;
   request.declarations = value.variables;
   request.limits = value.budget;
   request.expression = value.expression;
   request.result = value.result;
   return request;
}

forge::expression::artifact portable(const protocol::response& reply, std::vector<variable> variables, limits budget) {
   if (reply.expression.empty())
      FORGE_THROW_EXCEPTION(exceptions::internal, "expression worker returned an empty artifact");
   std::sort(variables.begin(), variables.end(),
             [](const auto& left, const auto& right) { return left.name < right.name; });
   forge::expression::artifact value;
   value.variables = std::move(variables);
   value.budget = budget;
   value.result = reply.result;
   value.expression = reply.expression;
   value.digest = artifact_digest(value);
   validate(value);
   return value;
}

evaluation evaluation_result(const protocol::response& reply) {
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
   forge::expression::artifact portable;

   ~impl() {
      runtime::release(owner.lock(), id);
   }
};

program::program(runtime& executor, std::string_view source, std::span<const variable> variables, limits budget,
                 operation_options operation) try {
   check_operation(operation);
   admit_transport(source, variables);
   auto request = preparation(std::string{source}, std::vector<variable>{variables.begin(), variables.end()}, budget);
   // Allocate the parent control record BEFORE accepting a remote program so
   // an allocation failure cannot leak an otherwise successfully prepared ID.
   auto state = std::make_shared<impl>();
   state->owner = executor._impl;
   state->variables = budget.variables;
   const auto reply = runtime::exchange(state->owner.lock(), std::move(request), {}, &state->id, operation);
   check(reply.code);
   if (reply.prepared_id == 0)
      FORGE_THROW_EXCEPTION(exceptions::internal, "expression worker returned an invalid program identity");
   state->id = reply.prepared_id;
   state->portable = portable(reply, std::vector<variable>{variables.begin(), variables.end()}, budget);
   check_operation(operation);
   _impl = std::move(state);
} catch (const std::bad_alloc&) {
   FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression control allocation failed");
}

program::program(runtime& executor, const forge::expression::artifact& value, operation_options operation) try {
   check_operation(operation);
   auto request = restoration(value);
   auto state = std::make_shared<impl>();
   state->owner = executor._impl;
   state->variables = value.budget.variables;
   state->portable = value;
   const auto reply = runtime::exchange(state->owner.lock(), std::move(request), {}, &state->id, operation);
   check(reply.code);
   if (reply.prepared_id == 0 || reply.expression != value.expression || reply.result != value.result)
      FORGE_THROW_EXCEPTION(exceptions::internal, "expression worker returned an incompatible artifact");
   state->id = reply.prepared_id;
   check_operation(operation);
   _impl = std::move(state);
} catch (const std::bad_alloc&) {
   FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression artifact control allocation failed");
}

forge::expression::artifact program::artifact() const try {
   if (!_impl)
      FORGE_THROW_EXCEPTION(exceptions::invalid_profile, "expression program has been moved");
   return _impl->portable;
} catch (const std::bad_alloc&) {
   FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression artifact control allocation failed");
}

program::~program() = default;
program::program(program&&) noexcept = default;
program& program::operator=(program&&) noexcept = default;

evaluation program::evaluate(const forge::variant_object& values, operation_options operation) const {
   if (!_impl)
      FORGE_THROW_EXCEPTION(exceptions::invalid_profile, "expression program has been moved");
   if (values.size() > _impl->variables)
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression input count limit exceeded");
   protocol::request request;
   request.op = protocol::operation::evaluate;
   request.prepared_id = _impl->id;
   const auto reply = runtime::exchange(_impl->owner.lock(), std::move(request), values, nullptr, operation);
   check_operation(operation);
   return evaluation_result(reply);
}

program::program(std::shared_ptr<impl> state) : _impl(std::move(state)) {}

boost::asio::awaitable<program> program::prepare_async(runtime& executor, std::string source,
                                                       std::vector<variable> variables, limits budget,
                                                       operation_options operation) try {
   // This outer function is deliberately not a coroutine: it owns source and
   // the runtime before returning a lazy awaitable to the caller.
   check_operation(operation);
   admit_transport(source, variables);
   auto request = preparation(std::move(source), std::move(variables), budget);
   auto state = std::make_shared<impl>();
   state->owner = executor._impl;
   state->variables = budget.variables;
   state->portable.variables = request.declarations;
   state->portable.budget = budget;
   return [](std::shared_ptr<impl> state, protocol::request request,
             operation_options operation) -> boost::asio::awaitable<program> {
      try {
         const auto reply =
             co_await runtime::async_exchange(state->owner.lock(), std::move(request), {}, &state->id, operation);
         check(reply.code);
         if (reply.prepared_id == 0)
            FORGE_THROW_EXCEPTION(exceptions::internal, "expression worker returned an invalid program identity");
         state->id = reply.prepared_id;
         state->portable = portable(reply, std::move(state->portable.variables), state->portable.budget);
         check_operation(operation);
         co_return program{std::move(state)};
      } catch (const std::bad_alloc&) {
         FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression asynchronous artifact allocation failed");
      }
   }(std::move(state), std::move(request), operation);
} catch (const std::bad_alloc&) {
   FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression asynchronous control allocation failed");
}

boost::asio::awaitable<program> program::load_async(runtime& executor, forge::expression::artifact value,
                                                    operation_options operation) try {
   check_operation(operation);
   auto request = restoration(value);
   auto state = std::make_shared<impl>();
   state->owner = executor._impl;
   state->variables = value.budget.variables;
   state->portable = std::move(value);
   return [](std::shared_ptr<impl> state, protocol::request request,
             operation_options operation) -> boost::asio::awaitable<program> {
      const auto reply =
          co_await runtime::async_exchange(state->owner.lock(), std::move(request), {}, &state->id, operation);
      check(reply.code);
      if (reply.prepared_id == 0 || reply.expression != state->portable.expression ||
          reply.result != state->portable.result)
         FORGE_THROW_EXCEPTION(exceptions::internal, "expression worker returned an incompatible artifact");
      state->id = reply.prepared_id;
      check_operation(operation);
      co_return program{std::move(state)};
   }(std::move(state), std::move(request), operation);
} catch (const std::bad_alloc&) {
   FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression asynchronous artifact allocation failed");
}

boost::asio::awaitable<evaluation> program::evaluate_async(forge::variant_object values,
                                                           operation_options operation) const try {
   if (!_impl)
      FORGE_THROW_EXCEPTION(exceptions::invalid_profile, "expression program has been moved");
   if (values.size() > _impl->variables)
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression input count limit exceeded");
   return [](std::shared_ptr<impl> state, forge::variant_object values,
             operation_options operation) -> boost::asio::awaitable<evaluation> {
      protocol::request request;
      request.op = protocol::operation::evaluate;
      request.prepared_id = state->id;
      const auto reply = co_await runtime::async_exchange(state->owner.lock(), std::move(request), std::move(values),
                                                          nullptr, operation);
      check_operation(operation);
      co_return evaluation_result(reply);
   }(_impl, std::move(values), operation);
} catch (const std::bad_alloc&) {
   FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression asynchronous control allocation failed");
}

} // namespace forge::expression
