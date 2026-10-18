#include "test_prelude.hpp"
import forge.vm.wasm.interpret.allocator;
import forge.vm.wasm.interpret.stack_elem;
import forge.vm.wasm.interpret.utils;
import forge.vm.wasm.interpret.backend;
#define FORGE_VM_WASM_INTERPRET_TEST_USES_BACKEND
#include "test_support.hpp"

#define FORGE_VM_WASM_INTERPRET_TEST_FILE implementation_limits_tests

#include <algorithm>
#include <vector>
#include <iterator>
#include <cstdlib>
#include <fstream>
#include <string>



using namespace forge::vm::wasm::interpret;

void host_call() {}

#include "implementation_limits.hpp"

namespace {

wasm_code implementation_limits_wasm_code{
   implementation_limits_wasm + 0,
   implementation_limits_wasm + sizeof(implementation_limits_wasm)};

struct dynamic_options {
   std::uint32_t max_call_depth;
};

}

BACKEND_TEST_CASE( "Test call depth", "[call_depth]") {
   wasm_allocator wa;
   using rhf_t     = forge::vm::wasm::interpret::registered_host_functions<standalone_function_t>;
   using backend_t = forge::vm::wasm::interpret::backend<rhf_t, TestType>;

   rhf_t::add<&host_call>("env", "host.call");

   backend_t bkend(implementation_limits_wasm_code, get_wasm_allocator());

   rhf_t::resolve(bkend.get_module());

   BOOST_TEST(static_cast<bool>(!bkend.call_with_return("env", "call", (uint32_t)250)));
   BOOST_CHECK_THROW(bkend.call("env", "call", (uint32_t)251), std::exception);
   BOOST_TEST(static_cast<bool>(!bkend.call_with_return("env", "call.indirect", (uint32_t)250)));
   BOOST_CHECK_THROW(bkend.call("env", "call.indirect", (uint32_t)251), std::exception);
   // The host call is added to the recursive function, so we have one fewer frames
   BOOST_TEST(static_cast<bool>(!bkend.call_with_return("env", "call.host", (uint32_t)249)));
   BOOST_CHECK_THROW(bkend.call("env", "call.host", (uint32_t)250), std::exception);
   BOOST_TEST(static_cast<bool>(!bkend.call_with_return("env", "call.indirect.host", (uint32_t)249)));
   BOOST_CHECK_THROW(bkend.call("env", "call.indirect.host", (uint32_t)250), std::exception);
}

BACKEND_TEST_CASE( "Test call depth dynamic", "[call_depth]") {
   wasm_allocator wa;
   using rhf_t     = forge::vm::wasm::interpret::registered_host_functions<standalone_function_t>;
   using backend_t = forge::vm::wasm::interpret::backend<rhf_t, TestType, dynamic_options>;
   rhf_t::add<&host_call>("env", "host.call");

   backend_t bkend(implementation_limits_wasm_code, nullptr, dynamic_options{151});
   bkend.set_wasm_allocator(&wa);
   bkend.initialize(nullptr);

   rhf_t::resolve(bkend.get_module());

   BOOST_TEST(static_cast<bool>(!bkend.call_with_return("env", "call", (uint32_t)150)));
   BOOST_CHECK_THROW(bkend.call("env", "call", (uint32_t)151), std::exception);
   BOOST_TEST(static_cast<bool>(!bkend.call_with_return("env", "call.indirect", (uint32_t)150)));
   BOOST_CHECK_THROW(bkend.call("env", "call.indirect", (uint32_t)151), std::exception);
   // The host call is added to the recursive function, so we have one fewer frames
   BOOST_TEST(static_cast<bool>(!bkend.call_with_return("env", "call.host", (uint32_t)149)));
   BOOST_CHECK_THROW(bkend.call("env", "call.host", (uint32_t)150), std::exception);
   BOOST_TEST(static_cast<bool>(!bkend.call_with_return("env", "call.indirect.host", (uint32_t)149)));
   BOOST_CHECK_THROW(bkend.call("env", "call.indirect.host", (uint32_t)150), std::exception);

   bkend.initialize(nullptr, dynamic_options{51});

   BOOST_TEST(static_cast<bool>(!bkend.call_with_return("env", "call", (uint32_t)50)));
   BOOST_CHECK_THROW(bkend.call("env", "call", (uint32_t)51), std::exception);
   BOOST_TEST(static_cast<bool>(!bkend.call_with_return("env", "call.indirect", (uint32_t)50)));
   BOOST_CHECK_THROW(bkend.call("env", "call.indirect", (uint32_t)51), std::exception);
   // The host call is added to the recursive function, so we have one fewer frames
   BOOST_TEST(static_cast<bool>(!bkend.call_with_return("env", "call.host", (uint32_t)49)));
   BOOST_CHECK_THROW(bkend.call("env", "call.host", (uint32_t)50), std::exception);
   BOOST_TEST(static_cast<bool>(!bkend.call_with_return("env", "call.indirect.host", (uint32_t)49)));
   BOOST_CHECK_THROW(bkend.call("env", "call.indirect.host", (uint32_t)50), std::exception);

   // Refuse over-profile configuration, then recover with the supported depth.
   BOOST_CHECK_THROW(bkend.initialize(nullptr, dynamic_options{1024*1024}), exceptions::profile);
   bkend.initialize(nullptr, dynamic_options{51});
   BOOST_TEST(static_cast<bool>(!bkend.call_with_return("env", "call", (uint32_t)50)));
}
