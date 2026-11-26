module;

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

module test.api_chain.counter_client;

import test.api_chain.counter;
import forge.api.chain.binding;
import forge.api.core.exceptions;
import forge.chain.protocol.action;
import forge.chain.api.table_key;
import forge.raw.codec;

namespace demo {
namespace protocol = forge::chain::protocol;

class counter_handler final : public demo::counter {
 public:
   boost::asio::awaitable<std::optional<std::uint64_t>> find(demo::find_request request) const override {
      if (request.key == 1 && request.state.verified(_seven)) {
         co_return std::uint64_t{7};
      }
      const auto key = forge::chain::api::encode_table_key(request.key);
      const auto rows = co_await request.state.get_table_rows({.code = protocol::account_name{"counter"},
          .scope = protocol::name{"counter"}, .table = protocol::table_name{"counts"},
          .lower_bound = key, .upper_bound = key, .limit = 1});
      if (rows.rows.empty())
         co_return std::nullopt;
      const auto value = forge::raw::unpack_exact<std::uint64_t>(rows.rows.front().value);
      if (request.key == 1 && value == 7) {
         request.state.confirm(_seven);
      }
      co_return value;
   }

   void increment(demo::increment_request request) const override {
      request.transaction.add_action(
          {protocol::permission_level{protocol::account_name{"alice"}, protocol::permission_name{"active"}},
           protocol::account_name{"counter"}, protocol::action_name{"increment"}, request.expected});
   }

 private:
   forge::api::chain::read_snapshot::check _seven;
};

void provide_counter(forge::app::application_context& context) {
   context.apis().install<counter>(std::make_shared<counter_handler>());
}
boost::asio::awaitable<protocol::transaction_id>
increment(forge::api::core::view apis, forge::api::chain::destination target, std::uint64_t expected) {
   auto counter = co_await forge::api::chain::remote<demo::counter>(apis, std::move(target));
   co_return co_await counter->increment(expected);
}

boost::asio::awaitable<protocol::transaction_id>
increment(forge::api::core::view apis, forge::api::chain::destination target, std::uint64_t expected,
          protocol::block_id block, protocol::time_point_sec expiration) {
   const auto counter = co_await forge::api::chain::remote<demo::counter>(apis, std::move(target));
   co_return co_await counter->increment(expected, forge::api::chain::action_options{block, expiration});
}

boost::asio::awaitable<protocol::block_id> finalized(forge::api::core::view apis, forge::api::chain::destination target) {
   return forge::api::chain::finalized(apis, std::move(target));
}

boost::asio::awaitable<std::optional<std::uint64_t>>
find_counter(forge::api::core::view apis, forge::api::chain::destination target, protocol::block_id block, std::uint64_t key) {
   const auto counter = co_await forge::api::chain::remote<demo::counter>(apis, std::move(target));
   co_return co_await counter->find(block, key);
}

boost::asio::awaitable<protocol::transaction_status_response>
status(forge::api::core::view apis, forge::api::chain::destination target, protocol::transaction_id id) {
   return forge::api::chain::status(apis, std::move(target), id);
}

boost::asio::awaitable<void> check_read_session(forge::api::core::view apis, forge::api::chain::destination target,
                                                 protocol::block_id block) {
   const auto counter = co_await forge::api::chain::remote<demo::counter>(apis, target);
   const auto other = co_await forge::api::chain::remote<demo::counter>(apis, forge::api::chain::http_endpoint{"other"});
   const auto number = protocol::calculate_block_num_from_id(block);
   co_await forge::api::chain::read(apis, target, number,
       [&](const forge::api::chain::read_session& session) -> boost::asio::awaitable<void> {
          if (session.anchor().block != block || (co_await counter->find(session, 1)) != 7 ||
              (co_await counter->find(session, 1)) != 7 || (co_await counter->find(session, 2))) {
             throw std::logic_error("Borrowed counter reads returned inconsistent values");
          }
          try {
             (void)co_await other->find(session, 1);
          } catch (const forge::api::core::exceptions::protocol_error&) {
             co_return;
          }
          throw std::logic_error("Counter read reused a snapshot for another destination");
       });
}

} // namespace demo
