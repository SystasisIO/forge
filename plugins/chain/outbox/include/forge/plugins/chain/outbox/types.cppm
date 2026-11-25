module;

#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

export module forge.plugins.chain.outbox.types;

import forge.api.core.types;
import forge.api.chain.outbox;
import forge.chain.api.verified_client_factory;
import forge.chain.protocol.transaction;
import forge.db.object.transaction;
import forge.schema.object;

export namespace forge::plugins::chain::outbox {

struct http_profile {
   std::string name;
   std::string url;
};

struct config {
   std::string store;
   std::vector<http_profile> http;
};

struct plugin_options {
   std::string store;
   forge::chain::api::verified_client_options verification;
   forge::api::chain::outbox::options delivery;
   std::function<forge::api::chain::outbox::delivery_policy(const forge::api::core::api_ref&, std::string_view)> policy;
   std::function<boost::asio::awaitable<void>(const forge::api::core::api_ref&, std::string_view,
                                             forge::db::object::transaction&,
                                             forge::chain::protocol::transaction_id,
                                             const forge::chain::protocol::packed_transaction&)> admit;
   std::vector<http_profile> http;
   bool p2p = true;
};

BOOST_DESCRIBE_STRUCT(http_profile, (), (name, url))
BOOST_DESCRIBE_STRUCT(config, (), (store, http))

} // namespace forge::plugins::chain::outbox

export template <> struct forge::schema::rules<forge::plugins::chain::outbox::http_profile> {
   static forge::schema::object_schema<forge::plugins::chain::outbox::http_profile> define();
};

export template <> struct forge::schema::rules<forge::plugins::chain::outbox::config> {
   static forge::schema::object_schema<forge::plugins::chain::outbox::config> define();
};
