module;

#include <boost/asio/awaitable.hpp>
#include <forge/api/core/macros.hpp>

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

export module forge.plugins.net.p2p.pubsub.api;

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.net.p2p.pubsub;
import forge.plugins.net.p2p.pubsub.types;
import forge.raw.raw;

export namespace forge::plugins::net::p2p::pubsub {

using handler = std::function<boost::asio::awaitable<forge::net::p2p::pubsub::validation_result>(message)>;
template <typename T>
using typed_handler = std::function<boost::asio::awaitable<forge::net::p2p::pubsub::validation_result>(typed_message<T>)>;

class api : public forge::api::core::contract<api> {
 public:
   virtual ~api() = default;

   virtual boost::asio::awaitable<message> publish(forge::net::p2p::pubsub::topic subject, std::vector<std::uint8_t> data,
                                                   publish_options options = {}) = 0;
   virtual boost::asio::awaitable<subscription> subscribe(forge::net::p2p::pubsub::topic subject, handler callback,
                                                          subscribe_options options = {}) = 0;
   virtual boost::asio::awaitable<void> unsubscribe(subscription value) = 0;
   [[nodiscard]] virtual std::vector<subscription> subscriptions() const = 0;
   [[nodiscard]] virtual snapshot snapshot() const = 0;

   template <typename T>
      requires(!std::same_as<std::remove_cvref_t<T>, std::vector<std::uint8_t>> &&
               !std::same_as<std::remove_cvref_t<T>, std::vector<char>>)
   boost::asio::awaitable<message> publish(forge::net::p2p::pubsub::topic subject, const T& value,
                                           publish_options options = {}) {
      auto packed = forge::raw::pack(value);
      auto bytes = std::vector<std::uint8_t>{packed.begin(), packed.end()};
      co_return co_await publish(std::move(subject), std::move(bytes), options);
   }

   template <typename T>
   boost::asio::awaitable<subscription> subscribe(forge::net::p2p::pubsub::topic subject, typed_handler<T> callback,
                                                  subscribe_options options = {}) {
      auto wrapper = [callback = std::move(callback)](message value) mutable
         -> boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> {
         auto typed = typed_message<T>{
            .source = std::move(value.source),
            .author = std::move(value.author),
            .subject = std::move(value.subject),
            .value = forge::raw::unpack<T>(value.data),
            .seqno = std::move(value.seqno),
         };
         co_return co_await callback(std::move(typed));
      };
      co_return co_await subscribe(std::move(subject), std::move(wrapper), options);
   }
};

} // namespace forge::plugins::net::p2p::pubsub

FORGE_EXPORT_API(::forge::plugins::net::p2p::pubsub::api, FORGE_API_CONTRACT("forge.plugins.net.p2p.pubsub", 1, 0))
