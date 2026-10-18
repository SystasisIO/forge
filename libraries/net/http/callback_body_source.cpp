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

callback_body_source::callback_body_source(body_reader::callback_type callback, std::function<void()> cancel)
  : _callback(std::make_shared<body_reader::callback_type>(std::move(callback)))
  , _cancel(std::move(cancel))
{
}

boost::asio::awaitable<std::optional<body_chunk>> callback_body_source::async_read()
{
   if (_cancelled)
   {
      FORGE_THROW_EXCEPTION(forge::asio::exceptions::canceled, "HTTP body source was cancelled");
   }

   // Keep one mutable callable and retain its coroutine closure through the complete awaited call.
   auto callback = _callback;
   auto result = co_await (*callback)();
   if (_cancelled)
   {
      FORGE_THROW_EXCEPTION(forge::asio::exceptions::canceled, "HTTP body source was cancelled");
   }

   if (result)
   {
      _bytes += result->bytes.size();
   }

   co_return result;
}

std::uint64_t callback_body_source::bytes_read() const noexcept
{
   return _bytes.load();
}

void callback_body_source::cancel() noexcept
{
   if (!_cancelled.exchange(true) && _cancel)
   {
      try
      {
         _cancel();
      }
      catch (...)
      {
         // Cancellation is best effort at this noexcept boundary; subsequent reads remain cancelled.
      }
   }
}

} // namespace forge::net::http
