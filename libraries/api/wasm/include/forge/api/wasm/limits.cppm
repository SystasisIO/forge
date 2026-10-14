module;

#include <cstdint>

export module forge.api.wasm.limits;

export import forge.raw.codec;

export namespace forge::api::wasm {

struct limits {
   raw::unpack_limits wire{
      .max_container_elements = 65536,
      .max_total_container_elements = 131072,
      .max_bytes = 2U * 1024U * 1024U,
      .first_container_elements = 65536,
   };
   std::uint32_t name_bytes = 128;
   std::uint32_t payload_bytes = 1024U * 1024U;
   std::uint32_t continuation_bytes = 1024U * 1024U;
   std::uint32_t target_bytes = 4096;
};

} // namespace forge::api::wasm
