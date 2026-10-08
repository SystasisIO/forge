module forge.net.p2p.resource_manager;

namespace forge::net::p2p::detail {
void fail_next_stream_reserve_prepare_for_test() noexcept;
}

// The private C++ hook belongs to resource_manager; the fixture belongs to node.
// A test-only C symbol bridges them without exporting either module's internals.
extern "C" void forge_test_pubsub_fail_next_stream_reserve_prepare() noexcept {
   forge::net::p2p::detail::fail_next_stream_reserve_prepare_for_test();
}
