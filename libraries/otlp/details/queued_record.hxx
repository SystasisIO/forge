#pragma once

#include <cstddef>

namespace forge::otlp {

struct queued_record {
   forge::log_record record;
   std::size_t bytes = 0;
};

} // namespace forge::otlp
