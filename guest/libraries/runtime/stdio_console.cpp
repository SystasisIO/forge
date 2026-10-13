#include "details/stdio_console.hxx"

namespace {

unsigned char stdout_buffer[1]{};
unsigned char stderr_buffer[1]{};

size_t console_write(FILE* file, const unsigned char* source, size_t size) {
   const auto buffered = static_cast<size_t>(file->wpos - file->wbase);
   if (buffered != 0) {
      prints_l(reinterpret_cast<const char*>(file->wbase), static_cast<uint32_t>(buffered));
   }
   if (size != 0) {
      prints_l(reinterpret_cast<const char*>(source), static_cast<uint32_t>(size));
   }
   file->wpos = file->wbase = file->buf;
   return size;
}

// A permanent marker lets canonical vfprintf restore its temporary buffer and
// flush before returning. There are no descriptors or filesystem operations.
FILE stdout_file{.write = console_write, .buf = stdout_buffer, .lbf = EOF, .lock = -1};
FILE stderr_file{.write = console_write, .buf = stderr_buffer, .lbf = EOF, .lock = -1};

} // namespace

extern "C" {
FILE* const stdout = &stdout_file;
FILE* const stderr = &stderr_file;
}
