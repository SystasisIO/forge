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

static bool write_all(const void* input, std::size_t size) {
   auto* cursor = static_cast<const char*>(input);
   while (size) {
      const auto count = ::write(0, cursor, size);
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
   if (mode.starts_with("diagnostic-")) {
      // Host bridge diagnostics only; this worker authenticates no TLS transcript.
      const auto server = mode.ends_with("-server");
      auto kind = std::uint8_t{};
      auto payload = std::vector<std::uint8_t>{};
      if (mode.starts_with("diagnostic-read-")) {
         kind = server ? 18 : 17;
         payload = {0, 0, 0, 8};
      } else if (mode.starts_with("diagnostic-write-")) {
         kind = server ? 2 : 1;
         payload = {'x'};
      } else if (mode.starts_with("diagnostic-close-")) {
         kind = server ? 4 : 3;
      } else {
         return 6;
      }
      header = {kind, 0, 0, 0, static_cast<std::uint8_t>(payload.size())};
      if (!write_all(header.data(), header.size()) || !write_all(payload.data(), payload.size()))
         return 7;
      for (;;)
         ::pause();
   }
   if (mode == "success-queued") {
      // Host ownership/drain fixture only. This worker performs no TLSNotary
      // operation and provides no evidence about cryptographic authentication.
      const auto queued = std::array<std::uint8_t, 9>{1, 0, 0, 0, 4, 't', 'a', 'i', 'l'};
      if (!write_all(queued.data(), queued.size()) || !read_all(header.data(), header.size()) ||
          header != std::array<std::uint8_t, 5>{6, 0, 0, 0, 0})
         return 4;
      auto payload = std::vector<std::uint8_t>{};
      const auto put = [&](std::uint32_t value) {
         for (auto shift = 24; shift >= 0; shift -= 8)
            payload.push_back(static_cast<std::uint8_t>(value >> shift));
      };
      put(0);
      payload.push_back(1);
      const auto name = std::string{"tlsn.local"};
      put(name.size());
      payload.insert(payload.end(), name.begin(), name.end());
      for (auto index = 0; index != 4; ++index)
         put(0);
      header = {128, 0, 0, 0, static_cast<std::uint8_t>(payload.size())};
      return write_all(header.data(), header.size()) && write_all(payload.data(), payload.size()) ? 0 : 5;
   }
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
