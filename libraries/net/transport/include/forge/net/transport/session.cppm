module;

#include <memory>

#include <boost/asio/awaitable.hpp>

export module forge.net.transport.session;

export import forge.net.transport.stream;

export namespace forge::net::transport {

namespace detail {
class session_concept;
struct session_access;
} // namespace detail

class session {
 public:
   session();
   ~session();

   session(session&&) noexcept;
   session& operator=(session&&) noexcept;

   session(const session&) = delete;
   session& operator=(const session&) = delete;

   [[nodiscard]] bool valid() const noexcept;

   boost::asio::awaitable<stream> async_open_stream();
   boost::asio::awaitable<stream> async_accept_stream();
   // Completion, including an exception, is a terminal cleanup barrier. Once
   // it resumes, callers may release native lifetime and resource ownership.
   // It still delegates after request_cancel() made the session operationally
   // invalid; only a default or moved-from session is a no-op.
   boost::asio::awaitable<void> async_close();
   void cancel();
   void request_cancel() noexcept;

 private:
   friend struct detail::session_access;

   struct impl;
   explicit session(std::shared_ptr<detail::session_concept> model);

   std::shared_ptr<impl> impl_;
};

namespace detail {

class session_concept {
 public:
   virtual ~session_concept() = default;

   [[nodiscard]] virtual bool valid() const noexcept = 0;
   virtual boost::asio::awaitable<stream> async_open_stream() = 0;
   virtual boost::asio::awaitable<stream> async_accept_stream() = 0;
   // Implementations must complete all terminal native cleanup before this
   // operation resumes, even when reporting the terminal cause by exception.
   virtual boost::asio::awaitable<void> async_close() = 0;
   virtual void cancel() = 0;
   virtual void request_cancel() noexcept;
};

struct session_access {
   [[nodiscard]] static session make(std::shared_ptr<session_concept> model);
   [[nodiscard]] static std::shared_ptr<session_concept> cancellation_owner(const session& value) noexcept;
};

} // namespace detail

} // namespace forge::net::transport
