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
   explicit constexpr conversion(std::uint64_t value) : value_{value} {}

   [[nodiscard]] constexpr consumer::result finish() && {
      return {value_};
   }

 private:
   std::uint64_t value_;
};

} // namespace forge::core
