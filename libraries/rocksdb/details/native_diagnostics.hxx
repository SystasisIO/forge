#pragma once

namespace forge::rocksdb::detail {

class native_diagnostics final : public ::rocksdb::Logger, public ::rocksdb::EventListener {
 public:
   native_diagnostics();
   [[nodiscard]] static native_diagnostics& process() noexcept;
   void record_result(const ::rocksdb::Status& status) noexcept;
   void record_commit(bool committed) noexcept;
   void acquire_transaction() noexcept;
   void release_transaction() noexcept;
   [[nodiscard]] bool idle() const noexcept;

   void Logv(const char*, va_list) noexcept override;
   void Logv(::rocksdb::InfoLogLevel level, const char*, va_list) noexcept override;
   void OnBackgroundError(::rocksdb::BackgroundErrorReason reason,
                          ::rocksdb::Status* status) noexcept override;

   // Only the owner calls this after returning from RocksDB, never a vendor callback.
   void drain() noexcept;

 private:
   std::array<std::atomic<std::uint64_t>, ::rocksdb::NUM_INFO_LOG_LEVELS> _messages{};
   std::array<std::atomic<std::uint64_t>, ::rocksdb::Status::kMaxCode + 1> _errors{};
   std::array<std::atomic<unsigned>, ::rocksdb::Status::kMaxCode + 1> _latest_details{};
   std::array<std::atomic<unsigned>, ::rocksdb::Status::kMaxCode + 1> _max_severity{};
   std::array<std::atomic<std::uint64_t>, ::rocksdb::Status::kMaxCode + 1> _results{};
   std::array<std::atomic<unsigned>, ::rocksdb::Status::kMaxCode + 1> _result_details{};
   std::atomic<std::uint64_t> _commits{0};
   std::atomic<std::uint64_t> _rollbacks{0};
   std::atomic<std::uint64_t> _transactions{0};
   std::atomic_flag _draining = ATOMIC_FLAG_INIT;
};

} // namespace forge::rocksdb::detail
