//! Native QUIC address readiness before the legacy DCUtR circuit is created.
use std::{collections::HashSet, error::Error, path::Path};
use libp2p::{Multiaddr, PeerId, identify, multiaddr::Protocol, swarm::ConnectionId};
use serde_json::{Value, json};

pub(crate) fn record_dcutr(path: &Path, readiness: &Option<Value>, events: &mut Vec<Value>,
                          event: &libp2p::dcutr::Event) -> Result<(), Box<dyn Error>> {
    if events.len() >= 16 { return Err("native DCUtR result limit".into()); }
    events.push(json!({"sequence": events.len(), "remote_peer_id": event.remote_peer_id.to_string(),
        "result": format!("{:?}", event.result).chars().take(512).collect::<String>()}));
    super::write_json(path, json!({"relay_readiness": readiness, "dcutr_events": events}))
}

pub(crate) fn final_receipt(mut value: Value, scenario: &str, events: Vec<Value>) -> Value {
    if scenario == "dcutr_relay_topology" { value["dcutr_events"] = json!(events); }
    value
}

pub(crate) struct State {
    local: PeerId,
    relay: PeerId,
    connection: Option<ConnectionId>,
    observed: Option<Multiaddr>,
    candidates: HashSet<Multiaddr>,
    events: Vec<Value>,
}

pub(crate) fn required(scenario: &str, transport: &str) -> bool {
    scenario == "dcutr_relay_topology" && transport == "quic"
}

fn canonical(address: &Multiaddr, local: PeerId) -> Option<Multiaddr> {
    let mut parts = address.iter();
    match parts.next()? {
        Protocol::Ip4(ip) if !ip.is_unspecified() && !ip.is_multicast() => {},
        Protocol::Ip6(ip) if !ip.is_unspecified() && !ip.is_multicast() => {},
        _ => return None,
    }
    if !matches!(parts.next(), Some(Protocol::Udp(port)) if port != 0)
        || !matches!(parts.next(), Some(Protocol::QuicV1)) { return None; }
    let mut result = address.clone();
    match parts.next() {
        None => {},
        Some(Protocol::P2p(peer)) if peer == local && parts.next().is_none() => { result.pop(); },
        _ => return None,
    }
    Some(result)
}

impl State {
    pub(crate) fn new(local: PeerId, relay: PeerId) -> Self {
        Self { local, relay, connection: None, observed: None,
            candidates: HashSet::new(), events: Vec::new() }
    }

    pub(crate) fn note(&mut self, kind: &str, detail: String) -> Result<(), Box<dyn Error>> {
        if self.events.len() >= 128 { return Err("DCUtR readiness event limit".into()); }
        self.events.push(json!({"sequence": self.events.len(), "kind": kind,
            "detail": detail.chars().take(512).collect::<String>()}));
        Ok(())
    }

    pub(crate) fn connected(&mut self, peer: PeerId, id: ConnectionId, relayed: bool)
        -> Result<(), Box<dyn Error>> {
        self.note("connection", format!("peer={peer}; connection={id}; relayed={relayed}"))?;
        if peer != self.relay || relayed { return Err("invalid direct relay connection".into()); }
        if self.connection != Some(id) { self.observed = None; }
        self.connection = Some(id);
        Ok(())
    }

    pub(crate) fn identified(&mut self, peer: PeerId, id: ConnectionId, info: &identify::Info)
        -> Result<(), Box<dyn Error>> {
        self.note("identify", format!("peer={peer}; connection={id}; observed={}", info.observed_addr))?;
        if peer != self.relay || self.connection != Some(id)
            || info.public_key.to_peer_id() != self.relay
            || !info.protocols.iter().any(|p| p.as_ref() == "/libp2p/circuit/relay/0.2.0/hop") {
            return Err("DCUtR Identify does not match authenticated relay connection".into());
        }
        self.observed = Some(canonical(&info.observed_addr, self.local)
            .ok_or("DCUtR relay observed address is not a usable native QUIC address")?);
        Ok(())
    }

    pub(crate) fn candidate(&mut self, address: &Multiaddr) -> Result<(), Box<dyn Error>> {
        self.note("native_candidate", address.to_string())?;
        if let Some(address) = canonical(address, self.local) {
            if !self.candidates.contains(&address) && self.candidates.len() >= 16 {
                return Err("DCUtR readiness candidate limit".into());
            }
            self.candidates.insert(address);
        }
        Ok(())
    }

    pub(crate) fn ready_address(&self) -> Option<Multiaddr> {
        self.observed.as_ref().filter(|a| self.connection.is_some()
            && self.candidates.contains(*a)).cloned()
    }

    pub(crate) fn snapshot(&self) -> Value {
        json!({"basis": "same_authenticated_relay_Identify_and_native_NewExternalAddrCandidate",
            "ready": self.ready_address().is_some(), "events": self.events})
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn identity() -> libp2p::identity::Keypair { libp2p::identity::Keypair::generate_ed25519() }
    fn info(key: &libp2p::identity::Keypair, addr: Multiaddr) -> identify::Info {
        identify::Info { public_key: key.public(), protocol_version: "ipfs/0.1.0".into(),
            agent_version: "fixture".into(), listen_addrs: Vec::new(), observed_addr: addr,
            protocols: vec![libp2p::StreamProtocol::new("/libp2p/circuit/relay/0.2.0/hop")],
            signed_peer_record: None }
    }
    #[test]
    fn readiness_requires_both_native_facts_in_either_order() {
        let local = identity().public().to_peer_id(); let key = identity();
        let relay = key.public().to_peer_id(); let id = ConnectionId::new_unchecked(7);
        let addr: Multiaddr = "/ip4/127.0.0.1/udp/4001/quic-v1".parse().unwrap();
        for candidate_first in [false, true] {
            let mut state = State::new(local, relay);
            state.connected(relay, id, false).unwrap();
            assert!(state.ready_address().is_none());
            if candidate_first { state.candidate(&addr).unwrap(); }
            state.identified(relay, id, &info(&key, addr.clone())).unwrap();
            if !candidate_first { assert!(state.ready_address().is_none()); state.candidate(&addr).unwrap(); }
            assert_eq!(state.ready_address(), Some(addr.clone()));
        }
    }
    #[test]
    fn wrong_connection_identity_protocol_and_addresses_are_rejected() {
        let local = identity().public().to_peer_id(); let key = identity();
        let relay = key.public().to_peer_id(); let id = ConnectionId::new_unchecked(7);
        let addr: Multiaddr = "/ip4/127.0.0.1/udp/4001/quic-v1".parse().unwrap();
        let mut state = State::new(local, relay); state.connected(relay, id, false).unwrap();
        assert!(state.identified(relay, ConnectionId::new_unchecked(8), &info(&key, addr.clone())).is_err());
        assert!(state.identified(local, id, &info(&key, addr.clone())).is_err());
        assert!(state.identified(relay, id, &info(&identity(), addr.clone())).is_err());
        let mut missing_hop = info(&key, addr.clone()); missing_hop.protocols.clear();
        assert!(state.identified(relay, id, &missing_hop).is_err());
        for text in ["/ip4/0.0.0.0/udp/1/quic-v1", "/ip4/224.0.0.1/udp/1/quic-v1",
            "/ip4/127.0.0.1/udp/0/quic-v1", "/ip4/127.0.0.1/tcp/4001",
            "/ip4/127.0.0.1/udp/1/quic-v1/p2p-circuit"] {
            assert!(canonical(&text.parse().unwrap(), local).is_none());
        }
        assert_eq!(canonical(&addr.clone().with(Protocol::P2p(local)), local), Some(addr.clone()));
        assert!(canonical(&addr.with(Protocol::P2p(relay)), local).is_none());
    }
    #[test]
    fn diagnostics_and_candidates_are_bounded() {
        let local = identity().public().to_peer_id(); let relay = identity().public().to_peer_id();
        let mut state = State::new(local, relay);
        for port in 1..=16 { state.candidate(&format!("/ip4/127.0.0.1/udp/{port}/quic-v1").parse().unwrap()).unwrap(); }
        assert!(state.candidate(&"/ip4/127.0.0.1/udp/17/quic-v1".parse().unwrap()).is_err());
        let mut state = State::new(local, relay);
        for _ in 0..128 { state.note("test", "x".repeat(1024)).unwrap(); }
        assert_eq!(state.snapshot()["events"][0]["detail"].as_str().unwrap().len(), 512);
        assert!(state.note("overflow", String::new()).is_err());
    }
    #[test]
    fn replacement_connection_requires_a_new_identify_and_only_quic_uses_barrier() {
        let local = identity().public().to_peer_id(); let key = identity();
        let relay = key.public().to_peer_id(); let old = ConnectionId::new_unchecked(7);
        let new = ConnectionId::new_unchecked(8);
        let addr: Multiaddr = "/ip4/127.0.0.1/udp/4001/quic-v1".parse().unwrap();
        let mut state = State::new(local, relay);
        state.connected(relay, old, false).unwrap();
        state.identified(relay, old, &info(&key, addr.clone())).unwrap();
        state.connected(relay, new, false).unwrap();
        state.candidate(&addr).unwrap();
        assert!(state.ready_address().is_none());
        assert!(state.identified(relay, old, &info(&key, addr.clone())).is_err());
        state.identified(relay, new, &info(&key, addr.clone())).unwrap();
        assert_eq!(state.ready_address(), Some(addr));
        assert!(required("dcutr_relay_topology", "quic"));
        for transport in ["tcp", "tcp-tls", "tcp-pnet"] { assert!(!required("dcutr_relay_topology", transport)); }
        assert!(!required("autorelay", "quic"));
    }
    #[test]
    fn final_success_preserves_actual_trace_without_changing_autorelay_schema() {
        let receipt = json!({"status": "ok", "direct_upgrade": true});
        let events = vec![json!({"sequence": 0, "remote_peer_id": "observed-peer", "result": "observed-result"})];
        let final_value = final_receipt(receipt.clone(), "dcutr_relay_topology", events.clone());
        assert_eq!(final_value["dcutr_events"], json!(events));
        assert_eq!(final_value["status"], "ok");
        assert_eq!(final_value["direct_upgrade"], true);
        assert_eq!(final_receipt(receipt.clone(), "autorelay", events), receipt);
    }

    // Exercise the pinned donor Swarm itself, not a model of its confirmation logic.
    struct CandidateEmitter { pending: Option<Multiaddr>, delivered: usize }
    impl libp2p::swarm::NetworkBehaviour for CandidateEmitter {
        type ConnectionHandler = libp2p::swarm::dummy::ConnectionHandler;
        type ToSwarm = ();
        fn handle_established_inbound_connection(&mut self, _: ConnectionId, _: PeerId, _: &Multiaddr,
            _: &Multiaddr) -> Result<Self::ConnectionHandler, libp2p::swarm::ConnectionDenied> {
            Ok(libp2p::swarm::dummy::ConnectionHandler)
        }
        fn handle_established_outbound_connection(&mut self, _: ConnectionId, _: PeerId, _: &Multiaddr,
            _: libp2p::core::Endpoint, _: libp2p::core::transport::PortUse)
            -> Result<Self::ConnectionHandler, libp2p::swarm::ConnectionDenied> {
            Ok(libp2p::swarm::dummy::ConnectionHandler)
        }
        fn on_swarm_event(&mut self, event: libp2p::swarm::FromSwarm) {
            if matches!(event, libp2p::swarm::FromSwarm::NewExternalAddrCandidate(_)) { self.delivered += 1; }
        }
        fn on_connection_handler_event(&mut self, _: PeerId, _: ConnectionId,
            event: libp2p::swarm::THandlerOutEvent<Self>) { match event {} }
        fn poll(&mut self, _: &mut std::task::Context<'_>)
            -> std::task::Poll<libp2p::swarm::ToSwarm<(), libp2p::swarm::THandlerInEvent<Self>>> {
            match self.pending.take() {
                Some(address) => std::task::Poll::Ready(libp2p::swarm::ToSwarm::NewExternalAddrCandidate(address)),
                None => std::task::Poll::Pending,
            }
        }
    }
    #[tokio::test]
    async fn pinned_swarm_delivers_candidate_before_confirmation_and_suppresses_it_after() {
        use futures::{FutureExt, StreamExt};
        use libp2p::Transport;
        let addr: Multiaddr = "/ip4/127.0.0.1/udp/4001/quic-v1".parse().unwrap();
        for confirm_first in [false, true] {
            let transport = libp2p::core::transport::dummy::DummyTransport::<(
                PeerId, libp2p::core::muxing::StreamMuxerBox)>::new().boxed();
            let mut swarm = libp2p::Swarm::new(transport,
                CandidateEmitter { pending: Some(addr.clone()), delivered: 0 },
                identity().public().to_peer_id(), libp2p::swarm::Config::with_tokio_executor());
            if confirm_first { swarm.add_external_address(addr.clone()); }
            let event = swarm.next().now_or_never();
            if confirm_first {
                assert!(event.is_none());
                assert_eq!(swarm.behaviour().delivered, 0);
            } else {
                assert!(matches!(event, Some(Some(libp2p::swarm::SwarmEvent::NewExternalAddrCandidate { address })) if address == addr));
                assert_eq!(swarm.behaviour().delivered, 1);
                swarm.add_external_address(addr.clone());
            }
        }
    }
}
