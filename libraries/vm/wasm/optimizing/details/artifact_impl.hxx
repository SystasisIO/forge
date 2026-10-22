#pragma once

namespace forge::vm::wasm::optimizing {

class native_image;

struct artifact::impl {
   struct retention {
      std::shared_ptr<void> charge;
      std::uint64_t control_bytes = 0;
   };
   template <typename T> struct allocator {
      using value_type = T;
      retention* lifetime;
      explicit allocator(retention* value) noexcept : lifetime(value) {}
      template <typename U> allocator(const allocator<U>& value) noexcept : lifetime(value.lifetime) {}
      T* allocate(std::size_t count) {
         if (count > static_cast<std::size_t>(-1) / sizeof(T)) {
            delete lifetime;
            throw std::bad_alloc{};
         }
         T* result;
         try {
            result = std::allocator<T>{}.allocate(count);
         } catch (...) {
            delete lifetime;
            throw;
         }
         lifetime->control_bytes = count * sizeof(T);
         return result;
      }
      void deallocate(T* pointer, std::size_t count) noexcept {
         auto charge = std::move(lifetime->charge);
         std::allocator<T>{}.deallocate(pointer, count);
         delete lifetime;
         charge.reset(); // Includes weak control-block storage, not just impl destruction.
      }
      template <typename U> bool operator==(const allocator<U>& value) const noexcept {
         return lifetime == value.lifetime;
      }
   };
   impl(artifact_payload, std::vector<std::uint8_t>, std::uint64_t, origin, retention*);
   retention* lifetime;
   artifact_payload image;
   std::vector<std::uint8_t> encoded;
   crypto::digest::sha256 identity;
   std::uint64_t owned_bytes = 0;
   origin authority = origin::untrusted;
   mutable std::mutex preparation;
   mutable std::optional<interpret::prepared_module::preparation> cleanup;
   mutable preparation_limits charged_limits{};
   mutable std::shared_ptr<const native_image> native;
   mutable std::atomic_bool prepared = false;
};

} // namespace forge::vm::wasm::optimizing
