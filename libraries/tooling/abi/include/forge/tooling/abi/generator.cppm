module;

#include <filesystem>
#include <string>
#include <vector>

export module forge.tooling.abi.generator;

export namespace forge::tooling::abi {

struct library_compilation {
   std::string owner;
   std::filesystem::path object_list;
};

struct known_module {
   std::string name;
   std::string owner;
};

struct request {
   std::string contract;
   std::filesystem::path abi;
   std::filesystem::path dispatcher;
   std::filesystem::path depfile;
   std::filesystem::path attribute_plugin;
   std::filesystem::path sysroot;
   std::filesystem::path ricardian_contracts;
   std::filesystem::path ricardian_clauses;
   std::vector<std::filesystem::path> module_paths;
   std::vector<std::filesystem::path> sdk_include_paths;
   std::vector<std::filesystem::path> include_paths;
   std::vector<std::filesystem::path> sources;
   std::vector<std::filesystem::path> dependency_sources;
   std::vector<library_compilation> library_compilations;
   std::vector<known_module> known_modules;
   std::vector<std::filesystem::path> source_wrappers;
   std::vector<std::string> compiler_arguments;
   std::vector<std::string> abi_root_types;
   std::filesystem::path metadata;
};

struct artifacts {
   std::filesystem::path abi;
   std::filesystem::path dispatcher;
   std::vector<std::filesystem::path> source_wrappers;
   std::filesystem::path metadata;
};

artifacts generate(const request& options);

} // namespace forge::tooling::abi
