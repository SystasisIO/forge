module;

#include <boost/describe.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

export module forge.vm.wasm.optimizing.artifact;

export import forge.vm.wasm.optimizing.compile_request;
export import forge.vm.wasm.optimizing.invocation_abi;

export namespace forge::vm::wasm::optimizing {

enum class section_kind : std::uint8_t { code, readonly_data };
enum class relocation_kind : std::uint8_t {
   absolute64, relative32,
   aarch64_call26, aarch64_jump26, aarch64_page21, aarch64_add12,
   aarch64_load_store8, aarch64_load_store16, aarch64_load_store32, aarch64_load_store64
};

struct image_section {
   section_kind kind = section_kind::code;
   std::uint64_t alignment = 1;
   std::vector<std::uint8_t> bytes;
};
BOOST_DESCRIBE_STRUCT(image_section, (), (kind, alignment, bytes))

struct image_location {
   std::uint32_t section = 0;
   std::uint64_t offset = 0;
};
BOOST_DESCRIBE_STRUCT(image_location, (), (section, offset))

struct image_relocation {
   relocation_kind kind = relocation_kind::relative32;
   image_location source;
   image_location target;
   std::int64_t addend = 0;
};
BOOST_DESCRIBE_STRUCT(image_relocation, (), (kind, source, target, addend))

struct compiled_function {
   image_location entry;
   std::uint32_t type_index = 0;
   std::uint32_t import_index = UINT32_MAX;
   std::uint64_t stack_bytes = 0;
};
BOOST_DESCRIBE_STRUCT(compiled_function, (), (entry, type_index, import_index, stack_bytes))

struct compiled_export {
   std::string name;
   std::uint32_t function_index = 0;
};
BOOST_DESCRIBE_STRUCT(compiled_export, (), (name, function_index))

struct global_initial_value {
   interpret::value_type type = interpret::i32;
   bool is_mutable = false;
   std::uint64_t bits = 0;
};
BOOST_DESCRIBE_STRUCT(global_initial_value, (), (type, is_mutable, bits))

struct memory_initial_state {
   bool defined = false;
   std::uint32_t initial_pages = 0;
   std::uint32_t max_pages = 0;
};
BOOST_DESCRIBE_STRUCT(memory_initial_state, (), (defined, initial_pages, max_pages))

struct memory_initial_segment {
   std::uint32_t offset = 0;
   std::vector<std::uint8_t> bytes;
};
BOOST_DESCRIBE_STRUCT(memory_initial_segment, (), (offset, bytes))

// Pointer-free persisted compilation output. Function indices are the
// module-wide indices, including import thunks. Type indices are canonical
// structural signature IDs, not process registry indices.
struct artifact_payload {
   crypto::digest::sha256 wasm;
   compile_configuration configuration;
   std::vector<image_section> sections;
   std::vector<image_relocation> relocations;
   std::vector<interpret::host_function> types;
   std::vector<compiled_function> functions;
   std::vector<compiled_export> exports;
   std::vector<global_initial_value> globals;
   memory_initial_state memory;
   std::vector<memory_initial_segment> data;
   std::vector<std::uint32_t> table;
   std::uint32_t start = UINT32_MAX;
};
BOOST_DESCRIBE_STRUCT(artifact_payload, (),
                      (wasm, configuration, sections, relocations, types, functions, exports, globals, memory, data,
                       table, start))

// Copies pin the same checked values and the same immutable native mapping.
// Borrowed views remain valid while any copy of this artifact is alive.
class artifact {
 public:
   static constexpr std::uint32_t current_version = 1;

   static artifact from_payload(artifact_payload payload);
   static artifact decode(std::span<const std::uint8_t> bytes, const crypto::digest::sha256& expected_identity,
                          std::uint64_t max_encoded_bytes);
   std::vector<std::uint8_t> encode() const;

   const crypto::digest::sha256& identity() const noexcept;
   const artifact_payload& image() const noexcept;
   std::span<const compiled_export> exports() const noexcept;
   std::uint64_t byte_size() const noexcept;
   // Exact prospective value of byte_size() after successful prepare(): owned
   // metadata/encoding plus the existing planner's full mapping reservation
   // and exact-sized native function table. Does not mmap/protect/publish;
   // may allocate temporary planner storage and throw existing validation or
   // budget errors. Not a host/page-compatibility or process-RSS guarantee.
   std::uint64_t prepared_byte_size() const;

   // Advisory shared-pin snapshot only. Use with external cache/index
   // exclusion; this is not synchronization, an eviction lease or permission
   // to mutate/unmap code. Existing artifact copies remain lifetime authority.
   bool is_unique() const noexcept;

   // Explicit preparation only; never called implicitly by the executor or
   // ready lookup. May allocate/protect native pages; run outside product writer.
   // Preparation requires the exact Linux x86-64 or Darwin ARM64 host profile.
   // The ELF container does not select the operating-system calling ABI.
   // A failed preparation
   // publishes nothing; retry is explicit and controlled by the session.
   void prepare() const;
   bool is_prepared() const noexcept;
   std::span<const std::byte> native_code() const;
   std::span<const native_function> functions() const;
   entry_function entry(std::uint32_t index) const;

 private:
   struct impl;
   explicit artifact(std::shared_ptr<const impl> value);
   std::shared_ptr<const impl> _impl;
};

} // namespace forge::vm::wasm::optimizing
