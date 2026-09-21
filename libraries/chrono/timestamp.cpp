module;
#include <chrono>
#include <compare>
#include <cstdint>
#include <stdexcept>

module forge.chrono.timestamp;

namespace forge::chrono {

timestamp::timestamp(std::chrono::sys_seconds whole_seconds, std::chrono::nanoseconds subsecond)
    : whole_seconds_{whole_seconds}, subsecond_{subsecond} {
   if (subsecond_ < std::chrono::nanoseconds::zero() || subsecond_ >= std::chrono::seconds{1}) {
      throw std::invalid_argument{"timestamp subsecond must be in [0, 1 second)"};
   }
}

timestamp::timestamp(std::chrono::sys_time<std::chrono::nanoseconds> value) {
   constexpr auto scale = std::int64_t{1'000'000'000};
   const auto count = value.time_since_epoch().count();
   auto seconds = count / scale;
   auto fraction = count % scale;
   // Never convert floored seconds back to nanoseconds: INT64_MIN would overflow.
   if (fraction < 0) {
      --seconds;
      fraction += scale;
   }
   whole_seconds_ = std::chrono::sys_seconds{std::chrono::seconds{seconds}};
   subsecond_ = std::chrono::nanoseconds{fraction};
}

std::chrono::sys_seconds timestamp::whole_seconds() const noexcept {
   return whole_seconds_;
}

std::chrono::nanoseconds timestamp::subsecond() const noexcept {
   return subsecond_;
}

std::strong_ordering operator<=>(const timestamp& left, const timestamp& right) noexcept {
   if (const auto seconds = left.whole_seconds_ <=> right.whole_seconds_; seconds != 0) {
      return seconds;
   }
   return left.subsecond_ <=> right.subsecond_;
}

} // namespace forge::chrono
