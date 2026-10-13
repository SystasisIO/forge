#pragma once

namespace forge::api::websocket::detail {

class websocket_stream final
    : public forge::net::transport::detail::stream_concept,
      public std::enable_shared_from_this<websocket_stream> {
 public:
   using timer = boost::asio::steady_timer;

   websocket_stream(forge::net::websocket::connection::ptr connection,
                    std::uint32_t max_frame_size,
                    std::uint64_t max_buffered_bytes);

   void install_handlers();

   [[nodiscard]] bool valid() const noexcept override;
   [[nodiscard]] std::int64_t id() const noexcept override;

   boost::asio::awaitable<void>
   async_write(std::span<const std::uint8_t> bytes) override;
   boost::asio::awaitable<void>
   async_write_chunk(forge::net::transport::chunk bytes) override;
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read() override;
   boost::asio::awaitable<forge::net::transport::chunk>
   async_read_chunk() override;
   boost::asio::awaitable<void> async_close() override;
   void cancel() override;

 private:
   boost::asio::awaitable<void>
   accept_message(forge::net::websocket::received_message message);
   boost::asio::awaitable<void> reserve_outbound(std::size_t bytes);
   void release_outbound(std::size_t bytes) noexcept;
   void remember_executor(boost::asio::any_io_executor executor);
   void close_state(std::exception_ptr failure) noexcept;

   static void wake(std::vector<std::weak_ptr<timer>> waiters) noexcept;

   forge::net::websocket::connection::ptr connection_;
   std::uint32_t max_frame_size_ = 0;
   std::size_t max_message_size_ = 0;
   std::size_t max_buffered_bytes_ = 0;
   mutable std::mutex mutex_;
   std::deque<forge::net::transport::chunk> inbound_;
   std::size_t inbound_bytes_ = 0;
   std::size_t outbound_bytes_ = 0;
   std::vector<std::weak_ptr<timer>> read_waiters_;
   std::vector<std::weak_ptr<timer>> inbound_waiters_;
   std::vector<std::weak_ptr<timer>> outbound_waiters_;
   std::optional<boost::asio::any_io_executor> executor_;
   std::exception_ptr failure_;
   bool closed_ = false;
};

} // namespace forge::api::websocket::detail
