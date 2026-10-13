#pragma once

namespace forge::app::detail {

struct shutdown_owner {
   std::unique_ptr<application_shell> app;
};

} // namespace forge::app::detail
