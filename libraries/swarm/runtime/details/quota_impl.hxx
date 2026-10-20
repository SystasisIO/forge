#pragma once

namespace forge::swarm::runtime {

struct quota::impl {
   explicit impl(options);
   options config;
   mutable std::mutex mutex;
   status state;
   std::stop_source stop;
};

} // namespace forge::swarm::runtime
