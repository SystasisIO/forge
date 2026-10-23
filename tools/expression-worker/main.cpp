#include "details/run_worker.hxx"
#include "details/backend.hxx"

#include <csignal>
#include <sys/resource.h>
#include <unistd.h>

// The final Mach-O initializer order is checked after linking. This must run
// before upstream C++ constructors can emit fatal or mutex diagnostics.
extern "C" __attribute__((constructor, visibility("hidden"))) void forge_expression_initialize_diagnostics() noexcept {
   constexpr rlim_t stack_bytes = 8U * 1024U * 1024U;
   const rlimit stack{stack_bytes, stack_bytes};
   const rlimit core{0, 0};
   if (::setrlimit(RLIMIT_STACK, &stack) != 0 || ::setrlimit(RLIMIT_CORE, &core) != 0 ||
       std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
      ::_exit(70);
   }
   forge::expression::detail::backend::program::initialize_diagnostics();
}

int main() {
   ::_exit(forge::expression::detail::run_worker());
}
