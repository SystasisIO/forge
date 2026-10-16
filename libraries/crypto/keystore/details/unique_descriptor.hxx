#pragma once

namespace forge::crypto::keystore {

class unique_descriptor {
 public:
   explicit unique_descriptor(int value = -1) noexcept;
   ~unique_descriptor();

   unique_descriptor(const unique_descriptor&) = delete;
   unique_descriptor& operator=(const unique_descriptor&) = delete;
   unique_descriptor(unique_descriptor&& other) noexcept;
   unique_descriptor& operator=(unique_descriptor&& other) noexcept;

   [[nodiscard]] int get() const noexcept;
   int close() noexcept;
   void reset() noexcept;

 private:
   int value_;
};

} // namespace forge::crypto::keystore
