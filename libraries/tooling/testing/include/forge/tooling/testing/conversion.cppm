module;

#include <optional>

export module forge.tooling.testing.conversion;

export import forge.core.conversion;
export import forge.expression.artifact;
import forge.expression.program;
import forge.variant.value;

export namespace forge::core {

template <> class conversion<forge::variant_object> {
 public:
   explicit conversion(const forge::expression::arguments&);
   [[nodiscard]] forge::variant_object finish() &&;

 private:
   std::optional<forge::variant_object> _value;
};

template <> class conversion<forge::expression::result> {
 public:
   explicit conversion(const forge::expression::evaluation&);
   [[nodiscard]] forge::expression::result finish() &&;

 private:
   std::optional<forge::expression::result> _value;
};

} // namespace forge::core
