#include <chrono>
#include <cstdint>
#include <span>

import forge.auth.credential.record;
import forge.auth.credential.serialization;
import forge.raw.codec;

int main() {
   namespace credentials = forge::auth::credential;
   const auto now = credentials::time_point{std::chrono::seconds{1}};
   auto owner = credentials::record{
       .id = {.value = "configured-credential"},
       .identity = "configured-subject",
       .scopes = credentials::canonicalize_scopes({"read", "write", "read"}),
       .issued_at = now,
       .updated_at = now,
   };
   credentials::validate_active(owner, now);
   const auto bytes = forge::raw::pack(owner);
   const auto decoded = forge::raw::unpack_exact<credentials::record>(std::span<const std::uint8_t>{bytes});
   const auto binding = credentials::binding{.id = owner.id, .generation = owner.generation};
   credentials::rotate_downscope(owner, {"read"}, now + std::chrono::seconds{1});
   credentials::revoke(owner, now + std::chrono::seconds{2});
   credentials::validate_record(owner);
   return decoded.id == binding.id && decoded.generation == binding.generation && owner.generation == 2U &&
                  owner.state == credentials::state::revoked && owner.revoked_at == owner.updated_at
              ? 0
              : 1;
}
