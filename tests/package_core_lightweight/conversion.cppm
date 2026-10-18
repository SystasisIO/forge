module;

#include <cstdint>

export module consumer.conversion;

import forge.core.conversion;

export namespace consumer {

struct result {
   std::uint64_t value;
};

} // namespace consumer

export namespace forge::core {

template <> class conversion<consumer::result> {
 public:
   explicit constexpr conversion(std::uint64_t value) : _value{value} {}

   [[nodiscard]] constexpr consumer::result finish() && {
      return {_value};
   }

 private:
   std::uint64_t _value;
};

} // namespace forge::core
