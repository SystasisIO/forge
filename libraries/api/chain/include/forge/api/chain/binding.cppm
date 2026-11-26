module;

#include <boost/asio/awaitable.hpp>

#include <exception>
#include <functional>
#include <memory>
#include <optional>
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

template <typename Value> struct action_request {
   Value value;
   ::forge::chain::transaction::builder& transaction;
};

template <typename Interface> class proxy;

class read_session {
 public:
   using handler = std::function<boost::asio::awaitable<void>(const read_session&)>;

   read_session(const read_session&) = delete;
   read_session& operator=(const read_session&) = delete;

   [[nodiscard]] const ::forge::chain::protocol::state_anchor& anchor() const noexcept { return _snapshot.anchor(); }
   [[nodiscard]] ::forge::chain::protocol::time_point_sec time() const noexcept { return _snapshot.time(); }
   [[nodiscard]] const read_snapshot& for_contract(const runtime& service, const destination& target,
                                                   const core::api_ref& contract) const;

 private:
   friend boost::asio::awaitable<void> read(core::view, destination, std::optional<block_reference>,
                                            handler, std::optional<core::api_ref>);
   read_session(const runtime& service, const destination& target, const std::optional<core::api_ref>& contract,
                const read_snapshot& snapshot)
       : _service(service), _target(target), _contract(contract), _snapshot(snapshot) {}

   const runtime& _service;
   const destination& _target;
   const std::optional<core::api_ref>& _contract;
   const read_snapshot& _snapshot;
};

boost::asio::awaitable<void> read(core::view apis, destination target, std::optional<block_reference> block,
                                  read_session::handler handler, std::optional<core::api_ref> contract = {});

namespace detail {

template <typename Method> struct method_signature;

template <typename Owner, typename Request, bool NoThrow>
struct method_signature<void (Owner::*)(Request) const noexcept(NoThrow)> {
   static_assert(std::is_aggregate_v<Request>, "Chain API action requires a request aggregate passed by value");
   static_assert(std::is_same_v<decltype(Request::transaction), ::forge::chain::transaction::builder&>,
                 "Chain API request must provide a transaction builder reference");

   using request_type = Request;
   using result_type = ::forge::chain::protocol::transaction_id;
};

template <typename Owner, typename Request, typename Result, bool NoThrow>
struct method_signature<boost::asio::awaitable<Result> (Owner::*)(Request) const noexcept(NoThrow)> {
   static_assert(std::is_aggregate_v<Request> && std::is_same_v<decltype(Request::state), const read_snapshot&>,
                 "Chain API read requires a request with a const read_snapshot reference");
   static_assert(!std::is_void_v<Result> && !std::is_reference_v<Result>, "Chain API read must return a value");
   using request_type = Request;
   using result_type = Result;
};

template <auto Method> using method_request_t = typename method_signature<decltype(Method)>::request_type;
template <auto Method> using method_result_t = typename method_signature<decltype(Method)>::result_type;

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

template <typename Interface, typename Result, typename Arguments, typename Dispatch>
boost::asio::awaitable<Result> intercepted(std::shared_ptr<const client> client, std::string_view method,
                                           const Arguments& arguments, Dispatch dispatch) {
   const auto contract = Interface::ref();
   auto context = core::call_context{
       .api = contract,
       .method = std::string{method},
       .codec = {.value = "forge.typed"},
       .request = core::request_view::borrow(arguments),
   };
   auto result = std::optional<Result>{};
   auto failure = std::exception_ptr{};
   try {
      co_await client->intercept(context);
      result.emplace(co_await dispatch());
   } catch (...) {
      failure = std::current_exception();
   }

   context.kind = failure ? core::frame_kind::error : core::frame_kind::response;
   context.request = failure ? core::request_view{} : core::request_view::borrow(*result);
   context.payload.clear();
   try {
      co_await client->intercept(context);
   } catch (...) {
      report_interceptor_failure(contract, method);
   }
   if (failure) {
      std::rethrow_exception(failure);
   }
   co_return std::move(*result);
}

template <typename Interface, auto Method, typename Arguments, typename MakeRequest>
boost::asio::awaitable<::forge::chain::protocol::transaction_id>
invoke(std::shared_ptr<const client> client, core::handle<Interface> implementation,
       std::string_view method, std::optional<action_options> options, Arguments arguments, MakeRequest make_request) {
   using result = ::forge::chain::protocol::transaction_id;
   auto dispatch = [&]() -> boost::asio::awaitable<result> {
      auto transaction = co_await client->service->begin(client->target, options);
      std::invoke(Method, implementation.operator->(), make_request(arguments, transaction));
      co_return co_await client->service->submit(client->target, Interface::ref(), std::string{method}, transaction.build());
   };
   co_return co_await intercepted<Interface, result>(client, method, arguments, dispatch);
}

template <typename Interface, auto Method, typename Arguments, typename MakeRequest>
boost::asio::awaitable<method_result_t<Method>>
read(std::shared_ptr<const client> client, core::handle<Interface> implementation,
     std::string_view method, ::forge::chain::protocol::block_id anchor, Arguments arguments, MakeRequest make_request) {
   using result = method_result_t<Method>;
   auto dispatch = [&]() -> boost::asio::awaitable<result> {
      std::optional<result> value;
      co_await client->service->read(client->target, anchor,
         [&](const read_snapshot& snapshot) -> boost::asio::awaitable<void> {
            value.emplace(co_await std::invoke(Method, implementation.operator->(), make_request(arguments, snapshot)));
         }, Interface::ref());
      co_return std::move(value.value());
   };
   co_return co_await intercepted<Interface, result>(client, method, arguments, dispatch);
}

template <typename Interface, auto Method, typename Arguments, typename MakeRequest>
boost::asio::awaitable<method_result_t<Method>>
read(std::shared_ptr<const client> client, core::handle<Interface> implementation,
     std::string_view method, const read_session& session, Arguments arguments, MakeRequest make_request) {
   using result = method_result_t<Method>;
   auto dispatch = [&]() -> boost::asio::awaitable<result> {
      const auto& snapshot = session.for_contract(*client->service.operator->(), client->target, Interface::ref());
      co_return co_await std::invoke(Method, implementation.operator->(), make_request(arguments, snapshot));
   };
   co_return co_await intercepted<Interface, result>(client, method, arguments, dispatch);
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

template <core::local_interface Interface, typename Handler>
boost::asio::awaitable<void> read(core::view apis, Handler handler) {
   auto contract = Interface::ref();
   auto target = apis.get<runtime>(runtime::ref())->read_target(contract);
   const auto bound = co_await remote<Interface>(apis, target);
   co_await read(apis, std::move(target), std::nullopt, [&](const read_session& session) {
      return std::invoke(handler, *bound.operator->(), session);
   }, std::move(contract));
}

boost::asio::awaitable<::forge::chain::protocol::block_id> finalized(core::view apis, destination target);

boost::asio::awaitable<::forge::chain::protocol::transaction_status_response>
status(core::view apis, destination target, ::forge::chain::protocol::transaction_id id);

boost::asio::awaitable<::forge::chain::protocol::time_point_sec>
block_time(core::view apis, destination target, ::forge::chain::protocol::block_id block);

} // namespace forge::api::chain
