#include "details/stdio_lock.hxx"

int nested_lock(FILE* file) {
   FLOCK(file);
   FUNLOCK(file);
   return file->lock;
}

int main() {
   auto file = FILE{};
   if (__lockfile(&file) != 1 || file.lock != 1) return 1;
   if (__lockfile(&file) != 0 || file.lock != 1) return 2;
   __unlockfile(&file);
   if (file.lock != 0 || __lockfile(&file) != 1) return 3;
   __unlockfile(&file);
   file.lock = -1;
   if (__lockfile(&file) != 0 || file.lock != -1) return 4;
   file.lock = 0;
   {
      FLOCK(&file);
      if (file.lock != 1 || nested_lock(&file) != 1) return 5;
      FUNLOCK(&file);
   }
   if (file.lock != 0) return 6;
   if (nested_lock(&file) != 0) return 7;
   file.lock = -1;
   if (nested_lock(&file) != -1) return 8;
   return 0;
}
