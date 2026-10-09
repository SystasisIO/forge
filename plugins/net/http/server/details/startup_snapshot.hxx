#pragma once

#include "pending_binding.hxx"

namespace forge::plugins::net::http::server {

struct startup_snapshot {
   std::vector<pending_binding> bindings;
   std::vector<middleware_descriptor> middleware;
   std::vector<forge::net::http::asset_bundle> asset_mounts;
   std::vector<route_mount> route_mounts;
};

} // namespace forge::plugins::net::http::server
