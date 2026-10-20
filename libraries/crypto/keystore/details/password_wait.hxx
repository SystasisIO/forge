#pragma once

#include <functional>
#include <optional>
#include <stop_token>

namespace forge::crypto::keystore {

class password_wait
{
 public:
   explicit password_wait(std::stop_token stop);
   ~password_wait();
   password_wait(const password_wait&) = delete;
   password_wait& operator=(const password_wait&) = delete;

   void ready(int descriptor) const;
   void require_running() const;

 private:
   std::stop_token _stop;
   int _reader = -1;
   int _writer = -1;
   std::optional<std::stop_callback<std::function<void()>>> _wake;
};

} // namespace forge::crypto::keystore
