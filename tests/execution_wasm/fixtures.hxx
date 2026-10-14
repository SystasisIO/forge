#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace fixtures {
using bytes = std::vector<std::uint8_t>;

inline void size(bytes& out, std::size_t value) {
   do {
      const auto byte = static_cast<std::uint8_t>(value & 127);
      value >>= 7;
      out.push_back(byte | (value ? 128 : 0));
   } while (value);
}

inline void text(bytes& out, std::string_view value) {
   size(out, value.size());
   out.insert(out.end(), value.begin(), value.end());
}

inline void section(bytes& out, std::uint8_t id, const bytes& body) {
   out.push_back(id);
   size(out, body.size());
   out.insert(out.end(), body.begin(), body.end());
}

inline bytes echo() {
   // Copy action data into guest memory, then return an owned copy to the host.
   return {0x41, 0, 0x10, 0, 0x10, 1, 0x1a, 0x41, 0, 0x10, 0, 0x10, 2};
}

inline bytes module(bytes body = echo(), bool start_loop = false,
                    std::string_view first_import = "action_data_size", bool wrong_signature = false,
                    std::string_view export_name = "apply", bool wrong_apply = false) {
   auto out = bytes{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   section(out, 1, {7, 0x60, 3, 0x7e, 0x7e, 0x7e, 0,
                      0x60, 0, 1, 0x7f,
                      0x60, 2, 0x7f, 0x7f, 1, 0x7f,
                      0x60, 2, 0x7f, 0x7f, 0,
                      0x60, 3, 0x7f, 0x7f, 0x7f, 0,
                      0x60, 2, 0x7f, 0x7f, 0,
                      0x60, 0, 0});
   auto imports = bytes{5};
   const std::string_view names[]{first_import, "read_action_data", "set_action_return_value",
                                  "eosio_assert_message", "prints_l"};
   for (std::uint8_t i = 0; i != 5; ++i) {
      text(imports, "env");
      text(imports, names[i]);
      imports.push_back(0);
      imports.push_back(wrong_signature && i == 0 ? 6 : i + 1);
   }
   section(out, 2, imports);
   const auto type = static_cast<std::uint8_t>(wrong_apply ? 6 : 0);
   section(out, 3, start_loop ? bytes{2, type, 6} : bytes{1, type});
   section(out, 5, {1, 0, 1});
   section(out, 6, {1, 0x7f, 1, 0x41, 0, 0x0b});
   auto exports = bytes{1};
   text(exports, export_name);
   exports.insert(exports.end(), {0, 5});
   section(out, 7, exports);
   if (start_loop) section(out, 8, {6});
   auto code = bytes{static_cast<std::uint8_t>(start_loop ? 2 : 1)};
   body.insert(body.begin(), 0); // No local declarations.
   body.push_back(0x0b);
   size(code, body.size());
   code.insert(code.end(), body.begin(), body.end());
   if (start_loop) code.insert(code.end(), {7, 0, 0x03, 0x40, 0x0c, 0, 0x0b, 0x0b});
   section(out, 10, code);
   return out;
}
} // namespace fixtures
