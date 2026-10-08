#include <array>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

import forge.contract;

namespace example {

enum class shape : std::uint8_t { circle = 0, tall = 2 };
enum class direction : std::int16_t { backward = -3, forward = 7 };
using shape_alias = shape;

struct nested {
   shape_alias value;
   direction movement;
};

struct request {
   nested child;
   std::optional<shape_alias> maybe;
   std::vector<shape_alias> list;
   std::array<shape, 2> fixed;
   std::variant<shape_alias, nested> selection;
};

} // namespace example

class [[forge::contract("rootfixture")]] rootfixture : public forge::contract::context {
 public:
   using context::context;
   [[forge::action]] void submit(std::vector<std::uint8_t> payload) { static_cast<void>(payload); }
};
