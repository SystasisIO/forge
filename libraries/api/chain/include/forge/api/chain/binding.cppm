module;

#include <boost/asio/awaitable.hpp>

#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module forge.api.chain.binding;

export import forge.api.core.binding;
export import forge.api.chain.runtime;
export import forge.chain.protocol.transaction_query;
export import forge.chain.transaction.builder;

export namespace forge::api::chain {

template <typename Interface> class proxy;

namespace detail {

template <typename Method> struct action_signature;

template <typename Owner, typename Request, bool NoThrow>
struct action_signature<void (Owner::*)(Request) const noexcept(NoThrow)> {
   static_assert(std::is_aggregate_v<Request>, "Chain API action requires a request aggregate passed by value");
   static_assert(std::is_same_v<decltype(Request::transaction), ::forge::chain::transaction::builder&>,
                 "Chain API request must provide a transaction builder reference");

   using request_type = Request;
};

template <auto Method> using action_request_t = typename action_signature<decltype(Method)>::request_type;

[[nodiscard]] core::api_id contract_id(std::string_view type);
void report_interceptor_failure(const core::api_ref& contract, std::string_view method) noexcept;

struct client {
   core::handle<runtime> service;
   destination target;
   std::vector<core::interceptor_step> interceptors;

   client(core::handle<runtime> service, destination target,
          std::vector<core::interceptor_step> steps);

   boost::asio::awaitable<void> intercept(core::call_context& context) const;
};

template <typename Interface, auto Method, typename Arguments, typename MakeRequest>
boost::asio::awaitable<::forge::chain::protocol::transaction_id>
invoke(std::shared_ptr<const client> client, core::handle<Interface> implementation,
       std::string_view method, Arguments arguments, MakeRequest make_request) {
   const auto contract = Interface::ref();
   auto context = core::call_context{
       .api = contract,
       .method = std::string{method},
       .codec = {.value = "forge.typed"},
       .request = core::request_view::borrow(arguments),
   };
   auto id = ::forge::chain::protocol::transaction_id{};
   auto failure = std::exception_ptr{};
   try {
      co_await client->intercept(context);
      auto transaction = co_await client->service->begin(client->target);
      std::invoke(Method, implementation.operator->(), make_request(arguments, transaction));
      id = co_await client->service->submit(client->target, contract, std::string{method}, transaction.build());
   } catch (...) {
      failure = std::current_exception();
   }

   context.kind = failure ? core::frame_kind::error : core::frame_kind::response;
   context.request = failure ? core::request_view{} : core::request_view::borrow(id);
   context.payload.clear();
   try {
      co_await client->intercept(context);
   } catch (...) {
      report_interceptor_failure(contract, method);
   }
   if (failure) {
      std::rethrow_exception(failure);
   }

   co_return id;
}

} // namespace detail

template <core::local_interface Interface>
[[nodiscard]] boost::asio::awaitable<core::handle<proxy<Interface>>>
remote(core::view apis, destination target, std::vector<core::interceptor_step> interceptors = {}) {
   auto implementation = apis.get<Interface>(Interface::ref());
   auto client = std::make_shared<const detail::client>(apis.get<runtime>(runtime::ref()), std::move(target),
                                                       std::move(interceptors));
   co_return core::handle<proxy<Interface>>{
       std::make_shared<proxy<Interface>>(std::move(client), std::move(implementation))};
}

boost::asio::awaitable<::forge::chain::protocol::transaction_status_response>
status(core::view apis, destination target, ::forge::chain::protocol::transaction_id id);

} // namespace forge::api::chain
