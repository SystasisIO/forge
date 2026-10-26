module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/strand.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>

module forge.asio.cancellation_scope;

#include "details/cancellation_scope_impl.hxx"

namespace forge::asio {

cancellation_scope::impl::impl(boost::asio::any_io_executor target, std::span<const std::stop_token> sources,
                               boost::asio::cancellation_state parent)
    : executor(boost::asio::make_strand(std::move(target))), stopping(std::make_shared<std::stop_source>()),
      parent_slot(parent.slot()) {
   const auto cancel = std::function<void()>{[source = stopping] { source->request_stop(); }};
   // reserve checks length/overflow before registration. Failure while creating
   // any later callback unregisters the completed prefix through unique owners.
   callbacks.reserve(sources.size());
   for (const auto& source : sources) {
      if (source.stop_possible()) {
         callbacks.push_back(std::make_unique<std::stop_callback<std::function<void()>>>(source, cancel));
      }
   }

   // No throwing work follows successful native slot installation. Constructor
   // failure before this point has not installed a parent handler.
   if (parent_slot.is_connected()) {
      parent_slot.assign([cancel](boost::asio::cancellation_type) { cancel(); });
   }

   if (parent.cancelled() != boost::asio::cancellation_type::none) {
      stopping->request_stop();
   }
}

cancellation_scope::impl::~impl() {
   // Callers serialize parent access and keep it alive through joined work.
   parent_slot.clear();
}

} // namespace forge::asio
