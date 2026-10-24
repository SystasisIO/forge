module;

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>
#include <boost/describe.hpp>

export module forge.expression.artifact;

export import forge.expression.types;
export import forge.raw.codec;
export import forge.crypto.digest.sha256;

export namespace forge::expression {

inline constexpr std::uint32_t artifact_format = 1;
inline constexpr std::uint32_t semantics_version = 1;
inline constexpr std::uint32_t max_expression_bytes = 64U * 1024U;
inline constexpr std::uint32_t max_artifact_bytes = 96U * 1024U;
inline constexpr std::uint32_t max_wire_bytes = 128U * 1024U;

// A portable checked CEL expression, not a native plan or process handle.
// The digest identifies these exact versioned bytes. Loading still requires
// independent CEL type/reference checking; the envelope is not a certificate.
struct artifact {
   std::uint32_t format = artifact_format;
   std::uint32_t profile = profile_version;
   std::uint32_t semantics = semantics_version;
   std::vector<variable> variables;
   limits budget;
   type result = type::int64;
   raw::bytes expression;
   crypto::digest::sha256 digest;

   bool operator==(const artifact&) const = default;
};

// A finite wire schema for the admitted CEL types. Raw owns its encoding;
// the native Forge variant runtime is not a guest dependency.
using value =
    std::variant<std::int64_t, std::uint64_t, bool, std::string, std::vector<std::int64_t>, std::vector<std::uint64_t>,
                 std::map<std::string, std::int64_t>, std::map<std::string, std::uint64_t>>;

struct binding {
   std::string name;
   value data;

   bool operator==(const binding&) const = default;
};

using arguments = std::vector<binding>;

struct result {
   std::variant<std::int64_t, std::uint64_t, bool> value;
   std::uint64_t ast_events = 0;

   bool operator==(const result&) const = default;
};

struct compilation {
   std::string source;
   std::vector<variable> variables;
   limits budget;
   std::uint32_t profile = profile_version;
   std::uint32_t semantics = semantics_version;
};

struct calculation {
   artifact program;
   arguments inputs;
};

BOOST_DESCRIBE_STRUCT(artifact, (), (format, profile, semantics, variables, budget, result, expression, digest))
BOOST_DESCRIBE_STRUCT(binding, (), (name, data))
BOOST_DESCRIBE_STRUCT(result, (), (value, ast_events))
BOOST_DESCRIBE_STRUCT(compilation, (), (source, variables, budget, profile, semantics))
BOOST_DESCRIBE_STRUCT(calculation, (), (program, inputs))

} // namespace forge::expression
