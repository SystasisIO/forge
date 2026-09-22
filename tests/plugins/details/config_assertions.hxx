#pragma once

#include <string_view>

// Include after importing forge.config.core.component.
namespace forge::tests::plugins {

[[nodiscard]] const forge::config::core::field_descriptor&
require_field(const forge::config::core::component_descriptor& descriptor, std::string_view name);

} // namespace forge::tests::plugins
