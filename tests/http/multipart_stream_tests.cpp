#include <boost/asio/awaitable.hpp>
#include <boost/test/unit_test.hpp>
#include <boost/scope/scope_exit.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

import forge.asio.blocking;
import forge.asio.compute;
import forge.asio.exceptions;
import forge.asio.runtime;
import forge.net.http.body;
import forge.net.http.exceptions;
import forge.net.http.file;
import forge.net.http.stream;
import forge.net.http.types;
import forge.net.http.upload;

namespace forge::tests::http_multipart_stream {

class generated_body final : public forge::net::http::body_reader::source
{
 public:
   explicit generated_body(std::uint64_t size, bool fail = false)
     : _size(size)
     , _fail(fail)
   {
   }

   boost::asio::awaitable<std::optional<forge::net::http::body_chunk>> async_read() override
   {
      ++reads;
      if (_fail && _offset != 0)
      {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::unavailable, "Fixture source failed");
      }

      if (_offset == _size)
      {
         co_return std::nullopt;
      }

      const auto count = static_cast<std::size_t>((std::min)(_size - _offset, std::uint64_t{65536}));
      auto result = forge::net::http::body_chunk{.bytes = std::vector<std::byte>(count)};
      for (auto index = std::size_t{}; index != count; ++index)
      {
         result.bytes[index] = static_cast<std::byte>((_offset + index) % 251);
      }

      _offset += count;
      co_return result;
   }

   std::uint64_t bytes_read() const noexcept override
   {
      return _offset;
   }

   void cancel() noexcept override
   {
      cancelled = true;
   }

   std::uint64_t reads = 0;
   bool cancelled = false;

 private:
   std::uint64_t _size;
   bool _fail;
   std::uint64_t _offset = 0;
};

std::uint64_t drain(forge::asio::runtime& runtime, forge::net::http::body_reader& body,
                    std::size_t maximum_chunk = 65536)
{
   return forge::asio::blocking::run(
       runtime,
       [&body, maximum_chunk]() -> boost::asio::awaitable<std::uint64_t>
       {
          auto count = std::uint64_t{};
          while (auto chunk = co_await body.async_read())
          {
             FORGE_ASSERT(chunk->bytes.size() <= maximum_chunk, "Multipart output exceeded its chunk bound");
             count += chunk->bytes.size();
          }

          co_return count;
       }());
}

BOOST_AUTO_TEST_CASE(multipart_stream_is_lazy_and_roundtrips_binary_with_existing_reader)
{
   auto runtime = forge::asio::runtime{};
   auto source = std::make_shared<generated_body>(8193);
   auto empty = std::make_shared<generated_body>(0);
   auto written = forge::net::http::write_multipart_stream(
       {{.name = "binary", .filename = "quoted\"file.bin", .content_type = "application/octet-stream",
         .body = forge::net::http::body_reader{source}},
        {.name = "empty", .body = forge::net::http::body_reader{empty}}},
       {.max_part_bytes = 16384, .max_total_bytes = 32768, .chunk_bytes = 4096});
   BOOST_TEST(source->reads == 0U);
   auto reader = forge::net::http::upload_reader{std::move(written.body),
                                                 {.max_file_bytes = 16384, .max_total_bytes = 32768}};
   const auto form = forge::asio::blocking::run(runtime, reader.async_read_multipart(written.content_type));
   BOOST_REQUIRE_EQUAL(form.files.size(), 1U);
   BOOST_TEST(form.files.front().name == "binary");
   BOOST_TEST(form.files.front().filename.value() == "quoted\"file.bin");
   BOOST_REQUIRE_EQUAL(form.files.front().memory.size(), 8193U);
   for (auto index = std::size_t{}; index != 8193; ++index)
   {
      BOOST_TEST(std::to_integer<unsigned>(form.files.front().memory[index]) == index % 251);
   }

   BOOST_REQUIRE(form.field("empty").has_value());
   BOOST_TEST(form.field("empty")->empty());
}

BOOST_AUTO_TEST_CASE(multipart_stream_large_generated_body_stays_incremental_and_bounded)
{
   auto runtime = forge::asio::runtime{};
   constexpr auto size = std::uint64_t{64} << 20;
   auto source = std::make_shared<generated_body>(size);
   auto written = forge::net::http::write_multipart_stream(
       {{.name = "file", .filename = "large.bin", .body = forge::net::http::body_reader{source}}},
       {.max_part_bytes = size, .max_total_bytes = size + 4096, .chunk_bytes = 4096});
   BOOST_TEST(source->reads == 0U);
   const auto transferred = drain(runtime, written.body, 4096);
   BOOST_TEST(transferred > size);
   BOOST_TEST(transferred <= size + 4096);
   BOOST_TEST(source->bytes_read() == size);
   BOOST_TEST(source->reads <= size / 65536 + 1);
}

BOOST_AUTO_TEST_CASE(multipart_stream_rejects_unsafe_metadata_before_consuming_sources)
{
   for (const auto* name : {"bad\rname", "bad\nname", "bad\x01name"})
   {
      auto source = std::make_shared<generated_body>(10);
      BOOST_CHECK_THROW(static_cast<void>(forge::net::http::write_multipart_stream(
                            {{.name = name, .body = forge::net::http::body_reader{source}}})),
                        forge::net::http::exceptions::bad_request);
      BOOST_TEST(source->reads == 0U);
   }

   auto source = std::make_shared<generated_body>(10);
   BOOST_CHECK_THROW(static_cast<void>(forge::net::http::write_multipart_stream(
                         {{.name = "file", .content_type = "text/plain\r\nInjected: yes",
                           .body = forge::net::http::body_reader{source}}})),
                     forge::net::http::exceptions::bad_request);
   BOOST_TEST(source->reads == 0U);
}

BOOST_AUTO_TEST_CASE(multipart_stream_enforces_part_and_complete_wire_limits)
{
   auto runtime = forge::asio::runtime{};
   auto source = std::make_shared<generated_body>(8193);
   auto part_limited = forge::net::http::write_multipart_stream(
       {{.name = "file", .body = forge::net::http::body_reader{source}}},
       {.max_part_bytes = 8192, .max_total_bytes = 32768});
   BOOST_CHECK_THROW(drain(runtime, part_limited.body), forge::net::http::exceptions::payload_too_large);
   auto wire_limited = forge::net::http::write_multipart_stream(
       {{.name = "file", .body = forge::net::http::body_reader{std::make_shared<generated_body>(8192)}}},
       {.max_part_bytes = 8192, .max_total_bytes = 8192});
   BOOST_CHECK_THROW(drain(runtime, wire_limited.body), forge::net::http::exceptions::payload_too_large);
}

BOOST_AUTO_TEST_CASE(multipart_stream_propagates_source_failure_and_cancels_without_further_reads)
{
   auto runtime = forge::asio::runtime{};
   auto broken = forge::net::http::write_multipart_stream(
       {{.name = "file", .body = forge::net::http::body_reader{std::make_shared<generated_body>(131072, true)}}});
   BOOST_CHECK_THROW(drain(runtime, broken.body), forge::net::http::exceptions::unavailable);
   auto source = std::make_shared<generated_body>(131072);
   auto cancelled = forge::net::http::write_multipart_stream(
       {{.name = "file", .body = forge::net::http::body_reader{source}}});
   while (source->reads == 0)
   {
      BOOST_REQUIRE(forge::asio::blocking::run(runtime, cancelled.body.async_read()).has_value());
   }

   cancelled.body.cancel();
   BOOST_TEST(source->cancelled);
   const auto reads = source->reads;
   BOOST_CHECK_THROW(drain(runtime, cancelled.body), forge::asio::exceptions::canceled);
   BOOST_TEST(source->reads == reads);
}

BOOST_AUTO_TEST_CASE(multipart_stream_owns_materialized_file_after_its_path_is_erased)
{
   auto runtime = forge::asio::runtime{};
   auto compute = forge::asio::compute::pool{{.worker_threads = 1}};
   const auto directory = std::filesystem::temp_directory_path() /
                          ("forge-multipart-file-" + std::to_string(reinterpret_cast<std::uintptr_t>(&runtime)));
   FORGE_ASSERT(std::filesystem::create_directory(directory), "Cannot create private multipart file fixture");
   const auto cleanup = boost::scope::scope_exit{
       [&directory]
       {
          auto ignored = std::error_code{};
          std::filesystem::remove_all(directory, ignored);
       }};
   const auto path = directory / "artifact.bin";
   const auto payload = std::string(8193, 'x');
   {
      auto output = std::ofstream{path, std::ios::binary};
      output.write(payload.data(), payload.size());
      FORGE_ASSERT(output.good(), "Cannot prepare multipart file bytes");
   }

   auto response = forge::asio::blocking::run(
       runtime, forge::net::http::file_response::from_path(path, compute.get_executor(),
                                                          {.chunk_bytes = 1024, .etag = false, .last_modified = false})
                    .materialize(forge::net::http::request{forge::net::http::method::get, "/artifact", 11}));
   auto body = forge::net::http::body_reader::from_callback(std::move(response.body));
   BOOST_TEST(body.bytes_read() == 0U);
   BOOST_REQUIRE(std::filesystem::remove(path));
   auto written = forge::net::http::write_multipart_stream(
       {{.name = "artifact", .filename = "artifact.bin", .body = body}});
   auto reader = forge::net::http::upload_reader{std::move(written.body)};
   const auto form = forge::asio::blocking::run(runtime, reader.async_read_multipart(written.content_type));
   BOOST_REQUIRE_EQUAL(form.files.size(), 1U);
   BOOST_TEST(form.files.front().text() == payload);
   BOOST_TEST(body.bytes_read() == payload.size());

   auto cancelled = false;
   auto callback_body = forge::net::http::body_reader::from_callback(
       []() -> boost::asio::awaitable<std::optional<forge::net::http::body_chunk>>
       {
          FORGE_THROW_EXCEPTION(forge::net::http::exceptions::unavailable, "Fixture callback failed");
          co_return std::nullopt;
       },
       [&cancelled] { cancelled = true; });
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, callback_body.async_read()),
                     forge::net::http::exceptions::unavailable);
   BOOST_TEST(callback_body.bytes_read() == 0U);
   callback_body.cancel();
   BOOST_TEST(cancelled);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, callback_body.async_read()), forge::asio::exceptions::canceled);
   forge::asio::blocking::run(runtime, compute.shutdown());
}

} // namespace forge::tests::http_multipart_stream
