#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

static bool read_all(void* output, std::size_t size) {
   auto* cursor = static_cast<char*>(output);
   while (size) {
      const auto count = ::read(0, cursor, size);
      if (count <= 0)
         return false;
      cursor += count;
      size -= count;
   }

   return true;
}

int main(int, char** argv) {
   auto header = std::array<std::uint8_t, 5>{};
   if (!read_all(header.data(), header.size()))
      return 1;
   auto size = std::uint32_t{};
   for (int n = 1; n < 5; ++n)
      size = (size << 8) | header[n];
   if (size > 2097152)
      return 2;
   auto config = std::vector<std::uint8_t>(size);
   if (!read_all(config.data(), config.size()))
      return 3;
   const auto mode = std::filesystem::path{argv[0]}.filename().string();
   const auto marker_path = std::string{argv[0]} + ".started";
   if (const auto marker = ::open(marker_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600); marker >= 0) {
      ::write(marker, "1", 1);
      ::close(marker);
   }
   if (mode == "hang") {
      for (;;)
         ::pause();
   }

   if (mode == "crash")
      return 9;
   if (mode == "oversize") {
      auto frame = std::array<std::uint8_t, 5>{128, 255, 255, 255, 255};
      ::write(0, frame.data(), frame.size());
   } else if (mode == "truncated") {
      auto frame = std::array<std::uint8_t, 7>{128, 0, 0, 0, 4, 0, 0};
      ::write(0, frame.data(), frame.size());
   } else if (mode == "flood") {
      auto frame = std::array<std::uint8_t, 9>{17, 0, 0, 0, 4, 0, 0, 0, 8};
      for (unsigned n = 0; n < 256; ++n)
         ::write(0, frame.data(), frame.size());
      for (;;)
         ::pause();
   } else if (mode == "read") {
      auto frame = std::array<std::uint8_t, 9>{17, 0, 0, 0, 4, 0, 0, 0, 8};
      ::write(0, frame.data(), frame.size());
      for (;;)
         ::pause();
   } else {
      auto frame = std::array<std::uint8_t, 5>{255, 0, 0, 0, 0};
      ::write(0, frame.data(), frame.size());
   }

   return 0;
}
