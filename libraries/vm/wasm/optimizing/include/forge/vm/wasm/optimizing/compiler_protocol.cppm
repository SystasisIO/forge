module;

#include <array>
#include <boost/describe.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

export module forge.vm.wasm.optimizing.compiler_protocol;

export import forge.vm.wasm.optimizing.artifact;
export import forge.vm.wasm.optimizing.exceptions;

export namespace forge::vm::wasm::optimizing::compiler_protocol {

inline constexpr std::uint32_t current_version = 2;
inline constexpr std::uint32_t magic = 0x464f4343;
enum class message_kind : std::uint8_t { hello, request, reply, hello_ack, preflight };

struct exchange_id {
   std::uint64_t session = 0;
   std::uint64_t job = 0;
   std::uint64_t attempt = 0;
   friend bool operator==(const exchange_id&, const exchange_id&) = default;
};

struct header {
   std::uint32_t magic = compiler_protocol::magic;
   std::uint32_t version = current_version;
   message_kind kind = message_kind::hello;
   exchange_id exchange;
   crypto::digest::sha256 identity{};
   std::uint64_t body_bytes = 0;
};

struct resource_limits {
   std::uint64_t cpu_seconds = 0;
   std::uint64_t address_space_bytes = 0;
   std::uint64_t stack_bytes = 0;
   friend bool operator==(const resource_limits&, const resource_limits&) = default;
};

struct hello {
   std::array<std::uint32_t, 3> forge_version{};
   std::string build_revision;
   std::uint32_t build_timestamp = 0;
   target_profile target;
   std::uint32_t artifact_version = artifact::current_version;
   std::uint32_t configuration_version = compile_configuration::current_version;
   resource_limits limits;
   friend bool operator==(const hello&, const hello&) = default;
};

struct reply {
   std::optional<exceptions::code> failure;
   // Existing artifact::encode() only; no alternative artifact format.
   std::vector<std::uint8_t> image;
};

// Cold initialization derives this from SAME Raw field order, not sizeof or a
// manually implemented wire-length/endian codec.
std::size_t header_size();
std::vector<std::uint8_t> encode(const header&);
std::vector<std::uint8_t> encode(const hello&);
std::vector<std::uint8_t> encode(const reply&);
// HELLO-only cold control body: the SAME canonical profile, no fake WASM.
std::vector<std::uint8_t> encode(const target_profile&);
header decode_header(std::span<const std::uint8_t>);
hello decode_hello(std::span<const std::uint8_t>, std::uint64_t max_bytes);
reply decode_reply(std::span<const std::uint8_t>, std::uint64_t max_bytes);
target_profile decode_target(std::span<const std::uint8_t>, std::uint64_t max_bytes);

BOOST_DESCRIBE_STRUCT(exchange_id, (), (session, job, attempt))
BOOST_DESCRIBE_STRUCT(header, (), (magic, version, kind, exchange, identity, body_bytes))
BOOST_DESCRIBE_STRUCT(resource_limits, (), (cpu_seconds, address_space_bytes, stack_bytes))
BOOST_DESCRIBE_STRUCT(hello, (),
                      (forge_version, build_revision, build_timestamp, target, artifact_version, configuration_version,
                       limits))
BOOST_DESCRIBE_STRUCT(reply, (), (failure, image))

} // namespace forge::vm::wasm::optimizing::compiler_protocol
