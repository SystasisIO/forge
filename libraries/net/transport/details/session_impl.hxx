#pragma once

#include <memory>

namespace forge::net::transport {

struct session::impl {
   std::shared_ptr<detail::session_concept> model;
};

} // namespace forge::net::transport
