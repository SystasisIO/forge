#pragma once

namespace forge::crypto::keystore {

class ownership::impl {
 public:
   explicit impl(std::filesystem::path path);
   ~impl();
   void verify() const;
   void require_file(std::uint64_t max_bytes) const;
   void attach();
   void detach() noexcept;
   [[nodiscard]] const std::filesystem::path& path() const noexcept;

 private:
   std::filesystem::path path_;
   std::filesystem::path lock_path_;
   int descriptor_ = -1;
   pid_t process_ = 0;
   std::atomic_bool attached_ = false;
};

} // namespace forge::crypto::keystore
