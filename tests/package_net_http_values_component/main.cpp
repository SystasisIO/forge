#include <type_traits>
#include <vector>

import forge.net.http.method;
import forge.raw.codec;

static_assert(sizeof(std::underlying_type_t<forge::net::http::method>) == 4);
static_assert(static_cast<int>(forge::net::http::method::post) == 6);
static_assert(forge::raw::shared_enum_wire_v<forge::net::http::method>);

int main() {
   using forge::net::http::method;
   using forge::net::http::is_idempotent;
   return is_idempotent(method::get) && is_idempotent(method::head) && is_idempotent(method::put) &&
          is_idempotent(method::delete_) && is_idempotent(method::options) && !is_idempotent(method::post) &&
          !is_idempotent(method::patch) && !is_idempotent(method::unknown) &&
          forge::raw::pack(method::post) == forge::raw::bytes{6, 0, 0, 0} ? 0 : 1;
}
