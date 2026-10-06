#pragma once

#include <sys/socket.h>
#include <ngtcp2/ngtcp2.h>

namespace forge::net::quic::detail {
struct path_storage {
   sockaddr_storage local_storage{};
   sockaddr_storage remote_storage{};
   ngtcp2_path path{};
};
} // namespace forge::net::quic::detail
