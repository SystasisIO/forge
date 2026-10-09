module;

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/awaitable.hpp>
#include <coroutine>
#include <chrono>
#include <stop_token>
#include <string>
#include <type_traits>
#include <vector>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <variant>

module forge.net.s3.client;

import forge.asio.compute;

#include "details/client_impl.hxx"

namespace forge::net::s3 {

client::client(asio::compute::executor executor, config options)
    : _impl{std::make_shared<impl>(std::move(executor), std::move(options))} {}

client::~client() {
   request_stop();
}

client::client(client&& other) noexcept : _impl{std::move(other._impl)} {}

client& client::operator=(client&& other) noexcept {
   if (this != &other) {
      request_stop();
      _impl = std::move(other._impl);
   }
   return *this;
}

boost::asio::awaitable<metadata> client::head(object target, request_options options) const {
   return impl::run(_impl, "s3.head", false, std::move(options),
                    [target = std::move(target)](impl& self, impl::activity& call) { return self.head(target, call); });
}

boost::asio::awaitable<metadata> client::put(object target, std::vector<std::byte> bytes, write_options write,
                                             request_options options) const {
   return impl::run(_impl, "s3.put", true, std::move(options),
                    [target = std::move(target), source = std::move(bytes),
                     write = std::move(write)](impl& self, impl::activity& call) mutable {
                       return self.put(target, std::move(source), write, call);
                    });
}

boost::asio::awaitable<metadata> client::put(object target, std::filesystem::path source, write_options write,
                                             request_options options) const {
   return impl::run(_impl, "s3.put", true, std::move(options),
                    [target = std::move(target), source = std::move(source),
                     write = std::move(write)](impl& self, impl::activity& call) mutable {
                       return self.put(target, std::move(source), write, call);
                    });
}

boost::asio::awaitable<std::vector<std::byte>> client::get(object target, read_options read,
                                                           request_options options) const {
   return impl::run(_impl, "s3.get", false, std::move(options),
                    [target = std::move(target), read = std::move(read)](impl& self, impl::activity& call) {
                       return self.get(target, read, call);
                    });
}

boost::asio::awaitable<metadata> client::get(object target, std::filesystem::path destination, read_options read,
                                             request_options options) const {
   return impl::run(_impl, "s3.get.file", false, std::move(options),
                    [target = std::move(target), destination = std::move(destination), read = std::move(read)](
                        impl& self, impl::activity& call) { return self.get(target, destination, read, call); });
}

boost::asio::awaitable<void> client::erase(object target, request_options options) const {
   return impl::run(_impl, "s3.erase", true, std::move(options),
                    [target = std::move(target)](impl& self, impl::activity& call) { self.erase(target, call); });
}

boost::asio::awaitable<signed_url> client::presign(object target, std::chrono::seconds lifetime, method verb,
                                                   request_options options) const {
   return impl::run(_impl, "s3.presign", false, std::move(options),
                    [target = std::move(target), lifetime, verb](impl& self, impl::activity& call) {
                       return self.presign(target, lifetime, verb, call);
                    });
}

boost::asio::awaitable<multipart> client::begin(object target, write_options write, request_options options) const {
   return impl::run(_impl, "s3.multipart.begin", true, std::move(options),
                    [target = std::move(target), write = std::move(write)](impl& self, impl::activity& call) {
                       return self.begin(target, write, call);
                    });
}

boost::asio::awaitable<part> client::upload(multipart session, std::uint32_t number, std::vector<std::byte> bytes,
                                            request_options options) const {
   return impl::run(
       _impl, "s3.multipart.upload", true, std::move(options),
       [session = std::move(session), number, bytes = std::move(bytes)](impl& self, impl::activity& call) mutable {
          return self.upload(session, number, std::move(bytes), {}, call);
       });
}

boost::asio::awaitable<part> client::upload(multipart session, std::uint32_t number, std::filesystem::path source,
                                            byte_range range, request_options options) const {
   return impl::run(_impl, "s3.multipart.upload", true, std::move(options),
                    [session = std::move(session), number, source = std::move(source),
                     range](impl& self, impl::activity& call) mutable {
                       return self.upload(session, number, std::move(source), range, call);
                    });
}

boost::asio::awaitable<part_page> client::parts(multipart session, std::uint32_t after, std::size_t limit,
                                                request_options options) const {
   return impl::run(_impl, "s3.multipart.parts", false, std::move(options),
                    [session = std::move(session), after, limit](impl& self, impl::activity& call) {
                       return self.parts(session, after, limit, call);
                    });
}

boost::asio::awaitable<metadata> client::complete(multipart session, std::vector<part> parts, write_options conditions,
                                                  request_options options) const {
   return impl::run(_impl, "s3.multipart.complete", true, std::move(options),
                    [session = std::move(session), parts = std::move(parts),
                     conditions = std::move(conditions)](impl& self, impl::activity& call) mutable {
                       return self.complete(session, std::move(parts), conditions, call);
                    });
}

boost::asio::awaitable<void> client::abort(multipart session, request_options options) const {
   return impl::run(_impl, "s3.multipart.abort", true, std::move(options),
                    [session = std::move(session)](impl& self, impl::activity& call) { self.abort(session, call); });
}

void client::update_credentials(credentials identity) {
   if (!_impl) {
      throw exceptions::stopped{"S3 client is empty"};
   }
   _impl->update(std::move(identity));
}
void client::request_stop() noexcept {
   if (_impl) {
      _impl->stop();
   }
}
boost::asio::awaitable<void> client::shutdown() {
   return impl::drain(_impl);
}

} // namespace forge::net::s3
