module;

#include <atomic>
#include <csignal>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/system/error_code.hpp>
#include <forge/exceptions/macros.hpp>

module forge.asio.child_signal_registration;

#include "details/child_signal_registration_impl.hxx"

namespace forge::asio {

#if defined(__APPLE__)
// Cold registration bookkeeping only. Asio owns native signal delivery and
// process-wide fanout; this mutex never protects child waits, I/O, or callbacks.
std::mutex child_signal_registration::impl::mutex;
std::size_t child_signal_registration::impl::registrations = 0;
std::optional<struct sigaction> child_signal_registration::impl::disposition;

bool child_signal_registration::impl::same_disposition(const struct sigaction& current) noexcept {
   if (!disposition || current.sa_flags != disposition->sa_flags)
      return false;
   if ((current.sa_flags & SA_SIGINFO) != 0) {
      if (current.sa_sigaction != disposition->sa_sigaction)
         return false;
   } else if (current.sa_handler != disposition->sa_handler) {
      return false;
   }
   for (auto signal = 1; signal < NSIG; ++signal)
      if (sigismember(&current.sa_mask, signal) != sigismember(&disposition->sa_mask, signal))
         return false;
   return true;
}

bool child_signal_registration::impl::cold_default(const struct sigaction& current) noexcept {
   if (current.sa_flags != 0 || current.sa_handler != SIG_DFL)
      return false;
   for (auto signal = 1; signal < NSIG; ++signal)
      if (sigismember(&current.sa_mask, signal) != 0)
         return false;
   return true;
}
#endif

void child_signal_registration::impl::acquire(boost::asio::any_io_executor executor, void (*after_install)()) {
   if (!executor)
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "child signal registration requires an executor");
#if defined(__APPLE__)
   const auto lock = std::scoped_lock{mutex};
   if (owner)
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "child signal registration is already acquired");
   struct sigaction current{};
   if (::sigaction(SIGCHLD, nullptr, &current) != 0 ||
       (registrations == 0 ? !cold_default(current) : !same_disposition(current)))
      FORGE_THROW_EXCEPTION(exceptions::rejected, "incompatible existing child signal ownership");
   if (registrations == std::numeric_limits<std::size_t>::max())
      FORGE_THROW_EXCEPTION(exceptions::rejected, "child signal registration capacity exhausted");
   // Validate first: constructing a signal_set may initialize Asio's signal
   // service, so even empty native ownership is not created for a foreign owner.
   auto next = std::make_unique<boost::asio::signal_set>(std::move(executor));
   auto error = boost::system::error_code{};
   next->add(SIGCHLD, error);
   if (error)
      FORGE_THROW_EXCEPTION(exceptions::internal, "child signal registration failed",
                            forge::exceptions::ctx("native_code", error.value()));
   owner = std::move(next);
   ++registrations;
   active.store(true, std::memory_order_release);
   // No throwing allocation occurs between native commit and retained ownership.
   struct sigaction installed{};
   if (::sigaction(SIGCHLD, nullptr, &installed) != 0)
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "child signal registration could not be verified");
   const auto native_handler = (installed.sa_flags & (SA_SIGINFO | SA_NOCLDWAIT)) == 0 &&
                               installed.sa_handler != SIG_DFL && installed.sa_handler != SIG_IGN;
   if (registrations == 1 && native_handler)
      disposition = installed;
   if (after_install)
      after_install(); // Private deterministic failure seam; never stored.
   struct sigaction confirmed{};
   if (!native_handler || ::sigaction(SIGCHLD, nullptr, &confirmed) != 0 || !same_disposition(confirmed))
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "child signal registration changed during initialization");
#else
   static_cast<void>(executor);
   static_cast<void>(after_install);
   FORGE_THROW_EXCEPTION(exceptions::rejected, "native child signal registration is not qualified on this platform");
#endif
}

void child_signal_registration::impl::release() {
#if defined(__APPLE__)
   const auto lock = std::scoped_lock{mutex};
   if (!owner)
      return;
   struct sigaction current{};
   if (registrations == 0 || ::sigaction(SIGCHLD, nullptr, &current) != 0 || !same_disposition(current))
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "child signal registration no longer has confirmed ownership");
   auto error = boost::system::error_code{};
   owner->clear(error);
   if (error)
      FORGE_THROW_EXCEPTION(exceptions::internal, "child signal registration could not be released",
                            forge::exceptions::ctx("native_code", error.value()));
   --registrations;
   owner.reset(); // Empty registration cannot overwrite a subsequent disposition.
   active.store(false, std::memory_order_release);
   if (registrations == 0)
      disposition.reset();
#endif
}

bool child_signal_registration::impl::registered() const noexcept {
   return active.load(std::memory_order_acquire);
}

} // namespace forge::asio
