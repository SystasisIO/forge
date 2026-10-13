#include "details/stdio_lock.hxx"

extern "C" {

// A guest invocation has one thread. Preserve ownership across nested calls:
// only the first acquisition requests an unlock; lock=-1 is an unlocked FILE.
int __lockfile(FILE* file) {
   if (file->lock != 0) {
      return 0;
   }
   file->lock = 1;
   return 1;
}

void __unlockfile(FILE* file) {
   file->lock = 0;
}
}
