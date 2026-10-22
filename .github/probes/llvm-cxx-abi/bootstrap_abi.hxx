#pragma once

#include <memory>
#include <string>
#include <typeinfo>

static_assert(_LIBCPP_VERSION == 220108);

namespace bootstrap_abi {
class object {
 public:
   virtual ~object();
   virtual std::string exchange(std::string value) const = 0;
};

std::unique_ptr<object> make();
const std::type_info& dynamic_type();
unsigned destroyed();
void throw_bad_alloc();
void throw_runtime_error();
} // namespace bootstrap_abi
