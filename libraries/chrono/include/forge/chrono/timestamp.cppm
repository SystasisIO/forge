module;
#include <chrono>
#include <compare>

export module forge.chrono.timestamp;

export namespace forge::chrono {

// Wide wall-time value; the nanosecond remainder is always in [0, 1 second).
// Clock acquisition and deadline arithmetic remain with std::chrono callers.
class timestamp {
 public:
   timestamp() = default;
   explicit timestamp(std::chrono::sys_seconds whole_seconds, std::chrono::nanoseconds subsecond = {});
   timestamp(std::chrono::sys_time<std::chrono::nanoseconds> value);

   [[nodiscard]] std::chrono::sys_seconds whole_seconds() const noexcept;
   [[nodiscard]] std::chrono::nanoseconds subsecond() const noexcept;

   [[nodiscard]] friend bool operator==(const timestamp&, const timestamp&) noexcept = default;
   friend std::strong_ordering operator<=>(const timestamp& left, const timestamp& right) noexcept;

 private:
   std::chrono::sys_seconds whole_seconds_{};
   std::chrono::nanoseconds subsecond_{};
};

} // namespace forge::chrono
