module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rocksdb/iterator.h>
#include <rocksdb/env.h>
#include <rocksdb/listener.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/status.h>

module forge.rocksdb.store;

import forge.exceptions;
import forge.rocksdb.exceptions;

#include "details/native.hxx"
#include "details/native_diagnostics.hxx"

namespace forge::rocksdb::detail {

::rocksdb::Slice to_slice(std::span<const std::byte> bytes) {
   return ::rocksdb::Slice{
      reinterpret_cast<const char*>(bytes.data()),
      bytes.size(),
   };
}

std::vector<std::byte> bytes_from_slice(const ::rocksdb::Slice& value) {
   std::vector<std::byte> bytes;
   bytes.resize(value.size());
   std::memcpy(bytes.data(), value.data(), value.size());
   return bytes;
}

bool starts_with(std::span<const std::byte> value, std::span<const std::byte> prefix) {
   return value.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), value.begin());
}

scan_result read_scan_page(std::unique_ptr<::rocksdb::Iterator> iterator,
                           scan_request request,
                           std::string_view context) {
   auto result = scan_result{};
   if (request.limit == 0) {
      return result;
   }
   const auto has_cursor = request.has_cursor || !request.cursor.empty();
   const auto& lower_bound = request.lower_bound.empty()
                                ? request.prefix
                                : request.lower_bound;
   if (!lower_bound.empty() && !starts_with(lower_bound, request.prefix)) {
      return result;
   }
   const auto cursor_before_lower_bound =
      has_cursor && request.cursor < lower_bound;
   if (has_cursor && !cursor_before_lower_bound &&
       !starts_with(request.cursor, request.prefix)) {
      return result;
   }

   if (!has_cursor) {
      iterator->Seek(to_slice(lower_bound));
   } else {
      iterator->Seek(to_slice(cursor_before_lower_bound
                                 ? lower_bound
                                 : request.cursor));
      if (iterator->Valid()) {
         const auto key = bytes_from_slice(iterator->key());
         if (key == request.cursor) {
            iterator->Next();
         }
      }
   }

   for (; iterator->Valid(); iterator->Next()) {
      auto key = bytes_from_slice(iterator->key());
      if (!starts_with(key, request.prefix)) {
         break;
      }
      result.entries.push_back(entry{.key = std::move(key), .value = bytes_from_slice(iterator->value())});
      if (result.entries.size() >= request.limit) {
         auto cursor = result.entries.back().key;
         iterator->Next();
         if (iterator->Valid()) {
            const auto next_key = bytes_from_slice(iterator->key());
            if (starts_with(next_key, request.prefix)) {
               result.next_cursor = std::move(cursor);
               result.has_next_cursor = true;
            }
         }
         break;
      }
   }
   throw_if_error(iterator->status(), context);
   return result;
}

namespace {

[[nodiscard]] status_code to_status_code(const ::rocksdb::Status& status) {
   if (status.ok()) {
      return status_code::ok;
   }
   if (status.IsNotFound()) {
      return status_code::not_found;
   }
   if (status.IsInvalidArgument()) {
      return status_code::invalid_argument;
   }
   if (status.IsCorruption()) {
      return status_code::corruption;
   }
   if (status.IsIOError()) {
      return status_code::io_error;
   }
   if (status.IsTimedOut()) {
      return status_code::timed_out;
   }
   if (status.IsBusy()) {
      return status_code::busy;
   }
   return status_code::unknown;
}

[[noreturn]] void throw_status(status_code code, std::string message) {
   switch (code) {
      case status_code::invalid_argument:
         FORGE_THROW_EXCEPTION(exceptions::invalid_argument, std::move(message));
      case status_code::corruption:
         FORGE_THROW_EXCEPTION(exceptions::corruption, std::move(message));
      case status_code::io_error:
         FORGE_THROW_EXCEPTION(exceptions::io_error, std::move(message));
      case status_code::timed_out:
         FORGE_THROW_EXCEPTION(exceptions::timed_out, std::move(message));
      case status_code::busy:
         FORGE_THROW_EXCEPTION(exceptions::busy, std::move(message));
      case status_code::ok:
      case status_code::not_found:
      case status_code::unknown:
         break;
   }
   FORGE_THROW_EXCEPTION(exceptions::internal_error, std::move(message));
}

} // namespace

void throw_if_error(const ::rocksdb::Status& status, std::string_view context) {
   if (status.ok()) {
      return;
   }
   native_diagnostics::process().record_result(status);
   throw_status(to_status_code(status), std::string{context} + ": " + status.ToString());
}

void drain_process_diagnostics() noexcept {
   native_diagnostics::process().drain();
}

::rocksdb::ReadOptions to_native_options(const read_options& options) {
   ::rocksdb::ReadOptions native;
   native.verify_checksums = options.verify_checksums;
   native.fill_cache = options.fill_cache;
   return native;
}

::rocksdb::ReadOptions to_native_options(const read_options& options, const ::rocksdb::Snapshot* snapshot) {
   auto native = to_native_options(options);
   native.snapshot = snapshot;
   return native;
}

::rocksdb::WriteOptions to_native_options(const write_options& options) {
   ::rocksdb::WriteOptions native;
   native.sync = options.sync;
   native.disableWAL = options.disable_wal;
   return native;
}

::rocksdb::CompressionType to_native_compression(compression_type value) {
   switch (value) {
      case compression_type::none:
         return ::rocksdb::kNoCompression;
      case compression_type::snappy:
         return ::rocksdb::kSnappyCompression;
      case compression_type::zlib:
         return ::rocksdb::kZlibCompression;
      case compression_type::bzip2:
         return ::rocksdb::kBZip2Compression;
      case compression_type::lz4:
         return ::rocksdb::kLZ4Compression;
      case compression_type::lz4hc:
         return ::rocksdb::kLZ4HCCompression;
      case compression_type::xpress:
         return ::rocksdb::kXpressCompression;
      case compression_type::zstd:
         return ::rocksdb::kZSTD;
   }
   return ::rocksdb::kNoCompression;
}

::rocksdb::ColumnFamilyOptions to_native_options(const column_family_config& value) {
   auto native = ::rocksdb::ColumnFamilyOptions{};
   native.enable_blob_files = value.blobs.enable_blob_files;
   native.min_blob_size = value.blobs.min_blob_size;
   native.blob_file_size = value.blobs.blob_file_size;
   native.blob_compression_type = to_native_compression(value.blobs.blob_compression_type);
   native.enable_blob_garbage_collection = value.blobs.enable_blob_garbage_collection;
   native.blob_garbage_collection_age_cutoff = value.blobs.blob_garbage_collection_age_cutoff;
   return native;
}

} // namespace forge::rocksdb::detail
