#include <cstdint>

import forge.core.string;
import forge.core.conversion;
import consumer.conversion;

int main()
{
   return forge::core::conversion<consumer::result>{forge::to_uint64("1")}.finish().value == std::uint64_t{1} ? 0 : 1;
}
