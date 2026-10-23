module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

export module forge.expression.worker_protocol;

export import forge.variant.value;
export import forge.expression.types;

// Internal worker transport. These process-local records are not a persisted
// expression format, a network API or a portable prepared-program identifier.
export namespace forge::expression::worker_protocol {

inline constexpr std::uint32_t version = 2;
inline constexpr std::size_t frame_limit = 128U * 1024U;
inline constexpr std::size_t prefix_bytes = sizeof(std::uint32_t);

enum class operation : std::uint8_t { hello, prepare, evaluate, release, stop, memory_probe, load };
enum class status : std::uint8_t {
   ok,
   invalid_profile,
   invalid_program,
   invalid_input,
   resource_exhausted,
   evaluation_failed,
   internal
};
enum class value_kind : std::uint8_t { null, int64, uint64, boolean, string, list, map };

struct request {
   std::uint32_t version = worker_protocol::version;
   std::uint64_t sequence = 0;
   operation op = operation::hello;
   std::string source;
   std::vector<forge::expression::variable> declarations;
   forge::expression::limits limits;
   std::uint64_t prepared_id = 0;
   std::uint64_t probe_bytes = 0;
   std::vector<std::uint8_t> expression;
   forge::expression::type result = forge::expression::type::int64;
   forge::variant_object values;
};

struct response {
   std::uint32_t version = worker_protocol::version;
   std::uint64_t sequence = 0;
   status code = status::ok;
   value_kind kind = value_kind::null;
   std::int64_t i64 = 0;
   std::uint64_t u64 = 0;
   bool boolean = false;
   std::uint64_t ast_events = 0;
   std::uint64_t prepared_id = 0;
   std::uint64_t heap_live = 0;
   std::uint64_t heap_peak = 0;
   std::uint64_t heap_cap = 0;
   std::uint64_t heap_failures = 0;
   std::vector<std::uint8_t> expression;
   forge::expression::type result = forge::expression::type::int64;
};

// The parent can encode the caller's borrowed object directly into its bounded
// frame. It need not clone a complete activation into the request record.
std::size_t encode_into(const request&, const forge::variant_object&, std::span<std::uint8_t>);
std::size_t encode_into(const request&, std::span<std::uint8_t>);
std::size_t encode_into(const response&, std::span<std::uint8_t>);
request decode_request(std::span<const std::uint8_t>);
response decode_response(std::span<const std::uint8_t>);
void encode_length(std::uint32_t, std::span<std::uint8_t>);
std::uint32_t decode_length(std::span<const std::uint8_t>);

} // namespace forge::expression::worker_protocol
