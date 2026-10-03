module;

#include <cstddef>
#include <string>
#include <string_view>

export module forge.crypto.core.secret_string;

export namespace forge::crypto::core {

class secret_string {
 public:
   secret_string() = default;
   secret_string(const std::string& value);
   secret_string(std::string&& value) noexcept;
   secret_string(const char* value);
   ~secret_string();

   secret_string(const secret_string& other);
   secret_string(secret_string&& other) noexcept;
   secret_string& operator=(const secret_string& other);
   secret_string& operator=(secret_string&& other) noexcept;

   secret_string& operator=(const std::string& value);
   secret_string& operator=(std::string&& value) noexcept;
   secret_string& operator=(const char* value);

   [[nodiscard]] bool empty() const noexcept;
   [[nodiscard]] std::size_t size() const noexcept;
   [[nodiscard]] const char* data() const noexcept;
   [[nodiscard]] std::string_view view() const noexcept;
   [[nodiscard]] operator std::string_view() const noexcept;

   void clear() noexcept;

 private:
   std::string value_;
};

[[nodiscard]] constexpr bool diagnostic_is_secret(const secret_string&) noexcept {
   return true;
}

} // namespace forge::crypto::core
