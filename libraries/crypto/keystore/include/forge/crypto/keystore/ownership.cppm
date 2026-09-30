module;

#include <filesystem>
#include <cstdint>
#include <memory>

export module forge.crypto.keystore.ownership;

export import forge.crypto.keystore.exceptions;

export namespace forge::crypto::keystore {

// Exclusive process ownership is independent from decrypted store lifetime.
// The persistent sibling lock file must never be removed or replaced.
class ownership final {
   class impl;

 public:
   class access final {
    public:
      ~access();
      access(const access&) = delete;
      access& operator=(const access&) = delete;

    private:
      explicit access(std::shared_ptr<impl> value);
      std::shared_ptr<impl> impl_;
      friend class ownership;
   };

   [[nodiscard]] static std::shared_ptr<ownership> acquire(std::filesystem::path path);
   ~ownership();
   ownership(const ownership&) = delete;
   ownership& operator=(const ownership&) = delete;

   [[nodiscard]] const std::filesystem::path& path() const noexcept;
   void verify() const;
   void require_file(std::uint64_t max_bytes) const;
   [[nodiscard]] std::shared_ptr<access> claim();

 private:
   explicit ownership(std::unique_ptr<impl> value);
   std::shared_ptr<impl> impl_;
};

} // namespace forge::crypto::keystore
