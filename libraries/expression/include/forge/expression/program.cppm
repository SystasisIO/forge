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
   program(runtime& executor, std::string_view source, std::span<const variable> variables = {}, limits budget = {});
   ~program();
   program(program&&) noexcept;
   program& operator=(program&&) noexcept;
   program(const program&) = delete;
   program& operator=(const program&) = delete;

   [[nodiscard]] evaluation evaluate(const forge::variant_object& values = {}) const;
   static boost::asio::awaitable<program> prepare_async(runtime& executor, std::string source,
                                                        std::vector<variable> variables = {}, limits budget = {});
   [[nodiscard]] boost::asio::awaitable<evaluation> evaluate_async(forge::variant_object values = {}) const;

 private:
   struct impl;
   explicit program(std::shared_ptr<impl> state);
   std::shared_ptr<impl> _impl;
};

} // namespace forge::expression
