#pragma once

namespace forge::db::mdbx::detail {

class native_diagnostics final {
 public:
   [[nodiscard]] static native_diagnostics& process() noexcept;
   static void capture(MDBX_log_level_t level, const char*, int line, const char*, va_list) noexcept;

   void install();
   [[nodiscard]] bool installed() const noexcept;
   void drain() noexcept;

 private:
   static void emergency_notice() noexcept;
   std::once_flag _installation;
   std::atomic_bool _installed{false};
   std::array<std::atomic<std::uint64_t>, 8> _messages{};
   std::array<std::atomic<int>, 8> _latest_line{};
   std::atomic_flag _draining = ATOMIC_FLAG_INIT;
};

} // namespace forge::db::mdbx::detail
