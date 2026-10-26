# API Core: remote unary methods returning void

The generated remote unary proxy now supports `boost::asio::awaitable<void>`
without constructing `std::optional<void>` or attempting to decode a Raw
`void` value. A successful Raw response must have an empty body; a non-empty
body remains a protocol error. Existing remote error projection is unchanged.

Typed invokers receive `typeid(void)` and a null response-storage pointer for
a void method. Non-void methods retain their optional result-storage contract.
This is a compatible Core correction, not a new HTTP response mapping.

The public `package_api_void_response_component` consumer covers the generated
proxy, real registry/dispatcher, zero/one/multiple arguments, direct invocation,
typed invocation, errors, malformed bodies and an existing non-void response.
Build-tree and installed-package qualification results are recorded separately.
