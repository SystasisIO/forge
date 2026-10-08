module;
#include <stdexcept>
#include <typeinfo>
#include <type_traits>
#include <boost/core/typeinfo.hpp>
#include <variant>

export module forge.variant.static_variant;

import forge.variant.exceptions;
import forge.variant.value;
import forge.variant.conversion;
import forge.variant.containers;
import forge.variant.chrono;
import forge.variant.multiprecision;
import forge.variant.format;
import forge.variant.described;

export namespace forge {

template <typename Result> struct visitor {};

template <typename variant, int32_t i = 0> void from_index(variant& v, int index) {
   if constexpr (i >= std::variant_size_v<variant>) {
      throw std::out_of_range("Provided index out of range for variant.");
   } else if (index == 0) {
      auto value = variant(std::in_place_index<i>);
      v = std::move(value);
   } else {
      from_index<variant, i + 1>(v, index - 1);
   }
}

template <typename VariantType, typename T, std::size_t index = 0> constexpr std::size_t get_index() {
   if constexpr (index == std::variant_size_v<VariantType>) {
      return index;
   } else if constexpr (std::is_same_v<std::variant_alternative_t<index, VariantType>, T>) {
      return index;
   } else {
      return get_index<VariantType, T, index + 1>();
   }
}

struct from_static_variant {
   variant& var;
   from_static_variant(variant& dv) : var(dv) {}

   template <typename T> void operator()(const T& v) const {
      if constexpr (std::is_arithmetic_v<T>) {
         // Scalar constructors preserve the value's exact dynamic kind. The
         // free scalar overload set can otherwise narrow bool or be ambiguous.
         var = variant{v};
      } else {
         to_variant(v, var);
      }
   }
};

struct to_static_variant {
   const variant& var;
   to_static_variant(const variant& dv) : var(dv) {}

   template <typename T> void operator()(T& v) const {
      from_variant(var, v);
   }
};

template <typename... T> void to_variant(const std::variant<T...>& s, forge::variant& v) {
   variants vars(2);
   vars[0] = s.index();
   std::visit(from_static_variant(vars[1]), s);
   v = std::move(vars);
}

template <typename... T> void from_variant(const forge::variant& v, std::variant<T...>& s) {
   auto ar = v.get_array();
   if (ar.size() < 2) {
      s = std::variant<T...>();
      return;
   }
   const auto selected = ar[0].as_uint64();
   if (selected >= sizeof...(T)) {
      throw std::out_of_range("Provided index out of range for variant.");
   }
   from_index(s, static_cast<int>(selected));
   std::visit(to_static_variant(ar[1]), s);
}

} // namespace forge
