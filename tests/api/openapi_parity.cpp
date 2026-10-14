#include "openapi_parity_fixture.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc, char** argv) {
   if (argc != 3 || std::string_view{argv[1]} != "--output" || std::string_view{argv[2]}.empty()) {
      std::cerr << "usage: openapi_parity --output <directory>\n";
      return 2;
   }
   try {
      const auto document = forge::tests::openapi_parity::make_document();
      auto cases = forge::variants{};
      for (const auto& entry : forge::tests::openapi_parity::make_cases()) {
         if (!document["components"]["schemas"].get_object().contains(entry.schema)) {
            throw std::runtime_error{"missing generated schema for: " + entry.name};
         }
         cases.push_back(forge::tests::openapi_parity::evaluate_case(entry));
      }
      const auto output = std::filesystem::path{argv[2]};
      std::filesystem::create_directories(output);
      const auto schema_result = forge::codec::json::save_value(output / "openapi.json", document, {.pretty = true});
      const auto cases_result = forge::codec::json::save_value(
          output / "cases.json", forge::variant{cases}, {.pretty = true});
      if (!schema_result.ok() || !cases_result.ok()) {
         throw std::runtime_error{"failed to write OpenAPI parity fixtures"};
      }
      std::cout << "exported " << cases.size() << " cases and "
                << forge::tests::openapi_parity::schema_names.size() << " generated schemas\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
}
