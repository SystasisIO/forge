#pragma once

#include "fixtures.hxx"
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace export_name_fixtures {
using fixtures::bytes;

inline std::vector<std::string> names() {
   return {"",         std::string(1, '\0'), "foo", std::string("foo\0a", 5), std::string("foo\0b", 5), "plain_ascii",
           "\xc3\xa9", "e\xcc\x81"};
}

inline std::vector<std::string> many_names() {
   auto result = names();
   for (unsigned i = 0; i < 40; ++i)
      result.push_back("ordinary_" + std::to_string(i));
   return result;
}

inline std::string suffixed(std::string_view name) {
   auto result = std::string{name};
   result.append("\0suffix", 7);
   return result;
}

inline void positive_i32(bytes& out, std::uint32_t value) {
   if (value > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
      throw std::length_error("Export fixture result is not a positive i32");
   for (;;) {
      const auto byte = static_cast<std::uint8_t>(value & 127);
      value >>= 7;
      const auto more = value != 0 || (byte & 64) != 0;
      out.push_back(static_cast<std::uint8_t>(byte | (more ? 128 : 0)));
      if (!more)
         return;
   }
}

inline bytes module(const std::vector<std::string>& exported) {
   if (exported.size() >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
      throw std::length_error("Too many export fixture functions");
   auto out = bytes{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   fixtures::section(out, 1, {1, 0x60, 0, 1, 0x7f});
   auto functions = bytes{};
   fixtures::size(functions, exported.size());
   functions.insert(functions.end(), exported.size(), 0);
   fixtures::section(out, 3, functions);
   auto exports = bytes{};
   auto code = bytes{};
   fixtures::size(exports, exported.size());
   fixtures::size(code, exported.size());
   for (std::size_t i = 0; i < exported.size(); ++i) {
      fixtures::text(exports, exported[i]);
      exports.push_back(0);
      fixtures::size(exports, i);
      auto body = bytes{0, 0x41};
      positive_i32(body, static_cast<std::uint32_t>(i + 1));
      body.push_back(0x0b);
      fixtures::size(code, body.size());
      code.insert(code.end(), body.begin(), body.end());
   }
   fixtures::section(out, 7, exports);
   fixtures::section(out, 10, code);
   return out;
}

inline bytes hook(std::string_view name) {
   auto out = bytes{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   fixtures::section(out, 1, {2, 0x60, 0, 1, 0x7f, 0x60, 0, 0});
   fixtures::section(out, 3, {2, 0, 1});
   fixtures::section(out, 6, {1, 0x7f, 1, 0x41, 0, 0x0b});
   auto exports = bytes{2};
   fixtures::text(exports, "read");
   exports.insert(exports.end(), {0, 0});
   fixtures::text(exports, name);
   exports.insert(exports.end(), {0, 1});
   fixtures::section(out, 7, exports);
   // read() returns the counter; the hook increments it without host effects.
   fixtures::section(out, 10, {2, 4, 0, 0x23, 0, 0x0b, 9, 0, 0x23, 0, 0x41, 1, 0x6a, 0x24, 0, 0x0b});
   return out;
}
} // namespace export_name_fixtures
