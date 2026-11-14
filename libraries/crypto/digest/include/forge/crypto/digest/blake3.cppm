module;

#include <array>
#include <boost/describe.hpp>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

export module forge.crypto.digest.blake3;

export namespace forge::crypto::digest {

struct blake3 {
   std::array<std::uint8_t, 32> bytes{};

   [[nodiscard]] std::string str() const;
   [[nodiscard]] auto operator<=>(const blake3&) const = default;
   [[nodiscard]] static blake3 from_hex(std::string_view input);
   [[nodiscard]] static blake3 hash(std::span<const std::byte> input);
   [[nodiscard]] constexpr std::array<std::uint8_t, 32> extract_as_byte_array() const noexcept {
      return bytes;
   }

   class encoder {
    public:
      encoder();
      ~encoder();

      void write(std::span<const std::byte> input) noexcept;
      void reset() noexcept;
      [[nodiscard]] blake3 result() const noexcept;

    private:
      struct impl;
      std::unique_ptr<impl> _state;
   };
};

BOOST_DESCRIBE_STRUCT(blake3, (), (bytes))

} // namespace forge::crypto::digest
