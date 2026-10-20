#pragma once

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <unistd.h>

import forge.swarm.storage.file_store;
import forge.raw.raw;

namespace swarm_test {

namespace core = forge::swarm::core;
namespace storage = forge::swarm::storage;

template <typename T> boost::asio::awaitable<std::optional<T>> boxed(boost::asio::awaitable<T> work) {
   co_return co_await std::move(work);
}

template <typename T> T run(boost::asio::awaitable<T> work) {
   auto io = boost::asio::io_context{};
   if constexpr (std::is_void_v<T>) {
      auto result = boost::asio::co_spawn(io, std::move(work), boost::asio::use_future);
      io.run();
      result.get();
   } else {
      auto result = boost::asio::co_spawn(io, boxed(std::move(work)), boost::asio::use_future);
      io.run();
      return std::move(result.get()).value();
   }
}

inline std::vector<std::uint8_t> data(std::size_t size, std::uint8_t seed = 1) {
   auto bytes = std::vector<std::uint8_t>(size);
   for (auto i = std::size_t{}; i < size; ++i) { bytes[i] = static_cast<std::uint8_t>((i * 31U + seed) % 251U); }
   return bytes;
}

inline core::descriptor describe(const std::vector<std::vector<std::uint8_t>>& files,
                                std::uint32_t piece_size = 2U * core::block_size) {
   auto result = core::descriptor{core::descriptor_version::v1, piece_size, {}};
   for (auto i = std::size_t{}; i < files.size(); ++i) {
      const auto hashes = core::hash_file(files[i], piece_size);
      result.files.push_back({{"dir" + std::to_string(i), "file"}, files[i].size(), hashes.root, hashes.pieces});
   }
   return result;
}

struct fixture {
   std::filesystem::path root;
   forge::asio::compute::pool disk{{.worker_threads = 2, .max_pending_tasks = 16, .max_waiting_submissions = 16, .thread_name = "swarm-test-disk"}};
   forge::asio::compute::pool cpu{{.worker_threads = 2, .max_pending_tasks = 16, .max_waiting_submissions = 16, .thread_name = "swarm-test-cpu"}};

   fixture() {
      auto pattern = (std::filesystem::temp_directory_path() / "forge-swarm-pr2-XXXXXX").string();
      const auto created = ::mkdtemp(pattern.data());
      if (!created) { throw std::runtime_error{"mkdtemp failed"}; }
      root = std::filesystem::canonical(created);
   }
   ~fixture() {
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
   }
   storage::file_store::options options(core::descriptor manifest, storage::open_mode mode = storage::open_mode::create) {
      return {.manifest = std::move(manifest), .destination = root / "result", .mode = mode,
              .disk = disk.get_executor(), .cpu = cpu.get_executor()};
   }
   std::filesystem::path stage() const {
      for (const auto& item : std::filesystem::directory_iterator(root)) {
         if (item.path().extension() == ".part") { return item.path(); }
      }
      throw std::runtime_error{"staging object missing"};
   }
   std::filesystem::path sidecar() const {
      for (const auto& item : std::filesystem::directory_iterator(root)) {
         if (item.path().extension() == ".resume") { return item.path(); }
      }
      throw std::runtime_error{"checkpoint missing"};
   }
};

inline std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
   auto file = std::ifstream{path, std::ios::binary};
   return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

inline void write_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
   auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
   file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
   if (!file) { throw std::runtime_error{"test file write failed"}; }
}

inline void fill(core::store& store, const core::descriptor& manifest,
                 const std::vector<std::vector<std::uint8_t>>& files) {
   for (auto file = 0U; file < files.size(); ++file) {
      const auto count = core::piece_count(files[file].size(), manifest.piece_size);
      for (auto index = count; index > 0U; --index) {
         const auto piece = core::piece_id{file, index - 1U};
         auto writer = store.begin(piece);
         const auto length = core::piece_length(manifest, piece);
         const auto start = std::size_t{piece.index} * manifest.piece_size;
         // Reverse block order proves positional writes rather than append-only buffering.
         for (auto block = (length + core::block_size - 1U) / core::block_size; block > 0U; --block) {
            const auto offset = (block - 1U) * core::block_size;
            const auto size = std::min<std::uint32_t>(core::block_size, length - offset);
            auto bytes = std::vector<std::uint8_t>{files[file].begin() + start + offset, files[file].begin() + start + offset + size};
            run(writer->async_write(offset, std::move(bytes)));
         }
         run(writer->async_commit());
      }
   }
}

} // namespace swarm_test
