module;

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

export module forge.net.http.sse;

export namespace forge::net::http {

struct sse_event {
   std::string data;
   std::string event;
   // Complete logical last-event-id; an empty value resets a prior id.
   std::string id;
   std::optional<std::uint64_t> retry;
};

struct sse_limits {
   std::size_t max_line_bytes = 64U * 1024U;
   std::size_t max_event_bytes = 1024U * 1024U;
   std::size_t max_buffer_bytes = 1024U * 1024U;
};

// Feed bounded transport chunks, then drain next() before appending more.
// EOF discards an event that did not end with a blank line.
class sse_decoder {
 public:
   explicit sse_decoder(sse_limits limits = {});
   void append(std::string_view bytes);
   [[nodiscard]] std::optional<sse_event> next();
   void finish() noexcept;

 private:
   [[nodiscard]] std::optional<sse_event> consume_line();

   sse_limits _limits;
   std::string _buffer;
   std::size_t _offset = 0;
   std::string _line;
   std::string _data;
   std::string _event;
   std::string _id;
   std::optional<std::uint64_t> _retry;
   bool _first_line = true;
   bool _skip_lf = false;
   bool _seen_data = false;
   bool _finished = false;
};

[[nodiscard]] std::string format_sse(const sse_event& event, sse_limits limits = {});

} // namespace forge::net::http
