module;

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

module forge.tooling.abi.command;

import forge.tooling.abi.generator;

namespace forge::tooling::abi::command {
namespace {

request parse(int argc, const char* const* argv) {
   auto result = request{};
   constexpr auto library_compilation_prefix = std::string_view{"--library-compilation="};
   constexpr auto module_path_prefix = std::string_view{"--module-path="};
   constexpr auto include_prefix = std::string_view{"--include="};
   constexpr auto compiler_argument_prefix = std::string_view{"--compiler-argument="};
   for (auto index = 1; index < argc; ++index) {
      const auto option = std::string_view{argv[index]};
      const auto next = [&]() -> std::string_view {
         if (++index >= argc) {
            throw std::runtime_error{"missing value after " + std::string{option}};
         }
         return argv[index];
      };
      if (option == "--contract") {
         result.contract = next();
      } else if (option == "--abi") {
         result.abi = next();
      } else if (option == "--metadata") {
         result.metadata = next();
      } else if (option == "--abi-root") {
         result.abi_root_types.emplace_back(next());
      } else if (option == "--dispatch") {
         result.dispatcher = next();
      } else if (option == "--depfile") {
         result.depfile = next();
      } else if (option == "--attribute-plugin") {
         result.attribute_plugin = next();
      } else if (option == "--sysroot") {
         result.sysroot = next();
      } else if (option == "--ricardian-contracts") {
         result.ricardian_contracts = next();
      } else if (option == "--ricardian-clauses") {
         result.ricardian_clauses = next();
      } else if (option == "--module-path") {
         result.module_paths.emplace_back(next());
      } else if (option == "--sdk-include") {
         result.sdk_include_paths.emplace_back(next());
      } else if (option == "--include") {
         result.include_paths.emplace_back(next());
      } else if (option == "--dependency-source") {
         result.dependency_sources.emplace_back(next());
      } else if (option == "--library-compilation") {
         result.library_compilations.push_back(library_compilation{
             .owner = std::string{next()},
             .object_list = next(),
         });
      } else if (option.starts_with(library_compilation_prefix)) {
         const auto record = option.substr(library_compilation_prefix.size());
         const auto separator = record.find('|');
         if (separator == std::string_view::npos) {
            throw std::runtime_error{"invalid library compilation record"};
         }
         result.library_compilations.push_back(library_compilation{
             .owner = std::string{record.substr(0, separator)},
             .object_list = record.substr(separator + 1),
         });
      } else if (option.starts_with(module_path_prefix)) {
         result.module_paths.emplace_back(option.substr(module_path_prefix.size()));
      } else if (option.starts_with(include_prefix)) {
         result.include_paths.emplace_back(option.substr(include_prefix.size()));
      } else if (option == "--known-module") {
         result.known_modules.push_back(known_module{
             .name = std::string{next()},
             .owner = std::string{next()},
         });
      } else if (option == "--source-wrapper") {
         result.source_wrappers.emplace_back(next());
      } else if (option == "--compiler-argument") {
         result.compiler_arguments.emplace_back(next());
      } else if (option.starts_with(compiler_argument_prefix)) {
         result.compiler_arguments.emplace_back(option.substr(compiler_argument_prefix.size()));
      } else if (option.starts_with("--")) {
         throw std::runtime_error{"unknown argument: " + std::string{option}};
      } else {
         result.sources.emplace_back(option);
      }
   }
   if (result.contract.empty() || result.abi.empty() || result.dispatcher.empty() || result.attribute_plugin.empty() ||
       result.sysroot.empty() || result.sources.empty()) {
      throw std::runtime_error{"--contract, --abi, --dispatch, --attribute-plugin, "
                               "--sysroot and contract sources are required"};
   }
   if (!result.source_wrappers.empty() && result.source_wrappers.size() + 1U != result.sources.size()) {
      throw std::runtime_error{"--source-wrapper must be specified once for every non-dispatch source"};
   }
   return result;
}

} // namespace

int run(int argc, const char* const* argv) {
   try {
      generate(parse(argc, argv));
      return 0;
   } catch (const std::exception& error) {
      std::cerr << "abigen: " << error.what() << '\n';
      return 1;
   }
}

} // namespace forge::tooling::abi::command
