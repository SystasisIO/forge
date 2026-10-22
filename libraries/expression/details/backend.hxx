#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace forge::expression::detail::backend {

enum class status {
   ok,
   invalid_profile,
   invalid_program,
   invalid_input,
   resource_exhausted,
   evaluation_failed,
   internal
};

enum class value_kind { null, int64, uint64, boolean, string, list, map };

// Views are borrowed for the duration of evaluate(). The bridge never retains
// caller strings, container pointers or callbacks in a prepared program.
struct value_view {
   value_kind kind = value_kind::null;
   std::int64_t i64 = 0;
   std::uint64_t u64 = 0;
   bool boolean = false;
   std::string_view text;
   const void* context = nullptr;
   std::size_t count = 0;
   value_view (*element)(const void*, std::size_t) = nullptr;
   std::string_view (*key)(const void*, std::size_t) = nullptr;
};

struct named_value {
   std::string_view name;
   value_view value;
};

enum class schema_kind { int64, uint64, boolean, string, list_int64, list_uint64, map_int64, map_uint64 };

struct declaration {
   std::string_view name;
   schema_kind kind = schema_kind::int64;
};

// Defaults are also the fixed V1 ceilings. These are structural/input limits
// and a post-step AST-event limit, not a complete native heap-memory budget.
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

struct evaluation {
   status code = status::ok;
   // Static, redacted diagnostic; no source expression or input contents.
   std::string_view message;
   value_kind kind = value_kind::null;
   std::int64_t i64 = 0;
   std::uint64_t u64 = 0;
   bool boolean = false;
   // Upstream tracing is post-step: exhaustion can report limit + 1 events.
   std::size_t ast_events = 0;
};

struct preparation;

class program {
 public:
   virtual ~program();
   static preparation prepare(std::string_view source, std::span<const declaration> declarations, limits budget = {});
   // Immutable prepared state; each call owns its arena and activation.
   virtual evaluation evaluate(std::span<const named_value> values) const = 0;
};

struct preparation {
   std::unique_ptr<program> executable;
   status code = status::ok;
   std::string_view message;
};

} // namespace forge::expression::detail::backend
