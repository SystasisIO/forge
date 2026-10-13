module;

#include <boost/describe.hpp>

#include <string>
#include <vector>

export module forge.chain.protocol.abi_metadata;

import forge.variant.value;
import forge.variant.described;

export namespace forge::chain::protocol {

struct abi_root_def {
   std::string cpp_type;
   std::string type;
};

struct abi_enum_value {
   std::string name;
   // Canonical decimal text preserves the entire signed and unsigned 64-bit range.
   std::string value;
};

struct abi_enum_def {
   std::string name;
   std::string type;
   std::vector<abi_enum_value> values;
};

// Optional companion to abi_def; it never changes the standard Chain ABI wire format.
struct abi_metadata {
   std::string version = "forge::abi-metadata/1.0";
   std::vector<abi_root_def> roots;
   std::vector<abi_enum_def> enums;
};

BOOST_DESCRIBE_STRUCT(abi_root_def, (), (cpp_type, type))
BOOST_DESCRIBE_STRUCT(abi_enum_value, (), (name, value))
BOOST_DESCRIBE_STRUCT(abi_enum_def, (), (name, type, values))
BOOST_DESCRIBE_STRUCT(abi_metadata, (), (version, roots, enums))

inline void to_variant(const abi_metadata& value, forge::variant& variant) {
   forge::to_variant(value, variant);
}

inline void from_variant(const forge::variant& variant, abi_metadata& value) {
   forge::from_variant(variant, value);
}

} // namespace forge::chain::protocol
