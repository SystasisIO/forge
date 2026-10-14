module;

#include <forge/exceptions/macros.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/ssl.h>

export module forge.net.tls.context;

export import forge.net.tls.exceptions;
export import forge.net.tls.options;

export namespace forge::net::tls {

class context_snapshot;

using context_snapshot_ptr = std::shared_ptr<const context_snapshot>;
using asio_tls_stream = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;
using beast_tls_stream = boost::beast::ssl_stream<boost::beast::tcp_stream>;

[[nodiscard]] context_snapshot_ptr make_context(context_options options);
[[nodiscard]] std::shared_ptr<asio_tls_stream> make_asio_stream(context_snapshot_ptr snapshot,
                                                                boost::asio::ip::tcp::socket socket);
template <typename next_layer>
[[nodiscard]] std::shared_ptr<boost::asio::ssl::stream<next_layer>> make_asio_stream(context_snapshot_ptr snapshot,
                                                                                       next_layer stream);
[[nodiscard]] std::shared_ptr<beast_tls_stream> make_beast_stream(context_snapshot_ptr snapshot,
                                                                  boost::beast::tcp_stream stream);

class context_snapshot {
 public:
   ~context_snapshot();

   context_snapshot(const context_snapshot&) = delete;
   context_snapshot& operator=(const context_snapshot&) = delete;

   [[nodiscard]] std::span<const unsigned char> client_alpn_wire() const noexcept;
   [[nodiscard]] peer_verification verification() const noexcept;

 private:
   friend context_snapshot_ptr make_context(context_options options);
   friend std::shared_ptr<asio_tls_stream> make_asio_stream(context_snapshot_ptr snapshot,
                                                            boost::asio::ip::tcp::socket socket);
   template <typename next_layer>
   friend std::shared_ptr<boost::asio::ssl::stream<next_layer>> make_asio_stream(context_snapshot_ptr snapshot,
                                                                                   next_layer stream);
   friend std::shared_ptr<beast_tls_stream> make_beast_stream(context_snapshot_ptr snapshot,
                                                              boost::beast::tcp_stream stream);

   struct impl;

   explicit context_snapshot(std::shared_ptr<impl> impl_value);
   [[nodiscard]] boost::asio::ssl::context& context_for_stream() const noexcept;

   std::shared_ptr<impl> impl_;
};

template <typename next_layer>
[[nodiscard]] std::shared_ptr<boost::asio::ssl::stream<next_layer>> make_asio_stream(context_snapshot_ptr snapshot,
                                                                                       next_layer stream) {
   if (!snapshot) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "TLS stream requires a context snapshot");
   }

   auto* result = new boost::asio::ssl::stream<next_layer>{std::move(stream), snapshot->context_for_stream()};
   return {result, [snapshot = std::move(snapshot)](auto* value) { delete value; }};
}

void configure_client_stream(SSL* native_handle, const context_snapshot& snapshot, client_stream_options options);
void classify_handshake_failure(SSL* native_handle, const context_snapshot& snapshot);
[[nodiscard]] std::optional<peer_certificate> extract_peer_certificate(SSL* native_handle);
[[nodiscard]] certificate_chain extract_peer_certificate_chain(SSL* native_handle);
void validate_peer(SSL* native_handle, const context_snapshot& snapshot, const peer_validation& validation);
[[nodiscard]] std::string selected_alpn(SSL* native_handle);

class context_provider {
 public:
   explicit context_provider(context_options initial);

   context_provider(const context_provider&) = delete;
   context_provider& operator=(const context_provider&) = delete;

   [[nodiscard]] context_snapshot_ptr snapshot() const noexcept;
   // Prepare with make_context before admission. Retain the returned owner until
   // after any caller-owned publication lock has been released.
   [[nodiscard]] context_snapshot_ptr exchange(context_snapshot_ptr prepared);
   void replace(context_options replacement);

 private:
   context_snapshot_ptr current_;
};

} // namespace forge::net::tls
