#include "details/password_wait.hxx"

#include <forge/exceptions/macros.hpp>

#include <cerrno>
#include <array>
#include <fcntl.h>
#include <functional>
#include <optional>
#include <poll.h>
#include <stop_token>
#include <unistd.h>

import forge.crypto.keystore.exceptions;

namespace forge::crypto::keystore {

password_wait::password_wait(std::stop_token stop)
  : _stop(stop)
{
   require_running();
   if (!_stop.stop_possible())
   {
      return;
   }

   auto descriptors = std::array<int, 2>{-1, -1};
   if (::pipe(descriptors.data()) != 0)
   {
      FORGE_THROW_EXCEPTION(exceptions::password_unavailable, "Cannot prepare cancellable password input");
   }

   _reader = descriptors[0];
   _writer = descriptors[1];
   try
   {
      for (const auto descriptor : {_reader, _writer})
      {
         if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0 ||
             ::fcntl(descriptor, F_SETFL, O_NONBLOCK) != 0)
         {
            FORGE_THROW_EXCEPTION(exceptions::password_unavailable, "Cannot protect password cancellation channel");
         }
      }

      _wake.emplace(_stop,
                    [writer = _writer]() noexcept
                    {
                       constexpr auto marker = char{'c'};
                       while (::write(writer, &marker, 1) < 0 && errno == EINTR)
                       {
                       }
                    });
   }
   catch (...)
   {
      ::close(_reader);
      ::close(_writer);
      throw;
   }
}

password_wait::~password_wait()
{
   // Removing the callback joins a concurrent stop notification before closing its file descriptors.
   _wake.reset();
   if (_reader >= 0)
   {
      ::close(_reader);
      ::close(_writer);
   }
}

void password_wait::require_running() const
{
   if (_stop.stop_requested())
   {
      FORGE_THROW_EXCEPTION(exceptions::password_cancelled, "Password input was cancelled");
   }
}

void password_wait::ready(int descriptor) const
{
   require_running();
   if (_reader < 0)
   {
      return;
   }

   for (;;)
   {
      auto descriptors = std::array<pollfd, 2>{{{descriptor, POLLIN, 0}, {_reader, POLLIN, 0}}};
      const auto result = ::poll(descriptors.data(), descriptors.size(), -1);
      require_running();
      if (result < 0 && errno == EINTR)
      {
         continue;
      }

      if (result < 0 || (descriptors[0].revents & POLLNVAL) != 0)
      {
         FORGE_THROW_EXCEPTION(exceptions::password_unavailable, "Cannot wait for password input");
      }

      if ((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
      {
         return;
      }
   }
}

} // namespace forge::crypto::keystore
