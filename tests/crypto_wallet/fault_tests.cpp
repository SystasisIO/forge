#include "fsync_failure.hxx"

#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>
#include <unistd.h>
#include <filesystem>
#include <string>

import forge.asio.blocking;
import forge.asio.runtime;
import forge.asio.task;
import forge.asio.compute;
import forge.crypto.keystore.exceptions;
import forge.crypto.wallet.exceptions;
import forge.plugins.crypto.wallet.provider;

BOOST_AUTO_TEST_CASE(uncertain_commit_faults_wallet_before_any_further_signing) {
   namespace wallet = forge::crypto::wallet;
   using forge::asio::blocking::run;
   auto pattern = (std::filesystem::temp_directory_path() / "forge-wallet-fault-XXXXXX").string();
   BOOST_REQUIRE(::mkdtemp(pattern.data()) != nullptr);
   auto cleanup = boost::scope::scope_exit{[&] {
      forge::tests::wallet::clear_sync_failure();
      std::error_code error;
      std::filesystem::remove_all(pattern, error);
   }};
   forge::asio::runtime runtime;
   forge::asio::task::scheduler scheduler{runtime};
   forge::asio::compute::pool compute{{.worker_threads = 1}};
   forge::plugins::crypto::wallet::provider provider{"producer"};
   provider.initialize(pattern, scheduler, compute.get_executor(), 0);
   provider.startup();
   auto stop = boost::scope::scope_exit{[&] { run(runtime, provider.shutdown()); }};
   run(runtime, provider.create("test-password"));
   run(runtime, provider.unlock("test-password"));
   run(runtime, provider.create_key("before"));
   forge::tests::wallet::fail_commit_sync(pattern);
   BOOST_CHECK_THROW(run(runtime, provider.create_key("uncertain")),
                     forge::crypto::keystore::exceptions::durability_unknown);
   BOOST_REQUIRE(forge::tests::wallet::commit_sync_failed());
   BOOST_CHECK(run(runtime, provider.status()).status == wallet::state::fault);
   BOOST_CHECK_THROW(run(runtime, provider.unlock("test-password")), wallet::exceptions::storage_error);
   BOOST_CHECK_THROW(run(runtime, provider.sign_digest({.id = {.value = "before"}})), wallet::exceptions::locked);
   BOOST_CHECK_THROW(run(runtime, provider.remove_key("before")), wallet::exceptions::storage_error);
}
