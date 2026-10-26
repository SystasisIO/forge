module;

#include <boost/describe.hpp>
#include <array>
#include <cstdint>
#include <span>
#include <vector>
#if !defined(FORGE_CONTRACT_GUEST)
#include <forge/exceptions/macros.hpp>
#endif

export module forge.crypto.exchange;

export import forge.crypto.core.types;
export import forge.crypto.digest.sha256;
#if !defined(FORGE_CONTRACT_GUEST)
export import forge.exceptions;
#endif

export namespace forge::crypto::exchange {

inline constexpr std::uint32_t block_bytes = 2048;
inline constexpr std::uint32_t max_blocks = 16;
inline constexpr std::uint32_t blinder_bytes = 16;
inline constexpr std::uint32_t key_bytes = 32;
inline constexpr std::uint32_t word_stride = 65;

using key = std::array<std::uint8_t, key_bytes>;

class exceptions {
 public:
   enum class code : std::uint16_t { invalid_input = 1, invalid_delivery = 2 };
#if defined(FORGE_CONTRACT_GUEST)
   struct invalid_input final {};
   struct invalid_delivery final {};
#else
   using invalid_input = forge::exceptions::coded_exception<code, code::invalid_input>;
   using invalid_delivery = forge::exceptions::coded_exception<code, code::invalid_delivery>;
#endif
};

struct block {
   std::uint32_t size = 0;
   forge::crypto::digest::sha256 source;
   forge::crypto::digest::sha256 ciphertext;
};

struct manifest {
   std::uint32_t version = 1;
   forge::crypto::digest::sha256 context;
   forge::crypto::digest::sha256 evidence;
   forge::crypto::digest::sha256 key_hash;
   std::vector<block> blocks;
};

struct fault {
   std::uint32_t index = 0;
   forge::crypto::core::bytes ciphertext;
};

void validate(const manifest& value);
[[nodiscard]] forge::crypto::core::bytes encrypt(const key& secret, std::uint32_t index,
                                                 std::span<const std::uint8_t> opening);
[[nodiscard]] forge::crypto::core::bytes open(const manifest& value, const key& secret, const fault& witness);
[[nodiscard]] bool proves_fault(const manifest& value, const key& secret, const fault& witness);

BOOST_DESCRIBE_STRUCT(block, (), (size, source, ciphertext))
BOOST_DESCRIBE_STRUCT(manifest, (), (version, context, evidence, key_hash, blocks))
BOOST_DESCRIBE_STRUCT(fault, (), (index, ciphertext))
#if !defined(FORGE_CONTRACT_GUEST)
FORGE_DECLARE_EXCEPTION_CATEGORY(exceptions::code, "forge.crypto.exchange")
#endif

} // namespace forge::crypto::exchange
