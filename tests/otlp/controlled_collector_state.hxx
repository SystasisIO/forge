#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

import forge.asio.notification;

namespace otlp_concurrency_fixtures {

struct controlled_collector::state {
   mutable std::mutex mutex;
   std::condition_variable requests_changed;
   forge::asio::notification responses_released;
   forge::net::http::status status = forge::net::http::status::ok;
   std::vector<std::string> bodies;
   bool released = false;
};

} // namespace otlp_concurrency_fixtures
