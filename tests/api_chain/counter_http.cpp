module;

#include <boost/asio/awaitable.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>

module test.api_chain.counter_http;

import forge.api.core.binding;
import forge.api.http.binding;
import forge.asio.runtime;
import forge.chain.api.info;
import forge.chain.api.submission;
import forge.chain.api.transaction;
import forge.net.http.body;
import forge.net.http.route_context;
import forge.net.http.router;
import forge.net.http.server;
import forge.net.http.stream;
import forge.net.http.types;

namespace demo {

namespace api = forge::chain::api;
namespace http = forge::net::http;

struct counter_http_server::impl {
   forge::asio::runtime runtime{{.worker_threads = 2}};
   forge::api::core::registry apis;
   std::atomic_bool drop_response{true};
   std::atomic_uint disconnected{0};
   http::router submission;
   std::unique_ptr<http::server> listener;

   explicit impl(std::function<void(forge::api::core::installer)> install) {
      install(forge::api::core::installer{apis});
      http::router routes;
      const auto methods = forge::api::core::binding().serve(apis).build();
      routes.mount(forge::api::http::binding().use(methods).bind<api::info>().build());
      routes.mount(forge::api::http::binding().use(methods).bind<api::transaction>().build());
      submission.mount(forge::api::http::binding().use(methods).bind<api::submission>().build());
      routes.post_stream("/v1/chain/transactions/submit",
                         [this](http::stream_request& request) { return handle_submission(request); });
      listener = std::make_unique<http::server>(runtime, http::server_config{}, std::move(routes));
      listener->start();
   }

   boost::asio::awaitable<http::stream_response> handle_submission(http::stream_request& request) {
      http::stream_response response;
      if (submission.can_handle_stream(request.context)) {
         response = co_await submission.handle_stream(request);
      } else {
         auto buffered = request.context.request;
         buffered.body() = co_await request.body.async_read_all();
         auto context = http::make_route_context(buffered);
         context.runtime = request.context.runtime;
         response = http::stream_response::buffered(co_await submission.handle(context));
      }

      if (drop_response && response.head.result() == http::status::accepted) {
         // The typed handler accepted this signed packet. Close the connection
         // before the HTTP server writes the acknowledgement on this socket.
         response.send_guard = [this, original = std::move(response.send_guard)](
                                   boost::asio::awaitable<void> send,
                                   std::function<void()> close) -> boost::asio::awaitable<void> {
            ++disconnected;
            close();
            if (original)
               co_await original(std::move(send), std::move(close));
            else
               co_await std::move(send);
         };
      }
      co_return std::move(response);
   }
};

counter_http_server::counter_http_server(std::function<void(forge::api::core::installer)> install)
    : state_(std::make_unique<impl>(std::move(install))) {}

counter_http_server::~counter_http_server() = default;

std::string counter_http_server::url() const {
   return "http://127.0.0.1:" + std::to_string(state_->listener->port());
}

unsigned counter_http_server::disconnected() const {
   return state_->disconnected.load();
}
void counter_http_server::allow_responses() {
   state_->drop_response = false;
}
void counter_http_server::stop() {
   state_->listener->stop();
}

} // namespace demo
