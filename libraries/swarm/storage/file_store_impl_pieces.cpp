module;

#include <boost/asio/awaitable.hpp>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

module forge.swarm.storage.file_store;

import forge.asio.gate;
import forge.asio.notification;

#include "details/file_layout.hxx"
#include "details/file_store_impl.hxx"

namespace forge::swarm::storage {

bool file_store::impl::matches(core::piece_id piece, std::span<const std::uint8_t> bytes) const {
   const auto& file = config.manifest.files[piece.file];
   const auto hash = core::hash_piece(bytes, file.size, config.manifest.piece_size, piece.index);
   return hash == (file.size <= config.manifest.piece_size ? file.root : file.pieces[piece.index]);
}

boost::asio::awaitable<void> file_store::impl::write(std::shared_ptr<impl> self, core::piece_id piece,
   std::uint64_t generation, std::uint32_t offset, std::vector<std::uint8_t> bytes, std::stop_token token) {
   self->check_stop(token);
   const auto length = core::piece_length(self->config.manifest, piece);
   if (bytes.empty() || bytes.size() > self->config.max_io_size || offset >= length || bytes.size() > length - offset ||
       offset % core::block_size != 0U || (bytes.size() % core::block_size != 0U && offset + bytes.size() != length)) {
      throw core::exceptions::invalid_write{"write must contain complete blocks or the exact final short block"};
   }
   auto guard = operation{self, piece, true, generation, token};
   token = guard.cancel.source.get_token();
   const auto count = static_cast<std::uint32_t>(bytes.size());
   const auto start = std::uint64_t{piece.index} * self->config.manifest.piece_size + offset;
   co_await self->config.disk.execute({"swarm.write", token}, [self, piece, start, token, bytes = std::move(bytes)] {
      self->check_stop(token);
      self->layout->write(piece.file, start, bytes);
   });
   self->check_stop(token);
   const auto lock = std::scoped_lock{self->mutex};
   auto& state = self->pending.at(piece);
   if (state.generation != generation || !state.owned) { throw core::exceptions::invalid_write{"writer was abandoned during I/O"}; }
   for (auto block = offset / core::block_size; block < (offset + count + core::block_size - 1U) / core::block_size; ++block) {
      state.blocks[block / 8U] |= static_cast<std::uint8_t>(1U << (block % 8U));
   }
   self->changed_locked();
}

boost::asio::awaitable<void> file_store::impl::commit(
   std::shared_ptr<impl> self, core::piece_id piece, std::uint64_t generation, std::stop_token token) {
   self->check_stop(token);
   auto guard = operation{self, piece, true, generation, token};
   token = guard.cancel.source.get_token();
   const auto length = core::piece_length(self->config.manifest, piece);
   {
      const auto lock = std::scoped_lock{self->mutex};
      const auto& blocks = self->pending.at(piece).blocks;
      for (auto i = 0U; i < (length + core::block_size - 1U) / core::block_size; ++i) {
         if (!core::bitmap_test(blocks, i)) { throw core::exceptions::invalid_write{"piece has missing blocks"}; }
      }
   }
   const auto offset = std::uint64_t{piece.index} * self->config.manifest.piece_size;
   auto bytes = co_await self->config.disk.execute({"swarm.verify.read", token}, [self, piece, offset, length, token] {
      self->check_stop(token);
      return self->layout->read(piece.file, offset, length);
   });
   const auto valid = co_await self->config.cpu.execute({"swarm.verify.hash", token}, [self, piece, token, bytes = std::move(bytes)] {
      self->check_stop(token);
      return self->matches(piece, bytes);
   });
   self->check_stop(token);
   const auto lock = std::scoped_lock{self->mutex};
   auto& state = self->pending.at(piece);
   if (state.generation != generation || !state.owned) { throw core::exceptions::invalid_write{"writer was abandoned during verification"}; }
   if (!valid) {
      std::fill(state.blocks.begin(), state.blocks.end(), 0U);
      self->changed_locked();
      throw core::exceptions::integrity_failure{"piece does not match its authenticated hash"};
   }
   self->set_verified_locked(piece, true);
   self->pending.erase(piece);
   self->changed_locked();
}

boost::asio::awaitable<std::vector<std::uint8_t>> file_store::impl::read(std::shared_ptr<impl> self,
   core::piece_id piece, std::uint32_t offset, std::uint32_t length, std::stop_token token) {
   self->check_stop(token);
   const auto size = core::piece_length(self->config.manifest, piece);
   if (length == 0U || length > self->config.max_io_size || offset >= size || length > size - offset) {
      throw core::exceptions::invalid_geometry{"read range exceeds its piece or I/O bound"};
   }
   auto guard = operation{self, piece, false, 0, token};
   token = guard.cancel.source.get_token();
   const auto start = std::uint64_t{piece.index} * self->config.manifest.piece_size;
   auto bytes = co_await self->config.disk.execute({"swarm.read", token}, [self, piece, start, size, token] {
      self->check_stop(token);
      return self->layout->read(piece.file, start, size);
   });
   // Ordinary files can be modified outside this process. Verify the actual returned piece,
   // not an inode/mtime hint or a different pre-read buffer; memory is bounded by max_piece_size.
   auto result = co_await self->config.cpu.execute({"swarm.read.verify", token},
      [self, piece, offset, length, token, bytes = std::move(bytes)]() mutable {
         self->check_stop(token);
         if (!self->matches(piece, bytes)) {
            const auto lock = std::scoped_lock{self->mutex};
            self->set_verified_locked(piece, false);
            self->changed_locked();
            throw core::exceptions::integrity_failure{"stored piece has changed"};
         }
         if (offset == 0U && length == bytes.size()) { return std::move(bytes); }
         return std::vector<std::uint8_t>{bytes.begin() + offset, bytes.begin() + offset + length};
      });
   self->check_stop(token);
   co_return result;
}

boost::asio::awaitable<void> file_store::impl::discard_writer(
   std::shared_ptr<impl> self, core::piece_id piece, std::uint64_t generation) {
   auto guard = operation{self, piece, true, generation, {}};
   const auto lock = std::scoped_lock{self->mutex};
   self->pending.erase(piece);
   self->changed_locked();
   co_return;
}

} // namespace forge::swarm::storage
