module;

#include <chrono>
#include <coroutine>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/buffered_read_stream.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

module forge.net.websocket.connection;

import forge.asio.notification;
import forge.exceptions;
import forge.net.tls.context;
import forge.net.websocket.exceptions;

namespace boost::beast {

template <typename Stream, typename Buffer>
void teardown(role_type role, buffered_read_stream<Stream, Buffer>& stream, boost::system::error_code& error) {
   using boost::beast::websocket::teardown;
   teardown(role, stream.next_layer(), error);
}

template <typename Stream, typename Buffer, typename Handler>
void async_teardown(role_type role, buffered_read_stream<Stream, Buffer>& stream, Handler&& handler) {
   using boost::beast::websocket::async_teardown;
   async_teardown(role, stream.next_layer(), std::forward<Handler>(handler));
}

} // namespace boost::beast

namespace forge::net::websocket {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace beast_websocket = boost::beast::websocket;
using asio::use_awaitable;

using plain_input_stream = beast::buffered_read_stream<beast::tcp_stream, beast::flat_buffer>;
using tls_input_stream = beast::buffered_read_stream<beast::ssl_stream<beast::tcp_stream>, beast::flat_buffer>;
using plain_stream = beast_websocket::stream<plain_input_stream>;
using tls_stream = beast_websocket::stream<tls_input_stream>;
using stream_variant = std::variant<plain_stream, tls_stream>;

struct completion_state {
   void complete() {
      {
         const auto lock = std::scoped_lock{mutex};
         completed = true;
      }
      ready.notify();
   }

   void complete_error(std::exception_ptr error_value) {
      {
         const auto lock = std::scoped_lock{mutex};
         error = std::move(error_value);
         completed = true;
      }
      ready.notify();
   }

   bool is_completed() const {
      const auto lock = std::scoped_lock{mutex};
      return completed;
   }

   void rethrow_if_failed() const {
      const auto lock = std::scoped_lock{mutex};
      if (error) {
         std::rethrow_exception(error);
      }
   }

   forge::asio::notification ready;
   mutable std::mutex mutex;
   std::exception_ptr error;
   bool completed = false;
};

asio::awaitable<void> wait_completion(const std::shared_ptr<completion_state>& completion) {
   for (;;) {
      const auto observed = completion->ready.epoch();
      if (completion->is_completed()) {
         break;
      }
      static_cast<void>(co_await completion->ready.async_wait(observed));
   }
   completion->rethrow_if_failed();
}

struct write_operation {
   std::string message;
   bool binary = false;
   bool ping = false;
   bool close = false;
   std::shared_ptr<completion_state> completion;
};

} // namespace

struct connection::impl {
   explicit impl(beast::tcp_stream stream_value)
       : stream(plain_stream{plain_input_stream{std::move(stream_value)}}), message_handler_ready(executor()) {
      message_handler_ready.expires_at(asio::steady_timer::time_point::max());
   }

   explicit impl(beast::ssl_stream<beast::tcp_stream> stream_value,
                 forge::net::tls::context_snapshot_ptr tls_context_snapshot_value = {})
       : stream(tls_stream{tls_input_stream{std::move(stream_value)}}),
         tls_context_snapshot(std::move(tls_context_snapshot_value)), message_handler_ready(executor()) {
      message_handler_ready.expires_at(asio::steady_timer::time_point::max());
   }

   [[nodiscard]] asio::any_io_executor executor() {
      return std::visit([](auto& stream_value) -> asio::any_io_executor { return stream_value.get_executor(); },
                        stream);
   }

   void complete_pending_with_error(std::exception_ptr error) {
      while (!writes.empty()) {
         writes.front()->completion->complete_error(error);
         writes.pop_front();
      }
      writing = false;
      if (cancellation_error) {
         closed = true;
      }
      for (const auto& waiter : close_waiters) {
         waiter->complete_error(error);
      }
      close_waiters.clear();
      refresh_queue_depth();
   }

   void cancel() {
      if (cancellation_error || closed) {
         return;
      }
      cancellation_error = std::make_exception_ptr(exceptions::closed{"WebSocket connection was canceled"});
      closing = true;
      message_handler_ready.cancel();
      std::visit([](auto& stream_value) {
         auto& socket = beast::get_lowest_layer(stream_value).socket();
         auto ignored = boost::system::error_code{};
         socket.cancel(ignored);
         socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
         socket.close(ignored);
      }, stream);
      record_close();

      // The active operation owns a native buffer and still pops the front in its completion callback.
      if (writing) {
         while (writes.size() > 1U) {
            auto& pending = writes.back();
            if (pending->close) {
               // A queued close is also a native-write join, not just an outbound item to discard.
               close_waiters.push_back(std::move(pending->completion));
            } else {
               pending->completion->complete_error(cancellation_error);
            }
            writes.pop_back();
         }
         refresh_queue_depth();
      } else {
         complete_pending_with_error(cancellation_error);
      }
   }

   void start_write(connection& owner) {
      if (writing || writes.empty()) {
         return;
      }

      writing = true;
      auto operation = writes.front();
      auto self = owner.shared_from_this();
      std::visit(
          [this, self, operation](auto& stream_value) {
             if (operation->close) {
                stream_value.async_close(beast_websocket::close_code::normal,
                                         asio::bind_executor(stream_value.get_executor(),
                                                             [this, self, operation](boost::system::error_code error) {
                                                                writes.pop_front();
                                                                writing = false;
                                                                closed = true;
                                                                if (error || cancellation_error) {
                                                                   auto failure = cancellation_error ? cancellation_error :
                                                                       std::make_exception_ptr(boost::system::system_error{error});
                                                                   operation->completion->complete_error(failure);
                                                                   complete_pending_with_error(failure);
                                                                   return;
                                                                }
                                                                record_close();
                                                                operation->completion->complete();
                                                                for (const auto& waiter : close_waiters) {
                                                                   waiter->complete();
                                                                }
                                                                close_waiters.clear();
                                                                refresh_queue_depth();
                                                             }));
                return;
             }
             if (operation->ping) {
                stream_value.async_ping(beast_websocket::ping_data{operation->message},
                                        asio::bind_executor(stream_value.get_executor(),
                                                            [this, self, operation](boost::system::error_code error) {
                                                               writes.pop_front();
                                                               writing = false;
                                                               if (error || cancellation_error) {
                                                                  auto failure = cancellation_error ? cancellation_error :
                                                                      std::make_exception_ptr(boost::system::system_error{error});
                                                                  operation->completion->complete_error(failure);
                                                                  complete_pending_with_error(failure);
                                                                  return;
                                                               }
                                                               record_ping();
                                                               operation->completion->complete();
                                                               start_write(*self);
                                                            }));
                return;
             }
             stream_value.binary(operation->binary);
             stream_value.async_write(
                 asio::buffer(operation->message),
                 asio::bind_executor(stream_value.get_executor(),
                                     [this, self, operation](boost::system::error_code error, std::size_t bytes) {
                                        static_cast<void>(bytes);
                                        writes.pop_front();
                                        writing = false;
                                        if (error || cancellation_error) {
                                           auto failure = cancellation_error ? cancellation_error :
                                               std::make_exception_ptr(boost::system::system_error{error});
                                           operation->completion->complete_error(failure);
                                           record_failed_write();
                                           complete_pending_with_error(failure);
                                           return;
                                        }

                                        record_sent();
                                        operation->completion->complete();
                                        start_write(*self);
                                     }));
          },
          stream);
   }

   asio::awaitable<void> accept(const boost::beast::http::request<boost::beast::http::string_body>& request_value,
                                std::string buffered_input = {}) {
      co_await std::visit(
          [&request_value, &buffered_input](auto& stream_value) -> asio::awaitable<void> {
             auto& input = stream_value.next_layer().buffer();
             input.commit(asio::buffer_copy(input.prepare(buffered_input.size()), asio::buffer(buffered_input)));
             stream_value.set_option(beast_websocket::stream_base::timeout::suggested(beast::role_type::server));
             co_await stream_value.async_accept(request_value, use_awaitable);
          },
          stream);
   }

   asio::awaitable<void> handshake(std::string host, std::string target) {
      co_await std::visit(
          [&host, &target](auto& stream_value) -> asio::awaitable<void> {
             stream_value.set_option(beast_websocket::stream_base::timeout::suggested(beast::role_type::client));
             co_await stream_value.async_handshake(host, target, use_awaitable);
          },
          stream);
   }

   asio::awaitable<std::pair<boost::system::error_code, std::size_t>> read_one() {
      co_return co_await std::visit(
          [this](auto& stream_value) -> asio::awaitable<std::pair<boost::system::error_code, std::size_t>> {
             co_return co_await stream_value.async_read(buffer, asio::as_tuple(use_awaitable));
          },
          stream);
   }

   void record_queued_write() {
      const auto lock = std::scoped_lock{metrics_mutex};
      current_metrics.queued_writes = writes.size();
   }

   void refresh_queue_depth() {
      const auto lock = std::scoped_lock{metrics_mutex};
      current_metrics.queued_writes = writes.size();
   }

   void record_sent() {
      const auto lock = std::scoped_lock{metrics_mutex};
      ++current_metrics.sent_messages;
      current_metrics.queued_writes = writes.size();
   }

   void record_received() {
      const auto lock = std::scoped_lock{metrics_mutex};
      ++current_metrics.received_messages;
   }

   void record_failed_write() {
      const auto lock = std::scoped_lock{metrics_mutex};
      ++current_metrics.failed_writes;
      current_metrics.queued_writes = writes.size();
   }

   void record_ping() {
      const auto lock = std::scoped_lock{metrics_mutex};
      ++current_metrics.ping_count;
   }

   void record_close() {
      const auto lock = std::scoped_lock{metrics_mutex};
      ++current_metrics.close_count;
   }

   void record_handler_failure() {
      const auto lock = std::scoped_lock{metrics_mutex};
      ++current_metrics.handler_failures;
   }

   [[nodiscard]] connection_metrics metrics() const {
      const auto lock = std::scoped_lock{metrics_mutex};
      return current_metrics;
   }

   stream_variant stream;
   beast::flat_buffer buffer;
   forge::net::tls::context_snapshot_ptr tls_context_snapshot;
   asio::steady_timer message_handler_ready;
   message_handler on_message;
   received_message_handler on_received_message;
   close_handler on_close;
   std::deque<std::shared_ptr<write_operation>> writes;
   std::vector<std::shared_ptr<completion_state>> close_waiters;
   mutable std::mutex metrics_mutex;
   connection_metrics current_metrics{};
   std::exception_ptr cancellation_error;
   bool writing = false;
   bool closing = false;
   bool closed = false;
};

connection::connection(beast::tcp_stream stream) : impl_(std::make_unique<impl>(std::move(stream))) {}

connection::connection(beast::ssl_stream<beast::tcp_stream> stream)
    : impl_(std::make_unique<impl>(std::move(stream))) {}

connection::connection(beast::ssl_stream<beast::tcp_stream> stream,
                       forge::net::tls::context_snapshot_ptr tls_context_snapshot)
    : impl_(std::make_unique<impl>(std::move(stream), std::move(tls_context_snapshot))) {}

connection::~connection() = default;

connection::ptr connection::create(beast::tcp_stream stream) {
   return connection::ptr{new connection(std::move(stream))};
}

connection::ptr connection::create(beast::ssl_stream<beast::tcp_stream> stream) {
   return connection::ptr{new connection(std::move(stream))};
}

connection::ptr connection::create(beast::ssl_stream<beast::tcp_stream> stream,
                                   forge::net::tls::context_snapshot_ptr tls_context_snapshot) {
   return connection::ptr{new connection(std::move(stream), std::move(tls_context_snapshot))};
}

void connection::on_message(message_handler handler) {
   asio::post(impl_->executor(), [impl = impl_.get(), handler = std::move(handler)]() mutable {
      impl->on_message = std::move(handler);
      impl->message_handler_ready.cancel();
   });
}

void connection::on_received_message(received_message_handler handler) {
   asio::post(impl_->executor(), [impl = impl_.get(), handler = std::move(handler)]() mutable {
      impl->on_received_message = std::move(handler);
      impl->message_handler_ready.cancel();
   });
}

void connection::on_close(close_handler handler) {
   asio::post(impl_->executor(),
              [impl = impl_.get(), handler = std::move(handler)]() mutable { impl->on_close = std::move(handler); });
}

boost::asio::awaitable<void> connection::send(std::string message) {
   return send_message(std::move(message), false);
}

boost::asio::awaitable<void> connection::send_binary(std::string message) {
   return send_message(std::move(message), true);
}

boost::asio::awaitable<void> connection::send_message(std::string message, bool binary) {
   auto completion = std::make_shared<completion_state>();
   asio::post(
       impl_->executor(), [self = shared_from_this(), message = std::move(message), binary, completion]() mutable {
          if (self->impl_->closing) {
             completion->complete_error(std::make_exception_ptr(exceptions::closed{"websocket connection is closing"}));
             return;
          }
          self->impl_->writes.push_back(std::make_shared<write_operation>(write_operation{
              .message = std::move(message),
              .binary = binary,
              .completion = std::move(completion),
          }));
          self->impl_->record_queued_write();
          self->impl_->start_write(*self);
       });
   co_await wait_completion(completion);
}

boost::asio::awaitable<void> connection::close() {
   auto completion = std::make_shared<completion_state>();
   asio::post(impl_->executor(), [self = shared_from_this(), completion]() {
      if (self->impl_->closed) {
         completion->complete();
         return;
      }
      if (self->impl_->closing) {
         self->impl_->close_waiters.push_back(completion);
         return;
      }

      self->impl_->closing = true;
      self->impl_->message_handler_ready.cancel();
      self->impl_->writes.push_back(std::make_shared<write_operation>(write_operation{
          .close = true,
          .completion = completion,
      }));
      self->impl_->record_queued_write();
      self->impl_->start_write(*self);
   });
   co_await wait_completion(completion);
}

void connection::cancel() {
   asio::dispatch(impl_->executor(), [self = shared_from_this()] { self->impl_->cancel(); });
}

boost::asio::awaitable<void> connection::ping(std::string payload) {
   auto completion = std::make_shared<completion_state>();
   asio::post(impl_->executor(), [self = shared_from_this(), payload = std::move(payload), completion]() {
      if (self->impl_->closing) {
         completion->complete_error(std::make_exception_ptr(exceptions::closed{"websocket connection is closing"}));
         return;
      }
      self->impl_->writes.push_back(std::make_shared<write_operation>(write_operation{
          .message = std::move(payload),
          .ping = true,
          .completion = completion,
      }));
      self->impl_->record_queued_write();
      self->impl_->start_write(*self);
   });
   co_await wait_completion(completion);
}

boost::asio::awaitable<void>
connection::accept(const boost::beast::http::request<boost::beast::http::string_body>& request_value) {
   co_await impl_->accept(request_value);
}

boost::asio::awaitable<void>
connection::accept(const boost::beast::http::request<boost::beast::http::string_body>& request_value,
                   std::string buffered_input) {
   co_await impl_->accept(request_value, std::move(buffered_input));
}

boost::asio::awaitable<void> connection::handshake(std::string host, std::string target) {
   co_await impl_->handshake(std::move(host), std::move(target));
}

void connection::start_read_loop() {
   auto self = shared_from_this();
   asio::co_spawn(
       impl_->executor(),
       [self]() -> asio::awaitable<void> {
          for (;;) {
             auto [error, bytes] = co_await self->impl_->read_one();
             static_cast<void>(bytes);
             if (error) {
                if (self->impl_->on_close) {
                   self->impl_->on_close(*self);
                }
                co_return;
             }

             auto message = beast::buffers_to_string(self->impl_->buffer.data());
             const auto binary =
                 std::visit([](const auto& stream_value) { return stream_value.got_binary(); }, self->impl_->stream);
             self->impl_->buffer.consume(self->impl_->buffer.size());
             self->impl_->record_received();
             while (!self->impl_->on_message && !self->impl_->on_received_message && !self->impl_->closing) {
                self->impl_->message_handler_ready.expires_at(asio::steady_timer::time_point::max());
                auto handler_error = boost::system::error_code{};
                co_await self->impl_->message_handler_ready.async_wait(
                    asio::redirect_error(use_awaitable, handler_error));
             }
             if (self->impl_->closing) {
                co_return;
             }
             if (self->impl_->on_received_message || self->impl_->on_message) {
                auto handler_failed = false;
                try {
                   if (self->impl_->on_received_message) {
                      co_await self->impl_->on_received_message(
                          *self, received_message{.payload = std::move(message), .binary = binary});
                   } else {
                      co_await self->impl_->on_message(*self, std::move(message));
                   }
                } catch (...) {
                   self->impl_->record_handler_failure();
                   if (self->impl_->on_close) {
                      self->impl_->on_close(*self);
                   }
                   handler_failed = true;
                }
                if (handler_failed) {
                   try {
                      co_await self->close();
                   } catch (...) {
                      // The handler failure remains the terminal cause.
                   }
                   co_return;
                }
             }
          }
       },
       [self](std::exception_ptr error) {
          if (error) {
             self->impl_->record_handler_failure();
          }
       });
}

connection_metrics connection::metrics() const {
   return impl_->metrics();
}

} // namespace forge::net::websocket
