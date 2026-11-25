module;

#include <functional>
#include <memory>
#include <string>

export module test.api_chain.counter_http;

import forge.api.core.registry;

export namespace demo {

class counter_http_server {
 public:
   explicit counter_http_server(std::function<void(forge::api::core::installer)> install);
   ~counter_http_server();

   std::string url() const;
   unsigned disconnected() const;
   void allow_responses();
   void stop();

 private:
   struct impl;
   std::unique_ptr<impl> state_;
};

} // namespace demo
