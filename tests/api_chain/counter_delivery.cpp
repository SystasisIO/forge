module;

#include <boost/asio/awaitable.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

module test.api_chain.counter_client;

import forge.plugins.chain.outbox.plugin;
import forge.api.chain.outbox;
import forge.api.core.types;
import forge.db.object.transaction;
import forge.db.core.driver;
import forge.db.core.record;

namespace demo {
namespace protocol = forge::chain::protocol;

namespace chain = forge::api::chain;
using namespace std::chrono_literals;
namespace {
void require(bool ok, const char* message) {
   if (!ok)
      throw std::runtime_error(message);
}
} // namespace
forge::app::plugin_descriptor delivery_plugin(std::shared_ptr<forge::chain::api::finality_verifier> finality,
                                              replay mode, bool p2p,
                                              std::shared_ptr<std::atomic_bool> reject) {
   forge::plugins::chain::outbox::plugin_options options{
       .store = "counter",
       .verification = {.chain = finality->trusted_chain().value(),
                        .state_domain = "forge.test.counter",
                        .finality = finality},
       .delivery = {.batch_limit = 2, .poll_interval = 250ms, .timeout = 5s},
       .policy =
           [mode](const forge::api::core::api_ref& contract, std::string_view method) {
              require(contract.id.value == "demo.counter" && method == "increment",
                      "Incorrect policy dispatch for counter");
              return chain::outbox::delivery_policy{.mode = mode == replay::same_packed
                                                                ? chain::outbox::replay::same_packed
                                                                : chain::outbox::replay::reconcile_only};
           },
       .admit = [reject](const forge::api::core::api_ref&, std::string_view, forge::db::object::transaction& write,
                         protocol::transaction_id id,
                         const protocol::packed_transaction& packed) -> boost::asio::awaitable<void> {
          require(packed.id() == id, "Admission received a different signed packet");
          const auto bytes = std::as_bytes(id.to_uint8_span());
          // Persist the client operation and the signed delivery in one MDBX commit.
          co_await write.db_transaction().put(forge::db::core::family{"counter.intent"},
                                              forge::db::core::record_key{{std::byte{1}}},
                                              std::vector<std::byte>{bytes.begin(), bytes.end()});
          if (reject && reject->load()) {
             throw std::domain_error("counter admission declined");
          }
       }};
   options.p2p = p2p;
   return forge::plugins::chain::outbox::plugin::descriptor(std::move(options));
}
} // namespace demo
