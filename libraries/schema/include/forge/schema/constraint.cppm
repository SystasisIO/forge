module;

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module forge.schema.constraint;

export import forge.schema.input_value;
import forge.schema.diagnostic;

export namespace forge::schema {

enum class constraint_kind { any, null, boolean, integer, number, string, array, object };

// A deliberately bounded, reference-free subset, not an executable JSON Schema.
struct constraint {
   constraint_kind kind = constraint_kind::any;
   std::map<std::string, constraint> properties;
   std::vector<std::string> required;
   bool additional_properties = false;
   std::vector<constraint> items; // Zero or one homogeneous item schema.
   std::optional<std::size_t> min_size;
   std::optional<std::size_t> max_size;
   std::string pattern;
   std::optional<input_value> minimum;
   std::optional<input_value> maximum;
   std::optional<std::uint64_t> decimal_minimum;
   std::optional<std::uint64_t> decimal_maximum;
   std::vector<input_value> enum_values;
   std::optional<input_value> constant;
   bool ascii = false;
   bool nullable = false;
};

struct artifact {
   std::uint32_t version = 1;
   constraint root;
};

struct constraint_limits {
   std::size_t max_depth = 32;
   std::size_t max_nodes = 4096;
   std::size_t max_string_bytes = 16384;
   std::size_t max_container_size = 256;
};

// Throws unsupported_export for malformed or unrepresentable artifacts.
void check_artifact(const artifact& input, constraint_limits limits = {});
[[nodiscard]] constraint refine_constraint(const constraint& type, const constraint& refinement);
[[nodiscard]] std::vector<diagnostic> validate(const constraint& rules, const input_value& input,
                                               std::string_view path = {}, constraint_limits limits = {});
[[nodiscard]] std::vector<diagnostic> validate(const artifact& rules, const input_value& input,
                                               constraint_limits limits = {});
[[nodiscard]] input_value json_schema(const artifact& input, constraint_limits limits = {});
[[nodiscard]] input_value export_artifact(const artifact& input, constraint_limits limits = {});
[[nodiscard]] artifact load_artifact(const input_value& local_input, constraint_limits limits = {});

} // namespace forge::schema
