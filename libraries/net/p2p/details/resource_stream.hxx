#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <string>

namespace forge::net::p2p::detail {

enum class stream_open_phase : std::uint8_t {
   admission,
   local_rejected,
   local_failed,
   native_open,
   committed,
};

class resource_stream final : public forge::net::transport::detail::stream_concept {
 public:
   explicit resource_stream(resource_manager::stream_reservation reservation);
   ~resource_stream() noexcept override;

   void attach(forge::net::transport::stream stream) noexcept;
   [[nodiscard]] bool valid() const noexcept override;
   [[nodiscard]] std::int64_t id() const noexcept override;
   [[nodiscard]] static bool is_memory_rejection(const std::exception& error) noexcept;
   [[nodiscard]] static std::exception_ptr preparation_failure(const std::exception& error) noexcept;
   [[nodiscard]] resource_manager::stream_reservation::bind_result bind_protocol(const protocol_id& value) noexcept;
   [[nodiscard]] resource_manager::stream_reservation::bind_result bind_service_for_protocol(const protocol_id& value,
                                                                                             bool dht_profile) noexcept;
   [[nodiscard]] std::optional<resource_manager::memory_reservation>
   reserve_memory(std::uint64_t bytes,
                  resource_manager::memory_priority priority = resource_manager::memory_priority::always) noexcept;

   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> bytes) override;
   boost::asio::awaitable<void> async_write_chunk(forge::net::transport::chunk bytes) override;
   boost::asio::awaitable<void> async_write_frame(std::span<const std::uint8_t> bytes) override;
   boost::asio::awaitable<void> async_write_frame_chunk(forge::net::transport::chunk bytes) override;
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read() override;
   boost::asio::awaitable<forge::net::transport::chunk> async_read_chunk() override;
   boost::asio::awaitable<void> async_close() override;
   void cancel() override;
   void request_cancel() noexcept;

 private:
   class memory_rejected final : public exceptions::backpressure_rejected {
    public:
      using exceptions::backpressure_rejected::backpressure_rejected;
   };

   class preparation_failed final : public std::bad_alloc {
    public:
      explicit preparation_failed(std::exception_ptr failure) noexcept;
      [[nodiscard]] std::exception_ptr failure() const noexcept;

    private:
      std::exception_ptr failure_;
   };

   enum class terminal_state : std::uint8_t {
      active,
      cancel_requested,
      owner,
      owner_cancel_requested,
      released,
   };

   [[nodiscard]] bool claim_terminal_owner() noexcept;
   void release_terminal_owner() noexcept;

   forge::net::transport::stream stream_;
   resource_manager::stream_reservation reservation_;
   std::atomic<terminal_state> terminal_{terminal_state::active};
};

class stream_admission_handler final {
 public:
   using callback = std::function<void()>;
   using admitted_callback = std::function<void(const std::shared_ptr<resource_stream>&)>;

   stream_admission_handler() = default;
   stream_admission_handler(admitted_callback admitted, callback commit);

   [[nodiscard]] explicit operator bool() const noexcept;
   void operator()(const std::shared_ptr<resource_stream>& resource) const;
   void commit() const;

 private:
   admitted_callback admitted_;
   callback commit_;
};

[[nodiscard]] std::pair<forge::net::transport::stream, std::shared_ptr<resource_stream>>
prepare_resource_stream(resource_manager::stream_reservation reservation);

boost::asio::awaitable<void> async_close_unescaped(const std::shared_ptr<resource_stream>& resource);

} // namespace forge::net::p2p::detail
