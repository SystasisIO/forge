module;

#include <cstddef>
#include <string>

export module forge.codec.json.document;

export namespace forge::codec::json {

// An owned, opaque JSON value. Guest code forwards its Raw string representation;
// native codecs validate it before exposing or consuming the JSON value.
struct document {
   std::string text;

   bool operator==(const document&) const = default;
};

struct document_limits {
   std::size_t max_bytes = 1024U * 1024U;
   std::size_t max_depth = 128;
};

} // namespace forge::codec::json
