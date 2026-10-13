module;

#include <boost/asio/awaitable.hpp>
#include <filesystem>
#include <memory>

export module forge.net.s3.client;

export import forge.net.s3.types;
export import forge.net.s3.exceptions;
import forge.asio.compute;

export namespace forge::net::s3 {

class client {
 public:
   client(asio::compute::executor blocking_executor, config options);
   ~client();

   client(const client&) = delete;
   client& operator=(const client&) = delete;
   client(client&&) noexcept;
   client& operator=(client&&) noexcept;

   boost::asio::awaitable<metadata> head(object target, request_options options = {}) const;
   boost::asio::awaitable<metadata> put(object target, std::vector<std::byte> bytes, write_options write = {},
                                        request_options options = {}) const;
   boost::asio::awaitable<metadata> put(object target, std::filesystem::path source, write_options write = {},
                                        request_options options = {}) const;
   boost::asio::awaitable<std::vector<std::byte>> get(object target, read_options read = {},
                                                      request_options options = {}) const;
   boost::asio::awaitable<metadata> get(object target, std::filesystem::path destination, read_options read = {},
                                        request_options options = {}) const;
   boost::asio::awaitable<void> erase(object target, request_options options = {}) const;
   boost::asio::awaitable<signed_url> presign(object target, std::chrono::seconds lifetime, method verb = method::get,
                                              request_options options = {}) const;

   boost::asio::awaitable<multipart> begin(object target, write_options write = {}, request_options options = {}) const;
   boost::asio::awaitable<part> upload(multipart session, std::uint32_t number, std::vector<std::byte> bytes,
                                       request_options options = {}) const;
   boost::asio::awaitable<part> upload(multipart session, std::uint32_t number, std::filesystem::path source,
                                       byte_range range, request_options options = {}) const;
   boost::asio::awaitable<part_page> parts(multipart session, std::uint32_t after = 0, std::size_t limit = 1000,
                                           request_options options = {}) const;
   boost::asio::awaitable<metadata> complete(multipart session, std::vector<part> parts, write_options conditions = {},
                                             request_options options = {}) const;
   boost::asio::awaitable<void> abort(multipart session, request_options options = {}) const;

   void update_credentials(credentials identity);
   void request_stop() noexcept;
   boost::asio::awaitable<void> shutdown();

 private:
   struct impl;
   std::shared_ptr<impl> _impl;
};

} // namespace forge::net::s3
