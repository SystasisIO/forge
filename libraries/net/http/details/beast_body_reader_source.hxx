#pragma once

namespace forge::net::http::detail {

template <typename Stream> class beast_body_reader_source final : public body_reader::source {
 public:
   beast_body_reader_source(Stream& stream, boost::beast::flat_buffer& buffer,
                            boost::beast::http::request_parser<boost::beast::http::buffer_body>& parser,
                            stream_limits limits, bool send_continue,
                            std::shared_ptr<request_read_ownership> read_ownership, server_log::request* access);
   boost::asio::awaitable<std::optional<body_chunk>> async_read() override;
   [[nodiscard]] std::uint64_t bytes_read() const noexcept override;
   [[nodiscard]] bool requires_continue_before_response() const noexcept override;
   [[nodiscard]] bool done() const noexcept;
   boost::asio::awaitable<void> send_continue_if_needed();

 private:
   Stream& stream_;
   boost::beast::flat_buffer& buffer_;
   boost::beast::http::request_parser<boost::beast::http::buffer_body>& parser_;
   stream_limits limits_;
   bool send_continue_ = false;
   std::uint64_t bytes_read_ = 0;
   std::shared_ptr<request_read_ownership> read_ownership_;
   server_log::request* access_;
};

extern template class beast_body_reader_source<boost::beast::tcp_stream>;
extern template class beast_body_reader_source<forge::net::tls::beast_tls_stream>;

} // namespace forge::net::http::detail
