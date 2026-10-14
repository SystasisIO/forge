#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>
#include <forge/api/core/macros.hpp>
#include <forge/api/http/macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <variant>

import forge.api.core.connection;
import forge.api.http.binding;
import forge.api.http.proxy;
import forge.api.http.openapi;
import forge.net.http.types;
import forge.variant.value;

namespace package_fixture {

struct request {
   std::uint64_t count = 0;
   std::variant<std::int8_t, std::string> choice;
};

class api : public forge::api::core::contract<api, forge::api::core::surface::local |
                                                forge::api::core::surface::remote> {
 public:
   virtual ~api() = default;
   virtual boost::asio::awaitable<request> inspect(request input) = 0;
};

BOOST_DESCRIBE_STRUCT(request, (), (count, choice))

} // namespace package_fixture

FORGE_API(package_fixture::api, FORGE_API_CONTRACT("package-schema", 1, 0),
          FORGE_API_METHOD(inspect, input))
FORGE_HTTP_API(package_fixture::api, FORGE_HTTP_POST(inspect, "/inspect", ok))

int main() {
   auto builder = forge::api::http::binding();
   (void)builder;
   const auto document = forge::api::http::openapi<package_fixture::api>();
   const auto& properties = document["paths"]["/inspect"]["post"]["requestBody"]
                                    ["content"]["application/json"]["schema"]["properties"];
   const auto& integer = properties["count"];
   if (integer["minimum"].as_uint64() != 0 ||
       integer["maximum"].as_uint64() != std::numeric_limits<std::uint64_t>::max()) {
      return 1;
   }
   const auto& choice = properties["choice"];
   const auto& branches = choice["oneOf"].get_array();
   if (branches.size() != 2 ||
       branches[0]["prefixItems"].get_array()[0]["const"].as_uint64() != 0 ||
       branches[1]["prefixItems"].get_array()[0]["const"].as_uint64() != 1 ||
       branches[0]["prefixItems"].get_array()[1]["minimum"].as_int64() != -128 ||
       branches[1]["prefixItems"].get_array()[1]["type"].as_string() != "string") {
      return 2;
   }
   return 0;
}
