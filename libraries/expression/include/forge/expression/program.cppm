module;

#include <cstddef>
#include <coroutine>
#include <boost/asio/awaitable.hpp>
#include <memory>
#include <span>
#include <string_view>
#include <string>
#include <vector>

export module forge.expression.program;

export import forge.expression.types;
export import forge.expression.artifact;
export import :runtime;
export import forge.variant.value;

export namespace forge::expression {

struct evaluation {
   forge::variant value;
   std::size_t ast_events = 0;
};

// Native Preview profile. The runtime must outlive its programs and active
// operations. Concurrent callers serialize in that one bounded helper.
class program {
 public:
   program(runtime& executor, std::string_view source, std::span<const variable> variables = {}, limits budget = {},
           operation_options operation = {});
   program(runtime& executor, const forge::expression::artifact& value, operation_options operation = {});
   ~program();
   program(program&&) noexcept;
   program& operator=(program&&) noexcept;
   program(const program&) = delete;
   program& operator=(const program&) = delete;

   [[nodiscard]] forge::expression::artifact artifact() const;
   [[nodiscard]] evaluation evaluate(const forge::variant_object& values = {}, operation_options operation = {}) const;
   static boost::asio::awaitable<program> prepare_async(runtime& executor, std::string source,
                                                        std::vector<variable> variables = {}, limits budget = {},
                                                        operation_options operation = {});
   static boost::asio::awaitable<program> load_async(runtime& executor, forge::expression::artifact value,
                                                     operation_options operation = {});
   [[nodiscard]] boost::asio::awaitable<evaluation> evaluate_async(forge::variant_object values = {},
                                                                   operation_options operation = {}) const;

 private:
   struct impl;
   explicit program(std::shared_ptr<impl> state);
   std::shared_ptr<impl> _impl;
};

} // namespace forge::expression
