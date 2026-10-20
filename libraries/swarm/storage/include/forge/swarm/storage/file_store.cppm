module;

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <vector>

export module forge.swarm.storage.file_store;

export import forge.swarm.core.store;
export import forge.swarm.storage.exceptions;
export import forge.asio.compute;

export namespace forge::swarm::storage {

enum class open_mode { create, resume, seed };

class file_store final : public core::store {
 public:
   struct options {
      core::descriptor manifest;
      std::filesystem::path destination;
      open_mode mode = open_mode::create;
      forge::asio::compute::executor disk;
      forge::asio::compute::executor cpu;
      std::shared_ptr<core::checkpoint_store> checkpoint;
      std::uint32_t max_unfinished = 16;
      std::uint32_t max_parallel_io = 8;
      std::uint32_t max_io_size = 1024U * 1024U;
   };

   [[nodiscard]] static boost::asio::awaitable<std::unique_ptr<file_store>> async_open(options, std::stop_token = {});
   ~file_store() override;
   file_store(const file_store&) = delete;
   file_store& operator=(const file_store&) = delete;

   [[nodiscard]] const core::descriptor& manifest() const noexcept;
   [[nodiscard]] core::availability snapshot() const override;
   [[nodiscard]] std::unique_ptr<core::store::writer> begin(core::piece_id) override;
   void discard(core::piece_id) override;
   [[nodiscard]] core::store::lease acquire_lease() override;
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read(
      core::piece_id, std::uint32_t offset, std::uint32_t length, std::stop_token = {}) override;
   boost::asio::awaitable<core::availability> async_wait_change(std::uint64_t generation, std::stop_token = {}) override;
   boost::asio::awaitable<core::resume_state> async_checkpoint(std::stop_token = {}) override;
   boost::asio::awaitable<void> async_publish(std::stop_token = {}) override;
   void request_stop() noexcept override;
   boost::asio::awaitable<void> async_close() override;

 private:
   struct impl;
   explicit file_store(std::shared_ptr<impl>);
   std::shared_ptr<impl> _impl;
};

} // namespace forge::swarm::storage
