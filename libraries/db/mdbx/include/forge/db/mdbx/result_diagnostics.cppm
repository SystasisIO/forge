module;

#include <array>
#include <atomic>
#include <cstdint>

export module forge.db.mdbx.result_diagnostics;

export namespace forge::db::mdbx::detail {

// Private target-owned module, not an installed public diagnostic injection API.
class result_diagnostics final {
 public:
   [[nodiscard]] static result_diagnostics& process() noexcept;
   void record_result(int code) noexcept;
   void record_commit(bool committed) noexcept;
   void drain() noexcept;

 private:
   std::array<std::atomic<int>, 32> _codes{};
   std::array<std::atomic<std::uint64_t>, 32> _counts{};
   std::atomic<std::uint64_t> _overflow{0};
   std::atomic<int> _overflow_code{0};
   std::atomic<std::uint64_t> _commits{0};
   std::atomic<std::uint64_t> _rollbacks{0};
   std::atomic_flag _draining = ATOMIC_FLAG_INIT;
};

} // namespace forge::db::mdbx::detail
