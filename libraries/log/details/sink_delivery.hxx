#pragma once

#include <memory>
#include <vector>

namespace forge::detail {

class sink_delivery {
 public:
   static void deliver(const std::vector<std::shared_ptr<sink>>&, const log_record&);

 private:
   explicit sink_delivery(const sink*) noexcept;
   ~sink_delivery();
   sink_delivery(const sink_delivery&) = delete;
   sink_delivery& operator=(const sink_delivery&) = delete;

   [[nodiscard]] static bool active(const sink*) noexcept;

   const sink* _sink;
   const sink_delivery* _previous;
   static thread_local const sink_delivery* _active;
};

} // namespace forge::detail
