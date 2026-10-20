#pragma once

namespace forge::swarm::storage {

struct file_store::impl::writer final : core::store::writer {
   writer(std::shared_ptr<impl>, core::piece_id, std::uint64_t);
   ~writer() override;
   writer(const writer&) = delete;
   writer& operator=(const writer&) = delete;
   [[nodiscard]] core::piece_id piece() const noexcept override;
   boost::asio::awaitable<void> async_write(std::uint32_t, std::vector<std::uint8_t>, std::stop_token) override;
   boost::asio::awaitable<void> async_commit(std::stop_token) override;
   boost::asio::awaitable<void> async_discard() override;

   std::shared_ptr<impl> _owner;
   core::piece_id _piece;
   std::uint64_t _generation;
};

} // namespace forge::swarm::storage
