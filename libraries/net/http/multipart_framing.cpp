module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

module forge.net.http.upload;

import forge.net.http.exceptions;

#include "details/multipart_framing.hxx"

namespace forge::net::http {

bool multipart_framing::valid_header_value(std::string_view value) noexcept
{
   return std::ranges::none_of(value,
                                [](char character)
                                {
                                   return static_cast<unsigned char>(character) < 0x20U || character == 0x7FU;
                                });
}

std::string multipart_framing::quote(std::string_view value)
{
   if (!valid_header_value(value))
   {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "Multipart parameter contains unsafe characters");
   }

   auto output = std::string{};
   output.reserve(value.size());
   for (const auto character : value)
   {
      if (character == '"' || character == '\\')
      {
         output.push_back('\\');
      }

      output.push_back(character);
   }

   return output;
}

std::string multipart_framing::boundary()
{
   static auto next = std::atomic<std::uint64_t>{0};
   const auto value = next.fetch_add(1U, std::memory_order_relaxed) + 1U;
   return "forge-http-" +
          std::to_string(static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())) +
          '-' + std::to_string(value);
}

std::string multipart_framing::header(std::string_view boundary, std::string_view name,
                                      const std::optional<std::string>& filename, std::string_view content_type)
{
   auto result = "--" + std::string{boundary} + "\r\nContent-Disposition: form-data; name=\"" + quote(name) + '"';
   if (filename)
   {
      result += "; filename=\"" + quote(*filename) + '"';
   }

   result += "\r\n";
   if (!content_type.empty())
   {
      if (!valid_header_value(content_type))
      {
         FORGE_THROW_EXCEPTION(exceptions::bad_request, "Multipart content type contains unsafe characters");
      }

      result += "Content-Type: ";
      result += content_type;
      result += "\r\n";
   }

   result += "\r\n";
   return result;
}

} // namespace forge::net::http
