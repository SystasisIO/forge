module;

#include <cstddef>
#include <cstdint>
#include <string>
#include <boost/describe.hpp>

export module forge.expression.types;

export namespace forge::expression {

inline constexpr std::uint32_t profile_version = 1;

enum class type { int64, uint64, boolean, string, list_int64, list_uint64, map_int64, map_uint64 };

struct variable {
   std::string name;
   type kind = type::int64;
};

// These defaults are also V1 ceilings. A caller may select smaller limits.
// The runtime separately bounds the helper's complete native allocation pool.
struct limits {
   std::size_t source_bytes = 8192;
   std::size_t nodes = 1024;
   std::size_t depth = 48;
   std::size_t variables = 64;
   std::size_t collection_elements = 256;
   std::size_t string_bytes = 1024;
   std::size_t total_input_bytes = 65536;
   std::size_t ast_events = 8192;
};

enum class rounding : std::int64_t { trunc = 0, floor = 1, ceil = 2, half_even = 3 };

BOOST_DESCRIBE_STRUCT(variable, (), (name, kind))
BOOST_DESCRIBE_STRUCT(limits, (),
                      (source_bytes, nodes, depth, variables, collection_elements, string_bytes, total_input_bytes,
                       ast_events))

} // namespace forge::expression
