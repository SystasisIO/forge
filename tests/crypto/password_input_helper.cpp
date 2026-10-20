#include <cerrno>
#include <charconv>
#include <iostream>
#include <filesystem>
#include <stop_token>
#include <string>
#include <thread>
#include <unistd.h>

import forge.crypto.keystore.password;

int main(int argc, char** argv)
{
   if (argc != 3 && argc != 4)
   {
      return 2;
   }

   auto control = int{-1};
   const auto argument = std::string{argv[2]};
   const auto converted = std::from_chars(argument.data(), argument.data() + argument.size(), control);
   if (converted.ec != std::errc{} || converted.ptr != argument.data() + argument.size() || control < 0)
   {
      return 2;
   }

   auto stop = std::stop_source{};
   auto observer = std::jthread{
       [&stop, control]
       {
          auto command = char{};
          auto count = ssize_t{};
          do
          {
             count = ::read(control, &command, 1);
          } while (count < 0 && errno == EINTR);
          if (count == 1 && command == 'c')
          {
             stop.request_stop();
          }

          ::close(control);
       }};
   try
   {
      const auto password = forge::crypto::keystore::read_password(
          {.source = std::string{argv[1]} == "stdin" ? forge::crypto::keystore::password_source::standard_input
                     : std::string{argv[1]} == "file" ? forge::crypto::keystore::password_source::file
                                                      : forge::crypto::keystore::password_source::terminal,
           .file = argc == 4 ? std::filesystem::path{argv[3]} : std::filesystem::path{},
           .prompt = "Secret: ",
           .max_bytes = 64,
           .stop_token = stop.get_token()});
      if (password.view() != "fixture-password")
      {
         return 1;
      }

      std::cout << "received\n";
      return 0;
   }
   catch (const forge::crypto::keystore::exceptions::password_cancelled&)
   {
      std::cerr << "cancelled\n";
      return 130;
   }
   catch (const forge::crypto::keystore::exceptions::size_limit_exceeded&)
   {
      std::cerr << "size_limit\n";
      return 3;
   }
   catch (const forge::crypto::keystore::exceptions::password_unavailable&)
   {
      std::cerr << "unavailable\n";
      return 4;
   }
   catch (...)
   {
      std::cerr << "unexpected\n";
      return 1;
   }
}
