#include <algorithm>
#include <string_view>
#include <boost/test/unit_test.hpp>

import forge.config.core.component;

#include "details/config_assertions.hxx"

namespace forge::tests::plugins {

[[nodiscard]] const forge::config::core::field_descriptor&
require_field(const forge::config::core::component_descriptor& descriptor, std::string_view name) {
   const auto found = std::ranges::find_if(descriptor.fields, [&](const auto& field) { return field.name == name; });
   BOOST_REQUIRE(found != descriptor.fields.end());
   return *found;
}

} // namespace forge::tests::plugins
