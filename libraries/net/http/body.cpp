module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

module forge.net.http.body;

import forge.asio.exceptions;

#include "details/callback_body_source.hxx"

namespace forge::net::http {

body_reader body_reader::from_callback(callback_type callback, std::function<void()> cancel)
{
   if (!callback)
   {
      FORGE_THROW_EXCEPTION(forge::asio::exceptions::invalid_options, "HTTP body callback must not be empty");
   }

   return body_reader{std::make_shared<callback_body_source>(std::move(callback), std::move(cancel))};
}

} // namespace forge::net::http
