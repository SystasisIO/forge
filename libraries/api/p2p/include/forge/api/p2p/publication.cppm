module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/any_io_executor.hpp>

#include <functional>
#include <memory>
#include <stop_token>

export module forge.api.p2p.publication;

export namespace forge::api::p2p {

namespace detail {
class publication_state;
struct publication_access;
} // namespace detail

class publication {
 public:
   publication();
   ~publication() noexcept;

   publication(const publication&) = delete;
   publication& operator=(const publication&) = delete;
   publication(publication&& other) noexcept;
   publication& operator=(publication&& other) noexcept;

   [[nodiscard]] bool active() const noexcept;
   // Loss of admission, including replacement; not a drain-completion fence.
   [[nodiscard]] std::stop_token stop_token() const noexcept;
   void close() noexcept;
   boost::asio::awaitable<void> async_close();

 private:
   explicit publication(std::shared_ptr<detail::publication_state> state);

   static boost::asio::awaitable<void>
   async_close_impl(std::shared_ptr<detail::publication_state> state);

   friend struct detail::publication_access;

   std::shared_ptr<detail::publication_state> state_;
};

namespace detail {

struct publication_access {
   [[nodiscard]] static publication make(
      boost::asio::any_io_executor owner_executor,
      std::function<void()> close,
      std::function<boost::asio::awaitable<void>()> drain,
      std::function<bool()> active, std::stop_token native = {});
};

} // namespace detail

} // namespace forge::api::p2p

namespace forge::api::p2p::detail {

[[nodiscard]] std::shared_ptr<publication_state> make_publication_state(
   boost::asio::any_io_executor owner_executor,
   std::function<void()> close,
   std::function<boost::asio::awaitable<void>()> drain,
   std::function<bool()> active, std::stop_token native);
[[nodiscard]] bool publication_active(
   std::shared_ptr<publication_state> state) noexcept;
[[nodiscard]] std::stop_token publication_stop_token(
   std::shared_ptr<publication_state> state) noexcept;
void close_publication(std::shared_ptr<publication_state> state) noexcept;
boost::asio::awaitable<void> async_close_publication(
   std::shared_ptr<publication_state> state);

} // namespace forge::api::p2p::detail
