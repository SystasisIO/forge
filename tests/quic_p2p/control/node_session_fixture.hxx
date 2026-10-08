#pragma once

namespace forge::net::p2p {

// This friend owner is linked only into the isolated control-queue executable.
struct node_session_fixture {
   static peer_id peer(std::uint8_t value);
   static void queue_bounds();
   static void queue_latest_ack();
   static void queue_failure_and_lifetime();
   static void queue_batch_rollback();
   static void queue_byte_bound();
   static void queue_legacy_bytes();
   static void queue_ephemeral_bound();
   static void backoff_allocation_refusal();
   static void backoff_graft_slack();
   static void control_preparation_allocation_rollback();
   static void native_heartbeat_allocation_recovery();
   static void native_retry(bool rejected, bool legacy = false);
   static void native_unsubscribe_rollback();
   static void native_gate_supersession();
   static void native_snapshot_supersession();
   static void native_dispatch_request(bool stop);
   static void native_control_failure_generation(bool stale);
   static void native_blocked_ihave_expiry();
   static void native_direct_publish_failure_generation(bool reconnect);
   static void native_protocol_open_failure_generation(bool reconnect);
   static void gossip_payload_bounds();
   static void gossip_cursor_bounds();
   static void native_gossip_chunks(bool legacy);
   static void native_iwant_chunk_failure();
   static void native_iwant_origin_retirement();
   static void native_cached_message_frames();
   static void native_graft_batches(bool inbound);
   static void native_neutral_wire_topic();
   static void native_owned_codec_topic(bool sign);
   static void native_retired_inbound();
   static void native_retired_announce();
   static void native_announce_capacity_retry();
   static void native_retired_rpc();

 private:
   static void run(forge::asio::runtime& runtime, boost::asio::awaitable<void> operation);
   static std::size_t controls(const forge::tests::p2p::pubsub_router_fixture& fixture,
                               const node& owner, const peer_id& peer, bool graft);
   static pubsub::options manual_options();
};

} // namespace forge::net::p2p
