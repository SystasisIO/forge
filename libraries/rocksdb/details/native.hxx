#pragma once

namespace forge::rocksdb {

namespace detail {

[[nodiscard]] ::rocksdb::Slice to_slice(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> bytes_from_slice(const ::rocksdb::Slice& value);
[[nodiscard]] bool starts_with(std::span<const std::byte> value, std::span<const std::byte> prefix);
[[nodiscard]] scan_result read_scan_page(std::unique_ptr<::rocksdb::Iterator> iterator, scan_request request, std::string_view context);
[[nodiscard]] ::rocksdb::ReadOptions to_native_options(const read_options& options);
[[nodiscard]] ::rocksdb::ReadOptions to_native_options(const read_options& options, const ::rocksdb::Snapshot* snapshot);
[[nodiscard]] ::rocksdb::WriteOptions to_native_options(const write_options& options);
[[nodiscard]] ::rocksdb::CompressionType to_native_compression(compression_type value);
[[nodiscard]] ::rocksdb::ColumnFamilyOptions to_native_options(const column_family_config& value);
void throw_if_error(const ::rocksdb::Status& status, std::string_view context);
void drain_process_diagnostics() noexcept;

} // namespace detail

} // namespace forge::rocksdb
