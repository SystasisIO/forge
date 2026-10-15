module;

#include <cstdint>

#if !defined(FORGE_CONTRACT_GUEST)
#include <boost/describe.hpp>
#endif

export module forge.net.http.method;

import forge.raw.enum_wire_type;

export namespace forge::net::http {

enum class method {
   unknown,
   delete_,
   get,
   head,
   options,
   patch,
   post,
   put,
};

#if !defined(FORGE_CONTRACT_GUEST)
BOOST_DESCRIBE_ENUM(method, unknown, delete_, get, head, options, patch, post, put)
#endif

[[nodiscard]] bool is_idempotent(method value) noexcept;

} // namespace forge::net::http

export namespace forge::raw {

template <> struct enum_wire_type<net::http::method> {
   using type = std::int32_t;
   static constexpr bool shared = true;
};

} // namespace forge::raw
