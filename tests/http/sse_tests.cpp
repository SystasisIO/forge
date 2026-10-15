#include <boost/test/unit_test.hpp>

#include <optional>
#include <string>
#include <string_view>

import forge.net.http.sse;
import forge.net.http.exceptions;

BOOST_AUTO_TEST_CASE(sse_fragmented_crlf_bom_and_multiline_data)
{
   auto decoder = forge::net::http::sse_decoder{};
   decoder.append("\xef");
   BOOST_CHECK(!decoder.next());
   decoder.append("\xbb\xbf: heartbeat\r");
   BOOST_CHECK(!decoder.next());
   decoder.append("\nevent: delta\r\nid: 7\r\nretry: 1500\r\ndata: first\r");
   BOOST_CHECK(!decoder.next());
   decoder.append("\ndata: second\r\n\r\n");
   const auto event = decoder.next();
   BOOST_REQUIRE(event);
   BOOST_TEST(event->data == "first\nsecond");
   BOOST_TEST(event->event == "delta");
   BOOST_TEST(event->id == "7");
   BOOST_REQUIRE(event->retry);
   BOOST_TEST(*event->retry == 1500U);
   BOOST_CHECK(!decoder.next());
}

BOOST_AUTO_TEST_CASE(sse_one_byte_fragments_dispatch_only_at_blank_lines)
{
   auto decoder = forge::net::http::sse_decoder{};
   const auto input = std::string{"data: {\"a\":1}\n\ndata: [DONE]\n\n"};
   auto count = 0;
   for (const auto byte : input)
   {
      decoder.append(std::string_view{&byte, 1});
      if (const auto event = decoder.next())
      {
         BOOST_TEST(event->data == (count == 0 ? "{\"a\":1}" : "[DONE]"));
         ++count;
      }
   }

   BOOST_TEST(count == 2);
}

BOOST_AUTO_TEST_CASE(sse_comments_empty_data_unknown_fields_and_eof)
{
   auto decoder = forge::net::http::sse_decoder{};
   decoder.append(": ignored\nunknown: ignored\n\ndata:\n\ndata: incomplete");
   auto event = decoder.next();
   BOOST_REQUIRE(event);
   BOOST_TEST(event->data.empty());
   BOOST_TEST(event->event == "message");
   decoder.finish();
   BOOST_CHECK(!decoder.next());
   BOOST_CHECK_THROW(decoder.append("data: later\n\n"), forge::net::http::exceptions::bad_request);
}

BOOST_AUTO_TEST_CASE(sse_cr_newlines_preserve_id_and_ignore_invalid_retry)
{
   auto decoder = forge::net::http::sse_decoder{};
   decoder.append("id: first\rdata: a\r\rid: invalid");
   auto first = decoder.next();
   BOOST_REQUIRE(first);
   BOOST_TEST(first->id == "first");
   constexpr char suffix[] = "\0id\rretry: -1\rdata: b\r\r";
   decoder.append(std::string_view{suffix, sizeof(suffix) - 1});
   const auto second = decoder.next();
   BOOST_REQUIRE(second);
   BOOST_TEST(second->id == "first");
   BOOST_CHECK(!second->retry);
}

BOOST_AUTO_TEST_CASE(sse_limits_bound_pending_line_event_and_input)
{
   auto decoder = forge::net::http::sse_decoder{{.max_line_bytes = 8, .max_event_bytes = 7, .max_buffer_bytes = 32}};
   decoder.append("data: aa\n");
   BOOST_CHECK(!decoder.next());
   decoder.append("data: bb\n");
   BOOST_CHECK(!decoder.next());
   decoder.append("data: cc\n\n");
   BOOST_CHECK_THROW(decoder.next(), forge::net::http::exceptions::payload_too_large);
   auto line = forge::net::http::sse_decoder{{.max_line_bytes = 8}};
   line.append("123456789");
   BOOST_CHECK_THROW(line.next(), forge::net::http::exceptions::payload_too_large);
   auto buffer = forge::net::http::sse_decoder{{.max_buffer_bytes = 8}};
   BOOST_CHECK_THROW(buffer.append("123456789"), forge::net::http::exceptions::payload_too_large);
}

BOOST_AUTO_TEST_CASE(sse_format_multiline_and_reject_metadata_injection)
{
   const auto text = forge::net::http::format_sse({.data = "first\nsecond", .event = "delta", .id = "7", .retry = 42});
   BOOST_TEST(text == "event: delta\nid: 7\nretry: 42\ndata: first\ndata: second\n\n");
   BOOST_CHECK_THROW(forge::net::http::format_sse({.data = "a", .event = "bad\nevent"}),
                     forge::net::http::exceptions::bad_request);
   BOOST_CHECK_THROW(forge::net::http::format_sse({.data = "long"}, {.max_event_bytes = 3}),
                     forge::net::http::exceptions::payload_too_large);
}
