module;

#include <memory>
#include <vector>

module forge.log.logger;

#include "details/sink_delivery.hxx"

namespace forge::detail {

// Trivial TLS survives thread-local destruction. Frames live only on the stack,
// including when static destructors log after main-thread TLS teardown.
thread_local const sink_delivery* sink_delivery::_active = nullptr;

sink_delivery::sink_delivery(const sink* value) noexcept : _sink{value}, _previous{_active} {
   _active = this;
}

sink_delivery::~sink_delivery() {
   _active = _previous;
}

bool sink_delivery::active(const sink* value) noexcept {
   for (auto frame = _active; frame != nullptr; frame = frame->_previous) {
      if (frame->_sink == value) {
         return true;
      }
   }
   return false;
}

void sink_delivery::deliver(const std::vector<std::shared_ptr<sink>>& sinks, const log_record& record) {
   for (const auto& target : sinks) {
      if (active(target.get())) {
         continue;
      }
      const auto frame = sink_delivery{target.get()};
      try {
         target->log(record);
      } catch (...) {
         report_log_failure();
      }
   }
}

} // namespace forge::detail
