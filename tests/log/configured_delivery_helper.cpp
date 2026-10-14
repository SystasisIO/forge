#include <forge/log/macros.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

import forge.asio.blocking;
import forge.asio.runtime;
import forge.log.logger;
import forge.log.logger_config;
import forge.net.http.router;
import forge.net.http.server;
import forge.variant.value;

namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace wire = boost::beast::http;
using namespace std::chrono_literals;

asio::awaitable<bool> probe(unsigned short port) {
   auto stream = beast::tcp_stream{co_await asio::this_coro::executor};
   stream.expires_after(2s);
   co_await stream.async_connect({asio::ip::make_address("127.0.0.1"), port}, asio::use_awaitable);
   auto request = wire::request<wire::empty_body>{wire::verb::get, "/probe", 11};
   request.set(wire::field::host, "localhost");
   co_await wire::async_write(stream, request, asio::use_awaitable);
   auto buffer = beast::flat_buffer{};
   auto response = wire::response<wire::string_body>{};
   co_await wire::async_read(stream, buffer, response, asio::use_awaitable);
   co_return response.result() == wire::status::not_found;
}

int blocked_output() {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto server = forge::net::http::server{runtime, {}, forge::net::http::router{}};
   server.start();
   const auto message = std::string(1024, 'x');
   for (int index = 0; index < 20000; ++index) {
      ilog("blocked-pipe", message);
   }
   for (int index = 0; index < 16; ++index) {
      if (!forge::asio::blocking::run(runtime, probe(server.port()))) {
         return 2;
      }
   }
   const auto before = forge::logging_delivery_metrics();
   if (before.dropped == 0 || before.queued_records == 0 || before.queued_bytes > 8 * 1024 * 1024) {
      return 3;
   }
   if (forge::asio::blocking::run(runtime, forge::async_flush_logging(10ms))) {
      return 4;
   }
   server.stop();
   std::cout << "HTTP_PROGRESS_WITH_BLOCKED_STDERR\n" << std::flush;
   std::string signal;
   std::getline(std::cin, signal);
   if (signal != "drain") {
      return 5;
   }
   if (!forge::asio::blocking::run(runtime, forge::async_shutdown_logging(10s))) {
      return 6;
   }
   const auto after = forge::logging_delivery_metrics();
   if (!after.closed || after.queued_records != 0 || after.accepted != after.delivered + after.failed) {
      return 7;
   }
   ilog("late record is dropped");
   if (forge::configure_logging(forge::logging_config::default_config())) {
      return 8;
   }
   std::cout << "DRAINED\n" << std::flush;
   return 0;
}

int retention(const std::filesystem::path& sentinel) {
   { auto file = std::ofstream{sentinel}; file << "preserved"; }
   auto held = std::vector<forge::logger>{};
   auto config = forge::logging_config{};
   config.sinks.push_back({.name = "output", .type = "console"});
   auto route = forge::logger_config{"default"};
   route.sinks = {"output"};
   config.loggers.push_back(route);
   auto reached_limit = false;
   for (int index = 0; index < 1100; ++index) {
      if (!forge::configure_logging(config)) {
         reached_limit = true;
         break;
      }
      held.push_back(forge::logger::get());
   }
   if (!reached_limit || held.empty()) {
      return 10;
   }
   config.sinks.front().type = "file";
   config.sinks.front().args = forge::mutable_variant_object{}("path", sentinel.string())("append", false);
   if (forge::configure_logging(config)) {
      return 11;
   }
   auto input = std::ifstream{sentinel};
   const auto content = std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
   if (content != "preserved") {
      return 12;
   }
   held.clear();
   if (!forge::configure_logging(config)) {
      return 13;
   }
   auto runtime = forge::asio::runtime{};
   return forge::asio::blocking::run(runtime, forge::async_shutdown_logging(2s)) ? 0 : 14;
}

int failed_output() {
   auto config = forge::logging_config{};
   config.sinks.push_back({.name = "broken", .type = "console",
       .args = forge::mutable_variant_object{}("stream", "std_out")});
   auto route = forge::logger_config{"default"};
   route.sinks = {"broken"};
   route.level = forge::log_level::info;
   config.loggers.push_back(route);
   if (!forge::configure_logging(config)) {
      return 30;
   }
   std::cout.setstate(std::ios::badbit);
   ilog("output failure must not count as delivery");
   auto runtime = forge::asio::runtime{};
   if (!forge::asio::blocking::run(runtime, forge::async_shutdown_logging(2s))) {
      return 31;
   }
   const auto metrics = forge::logging_delivery_metrics();
   return metrics.accepted == 1 && metrics.failed == 1 && metrics.delivered == 0 ? 0 : 32;
}

#if !defined(_WIN32)
int configure_during_shutdown(const std::filesystem::path& sentinel,
                              const std::filesystem::path& fifo) {
   auto runtime = forge::asio::runtime{};
   if (!forge::configure_logging({})) {
      return 20;
   }
   auto config = forge::logging_config{};
   config.sinks.push_back({.name = "marker", .type = "file",
       .args = forge::mutable_variant_object{}("path", sentinel.string())("append", false)});
   config.sinks.push_back({.name = "blocked-open", .type = "file",
       .args = forge::mutable_variant_object{}("path", fifo.string())});
   auto completed = std::promise<bool>{};
   auto result = completed.get_future();
   auto writer = std::jthread{[&] { completed.set_value(forge::configure_logging(config)); }};
   // Truncation of the first output proves preparation was admitted. Opening
   // the second output cannot finish until this fixture opens the FIFO reader.
   const auto deadline = std::chrono::steady_clock::now() + 2s;
   while (std::filesystem::file_size(sentinel) != 0 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(1ms);
   }
   const auto entered = std::filesystem::file_size(sentinel) == 0;
   const auto stopped_early = forge::asio::blocking::run(runtime, forge::async_shutdown_logging(20ms));
   const auto preparation_pending = result.wait_for(0ms) == std::future_status::timeout;
   const auto reader = ::open(fifo.c_str(), O_RDONLY | O_NONBLOCK);
   if (reader < 0) {
      std::quick_exit(21); // The parent bounds a failed blocking-I/O fixture.
   }
   writer.join();
   ::close(reader);
   if (!entered || (stopped_early && preparation_pending) || result.get()) {
      return 22;
   }
   if (!forge::asio::blocking::run(runtime, forge::async_shutdown_logging(2s))) {
      return 23;
   }
   { auto file = std::ofstream{sentinel}; file << "closed-owner"; }
   config.sinks.pop_back();
   if (forge::configure_logging(config) || std::filesystem::file_size(sentinel) != 12) {
      return 24;
   }
   return 0;
}
#endif
} // namespace

int main(int argc, char** argv) {
   if (argc == 2 && std::string{argv[1]} == "failed-output") {
      return failed_output();
   }
#if !defined(_WIN32)
   if (argc == 4 && std::string{argv[1]} == "configure-shutdown") {
      return configure_during_shutdown(argv[2], argv[3]);
   }
#endif
   if (argc == 3 && std::string{argv[1]} == "retention") {
      return retention(argv[2]);
   }
   return blocked_output();
}
