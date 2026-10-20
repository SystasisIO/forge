#pragma once

namespace forge::net::http {

class multipart_framing
{
 public:
   static std::string boundary();
   static std::string header(std::string_view boundary, std::string_view name,
                             const std::optional<std::string>& filename, std::string_view content_type);

 private:
   static bool valid_header_value(std::string_view value) noexcept;
   static std::string quote(std::string_view value);
};

} // namespace forge::net::http
