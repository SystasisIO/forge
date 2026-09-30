#pragma once

namespace forge::plugins::net::http::server {

struct pending_binding {
   forge::api::http::binding_plan binding;
   publish_options options;
};

} // namespace forge::plugins::net::http::server
