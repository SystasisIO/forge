#include "common.hxx"

#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/this_coro.hpp>

#include <exception>

import forge.asio.compute;
import forge.auth.oidc_agent.client;
import forge.auth.oauth2.interactive_authorizer;

#include "workflow.hxx"

int main(int argc, char** argv) {
   return auth_live::run(argc, argv, [](const auth_live::options& config) -> boost::asio::awaitable<void> {
      if (config.flow == "authorize" || config.flow == "invalidate") {
         throw forge::auth::oauth2::exceptions::unsupported_request{"agent lifecycle is external"};
      }
      forge::asio::compute::pool blocking{{
         .worker_threads = 1,
         .max_pending_tasks = 0,
         .max_waiting_submissions = 1,
         .thread_name = "forge-auth-live",
      }};
      std::exception_ptr failure;
      try {
         auto provider = forge::auth::oidc_agent::client::create({
            .selection = {.kind = forge::auth::oidc_agent::selection_kind::account, .value = config.account},
            .application_hint = "forge-auth-live",
         }, blocking.get_executor());
         co_await auth_live::probe(*provider, nullptr, config);
      } catch (...) {
         failure = std::current_exception();
      }
      blocking.request_stop();
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
      co_await blocking.shutdown();
      if (failure) {
         std::rethrow_exception(failure);
      }
   });
}
