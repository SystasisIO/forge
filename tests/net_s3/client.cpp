#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <source_location>
#include <stop_token>
#include <string>
#include <vector>
#include <utility>

import forge.asio.compute;
import forge.net.s3.client;

#include "../../libraries/net/s3/details/backend_call.hxx"

namespace asio = boost::asio;
namespace http = boost::beast::http;
namespace s3 = forge::net::s3;
using tcp = asio::ip::tcp;

void require(bool value, const char* message) {
   if (!value) {
      throw std::runtime_error{message};
   }
}

template <typename Error> void check_error(const Error& error) {
   require(std::string_view{error.code().category().name()} == "forge.net.s3", "S3 error category was lost");
   require(error.code().value() == static_cast<int>(Error::value), "S3 error code does not match its type");
   require(error.location().line() != 0 &&
               std::string_view{error.location().file_name()}.find("net/s3/") != std::string_view::npos,
           "S3 error source location was lost");
}

template <typename Error, typename Work>
asio::awaitable<void> expect(Work work, std::function<void(const Error&)> inspect = {}) {
   try {
      co_await work();
   } catch (const Error& error) {
      check_error(error);
      if (inspect) {
         inspect(error);
      }
      co_return;
   }
   throw std::runtime_error{"expected typed S3 exception was not raised"};
}

void backend_failures() {
   auto observed = false;
   try {
      forge::net::s3::detail::backend_call([] { throw std::runtime_error{"private SDK URL and credentials"}; });
   } catch (const s3::exceptions::service& error) {
      check_error(error);
      require(std::string_view{error.what()}.find("private SDK") == std::string_view::npos,
              "raw backend diagnostics escaped sanitization");
      observed = true;
   }
   require(observed, "unexpected backend read error was not typed");

   observed = false;
   std::atomic<bool> started{false};
   auto side_effects = 0;
   try {
      forge::net::s3::detail::backend_call(
          [&] {
             ++side_effects;
             started.store(true);
             throw std::runtime_error{"private SDK URL and credentials"};
          },
          &started);
   } catch (const s3::exceptions::unknown_outcome& error) {
      check_error(error);
      require(std::string_view{error.what()}.find("private SDK") == std::string_view::npos,
              "unknown mutation leaked raw backend diagnostics");
      observed = true;
   }
   require(observed && side_effects == 1, "unexpected mutation failure was retried or lost uncertainty");

   observed = false;
   started.store(false);
   try {
      forge::net::s3::detail::backend_call([] { throw 7; }, &started);
   } catch (const s3::exceptions::service& error) {
      check_error(error);
      observed = true;
   }
   require(observed, "unexpected failure before dispatch became an unknown mutation");

   observed = false;
   const auto location = std::source_location::current();
   try {
      forge::net::s3::detail::backend_call([&] {
         FORGE_THROW_EXCEPTION(s3::exceptions::denied, "typed fixture failure",
                               forge::exceptions::secret("identity", "test-secret"));
      });
   } catch (const s3::exceptions::denied& error) {
      require(error.location().line() > location.line() &&
                  std::string_view{error.location().file_name()} == location.file_name(),
              "backend boundary replaced the typed error source location");
      require(error.context().size() == 1 && error.context().front().redacted &&
                  std::string_view{error.what()}.find("test-secret") == std::string_view::npos,
              "backend boundary replaced typed redacted context");
      observed = true;
   }
   require(observed, "backend boundary replaced an existing Forge error type");
}

bool frame_has(const forge::exceptions::base& error, std::string_view key, std::string_view value) {
   for (const auto& frame : error.context_frames()) {
      for (const auto& field : frame.context) {
         if (field.key == key && field.value == value) {
            return true;
         }
      }
   }
   return false;
}

struct endpoint : std::enable_shared_from_this<endpoint> {
   tcp::acceptor listener;
   std::map<std::string, std::string> objects;
   std::map<unsigned, std::string> uploads;
   unsigned mutations = 0;
   unsigned aborts = 0;
   unsigned unknowns = 0;
   unsigned signed_requests = 0;
   bool fail_part = false;
   bool fail_abort = false;
   bool lose_completion_response = false;
   bool slow = false;
   bool delay_part = false;
   bool bad_range = false;
   bool part_started = false;
   std::function<void()> on_part;
   std::function<void()> on_request;
   bool huge_control = false;
   bool bad_checksum = false;

   explicit endpoint(asio::io_context& io) : listener{io, {asio::ip::make_address("127.0.0.1"), 0}} {}

   std::string address() {
      return "http://127.0.0.1:" + std::to_string(listener.local_endpoint().port());
   }

   asio::awaitable<void> serve() {
      while (listener.is_open()) {
         boost::system::error_code error;
         auto socket = co_await listener.async_accept(asio::redirect_error(asio::use_awaitable, error));
         if (error) {
            break;
         }
         asio::co_spawn(listener.get_executor(), exchange(shared_from_this(), std::move(socket)), asio::detached);
      }
   }

   static asio::awaitable<void> exchange(std::shared_ptr<endpoint> self, tcp::socket socket) {
      try {
         boost::beast::flat_buffer buffer;
         http::request_parser<http::string_body> parser;
         parser.body_limit(24 * 1024 * 1024);
         co_await http::async_read_header(socket, buffer, parser, asio::use_awaitable);
         if (parser.get()[http::field::expect] == "100-continue") {
            http::response<http::empty_body> interim{http::status::continue_, 11};
            co_await http::async_write(socket, interim, asio::use_awaitable);
         }
         co_await http::async_read(socket, buffer, parser, asio::use_awaitable);
         auto request = parser.release();
         const auto target = std::string{request.target()};
         const auto path = target.substr(0, target.find('?'));
         const auto query = target.find('?') == std::string::npos ? "" : target.substr(target.find('?') + 1);
         if (request[http::field::authorization].starts_with("AWS4-HMAC-SHA256 ")) {
            ++self->signed_requests;
         }
         if (self->on_request) {
            self->on_request();
         }
         if (path == "/bucket/unknown") {
            ++self->unknowns;
            socket.close();
            co_return;
         }
         if (self->slow || (self->delay_part && query.find("partNumber=") != std::string::npos)) {
            self->part_started = true;
            if (self->on_part) {
               self->on_part();
            }
            asio::steady_timer delay{socket.get_executor()};
            delay.expires_after(std::chrono::milliseconds{1500});
            co_await delay.async_wait(asio::use_awaitable);
         }
         http::response<http::string_body> response{http::status::ok, 11};
         response.set(http::field::content_type, "application/octet-stream");
         response.set(http::field::etag, "\"fixture-etag\"");
         response.set("x-amz-version-id", "fixture-version");
         if (self->bad_checksum) {
            response.set("x-amz-checksum-crc32", "AAAAAA==");
         }
         auto size = std::optional<std::size_t>{};
         const auto error = [&](http::status status, const std::string& code) {
            response.result(status);
            response.set(http::field::content_type, "application/xml");
            response.body() = "<Error><Code>" + code + "</Code><Message>fixture</Message></Error>";
         };
         if (query.find("partNumber=") != std::string::npos) {
            if (self->fail_part) {
               error(http::status::internal_server_error, "InternalError");
            } else {
               const auto position = query.find("partNumber=") + 11;
               const auto number = static_cast<unsigned>(std::stoul(query.substr(position)));
               self->uploads[number] = std::move(request.body());
            }
         } else if (query.find("uploads") != std::string::npos && request.method() == http::verb::post) {
            self->uploads.clear();
            response.body() = "<InitiateMultipartUploadResult><Bucket>bucket</Bucket><Key>multipart</Key>"
                              "<UploadId>fixture-upload</UploadId></InitiateMultipartUploadResult>";
         } else if (query.find("uploadId=") != std::string::npos) {
            if (request.method() == http::verb::delete_) {
               ++self->aborts;
               if (self->fail_abort) {
                  error(http::status::internal_server_error, "InternalError");
               } else {
                  self->uploads.clear();
               }
            } else if (request.method() == http::verb::post) {
               std::string combined;
               for (const auto& [number, bytes] : self->uploads) {
                  combined += bytes;
               }
               self->objects[path] = std::move(combined);
               if (self->lose_completion_response) {
                  socket.close();
                  co_return;
               }
               response.body() = "<CompleteMultipartUploadResult><Location>fixture</Location><Bucket>bucket</Bucket>"
                                 "<Key>multipart</Key><ETag>\"fixture-etag\"</ETag></CompleteMultipartUploadResult>";
            } else {
               response.body() =
                   "<ListPartsResult><Bucket>bucket</Bucket><Key>multipart</Key><UploadId>fixture-upload</UploadId>"
                   "<PartNumberMarker>0</PartNumberMarker><IsTruncated>false</IsTruncated>";
               for (const auto& [number, bytes] : self->uploads) {
                  response.body() += "<Part><PartNumber>" + std::to_string(number) +
                                     "</PartNumber><ETag>\"fixture-etag\"</ETag>"
                                     "<Size>" +
                                     std::to_string(bytes.size()) + "</Size></Part>";
               }
               response.body() += "</ListPartsResult>";
            }
         } else if (request.method() == http::verb::put) {
            ++self->mutations;
            if (request[http::field::if_none_match] == "*" && self->objects.contains(path)) {
               error(http::status::precondition_failed, "PreconditionFailed");
            } else {
               self->objects[path] = std::move(request.body());
            }
         } else if (request.method() == http::verb::delete_) {
            self->objects.erase(path);
         } else if (!self->objects.contains(path)) {
            error(http::status::not_found, "NoSuchKey");
         } else if (request.method() == http::verb::head) {
            size = self->objects[path].size();
         } else {
            response.body() = self->objects[path];
            if (!request[http::field::range].empty()) {
               const auto range = std::string{request[http::field::range]};
               const auto dash = range.find('-');
               const auto begin = static_cast<std::size_t>(std::stoull(range.substr(6, dash - 6)));
               const auto end = static_cast<std::size_t>(std::stoull(range.substr(dash + 1)));
               if (end >= response.body().size()) {
                  error(http::status::range_not_satisfiable, "InvalidRange");
               } else {
                  response.result(http::status::partial_content);
                  response.set(http::field::content_range, "bytes " + std::to_string(self->bad_range ? 0 : begin) +
                                                               "-" + std::to_string(end) + "/" +
                                                               std::to_string(response.body().size()));
                  response.body() = response.body().substr(begin, end - begin + 1);
               }
            }
         }
         response.keep_alive(false);
         if (self->huge_control) {
            response.body().assign(2 * 1024 * 1024, 'x');
         }
         response.prepare_payload();
         if (size) {
            response.content_length(*size);
         }
         co_await http::async_write(socket, response, asio::use_awaitable);
      } catch (const boost::system::system_error&) {
      }
   }
};

asio::awaitable<void> exercise(std::shared_ptr<endpoint> server, forge::asio::compute::pool& workers) {
   s3::config config;
   config.endpoint = server->address();
   config.signing_endpoint = "https://download.example.test";
   config.identity = {"test-access", "test-secret", "", {}};
   config.request_timeout = std::chrono::milliseconds{1000};
   config.multipart_threshold = 6 * 1024 * 1024;
   config.part_bytes = 5 * 1024 * 1024;
   auto client = s3::client{workers.get_executor(), config};
   const auto target = s3::object{"bucket", "binary", {}};
   std::vector<std::byte> bytes{std::byte{0}, std::byte{1}, std::byte{255}, std::byte{42}};
   const auto uploaded = co_await client.put(target, bytes);
   require(uploaded.size == bytes.size() && !uploaded.etag.empty(), "put metadata is wrong");
   const auto head = co_await client.head(target);
   require(head.size == bytes.size() && head.version == "fixture-version", "head metadata is wrong");
   require(co_await client.get(target) == bytes, "binary get did not preserve bytes");
   server->bad_checksum = true;
   co_await expect<s3::exceptions::transport>([&] { return client.get(target); });
   server->bad_checksum = false;
   s3::read_options range;
   range.range = s3::byte_range{1, 2};
   const auto ranged = co_await client.get(target, range);
   require(ranged.size() == 2 && ranged[0] == bytes[1] && ranged[1] == bytes[2], "range get is wrong");
   server->bad_range = true;
   co_await expect<s3::exceptions::transport>([&] { return client.get(target, range); });
   server->bad_range = false;
   s3::read_options small;
   small.max_bytes = 2;
   co_await expect<s3::exceptions::limit>([&] { return client.get(target, small); });
   co_await expect<s3::exceptions::conflict>([&] { return client.put(target, bytes, {.if_absent = true}); });
   co_await expect<s3::exceptions::not_found>([&] { return client.head({"bucket", "absent", {}}); });

   auto signed_url = co_await client.presign(target, std::chrono::seconds{60});
   require(signed_url.url.view().starts_with("https://download.example.test/bucket/binary?") &&
               signed_url.url.view().find("X-Amz-Signature=") != std::string_view::npos,
           "presign did not use signing endpoint");
   config.identity.expires = std::chrono::system_clock::now() + std::chrono::seconds{4};
   client.update_credentials(config.identity);
   auto short_url = co_await client.presign(target, std::chrono::seconds{60}, s3::method::put);
   require(short_url.expires <= *config.identity.expires, "presign exceeds credential expiry");
   config.identity.expires.reset();
   client.update_credentials(config.identity);

   const auto directory =
       std::filesystem::temp_directory_path() /
       ("forge-s3-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
   std::filesystem::create_directory(directory);
   struct cleanup {
      std::filesystem::path path;
      ~cleanup() {
         std::error_code ignored;
         std::filesystem::remove_all(path, ignored);
      }
   } cleanup{directory};
   const auto source = directory / "source";
   const auto destination = directory / "download";
   const auto large_size = 6 * 1024 * 1024 + 17;
   {
      std::ofstream file{source, std::ios::binary};
      const std::string block(64 * 1024, 'x');
      for (int index = 0; index != 96; ++index) {
         file.write(block.data(), static_cast<std::streamsize>(block.size()));
      }
      file.write("end-of-file-marker", 17);
   }
   const auto multi_target = s3::object{"bucket", "multipart", {}};
   const auto loop = directory / "loop";
   std::filesystem::create_symlink("loop", loop);
   const auto mutations_before = server->mutations;
   co_await expect<s3::exceptions::io>([&] { return client.put(target, loop); },
                                       [](const auto& error) {
                                          require(error.context().size() == 1 &&
                                                      error.context().front().key == "filesystem_status" &&
                                                      error.context().front().value != "0",
                                                  "filesystem failure was not typed with its numeric status");
                                       });
   require(server->mutations == mutations_before, "invalid file source reached a mutation request");
   const auto multipart = co_await client.put(multi_target, source);
   require(multipart.size == large_size && server->uploads.size() == 2, "automatic multipart is wrong");
   auto read = s3::read_options{};
   read.max_bytes = large_size;
   const auto downloaded = co_await client.get(multi_target, destination, read);
   require(downloaded.size == large_size && std::filesystem::file_size(destination) == large_size,
           "streaming file download is wrong");
   std::ifstream file{destination, std::ios::binary};
   file.seekg(6 * 1024 * 1024);
   std::string marker(17, '\0');
   file.read(marker.data(), 17);
   require(marker == "end-of-file-marke", "multipart source bytes were changed");

   auto session = co_await client.begin({"bucket", "explicit", {}});
   auto first = co_await client.upload(session, 1, source, {0, 5 * 1024 * 1024});
   const auto page = co_await client.parts(session);
   require(page.parts.size() == 1 && page.parts[0].size == first.size, "multipart recovery listing is wrong");
   server->huge_control = true;
   co_await expect<s3::exceptions::limit>([&] { return client.parts(session); });
   server->huge_control = false;
   auto second = co_await client.upload(session, 2, source, {5 * 1024 * 1024, 1024 * 1024 + 17});
   const auto completed = co_await client.complete(session, {first, second});
   require(completed.size == large_size, "explicit multipart completion is wrong");

   server->fail_part = true;
   const auto aborted_before = server->aborts;
   co_await expect<s3::exceptions::unknown_outcome>(
       [&] { return client.put(multi_target, source); },
       [](const auto& error) {
          require(frame_has(error, "upload_id", "fixture-upload") && frame_has(error, "bucket", "bucket") &&
                      frame_has(error, "key", "multipart") && frame_has(error, "cleanup_attempted", "true") &&
                      frame_has(error, "cleanup_confirmed", "true") &&
                      frame_has(error, "completion_attempted", "false"),
                  "multipart failure lost its upload identity or cleanup evidence");
       });
   require(server->aborts == aborted_before + 1, "failed multipart did not attempt cleanup");
   server->fail_abort = true;
   co_await expect<s3::exceptions::unknown_outcome>(
       [&] { return client.put(multi_target, source); },
       [](const auto& error) {
          require(frame_has(error, "upload_id", "fixture-upload") && frame_has(error, "cleanup_attempted", "true") &&
                      frame_has(error, "cleanup_confirmed", "false"),
                  "failed cleanup replaced the original multipart failure or its recovery evidence");
       });
   require(server->aborts == aborted_before + 2, "failed multipart cleanup was retried");
   server->fail_abort = false;
   server->fail_part = false;
   server->lose_completion_response = true;
   co_await expect<s3::exceptions::unknown_outcome>(
       [&] { return client.put(multi_target, source); },
       [](const auto& error) {
          require(frame_has(error, "upload_id", "fixture-upload") && frame_has(error, "cleanup_attempted", "false") &&
                      frame_has(error, "completion_attempted", "true"),
                  "unconfirmed completion lost its reconciliation evidence");
       });
   require(server->aborts == aborted_before + 2 && server->objects["/bucket/multipart"].size() == large_size,
           "client aborted a multipart upload after possible completion");
   server->lose_completion_response = false;
   const auto unknown_before = server->unknowns;
   co_await expect<s3::exceptions::unknown_outcome>([&] { return client.put({"bucket", "unknown", {}}, bytes); });
   require(server->unknowns == unknown_before + 1, "SDK retried an unknown mutation");

   std::stop_source canceled;
   canceled.request_stop();
   co_await expect<s3::exceptions::canceled>(
       [&] { return client.put(target, bytes, {}, {.stop = canceled.get_token()}); });
   co_await expect<s3::exceptions::deadline>(
       [&] { return client.head(target, {.deadline = std::chrono::steady_clock::now() - std::chrono::seconds{1}}); });

   server->delay_part = true;
   server->part_started = false;
   std::stop_source interrupt;
   server->on_part = [&interrupt] { interrupt.request_stop(); };
   co_await expect<s3::exceptions::unknown_outcome>(
       [&] { return client.put(multi_target, source, {}, {.stop = interrupt.get_token()}); });
   require(server->part_started, "cancellation did not interrupt an active part");
   server->on_part = {};
   server->delay_part = false;

   // Admission and shutdown are checked while a real synchronous SDK call is active.
   auto limited_config = config;
   limited_config.max_calls = 1;
   auto limited = s3::client{workers.get_executor(), limited_config};
   const auto executor = co_await asio::this_coro::executor;
   auto arrived = std::make_shared<asio::steady_timer>(executor);
   arrived->expires_at(std::chrono::steady_clock::time_point::max());
   server->slow = true;
   server->on_request = [arrived] { arrived->expires_at(std::chrono::steady_clock::now()); };
   std::exception_ptr canceled_request;
   bool canceled_done = false;
   asio::co_spawn(executor, limited.head(target), [&](std::exception_ptr error, s3::metadata) {
      canceled_request = error;
      canceled_done = true;
   });
   boost::system::error_code ignored;
   co_await arrived->async_wait(asio::redirect_error(asio::use_awaitable, ignored));
   server->on_request = {};
   co_await expect<s3::exceptions::busy>([&] { return limited.head(target); });
   co_await limited.shutdown();
   require(canceled_done && canceled_request != nullptr, "shutdown returned before active SDK call completed");
   try {
      std::rethrow_exception(canceled_request);
   } catch (const s3::exceptions::canceled&) {
   }
   server->slow = false;

   config.identity.expires = std::chrono::system_clock::now() + std::chrono::milliseconds{20};
   client.update_credentials(config.identity);
   asio::steady_timer expired{executor};
   expired.expires_after(std::chrono::milliseconds{30});
   co_await expired.async_wait(asio::use_awaitable);
   const auto signed_before = server->signed_requests;
   co_await expect<s3::exceptions::denied>([&] { return client.head(target); });
   require(server->signed_requests == signed_before, "expired credentials caused a network request");
   config.identity.expires.reset();
   client.update_credentials(config.identity);

   co_await client.erase(target);
   co_await expect<s3::exceptions::not_found>([&] { return client.head(target); });
   require(server->signed_requests != 0, "SDK did not sign fixture requests");
   co_await client.shutdown();
   co_await expect<s3::exceptions::stopped>([&] { return client.head(target); });
   co_await workers.shutdown();
}

int main() {
   backend_failures();
   asio::io_context io;
   auto server = std::make_shared<endpoint>(io);
   forge::asio::compute::pool workers{{.worker_threads = 2, .max_pending_tasks = 4, .max_waiting_submissions = 0}};
   std::exception_ptr error;
   asio::co_spawn(io, server->serve(), asio::detached);
   asio::co_spawn(io, exercise(server, workers), [&](std::exception_ptr value) {
      error = value;
      server->listener.close();
      io.stop();
   });
   io.run();
   if (error) {
      try {
         std::rethrow_exception(error);
      } catch (const std::exception& value) {
         std::cerr << value.what() << '\n';
      }
      return 1;
   }
   std::cout << "S3 SDK loopback protocol tests passed\n";
}
