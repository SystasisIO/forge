#pragma once

namespace forge::plugins::net::p2p::pubsub {

[[nodiscard]] message project_message(const forge::net::p2p::peer_id& source,
                                      const forge::net::p2p::pubsub::message& value);

} // namespace forge::plugins::net::p2p::pubsub
