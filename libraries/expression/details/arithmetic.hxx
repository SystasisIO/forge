#pragma once

#include "absl/status/status.h"

namespace cel {
class FunctionRegistry;
class TypeCheckerBuilder;
} // namespace cel

namespace forge::expression::detail {

absl::Status add_arithmetic_declarations(cel::TypeCheckerBuilder& builder);
absl::Status register_arithmetic_functions(cel::FunctionRegistry& registry);

} // namespace forge::expression::detail
