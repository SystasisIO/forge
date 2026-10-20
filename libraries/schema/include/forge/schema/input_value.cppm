module;

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>

export module forge.schema.input_value;

export namespace forge::schema {

struct input_value {
   using array_type = std::vector<input_value>;
   using object_type = std::map<std::string, input_value>;
   using storage_type =
       std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string, array_type, object_type>;

   storage_type storage;

   input_value() = default;
   input_value(bool input) : storage{input} {}
   input_value(std::int64_t input) : storage{input} {}
   input_value(std::uint64_t input) : storage{input} {}
   input_value(double input) : storage{input} {}
   input_value(std::string input) : storage{std::move(input)} {}
   input_value(array_type input) : storage{std::move(input)} {}
   input_value(object_type input) : storage{std::move(input)} {}

   [[nodiscard]] const array_type* as_array() const noexcept {
      return std::get_if<array_type>(&storage);
   }

   [[nodiscard]] const object_type* as_object() const noexcept {
      return std::get_if<object_type>(&storage);
   }
};

} // namespace forge::schema
