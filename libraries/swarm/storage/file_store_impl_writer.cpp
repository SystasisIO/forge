module;

#include <boost/asio/awaitable.hpp>
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
#include "details/file_store_impl_writer.hxx"

namespace forge::swarm::storage {

file_store::impl::writer::writer(std::shared_ptr<impl> owner, core::piece_id piece, std::uint64_t generation)
   : _owner{std::move(owner)}, _piece{piece}, _generation{generation} {}
file_store::impl::writer::~writer() { _owner->release_writer(_piece, _generation); }
core::piece_id file_store::impl::writer::piece() const noexcept { return _piece; }
boost::asio::awaitable<void> file_store::impl::writer::async_write(
   std::uint32_t offset, std::vector<std::uint8_t> bytes, std::stop_token stop) {
   return impl::write(_owner, _piece, _generation, offset, std::move(bytes), stop);
}
boost::asio::awaitable<void> file_store::impl::writer::async_commit(std::stop_token stop) {
   return impl::commit(_owner, _piece, _generation, stop);
}
boost::asio::awaitable<void> file_store::impl::writer::async_discard() {
   return impl::discard_writer(_owner, _piece, _generation);
}

} // namespace forge::swarm::storage
