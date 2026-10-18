module;

#include <boost/asio/awaitable.hpp>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

module forge.swarm.storage.file_store;

import forge.asio.gate;
import forge.asio.notification;

#include "details/file_layout.hxx"
#include "details/file_store_impl.hxx"

namespace forge::swarm::storage {

file_store::file_store(std::shared_ptr<impl> value) : _impl{std::move(value)} {}
file_store::~file_store() { _impl->request_stop(); }

boost::asio::awaitable<std::unique_ptr<file_store>> file_store::async_open(options config, std::stop_token stop) {
   auto state = std::make_shared<impl>(std::move(config));
   co_await impl::initialize(state, stop);
   co_return std::unique_ptr<file_store>{new file_store{std::move(state)}};
}

const core::descriptor& file_store::manifest() const noexcept { return _impl->config.manifest; }
core::availability file_store::snapshot() const { return _impl->snapshot(); }
std::unique_ptr<core::store::writer> file_store::begin(core::piece_id piece) { return _impl->begin(piece); }
void file_store::discard(core::piece_id piece) { _impl->discard(piece); }
core::store::lease file_store::acquire_lease() { return make_lease(_impl->lease()); }
void file_store::request_stop() noexcept { _impl->request_stop(); }
boost::asio::awaitable<void> file_store::async_close() { return impl::close_owned(_impl); }

boost::asio::awaitable<std::vector<std::uint8_t>> file_store::async_read(
   core::piece_id piece, std::uint32_t offset, std::uint32_t length, std::stop_token stop) {
   return impl::read(_impl, piece, offset, length, stop);
}
boost::asio::awaitable<core::availability> file_store::async_wait_change(std::uint64_t generation, std::stop_token stop) {
   return impl::wait_change(_impl, generation, stop);
}
boost::asio::awaitable<core::resume_state> file_store::async_checkpoint(std::stop_token stop) {
   return impl::checkpoint(_impl, stop);
}
boost::asio::awaitable<void> file_store::async_publish(std::stop_token stop) { return impl::publish(_impl, stop); }

} // namespace forge::swarm::storage
