module;

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

export module forge.app.daemon;

export import forge.app.application_environment;
import forge.app.application_shell;
import forge.app.runner;

export namespace forge::app {

struct daemon_context {
   std::string name;
   application_environment environment;
   std::filesystem::path config_path;
   std::filesystem::path dotenv_path;
   application_shell_options shell;
};

struct daemon_options {
   std::string name;
   std::string display_name;
   std::string default_data_dir_name;
   std::string env_prefix;

   std::string config_filename = "config.yml";
   std::string dotenv_filename = ".env";
   std::string default_profile = "dev_local";

   bool read_yaml = true;
   bool read_dotenv = true;
   bool read_process_env = true;
   bool read_cli = true;
   bool allow_configure = true;
   bool allow_check_config = true;
   bool allow_print_effective_config = true;

   run_options run = {};
};

using daemon_factory = std::function<std::unique_ptr<application_shell>(const daemon_context&)>;

int run_daemon(daemon_factory make_app, int argc, char** argv, daemon_options options);

} // namespace forge::app
