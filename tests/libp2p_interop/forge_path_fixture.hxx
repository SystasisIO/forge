#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

// Include after the node/stream modules. No native friendship/test switch added.
namespace forge::test::libp2p_interop {

class forge_path_fixture {
 public:
   using arguments = std::map<std::string, std::string>;
   struct support {
      std::function<forge::net::p2p::node::options(const arguments&)> make_options;
      std::function<forge::net::p2p::endpoint(const arguments&)> listen_endpoint;
   };

   explicit forge_path_fixture(std::string token);
   void record(std::string_view kind, std::string_view source, std::string fields_json);
   [[nodiscard]] std::string result(std::string_view peer, bool finalized, bool joined,
                                    std::string_view error = {}) const;
   static int run(const arguments&, const support&);

   boost::asio::awaitable<void> echo(forge::net::p2p::node&, forge::net::p2p::stream&,
                                     const forge::net::p2p::peer_id&, forge::net::p2p::path::kind,
                                     std::uint64_t session_id, std::string_view phase, bool server);

 private:
   std::string _token;
   mutable std::mutex _mutex;
   std::vector<std::string> _events;
   bool _overflow = false;
   std::vector<std::uint64_t> _captured_connections;
};

} // namespace forge::test::libp2p_interop
