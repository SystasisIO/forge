module;

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <memory>
#include <utility>

module test.api_chain.counter_client;

import test.api_chain.counter;
import forge.api.chain.binding;
import forge.chain.protocol.action;

namespace demo {
namespace protocol = forge::chain::protocol;

class counter_handler final : public demo::counter {
 public:
   void increment(demo::increment_request request) const override {
      request.transaction.add_action(
          {protocol::permission_level{protocol::account_name{"alice"}, protocol::permission_name{"active"}},
           protocol::account_name{"counter"}, protocol::action_name{"increment"}, request.expected});
   }
};

void provide_counter(forge::app::application_context& context) {
   context.apis().install<counter>(std::make_shared<counter_handler>());
}
boost::asio::awaitable<protocol::transaction_id>
increment(forge::api::core::view apis, forge::api::chain::destination target, std::uint64_t expected) {
   auto counter = co_await forge::api::chain::remote<demo::counter>(apis, std::move(target));
   co_return co_await counter->increment(expected);
}

boost::asio::awaitable<protocol::transaction_status_response>
status(forge::api::core::view apis, forge::api::chain::destination target, protocol::transaction_id id) {
   return forge::api::chain::status(apis, std::move(target), id);
}

} // namespace demo
