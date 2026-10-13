#include <eosio/action.hpp>
#include <eosio/asset.hpp>
#include <eosio/ignore.hpp>

#include <array>
#include <cerrno>
#include <clocale>
#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <regex>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include <cwchar>
#include <iomanip>
#include <sstream>
#include <cstdarg>
#include <cmath>
#include <limits>

import forge.contract;
import forge.chain.protocol.authority;
import forge.contract.system;
import forge.raw.codec;

namespace protocol = forge::chain::protocol;

extern "C" int __cxa_atexit(void (*function)(void*), void* argument, void* dso);
extern "C" void __cxa_finalize(void* dso);
extern "C" void __funcs_on_exit(void);

namespace {

std::uint32_t constructor_count = 0;
std::uint32_t destructor_count = 0;
std::uint32_t handler_count = 0;
std::array<std::uintptr_t, 40> handlers{};

struct lifetime_probe {
   lifetime_probe() {
      ++constructor_count;
   }
   ~lifetime_probe() {
      ++destructor_count;
   }
} probe;

std::set<protocol::name> strategies{protocol::name{"eosio.rex"}, protocol::name{"eosio.bonds"}};
std::uint32_t invocation_count = 0;

void record_handler(void* argument) {
   forge::contract::check(handler_count < handlers.size(), "atexit handler overflow");
   handlers[handler_count++] = reinterpret_cast<std::uintptr_t>(argument);
}

struct inline_target {
   void newaccount(protocol::name, protocol::name, forge::contract::ignore<protocol::authority>,
                   forge::contract::ignore<protocol::authority>) {}
   void newaccount_const(protocol::name, protocol::name, forge::contract::ignore<protocol::authority>,
                         forge::contract::ignore<protocol::authority>) const {}
};

constexpr protocol::symbol_code empty_code() {
   return {};
}

int print_arguments(FILE* stream, const char* format, ...) {
   va_list arguments;
   va_start(arguments, format);
   const auto result = stream ? std::vfprintf(stream, format, arguments) : std::vprintf(format, arguments);
   va_end(arguments);
   return result;
}

int format_arguments(char* buffer, std::size_t size, const char* format, ...) {
   va_list arguments;
   va_start(arguments, format);
   const auto result = std::vsnprintf(buffer, size, format, arguments);
   va_end(arguments);
   return result;
}

int unbounded_arguments(char* buffer, const char* format, ...) {
   va_list arguments;
   va_start(arguments, format);
   const auto result = std::vsprintf(buffer, format, arguments);
   va_end(arguments);
   return result;
}
static_assert(empty_code().raw() == 0);
static_assert(!std::convertible_to<std::uint64_t, protocol::symbol_code>);
static_assert(sizeof(protocol::symbol_code) == sizeof(std::uint64_t));

} // namespace

// Deliberately shadows the generated apply parameter named code.
class [[forge::contract("donorcompat")]] code : public forge::contract::context {
 public:
   using context::context;

   [[forge::action]] std::uint32_t lifetime() {
      verify_lifetime();
      strategies.insert(protocol::name{"mutated"});
      return ++invocation_count;
   }

   [[forge::action]] void trap() {
      verify_lifetime();
      strategies.clear();
      ++invocation_count;
      forge::contract::check(false, "donor compatibility trap");
   }

   [[forge::action]] void eosioexit() {
      verify_lifetime();
      ++invocation_count;
      forge::contract::eosio_exit(7);
   }

   [[forge::action]] std::uint32_t heap() {
      verify_lifetime();
      auto* allocation = static_cast<unsigned char*>(std::malloc(256U * 1024U));
      forge::contract::check(allocation != nullptr, "lifetime allocation failed");
      allocation[0] = 42;
      allocation[256U * 1024U - 1] = 17;
      return reinterpret_cast<std::uintptr_t>(allocation);
   }

   [[forge::action]] std::uint32_t registry() {
      for (std::uintptr_t index = 1; index <= handlers.size(); ++index) {
         forge::contract::check(__cxa_atexit(record_handler, reinterpret_cast<void*>(index), nullptr) == 0,
                                "atexit registration failed");
      }
      __cxa_finalize(nullptr);
      forge::contract::check(handler_count == 0 && destructor_count == 0, "CDT finalize must not drain");
      __funcs_on_exit();
      forge::contract::check(handler_count == handlers.size(), "atexit spill handlers missing");
      for (std::uintptr_t index = 0; index < handlers.size(); ++index) {
         forge::contract::check(handlers[index] == handlers.size() - index, "atexit must drain LIFO");
      }
      forge::contract::check(destructor_count == 1, "global destructor not registered");
      __funcs_on_exit();
      forge::contract::check(handler_count == handlers.size() && destructor_count == 1, "atexit drained twice");
      return handler_count;
   }

   [[forge::action]] void cexit() {
      forge::contract::check(__cxa_atexit([](void*) {
         forge::contract::check(destructor_count == 0, "C exit handler order changed");
      }, nullptr, nullptr) == 0, "C exit registration failed");
      std::exit(9);
   }

   [[forge::action]] std::vector<std::uint8_t> wrapcheck() {
      const auto creator = protocol::name{"eosio"};
      const auto account = protocol::name{"alice"};
      const auto owner = protocol::authority{.threshold = 3,
         .accounts = {{{protocol::name{"bob"}, protocol::name{"active"}}, 5}},
         .waits = {{17, 7}}};
      const auto active = protocol::authority{.threshold = 11, .waits = {{23, 13}}};
      const auto expected = forge::raw::pack(std::tuple{creator, account, owner, active});
      const auto wrapper = forge::contract::action_wrapper<protocol::name{"newaccount"}, &inline_target::newaccount>{
         protocol::name{"eosio"}};
      const auto actual = wrapper.to_action(creator, account, owner, active);
      forge::contract::check(actual.data == expected, "ignore action wrapper dropped authority bytes");
      const auto variants = forge::contract::variant_action_wrapper<protocol::name{"newaccount"},
         &inline_target::newaccount, &inline_target::newaccount_const>{
            protocol::name{"eosio"}, std::vector<protocol::permission_level>{}};
      const auto variant = variants.to_action<1>(creator, account, owner, active);
      auto expected_variant = std::vector<std::uint8_t>{1};
      expected_variant.insert(expected_variant.end(), expected.begin(), expected.end());
      forge::contract::check(variant.data == expected_variant, "ignore variant wrapper dropped authority bytes");
      return actual.data;
   }

   [[forge::action]] std::vector<std::string> formatting() {
      auto result = std::vector<std::string>{};
      for (const auto precision : {0U, 4U, 18U, 255U}) {
         const auto symbol = protocol::symbol{"ABCDEFG", static_cast<std::uint8_t>(precision)};
         for (const auto amount : {std::int64_t{0}, std::int64_t{42}, std::int64_t{-42},
                                   protocol::asset::max_amount, -protocol::asset::max_amount}) {
            const auto value = protocol::asset{amount, symbol};
            const auto legacy = eosio::asset{value};
            const auto text = protocol::to_string(value);
            forge::contract::check(legacy.to_string() == text, "asset format owners disagree");
            forge::contract::check(forge::raw::pack(value) == forge::raw::pack(legacy), "asset wire changed");
            result.push_back(text);
         }
      }
      return result;
   }

   [[forge::action]] std::vector<std::uint8_t> extwire(protocol::extended_symbol value) {
      return forge::raw::pack(value);
   }

   [[forge::action]] bool ipfs(std::string value) {
      // Exact admin.yield donor grammar; do not narrow or replace std::regex.
      const auto pattern = std::regex{"^Qm[1-9A-Za-z]{44}$"};
      return std::regex_match(value, pattern);
   }

   [[forge::action]] bool cstdlib() {
      const auto locale = newlocale(LC_ALL_MASK, "C", nullptr);
      forge::contract::check(locale != nullptr, "C locale missing");
      const auto posix = newlocale(LC_ALL_MASK, "POSIX", nullptr);
      forge::contract::check(posix != nullptr, "POSIX locale missing");
      errno = 0;
      forge::contract::check(newlocale(LC_ALL_MASK, "en_US.UTF-8", nullptr) == nullptr && errno == EINVAL,
                             "unsupported named locale accepted");
      forge::contract::check(setlocale(LC_ALL, "fr_FR") == nullptr, "ambient locale accepted");
      forge::contract::check(std::strcmp(localeconv()->decimal_point, ".") == 0, "non-C decimal point");
      forge::contract::check(std::use_facet<std::ctype<char>>(std::locale::classic()).is(
         std::ctype_base::alpha, 'Z'), "LLVM classic ctype missing");
      forge::contract::check(std::use_facet<std::ctype<char>>(std::locale::classic()).tolower('Z') == 'z',
                             "LLVM classic ctype folding failed");
      char* end = nullptr;
      forge::contract::check(strtol(" -123tail", &end, 10) == -123 && std::strcmp(end, "tail") == 0,
                             "musl integer parsing failed");
      forge::contract::check(strtod_l("12.5end", &end, locale) == 12.5 && std::strcmp(end, "end") == 0,
                             "musl number parsing failed");
      forge::contract::check(strtof_l("1.5tail", &end, locale) == 1.5F && std::strcmp(end, "tail") == 0,
                             "musl typed float locale parsing failed");
      auto integer = 0;
      auto decimal = 0.0;
      forge::contract::check(std::sscanf("-17 12.5", "%d %lf", &integer, &decimal) == 2 &&
                             integer == -17 && decimal == 12.5, "musl string scanning failed");
      auto state = std::mbstate_t{};
      auto character = wchar_t{};
      forge::contract::check(std::mbrtowc(&character, "A", 1, &state) == 1 && character == L'A' &&
                             std::mbsinit(&state), "musl C-byte conversion failed");
      auto buffer = std::array<char, 32>{};
      forge::contract::check(std::snprintf(buffer.data(), buffer.size(), "%lld", -1234567890123LL) == 14 &&
                             std::strcmp(buffer.data(), "-1234567890123") == 0, "donor printf failed");
      auto time = tm{};
      time.tm_year = 124;
      time.tm_mon = 1;
      time.tm_mday = 29;
      forge::contract::check(strftime_l(buffer.data(), buffer.size(), "%Y-%m-%d", &time, locale) == 10 &&
                             std::strcmp(buffer.data(), "2024-02-29") == 0, "musl C calendar formatting failed");
      freelocale(locale);
      freelocale(posix);
      return true;
   }

   [[forge::action]] void badlocale() {
      static_cast<void>(std::locale{"en_US.UTF-8"});
   }

   [[forge::action]] void timezone() {
      auto buffer = std::array<char, 32>{};
      auto time = tm{};
      static_cast<void>(strftime(buffer.data(), buffer.size(), "%Z", &time));
   }

   [[forge::action]] bool numfacets() {
      auto output = std::ostringstream{};
      output.imbue(std::locale::classic());
      output << 12345.0;
      forge::contract::check(output.str() == "12345", "num_put default double failed");
      output.str("");
      output << std::setprecision(3) << 12345.0;
      forge::contract::check(output.str() == "1.23e+04", "num_put g precision failed");
      output.str("");
      output << std::scientific << std::setprecision(2) << 1.5;
      forge::contract::check(output.str() == "1.50e+00", "num_put scientific failed");
      output.str("");
      output << std::fixed << std::setprecision(2) << static_cast<long double>(1.5);
      forge::contract::check(output.str() == "1.50", "num_put fixed long double failed");
      output.str("");
      output << std::hexfloat << 1.5;
      forge::contract::check(output.str() == "0x1.8p+0", "num_put hex float failed");
      char* end = nullptr;
      const auto extended = strtold_l("1e400", &end, newlocale(LC_ALL_MASK, "C", nullptr));
      forge::contract::check(std::isfinite(extended) && extended > std::numeric_limits<double>::max() && *end == 0,
                             "long double beyond double parsing failed");
      output.str("");
      output << std::scientific << std::setprecision(3) << extended;
      forge::contract::check(output.str() == "1.000e+400", "num_put extended long double failed");
      auto input = std::istringstream{"-17 12.5"};
      input.imbue(std::locale::classic());
      auto integer = 0;
      auto decimal = 0.0;
      input >> integer >> decimal;
      forge::contract::check(integer == -17 && decimal == 12.5, "num_get C parsing failed");
      return true;
   }

   [[forge::action]] bool printfcheck() {
      auto buffer = std::array<char, 64>{};
      auto expect = [&](const char* format, double value, const char* expected) {
         const auto size = std::snprintf(buffer.data(), buffer.size(), format, value);
         forge::contract::check(size == static_cast<int>(std::strlen(expected)) &&
                                std::strcmp(buffer.data(), expected) == 0, "musl float formatting failed");
      };
      expect("%g", 12.5, "12.5");
      expect("%.3g", 12345.0, "1.23e+04");
      expect("%.3G", 12345.0, "1.23E+04");
      expect("%+.2e", 1.5, "+1.50e+00");
      expect("%.2E", 1.5, "1.50E+00");
      expect("%08.3f", 1.5, "0001.500");
      expect("%.2F", 1.5, "1.50");
      expect("%a", 1.5, "0x1.8p+0");
      expect("%A", 1.5, "0X1.8P+0");
      expect("%g", -0.0, "-0");
      expect("%g", std::numeric_limits<double>::infinity(), "inf");
      expect("%g", std::numeric_limits<double>::quiet_NaN(), "nan");
      expect("%a", std::numeric_limits<double>::max(), "0x1.fffffffffffffp+1023");
      expect("%a", std::numeric_limits<double>::min(), "0x1p-1022");
      forge::contract::check(std::snprintf(buffer.data(), buffer.size(), "%.2Lf", static_cast<long double>(1.5)) == 4 &&
                             std::strcmp(buffer.data(), "1.50") == 0, "musl long double formatting failed");
      forge::contract::check(std::sprintf(buffer.data(), "%llu", 18446744073709551615ULL) == 20 &&
                             std::strcmp(buffer.data(), "18446744073709551615") == 0, "musl max integer failed");
      forge::contract::check(format_arguments(buffer.data(), buffer.size(), "%lld", -9223372036854775807LL - 1) == 20 &&
                             std::strcmp(buffer.data(), "-9223372036854775808") == 0, "musl min integer failed");
      forge::contract::check(unbounded_arguments(buffer.data(), "%g", 1.5) == 3 &&
                             std::strcmp(buffer.data(), "1.5") == 0, "musl vsprintf failed");
      forge::contract::check(format_arguments(buffer.data(), 4, "%s", "abcdefgh") == 8 &&
                             std::strcmp(buffer.data(), "abc") == 0, "musl bounded truncation failed");
      forge::contract::check(std::snprintf(nullptr, 0, "%.2f", 1.5) == 4, "musl count-only formatting failed");
      buffer[0] = 'X';
      forge::contract::check(std::snprintf(buffer.data(), 1, "%s", "abcd") == 4 && buffer[0] == 0,
                             "musl one-byte NUL termination failed");
      errno = 0;
      forge::contract::check(std::snprintf(buffer.data(), 2147483648ULL, "%s", "abcd") == -1 && errno == EOVERFLOW,
                             "musl oversized buffer count not rejected");
      forge::contract::check(std::printf("printf:%g|", 1.5) == 11, "musl printf console failed");
      forge::contract::check(print_arguments(nullptr, "vprintf:%g|", 1.5) == 12, "musl vprintf console failed");
      forge::contract::check(print_arguments(stderr, "vfprintf:%g|", 1.5) == 13, "musl stderr formatting failed");
      forge::contract::check(print_arguments(stdout, "stdout:%g|", 1.5) == 11, "musl stdout formatting failed");
      const auto large = std::string(257, 'x');
      forge::contract::check(std::printf("%s", large.c_str()) == 257 &&
                             print_arguments(stderr, "%s", large.c_str()) == 257, "musl console buffer flush failed");
      return true;
   }

 private:
   void verify_lifetime() {
      forge::contract::check(constructor_count == 1 && destructor_count == 0 && invocation_count == 0,
                             "invocation global lifetime leaked");
      forge::contract::check(strategies.size() == 2 && strategies.contains(protocol::name{"eosio.rex"}),
                             "donor global set not reconstructed");
   }
};
