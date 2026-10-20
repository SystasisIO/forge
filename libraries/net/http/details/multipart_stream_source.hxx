#pragma once

namespace forge::net::http {

class multipart_stream_source final : public body_reader::source
{
 public:
   multipart_stream_source(std::vector<multipart_stream_part> parts, std::string boundary,
                            multipart_stream_options options);
   boost::asio::awaitable<std::optional<body_chunk>> async_read() override;
   std::uint64_t bytes_read() const noexcept override;
   void cancel() noexcept override;

 private:
   enum class phase { header, body, ending, closing, finished };
   void account(std::size_t size);
   body_chunk take(std::span<const std::byte> bytes);

   std::vector<multipart_stream_part> _parts;
   std::vector<std::string> _headers;
   std::string _boundary;
   multipart_stream_options _options;
   phase _phase = phase::header;
   std::size_t _part = 0;
   std::uint64_t _part_bytes = 0;
   std::string _control;
   std::size_t _control_offset = 0;
   std::vector<std::byte> _pending;
   std::size_t _pending_offset = 0;
   std::atomic<std::uint64_t> _bytes = 0;
   std::atomic_bool _cancelled = false;
   std::exception_ptr _failure;
};

} // namespace forge::net::http
