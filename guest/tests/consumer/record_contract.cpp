#include <cstdint>
#include <string>
#include <vector>

import forge.contract;
import forge.contract.dispatcher;
import forge.raw.codec;

namespace contract_types {

enum class mode : std::uint8_t { one = 1, two = 2 };

struct record_value {
   std::string user;
   std::vector<std::uint32_t> values;
};

struct stream_bool_value {
   bool value = false;
   std::uint8_t marker = 0;

   bool operator==(const stream_bool_value&) const = default;

   template <typename Stream> friend Stream& operator<<(Stream& stream, const stream_bool_value& item) {
      return stream << item.value << item.marker;
   }

   template <typename Stream> friend Stream& operator>>(Stream& stream, stream_bool_value& item) {
      return stream >> item.value >> item.marker;
   }
};

struct bounded_value {
   bool flag = true;
   mode selected = mode::one;
   forge::unsigned_int count{7U};
   std::vector<std::vector<std::uint32_t>> values;
   std::vector<std::string> strings;

   template <typename Stream> friend Stream& operator<<(Stream& stream, const bounded_value& value) {
      return stream << value.flag << value.selected << value.count << value.values << value.strings;
   }
   template <typename Stream> friend Stream& operator>>(Stream& stream, bounded_value& value) {
      return stream >> value.flag >> value.selected >> value.count >> value.values >> value.strings;
   }
};

} // namespace contract_types

namespace forge::raw {
template <> struct enum_wire_type<contract_types::mode> {
   using type = std::uint8_t;
   static constexpr bool shared = true;
   static constexpr bool is_valid(type value) { return value == 1U || value == 2U; }
};
} // namespace forge::raw

class [[forge::contract("recordtest")]] record_contract : public forge::contract::context {
 public:
   static constexpr auto decode_policy = forge::contract::bounded_decode_policy{
       .max_action_bytes = 128U,
       .max_call_bytes = 128U,
       .unpack = {.allocations = {.max_container_elements = 4U,
                                  .max_total_container_elements = 6U,
                                  .max_bytes = 32U,
                                  .first_container_elements = 4U},
                  .max_total_bytes = 48U,
                  .reject_noncanonical_varints = true,
                  .reject_unknown_enum_values = true}};

   record_contract(forge::chain::protocol::name self, forge::chain::protocol::name first_receiver, stream_type stream)
       : context(self, first_receiver, stream) {
      // A rejected invocation must not reach this observable constructor effect.
      const auto marker = std::uint8_t{0xc0U};
      forge::contract::set_action_return_value(&marker, 1U);
   }

   [[forge::action]] contract_types::record_value echo(contract_types::record_value value) {
      const auto packed = forge::raw::pack(value);
      return forge::raw::unpack_exact<contract_types::record_value>(packed);
   }

   [[forge::action]] std::vector<std::uint8_t> boolwire(bool value) {
      const auto expected = contract_types::stream_bool_value{.value = value, .marker = 0xa5U};
      auto packed = forge::raw::pack(expected);
      forge::contract::check(forge::raw::unpack_exact<contract_types::stream_bool_value>(packed) == expected,
                             "stream bool round-trip failed");
      return packed;
   }

   [[forge::action]] std::uint8_t bounded(contract_types::bounded_value value) {
      return static_cast<std::uint8_t>(value.count.value);
   }

   [[forge::call]] std::uint8_t boundedcall(contract_types::bounded_value value) const {
      return static_cast<std::uint8_t>(value.count.value);
   }

   [[forge::action]] std::uint8_t ping() const {
      return 7U;
   }

   [[forge::call]] std::uint8_t pingcall() const {
      return 7U;
   }

   [[forge::action]] std::vector<std::uint8_t> limitswire() const {
      auto [id, invoke] = forge::contract::call_entry{};
      static_cast<void>(id);
      static_cast<void>(invoke);
      const auto [elements, total, bytes, first] = decode_policy.unpack.allocations;
      static_cast<void>(elements);
      static_cast<void>(total);
      static_cast<void>(bytes);
      static_cast<void>(first);
      return forge::raw::pack(decode_policy.unpack.allocations);
   }
};
