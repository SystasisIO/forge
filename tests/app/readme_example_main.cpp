#include "readme_example.hxx"

int main(int argc, char** argv) {
   return forge::app::run_daemon(service_example::make_application, argc, argv, service_example::daemon_options());
}
