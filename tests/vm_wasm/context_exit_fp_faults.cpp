#include <cfenv>

#pragma STDC FENV_ACCESS ON

namespace {
thread_local unsigned fail_get = 0;
thread_local unsigned fail_restore = 0;
thread_local unsigned denials = 0;

bool consume(unsigned& value) {
   return value && --value == 0;
}
} // namespace

extern "C" void context_exit_probe_fault(unsigned enter, unsigned restore) {
   fail_get = enter;
   fail_restore = restore;
   denials = 0;
}

extern "C" unsigned context_exit_probe_denials() {
   return denials;
}

extern "C" int context_exit_probe_fegetenv(fenv_t* env) {
   if (consume(fail_get)) {
      ++denials;
      return -1;
   }
   return fegetenv(env);
}

extern "C" int context_exit_probe_fesetenv(const fenv_t* env) {
   const int result = fesetenv(env);
   if (env != FE_DFL_ENV && consume(fail_restore)) {
      ++denials;
      // Report failure only after real restoration; do not corrupt the runner.
      return -1;
   }
   return result;
}

extern "C" int context_exit_probe_fesetround(int mode) {
   return fesetround(mode);
}
