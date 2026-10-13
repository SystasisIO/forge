module;

#include <boost/asio/awaitable.hpp>
#include <forge/api/core/macros.hpp>

export module forge.plugins.log.otlp.api;

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.plugins.log.otlp.types;

export namespace forge::plugins::log::otlp {

class api : public forge::api::core::contract<api, forge::api::core::surface::local> {
 public:
   virtual ~api() = default;

   boost::asio::awaitable<void> flush() {
      return discard_flush_result(flush(flush_request{}));
   }

   boost::asio::awaitable<::forge::plugins::log::otlp::metrics> metrics() {
      return metrics(metrics_request{});
   }

   virtual boost::asio::awaitable<flush_result> flush(flush_request value) = 0;
   virtual boost::asio::awaitable<::forge::plugins::log::otlp::metrics> metrics(metrics_request value) = 0;

 private:
   static boost::asio::awaitable<void> discard_flush_result(boost::asio::awaitable<flush_result> operation);
};

} // namespace forge::plugins::log::otlp

FORGE_EXPORT_API(::forge::plugins::log::otlp::api, FORGE_API_CONTRACT("forge.plugins.log.otlp", 1, 0),
        FORGE_API_METHOD_TYPED(flush, ::forge::plugins::log::otlp::flush_request, ::forge::plugins::log::otlp::flush_result),
        FORGE_API_METHOD_TYPED(metrics,
                             ::forge::plugins::log::otlp::metrics_request,
                             ::forge::plugins::log::otlp::metrics))
