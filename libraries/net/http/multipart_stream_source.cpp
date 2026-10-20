module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.net.http.upload;

import forge.asio.exceptions;
import forge.net.http.exceptions;

#include "details/multipart_framing.hxx"
#include "details/multipart_stream_source.hxx"

namespace forge::net::http {

multipart_stream_source::multipart_stream_source(std::vector<multipart_stream_part> parts, std::string boundary,
                                                 multipart_stream_options options)
  : _parts(std::move(parts))
  , _boundary(std::move(boundary))
  , _options(options)
{
   if (!_options.chunk_bytes || _options.chunk_bytes > 64 * 1024 || !_options.max_part_header_bytes)
   {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "Invalid multipart streaming bounds");
   }

   if (_parts.size() > _options.max_parts)
   {
      FORGE_THROW_EXCEPTION(exceptions::payload_too_large, "Too many multipart parts");
   }

   _headers.reserve(_parts.size());
   for (const auto& part : _parts)
   {
      if (part.name.size() > _options.max_part_header_bytes ||
          (part.filename && part.filename->size() > _options.max_part_header_bytes) ||
          part.content_type.size() > _options.max_part_header_bytes)
      {
         FORGE_THROW_EXCEPTION(exceptions::payload_too_large, "Multipart metadata exceeds its bound");
      }

      auto header = multipart_framing::header(_boundary, part.name, part.filename, part.content_type);
      if (header.size() > _options.max_part_header_bytes)
      {
         FORGE_THROW_EXCEPTION(exceptions::payload_too_large, "Multipart headers exceed their bound");
      }

      _headers.push_back(std::move(header));
   }
}

void multipart_stream_source::account(std::size_t size)
{
   if (size > _options.max_total_bytes - _bytes.load())
   {
      FORGE_THROW_EXCEPTION(exceptions::payload_too_large, "Multipart stream exceeds its complete wire bound");
   }

   _bytes += size;
}

body_chunk multipart_stream_source::take(std::span<const std::byte> bytes)
{
   const auto count = (std::min)(bytes.size(), _options.chunk_bytes);
   account(count);
   return {.bytes = std::vector<std::byte>{bytes.begin(), bytes.begin() + count}};
}

boost::asio::awaitable<std::optional<body_chunk>> multipart_stream_source::async_read()
{
   if (_cancelled)
   {
      FORGE_THROW_EXCEPTION(forge::asio::exceptions::canceled, "Multipart stream was cancelled");
   }

   if (_failure)
   {
      std::rethrow_exception(_failure);
   }

   try
   {
      for (;;)
      {
         if (_cancelled)
         {
            FORGE_THROW_EXCEPTION(forge::asio::exceptions::canceled, "Multipart stream was cancelled");
         }

         if (_control_offset < _control.size())
         {
            auto result = take(std::as_bytes(std::span{_control}).subspan(_control_offset));
            _control_offset += result.bytes.size();
            co_return result;
         }

         _control.clear();
         _control_offset = 0;
         switch (_phase)
         {
         case phase::header:
            if (_part == _parts.size())
            {
               _phase = phase::closing;
               continue;
            }

            _control = _headers[_part];
            _phase = phase::body;
            break;
         case phase::body:
            if (_pending_offset < _pending.size())
            {
               auto result = take(std::span<const std::byte>{_pending}.subspan(_pending_offset));
               _pending_offset += result.bytes.size();
               co_return result;
            }

            _pending.clear();
            _pending_offset = 0;
            if (auto chunk = co_await _parts[_part].body.async_read())
            {
               if (_cancelled)
               {
                  FORGE_THROW_EXCEPTION(forge::asio::exceptions::canceled, "Multipart stream was cancelled");
               }

               if (chunk->bytes.size() > _options.max_part_bytes - _part_bytes)
               {
                  FORGE_THROW_EXCEPTION(exceptions::payload_too_large, "Multipart part exceeds its byte bound");
               }

               if (chunk->bytes.empty())
               {
                  // An empty nonterminal source chunk must not starve cancellation on the caller's executor.
                  co_await boost::asio::post(boost::asio::use_awaitable);
               }

               _part_bytes += chunk->bytes.size();
               _pending = std::move(chunk->bytes);
            }
            else
            {
               _phase = phase::ending;
            }

            break;
         case phase::ending:
            _control = "\r\n";
            _part_bytes = 0;
            ++_part;
            _phase = phase::header;
            break;
         case phase::closing:
            _control = "--" + _boundary + "--\r\n";
            _phase = phase::finished;
            break;
         case phase::finished:
            co_return std::nullopt;
         }
      }
   }
   catch (...)
   {
      _failure = std::current_exception();
      for (auto& part : _parts)
      {
         part.body.cancel();
      }

      throw;
   }
}

std::uint64_t multipart_stream_source::bytes_read() const noexcept
{
   return _bytes.load();
}

void multipart_stream_source::cancel() noexcept
{
   if (!_cancelled.exchange(true))
   {
      for (auto& part : _parts)
      {
         part.body.cancel();
      }
   }
}

} // namespace forge::net::http
