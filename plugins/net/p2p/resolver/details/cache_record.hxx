#pragma once

namespace forge::plugins::net::p2p::resolver {

struct cache_record {
   std::vector<entry> apis;
   std::chrono::steady_clock::time_point expires_at;
   std::chrono::steady_clock::time_point stored_at;
};

} // namespace forge::plugins::net::p2p::resolver
