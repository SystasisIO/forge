#pragma once

namespace forge::auth::liboauth2::detail {

class vendor_runtime {
 public:
   vendor_runtime();
   ~vendor_runtime();
   vendor_runtime(const vendor_runtime&) = delete;
   vendor_runtime& operator=(const vendor_runtime&) = delete;
};

} // namespace forge::auth::liboauth2::detail
