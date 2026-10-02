#define FORGE_DISABLE_LOGGING
#include <forge/log/macros.hpp>
#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_CASE(disabled_logging_compiles_out_every_level_and_argument_form) {
   int evaluated = 0;
   tlog(++evaluated);
   dlog(++evaluated, ++evaluated);
   ilog(++evaluated, ++evaluated, ("value", ++evaluated));
   wlog(nonexistent_logger(), nonexistent_record());
   elog(nonexistent_logger(), nonexistent_message(), ("value", nonexistent_value()));
   BOOST_TEST(evaluated == 0);
}
