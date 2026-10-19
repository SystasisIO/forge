#include "bootstrap_abi.hxx"

#include <new>
#include <stdexcept>

namespace bootstrap_abi {
namespace {
unsigned destruction_count = 0;

class implementation final : public object {
 public:
   std::string exchange(std::string value) const override {
      return value + ":libc++220108";
   }
};
} // namespace

object::~object() {
   ++destruction_count;
}
std::unique_ptr<object> make() {
   return std::make_unique<implementation>();
}
const std::type_info& dynamic_type() {
   return typeid(implementation);
}
unsigned destroyed() {
   return destruction_count;
}
void throw_bad_alloc() {
   throw std::bad_alloc();
}
void throw_runtime_error() {
   throw std::runtime_error("bootstrap-typed-error");
}
} // namespace bootstrap_abi
