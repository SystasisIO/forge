#include "fsync_failure.hxx"

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <stdexcept>

namespace {
std::atomic<unsigned> phase = 0;
std::atomic<dev_t> directory_device = 0;
std::atomic<ino_t> directory_inode = 0;
} // namespace

extern "C" int fsync(int descriptor) {
   static const auto original = reinterpret_cast<int (*)(int)>(::dlsym(RTLD_NEXT, "fsync"));
   if (!original) {
      errno = EIO;
      return -1;
   }
   struct stat file{};
   const bool inspected = ::fstat(descriptor, &file) == 0;
   if (inspected && S_ISDIR(file.st_mode) && file.st_dev == directory_device.load() &&
       file.st_ino == directory_inode.load()) {
      unsigned expected = 2;
      if (phase.compare_exchange_strong(expected, 3)) {
         errno = EIO;
         return -1;
      }
   }
   const auto result = original(descriptor);
   if (result == 0 && inspected && S_ISREG(file.st_mode)) {
      unsigned expected = 1;
      phase.compare_exchange_strong(expected, 2);
   }
   return result;
}

namespace forge::tests::wallet {

void fail_commit_sync(const std::filesystem::path& directory) {
   struct stat file{};
   if (::stat(directory.c_str(), &file) != 0 || !S_ISDIR(file.st_mode)) {
      throw std::runtime_error{"Test directory does not exist"};
   }
   directory_device.store(file.st_dev);
   directory_inode.store(file.st_ino);
   phase.store(1);
}

bool commit_sync_failed() {
   return phase.load() == 3;
}
void clear_sync_failure() {
   phase.store(0);
}

} // namespace forge::tests::wallet
