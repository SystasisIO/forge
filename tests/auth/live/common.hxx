#pragma once

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace auth_live {

struct options {
   std::string issuer;
   std::string client;
   std::string account;
   std::string run_id;
   std::string flow;
   std::vector<std::string> scopes{"openid", "profile", "offline_access"};
   std::optional<std::string> audience;
   std::chrono::seconds minimum_validity{10};
   std::chrono::seconds timeout{180};
};

// Only fixture-owned literal labels and public expiry/count data reach stdout.
void event(std::string_view status, std::string_view name, bool success = false,
           long long expires_at = 0, unsigned issuer_match_count = 0);

using job = std::function<boost::asio::awaitable<void>(const options&)>;
int run(int argc, char** argv, job operation, std::function<void()> run_ui = {},
        std::function<void()> stop_ui = {});
int run_appauth(int argc, char** argv, std::function<void()> run_ui, std::function<void()> stop_ui);

} // namespace auth_live
