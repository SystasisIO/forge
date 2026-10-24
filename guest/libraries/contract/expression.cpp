module;

#include <forge/contract/internal/intrinsics.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

module forge.contract.expression;

import forge.contract.intrinsics;
import forge.raw.codec;

namespace forge::contract::expression {
namespace {

template <typename Result, typename Request, typename Function>
Result invoke(const Request& request, Function function) {
   auto sizing = forge::datastream<std::size_t>{};
   forge::raw::pack(sizing, request);
   const auto request_size = sizing.tellp();
   check(request_size <= forge::expression::max_wire_bytes, "expression request exceeds the wire limit");
   auto buffer = forge::raw::bytes(forge::expression::max_wire_bytes);
   auto output = forge::datastream<std::uint8_t*>{buffer.data(), request_size};
   forge::raw::pack(output, request);
   // The host owns the decoded request before writing a response. Reuse the
   // same bounded allocation for both sides of this synchronous operation.
   const auto size = function(reinterpret_cast<const char*>(buffer.data()), static_cast<std::uint32_t>(request_size),
                              reinterpret_cast<char*>(buffer.data()), static_cast<std::uint32_t>(buffer.size()));
   check(size <= buffer.size(), "expression host returned an invalid response size");
   return forge::raw::unpack_exact<Result>(
       std::span<const std::uint8_t>{buffer.data(), size},
       forge::raw::unpack_limits{.max_container_elements = forge::expression::max_wire_bytes,
                                 .max_total_container_elements = forge::expression::max_wire_bytes,
                                 .max_bytes = forge::expression::max_wire_bytes});
}

} // namespace

forge::expression::artifact compile(const forge::expression::compilation& request) {
   return invoke<forge::expression::artifact>(request, internal::expression_compile_v1);
}

forge::expression::artifact load(const forge::expression::artifact& program) {
   return invoke<forge::expression::artifact>(program, internal::expression_import_v1);
}

forge::expression::result evaluate(const forge::expression::calculation& request) {
   return invoke<forge::expression::result>(request, internal::expression_evaluate_v1);
}

} // namespace forge::contract::expression
