#include <boost/test/unit_test.hpp>
#include <array>
#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

import forge.expression.program;
import forge.expression.exceptions;

namespace {

namespace expression = forge::expression;
namespace errors = expression::exceptions;

class helper_fixture {
 public:
   helper_fixture() {
      auto pattern = (std::filesystem::temp_directory_path() / "forge-expression-faults-XXXXXX").string();
      const auto* created = ::mkdtemp(pattern.data());
      BOOST_REQUIRE(created != nullptr);
      _directory = created;
   }

   ~helper_fixture() {
      std::error_code ignored;
      std::filesystem::remove_all(_directory, ignored);
   }

   std::filesystem::path script(std::string name, const char* body) const {
      const auto file = _directory / std::move(name);
      {
         std::ofstream output{file};
         output << "#!/bin/sh\nulimit -c 0\n" << body << '\n';
         BOOST_REQUIRE(output.good());
      }
      BOOST_REQUIRE_EQUAL(::chmod(file.c_str(), 0700), 0);
      return file;
   }

 private:
   std::filesystem::path _directory;
};

template <typename Work> void failure(errors::code expected, Work&& work) {
   BOOST_CHECK_EXCEPTION(work(), forge::exceptions::base,
                         [&](const auto& error) { return error.code() == errors::make_error_code(expected); });
}

std::size_t open_descriptors() {
   std::size_t count = 0;
   const auto directory = ::opendir("/dev/fd");
   BOOST_REQUIRE(directory != nullptr);
   while (const auto* entry = ::readdir(directory)) {
      if (entry->d_name[0] != '.')
         ++count;
   }
   ::closedir(directory);
   return count;
}

std::string balanced_sum(std::size_t leaves) {
   if (leaves == 1)
      return "1";
   return "(" + balanced_sum(leaves / 2) + "+" + balanced_sum(leaves - leaves / 2) + ")";
}

} // namespace

BOOST_AUTO_TEST_SUITE(expression_runtime)

BOOST_AUTO_TEST_CASE(worker_faults_are_classified_and_drained) {
   helper_fixture fixtures;
   const auto absent = fixtures.script("eof", "exit 0");
   const auto exhausted = fixtures.script("oom", "printf '\\001' >&3\nkill -ABRT $$");
   const auto stalled = fixtures.script("stalled", "exec /bin/sleep 10");
   const auto malformed = fixtures.script("malformed", "printf '\\377\\377\\377\\177'");
   const auto truncated = fixtures.script("truncated", "printf '\\001\\000\\000\\000\\001'");
   const auto before = open_descriptors();
   for (int iteration = 0; iteration < 3; ++iteration) {
      failure(errors::code::worker_unavailable, [&] { expression::runtime value({.helper = absent}); });
      failure(errors::code::resource_exhausted, [&] { expression::runtime value({.helper = exhausted}); });
      failure(errors::code::deadline_exceeded,
              [&] { expression::runtime value({.helper = stalled, .timeout = std::chrono::milliseconds{100}}); });
      failure(errors::code::worker_unavailable, [&] { expression::runtime value({.helper = malformed}); });
      failure(errors::code::worker_unavailable, [&] { expression::runtime value({.helper = truncated}); });
   }
   BOOST_CHECK_EQUAL(open_descriptors(), before);

   // Fault fixtures are transport tests only. A fresh real native worker must
   // still prepare and execute after all of the failed owners were retired.
   expression::runtime healthy({.helper = FORGE_EXPRESSION_WORKER});
   BOOST_CHECK_EQUAL(expression::program(healthy, "9007199254740993 + 2").evaluate().value.as_int64(),
                     9007199254740995LL);
}

BOOST_AUTO_TEST_CASE(invalid_helper_configuration_has_no_ambient_fallback) {
   failure(errors::code::invalid_profile, [] { expression::runtime value({.helper = "forge-expression-worker"}); });
   failure(errors::code::invalid_profile,
           [] { expression::runtime value({.helper = FORGE_EXPRESSION_WORKER, .heap_bytes = 65535}); });
   failure(errors::code::invalid_profile, [] {
      expression::runtime value({.helper = FORGE_EXPRESSION_WORKER, .heap_bytes = 64U * 1024U * 1024U + 1});
   });
}

BOOST_AUTO_TEST_CASE(real_worker_exhaustion_reaches_the_public_error_boundary) {
   std::vector<expression::variable> variables;
   for (int index = 0; index < 64; ++index)
      variables.push_back({"v" + std::to_string(index)});
   const auto source = balanced_sum(256);
   const auto before = open_descriptors();
   {
      expression::runtime bounded({.helper = FORGE_EXPRESSION_WORKER, .heap_bytes = 512U * 1024U});
      failure(errors::code::resource_exhausted, [&] { expression::program program(bounded, source, variables); });
   }
   BOOST_CHECK_EQUAL(open_descriptors(), before);
   expression::runtime fresh({.helper = FORGE_EXPRESSION_WORKER});
   BOOST_CHECK_EQUAL(expression::program(fresh, "7 * 6").evaluate().value.as_int64(), 42);
}

BOOST_AUTO_TEST_SUITE_END()
