module;

#include <boost/describe.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module forge.vm.wasm.optimizing.compile_request;

export import forge.crypto.digest.sha256;
export import forge.vm.wasm.interpret.host_function;
export import forge.vm.wasm.interpret.options;
import forge.vm.wasm.optimizing.invocation_abi;

export namespace forge::vm::wasm::optimizing {

// Exact engine source and build-profile identity, not a user-selected label.
std::string_view engine_identity() noexcept;

struct import_signature {
   std::string module;
   std::string name;
   interpret::host_function signature;
   friend bool operator==(const import_signature&, const import_signature&) = default;
};

struct target_profile {
   std::string triple = "x86_64-unknown-linux-gnu";
   std::string cpu = "x86-64";
   std::string features;
   std::string llvm_version = "22.1.8";
   std::uint32_t codegen_version = 5;
   std::uint32_t host_abi_version = current_host_abi_version;
   std::string engine = std::string(engine_identity());
   // Full SAME canonical profile recognition plus supported host OS/CPU.
   // Does not promise mapping/page availability or process hardening.
   bool matches_host() const noexcept;
   // Sole current-host factory; unsupported hosts fail, never pretend Linux.
   static target_profile native();
   friend bool operator==(const target_profile&, const target_profile&) = default;
};

struct compile_limits {
   // Budgets are explicit. Zero is not an unbounded compiler configuration.
   std::uint64_t max_generated_bytes = 0;
   std::uint64_t max_function_stack_bytes = 0;
   friend bool operator==(const compile_limits&, const compile_limits&) = default;
};

// Shared by the owned request and immutable artifact. This is the sole
// compile configuration, not a second execution-options record.
struct compile_configuration {
   static constexpr std::uint32_t current_version = 1;
   std::uint32_t version = current_version;
   interpret::options execution;
   std::vector<import_signature> imports;
   target_profile target;
   compile_limits limits;
   friend bool operator==(const compile_configuration&, const compile_configuration&) = default;
};

struct compile_request {
   std::vector<std::uint8_t> wasm;
   compile_configuration configuration;

   std::vector<std::uint8_t> encode() const;
   static compile_request decode(std::span<const std::uint8_t> bytes, std::uint64_t max_encoded_bytes);
   friend bool operator==(const compile_request&, const compile_request&) = default;
};

// Canonical import order is module/name, never process registry ordinal.
// Duplicate names or non-numeric/multi-result signatures are rejected.
compile_configuration canonicalize(compile_configuration configuration);
crypto::digest::sha256 identify(const crypto::digest::sha256& wasm, const compile_configuration& configuration);
crypto::digest::sha256 identify(const compile_request& request);

BOOST_DESCRIBE_STRUCT(import_signature, (), (module, name, signature))
BOOST_DESCRIBE_STRUCT(target_profile, (),
                      (triple, cpu, features, llvm_version, codegen_version, host_abi_version, engine))
BOOST_DESCRIBE_STRUCT(compile_limits, (), (max_generated_bytes, max_function_stack_bytes))
BOOST_DESCRIBE_STRUCT(compile_configuration, (), (version, execution, imports, target, limits))
BOOST_DESCRIBE_STRUCT(compile_request, (), (wasm, configuration))

} // namespace forge::vm::wasm::optimizing
