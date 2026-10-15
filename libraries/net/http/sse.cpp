module;

#include <forge/exceptions/macros.hpp>

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

module forge.net.http.sse;

import forge.net.http.exceptions;

namespace forge::net::http {

sse_decoder::sse_decoder(sse_limits limits)
  : _limits(limits)
{
   if (_limits.max_line_bytes == 0 || _limits.max_event_bytes == 0 || _limits.max_buffer_bytes == 0)
   {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "SSE limits must be positive");
   }
}

void sse_decoder::append(std::string_view bytes)
{
   if (_finished)
   {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "SSE decoder is finished");
   }

   if (bytes.size() > _limits.max_buffer_bytes - (_buffer.size() - _offset))
   {
      FORGE_THROW_EXCEPTION(exceptions::payload_too_large, "SSE input buffer exceeds its limit");
   }

   if (_offset != 0)
   {
      _buffer.erase(0, _offset);
      _offset = 0;
   }

   _buffer.append(bytes);
}

std::optional<sse_event> sse_decoder::consume_line()
{
   auto line = std::string_view{_line};
   if (_first_line)
   {
      _first_line = false;
      if (line.starts_with("\xef\xbb\xbf"))
      {
         line.remove_prefix(3);
      }
   }

   if (line.empty())
   {
      _line.clear();
      if (!_seen_data)
      {
         _event.clear();
         return std::nullopt;
      }

      _seen_data = false;
      _data.pop_back();
      auto result = sse_event{.data = std::move(_data), .event = std::move(_event), .id = _id, .retry = _retry};
      _data.clear();
      _event.clear();
      if (result.event.empty())
      {
         result.event = "message";
      }

      return result;
   }

   if (!line.starts_with(':'))
   {
      const auto colon = line.find(':');
      const auto field = line.substr(0, colon);
      auto value = colon == std::string_view::npos ? std::string_view{} : line.substr(colon + 1);
      if (value.starts_with(' '))
      {
         value.remove_prefix(1);
      }

      if (field == "data")
      {
         if (_data.size() > _limits.max_event_bytes || value.size() > _limits.max_event_bytes - _data.size())
         {
            FORGE_THROW_EXCEPTION(exceptions::payload_too_large, "SSE event exceeds its limit");
         }

         _data.append(value);
         _data.push_back('\n');
         _seen_data = true;
      }
      else if (field == "event")
      {
         _event.assign(value);
      }
      else if (field == "id" && value.find('\0') == std::string_view::npos)
      {
         _id.assign(value);
      }
      else if (field == "retry" && !value.empty())
      {
         auto retry = std::uint64_t{};
         const auto parsed = std::from_chars(value.data(), value.data() + value.size(), retry);
         if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size())
         {
            _retry = retry;
         }
      }
   }

   _line.clear();
   return std::nullopt;
}

std::optional<sse_event> sse_decoder::next()
{
   while (_offset < _buffer.size())
   {
      const auto value = _buffer[_offset++];
      if (_skip_lf)
      {
         _skip_lf = false;
         if (value == '\n')
         {
            continue;
         }
      }

      if (value == '\r' || value == '\n')
      {
         _skip_lf = value == '\r';
         if (auto event = consume_line())
         {
            return event;
         }
      }
      else
      {
         if (_line.size() >= _limits.max_line_bytes)
         {
            FORGE_THROW_EXCEPTION(exceptions::payload_too_large, "SSE line exceeds its limit");
         }

         _line.push_back(value);
      }
   }

   _buffer.clear();
   _offset = 0;
   if (_finished)
   {
      _line.clear();
      _data.clear();
      _event.clear();
      _seen_data = false;
   }

   return std::nullopt;
}

void sse_decoder::finish() noexcept
{
   _finished = true;
}

std::string format_sse(const sse_event& event, sse_limits limits)
{
   if (limits.max_line_bytes == 0 || limits.max_event_bytes == 0 || limits.max_buffer_bytes == 0)
   {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "SSE limits must be positive");
   }

   if (event.data.size() > limits.max_event_bytes)
   {
      FORGE_THROW_EXCEPTION(exceptions::payload_too_large, "SSE event exceeds its limit");
   }

   if (event.event.find_first_of("\r\n") != std::string::npos || event.id.find_first_of("\r\n") != std::string::npos ||
       event.event.find('\0') != std::string::npos || event.id.find('\0') != std::string::npos)
   {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "SSE metadata contains an invalid character");
   }

   auto output = std::string{};
   const auto append_line = [&](std::string_view name, std::string_view value)
   {
      if (name.size() + 2 > limits.max_line_bytes || value.size() > limits.max_line_bytes - name.size() - 2 ||
          name.size() + 3 > limits.max_buffer_bytes - output.size() ||
          value.size() > limits.max_buffer_bytes - output.size() - name.size() - 3)
      {
         FORGE_THROW_EXCEPTION(exceptions::payload_too_large, "SSE formatted output exceeds its limit");
      }

      output.append(name);
      output.append(": ");
      output.append(value);
      output.push_back('\n');
   };
   if (!event.event.empty())
   {
      append_line("event", event.event);
   }

   append_line("id", event.id);

   if (event.retry)
   {
      append_line("retry", std::to_string(*event.retry));
   }

   auto remaining = std::string_view{event.data};
   for (;;)
   {
      const auto end = remaining.find_first_of("\r\n");
      append_line("data", remaining.substr(0, end));
      if (end == std::string_view::npos)
      {
         break;
      }

      const auto cr = remaining[end] == '\r';
      remaining.remove_prefix(end + 1);
      if (cr && remaining.starts_with('\n'))
      {
         remaining.remove_prefix(1);
      }
   }

   if (output.size() == limits.max_buffer_bytes)
   {
      FORGE_THROW_EXCEPTION(exceptions::payload_too_large, "SSE formatted output exceeds its limit");
   }

   output.push_back('\n');
   return output;
}

} // namespace forge::net::http
