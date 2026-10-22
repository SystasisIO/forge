module;

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

export module forge.expression.program;

export import forge.expression.types;
export import forge.variant.value;

export namespace forge::expression {

struct evaluation {
   forge::variant value;
   std::size_t ast_events = 0;
};

// Native Preview profile. Prepared programs are immutable and may be evaluated
// concurrently. No caller input or source view is retained after a call.
class program {
 public:
   program(std::string_view source, std::span<const variable> variables = {}, limits budget = {});
   ~program();
   program(program&&) noexcept;
   program& operator=(program&&) noexcept;
   program(const program&) = delete;
   program& operator=(const program&) = delete;

   [[nodiscard]] evaluation evaluate(const forge::variant_object& values = {}) const;

 private:
   struct impl;
   std::unique_ptr<impl> _impl;
};

} // namespace forge::expression
