#pragma once

#include <bit>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace expression_native {

struct input {
   std::string name;
   std::string type;
   std::string value;
   std::vector<std::string> items;
};

struct test_case {
   std::string id;
   std::string cel;
   std::string exprtk;
   std::vector<input> inputs;
   bool benchmark;
};

struct evaluation {
   enum class status { ok, evaluation_error, unsupported };

   status state = status::ok;
   std::string kind;
   std::variant<std::int64_t, std::uint64_t, double, bool> value = std::int64_t{0};
};

class program {
 public:
   virtual ~program() = default;
   virtual evaluation evaluate() = 0;
};

struct compilation {
   std::unique_ptr<program> executable;
   std::string error;
};

class backend {
 public:
   virtual ~backend() = default;
   virtual compilation compile(const test_case& value) = 0;
};

#include "cases.inc"

template <typename T> bool parse_integer(std::string_view text, T& value) {
   const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
   return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

inline std::string field(std::string value) {
   for (auto& character : value) {
      if (character == '\t' || character == '\r' || character == '\n') {
         character = ' ';
      }
   }
   return value;
}

inline std::uint64_t consume(const evaluation& value) {
   return std::visit(
       [](auto item) -> std::uint64_t {
          if constexpr (std::is_same_v<decltype(item), double>) {
             return std::bit_cast<std::uint64_t>(item);
          } else {
             return static_cast<std::uint64_t>(item);
          }
       },
       value.value);
}

inline void print_value(const evaluation& value) {
   std::visit(
       [](auto item) {
          if constexpr (std::is_same_v<decltype(item), double>) {
             std::cout << "double\t" << std::setprecision(std::numeric_limits<double>::max_digits10) << item;
          } else if constexpr (std::is_same_v<decltype(item), bool>) {
             std::cout << "bool\t" << (item ? "true" : "false");
          } else if constexpr (std::is_same_v<decltype(item), std::uint64_t>) {
             std::cout << "uint64\t" << item;
          } else {
             std::cout << "int64\t" << item;
          }
       },
       value.value);
}

// This is a benchmark result format, not a product API or an expression codec.
inline int run(int argc, char** argv, backend& engine, std::string_view name, std::string_view pin,
               std::string_view numeric_type) {
   if (argc != 5) {
      return 2;
   }

   std::uint64_t iterations = 0;
   std::uint64_t warmup = 0;
   std::uint64_t samples = 0;
   if (!parse_integer(argv[2], iterations) || !parse_integer(argv[3], warmup) || !parse_integer(argv[4], samples) ||
       iterations == 0 || iterations > 1'000'000 || warmup > 1'000'000 || samples == 0 || samples > 20) {
      return 2;
   }

   const test_case* selected = nullptr;
   for (const auto& value : cases) {
      if (value.id == argv[1]) {
         selected = &value;
         break;
      }
   }
   if (selected == nullptr) {
      return 2;
   }

   std::cout << "meta\t" << field(selected->id) << '\t' << name << '\t' << pin << '\t' << numeric_type << '\t'
             << corpus_sha256 << '\n';
   if (selected->cel.size() > 8192 || selected->exprtk.size() > 8192) {
      std::cout << "result\tguard_rejected\tsource_bytes\tnone\t0\n";
      return 0;
   }

   compilation compiled;
   for (std::uint64_t sample = 0; sample < samples; ++sample) {
      const auto started = std::chrono::steady_clock::now();
      auto next = engine.compile(*selected);
      const auto duration =
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
      std::cout << "compile\t" << sample << '\t' << duration << '\n';
      compiled = std::move(next);
      if (!compiled.executable) {
         std::cout << "result\tcompile_error\t" << field(compiled.error) << "\tnone\t0\n";
         return 0;
      }
   }

   auto initial = compiled.executable->evaluate();
   if (initial.state != evaluation::status::ok) {
      std::cout << "result\t" << (initial.state == evaluation::status::unsupported ? "unsupported" : "evaluation_error")
                << '\t' << field(initial.kind) << "\tnone\t0\n";
      return 0;
   }

   std::cout << "result\tok\t" << field(initial.kind) << '\t';
   print_value(initial);
   std::cout << '\n';
   if (!selected->benchmark) {
      return 0;
   }

   auto checksum = consume(initial);
   for (std::uint64_t index = 0; index < warmup; ++index) {
      const auto value = compiled.executable->evaluate();
      if (value.state != evaluation::status::ok) {
         return 3;
      }
      checksum ^= consume(value) + index;
   }

   for (std::uint64_t sample = 0; sample < samples; ++sample) {
      const auto started = std::chrono::steady_clock::now();
      for (std::uint64_t index = 0; index < iterations; ++index) {
         const auto value = compiled.executable->evaluate();
         if (value.state != evaluation::status::ok) {
            return 3;
         }
         checksum ^= consume(value) + index;
      }
      const auto duration =
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
      std::cout << "evaluate\t" << sample << '\t' << duration << '\t' << iterations << '\n';
   }
   std::cout << "checksum\t" << checksum << '\n';
   return 0;
}

} // namespace expression_native
