#pragma once

namespace forge::net::http {

class callback_body_source final : public body_reader::source
{
 public:
   callback_body_source(body_reader::callback_type callback, std::function<void()> cancel);
   boost::asio::awaitable<std::optional<body_chunk>> async_read() override;
   std::uint64_t bytes_read() const noexcept override;
   void cancel() noexcept override;

 private:
   body_reader::callback_type _callback;
   std::function<void()> _cancel;
   std::atomic<std::uint64_t> _bytes = 0;
   std::atomic_bool _cancelled = false;
};

} // namespace forge::net::http
