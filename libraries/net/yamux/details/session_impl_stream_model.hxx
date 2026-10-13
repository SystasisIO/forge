#pragma once

namespace forge::net::yamux {

class session::impl::stream_model final : public transport::detail::stream_concept {
 public:
   stream_model(std::shared_ptr<impl> owner, std::shared_ptr<stream_state> state);

   [[nodiscard]] bool valid() const noexcept override;
   [[nodiscard]] std::int64_t id() const noexcept override;
   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> value) override;
   boost::asio::awaitable<void> async_write_chunk(transport::chunk value) override;
   boost::asio::awaitable<detail::bytes> async_read() override;
   boost::asio::awaitable<transport::chunk> async_read_chunk() override;
   boost::asio::awaitable<void> async_close_write() override;
   boost::asio::awaitable<void> async_close() override;
   void cancel() override;
   void request_cancel() noexcept;

 private:
   std::weak_ptr<impl> owner_;
   std::shared_ptr<stream_state> state_;
};

} // namespace forge::net::yamux
