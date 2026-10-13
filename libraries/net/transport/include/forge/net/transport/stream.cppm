module;

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/compat/move_only_function.hpp>

export module forge.net.transport.stream;

export import forge.net.transport.buffer;
export import forge.net.transport.frame;

export namespace forge::net::transport {

namespace detail {
class stream_concept;
struct stream_access;
using stream_cancel_request = boost::compat::move_only_function<void() noexcept>;
} // namespace detail

class stream {
 public:
   stream();
   ~stream();

   stream(stream&&) noexcept;
   stream& operator=(stream&&) noexcept;

   stream(const stream&) = delete;
   stream& operator=(const stream&) = delete;

   [[nodiscard]] bool valid() const noexcept;
   [[nodiscard]] std::int64_t id() const noexcept;

   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> bytes);
   boost::asio::awaitable<void> async_write(chunk bytes);
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read();
   boost::asio::awaitable<chunk> async_read_chunk();
   boost::asio::awaitable<void> async_write_frame(std::span<const std::uint8_t> bytes);
   boost::asio::awaitable<void> async_write_frame(chunk bytes);
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read_frame();
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read_frame(frame_options options);
   boost::asio::awaitable<chunk> async_read_frame_chunk();
   boost::asio::awaitable<chunk> async_read_frame_chunk(frame_options options);
   // Send FIN without closing the read direction or releasing stream ownership.
   // Serialize with writes and terminal close; cancellation/destruction still
   // aborts the remaining direction. Unsupported models fail explicitly.
   boost::asio::awaitable<void> async_close_write();
   // Completion, including an exception, is a terminal cleanup barrier. All
   // concurrent and repeated callers join the same terminal result, so native
   // lifetime and resource ownership may be released only after it resumes.
   boost::asio::awaitable<void> async_close();
   void cancel();
   void request_cancel() noexcept;

 private:
   friend struct detail::stream_access;

   struct impl;
   explicit stream(std::shared_ptr<detail::stream_concept> model,
                   detail::stream_cancel_request request_cancel = {},
                   detail::stream_cancel_request abandon_cancel = {});

   std::shared_ptr<impl> impl_;
};

namespace detail {

class stream_concept {
 public:
   virtual ~stream_concept() = default;

   [[nodiscard]] virtual bool valid() const noexcept = 0;
   [[nodiscard]] virtual std::int64_t id() const noexcept = 0;

   virtual boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> bytes) = 0;
   virtual boost::asio::awaitable<void> async_write_chunk(chunk bytes);
   virtual boost::asio::awaitable<void> async_write_frame(std::span<const std::uint8_t> bytes);
   virtual boost::asio::awaitable<void> async_write_frame_chunk(chunk bytes);
   virtual boost::asio::awaitable<std::vector<std::uint8_t>> async_read() = 0;
   virtual boost::asio::awaitable<chunk> async_read_chunk();
   virtual boost::asio::awaitable<void> async_close_write();
   // Implementations must complete all native and lower-stream cleanup before
   // this resumes, including when reporting their terminal cause by exception.
   virtual boost::asio::awaitable<void> async_close() = 0;
   virtual void cancel() = 0;
};

struct stream_access {
   [[nodiscard]] static stream make(std::shared_ptr<stream_concept> model);
   [[nodiscard]] static stream make_cancelable(std::shared_ptr<stream_concept> model,
                                               stream_cancel_request request_cancel);
   [[nodiscard]] static stream make_cancelable(std::shared_ptr<stream_concept> model,
                                               stream_cancel_request request_cancel,
                                               stream_cancel_request abandon_cancel);
   [[nodiscard]] static stream with_buffer(stream value, std::vector<std::uint8_t> buffered);
};

} // namespace detail

} // namespace forge::net::transport
