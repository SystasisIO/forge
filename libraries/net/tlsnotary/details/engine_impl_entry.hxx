#pragma once

namespace forge::net::tlsnotary {

struct engine::impl::entry {
   std::uint64_t id = 0;
   std::shared_ptr<detail::native_exchange> exchange;
   std::stop_source stop;
   bool retired = false;
};
} // namespace forge::net::tlsnotary
