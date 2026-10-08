module;

#include <forge/exceptions/macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>

export module forge.chain.api.abi;

export import forge.chain.protocol.abi;
export import forge.chain.protocol.abi_metadata;
export import forge.chain.protocol.transaction;
export import forge.exceptions;
export import forge.variant.value;

export namespace forge::chain::api {

enum class abi_error_code : std::uint8_t {
   invalid_abi = 1,
   duplicate_definition = 2,
   circular_definition = 3,
   unknown_type = 4,
   invalid_json = 5,
   invalid_binary = 6,
   missing_field = 7,
   unexpected_field = 8,
   invalid_variant = 9,
   recursion_limit = 10,
   deadline_exceeded = 11,
   size_limit = 12,
   trailing_bytes = 13,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(abi_error_code, "forge.chain.api.abi")

struct abi_diagnostic {
   abi_error_code code = abi_error_code::invalid_abi;
   std::string message;
   std::string type;
   std::string path;
   std::size_t offset = 0;

   bool operator==(const abi_diagnostic&) const = default;
};

class abi_serialization_error final : public forge::exceptions::runtime_coded_exception<abi_error_code> {
 public:
   explicit abi_serialization_error(abi_diagnostic diagnostic);

   [[nodiscard]] const abi_diagnostic& diagnostic() const noexcept;

 private:
   abi_diagnostic diagnostic_;
};

struct abi_serialization_limits {
   std::size_t max_recursion_depth = 32;
   std::chrono::microseconds max_serialization_time{1'000'000};
   std::size_t max_binary_bytes = 8U << 20U;
   std::size_t max_string_bytes = 1U << 20U;
   std::size_t max_container_elements = 1U << 20U;
   std::size_t max_metadata_bytes = 1U << 20U;
   std::size_t max_metadata_entries = 1U << 16U;
   std::size_t max_total_container_elements = std::numeric_limits<std::size_t>::max();
};

enum class abi_json_scalar_policy : std::uint8_t { compatible = 0, exact = 1 };

using abi_resolver = std::function<std::optional<protocol::abi_def>(protocol::account_name)>;

[[nodiscard]] protocol::bytes abi_json_to_bin(const protocol::abi_def& abi, std::string_view type,
                                              const forge::variant& value, abi_serialization_limits limits = {});

[[nodiscard]] protocol::bytes abi_json_to_bin(const protocol::abi_def& abi, std::string_view type,
                                              const forge::variant& value, abi_serialization_limits limits,
                                              abi_json_scalar_policy policy);

[[nodiscard]] forge::variant abi_bin_to_json(const protocol::abi_def& abi, std::string_view type,
                                             std::span<const std::uint8_t> binary,
                                             abi_serialization_limits limits = {});

[[nodiscard]] protocol::bytes abi_json_to_bin(const protocol::abi_def& abi, const protocol::abi_metadata& metadata,
                                              std::string_view type, const forge::variant& value,
                                              abi_serialization_limits limits = {});

[[nodiscard]] protocol::bytes abi_json_to_bin(const protocol::abi_def& abi, const protocol::abi_metadata& metadata,
                                              std::string_view type, const forge::variant& value,
                                              abi_serialization_limits limits, abi_json_scalar_policy policy);

[[nodiscard]] forge::variant abi_bin_to_json(const protocol::abi_def& abi, const protocol::abi_metadata& metadata,
                                             std::string_view type, std::span<const std::uint8_t> binary,
                                             abi_serialization_limits limits = {});

[[nodiscard]] forge::variant action_to_variant(const protocol::action& action, const abi_resolver& resolve,
                                               abi_serialization_limits limits = {});

[[nodiscard]] forge::variant transaction_to_variant(const protocol::transaction& transaction,
                                                    const abi_resolver& resolve, abi_serialization_limits limits = {});

} // namespace forge::chain::api
