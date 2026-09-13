use std::{
    collections::{HashMap, HashSet},
    convert::Infallible,
    error::Error,
    future::Future,
    io,
    net::IpAddr,
    pin::Pin,
    str::FromStr,
    sync::{Arc, Mutex},
    task::{Context, Poll, Waker},
    time::Duration,
};

use futures::StreamExt;
use libp2p::{
    Multiaddr, PeerId, Swarm, SwarmBuilder, Transport, autonat,
    core::{
        ConnectedPoint, Endpoint,
        transport::{ListenerId, PortUse},
        upgrade::Version,
    },
    identify, identity,
    multiaddr::Protocol,
    noise,
    pnet::{PnetConfig, PreSharedKey},
    swarm::{
        ConnectionDenied, ConnectionId, Executor, FromSwarm, NetworkBehaviour, SwarmEvent, THandler,
        THandlerInEvent, THandlerOutEvent, ToSwarm, behaviour::toggle::Toggle, dial_opts::DialOpts, dummy,
    },
    tcp, tls, yamux,
};
use rand::rngs::OsRng;
use serde_json::{Value, json};
use tokio::{
    task::JoinHandle,
    time::{Instant, timeout_at},
};

use super::{Options, write_json};

type Result<T> = std::result::Result<T, Box<dyn Error>>;
const CONNECTION_LIMIT: usize = 64;
const CONNECTION_TRACE_LIMIT: usize = 128;
const EVENT_LIMIT: usize = 64;
const TASK_LIMIT: usize = 256;
const EXCHANGE_TIMEOUT: Duration = Duration::from_secs(30);
const DRAIN_TIMEOUT: Duration = Duration::from_secs(20);
const V1: &str = "/libp2p/autonat/1.0.0";
const V2: &str = "/libp2p/autonat/2/dial-request";

pub(super) fn is_scenario(value: &str) -> bool {
    matches!(value, "autonat_v1" | "autonat_v2")
}

fn address(value: &Multiaddr, transport: &str, zero_port: bool) -> Result<IpAddr> {
    let mut parts = value.iter();
    let ip = match parts.next() {
        Some(Protocol::Ip4(ip)) => IpAddr::V4(ip),
        Some(Protocol::Ip6(ip)) => IpAddr::V6(ip),
        _ => return Err("AutoNAT requires a numeric IP address".into()),
    };
    let port = match (transport, parts.next()) {
        ("quic", Some(Protocol::Udp(port))) => {
            if parts.next() != Some(Protocol::QuicV1) {
                return Err("AutoNAT QUIC address requires quic-v1".into());
            }
            port
        }
        ("tcp" | "tcp-tls" | "tcp-pnet", Some(Protocol::Tcp(port))) => port,
        _ => return Err("AutoNAT address does not match the configured transport".into()),
    };
    if parts.next().is_some() || (!zero_port && port == 0) || !public_ip(ip) {
        return Err(format!("AutoNAT requires a public-classified direct address: {value}").into());
    }
    Ok(ip)
}

fn public_ip(ip: IpAddr) -> bool {
    // Fixture preflight, not an override of the donor's private GlobalIp trait.
    // IPv4 follows pinned v1 exclusions, additionally excluding multicast.
    match ip {
        IpAddr::V4(ip) => {
            let [a, b, c, d] = ip.octets();
            let protocol_exception = a == 192 && b == 0 && c == 0 && (d == 9 || d == 10);
            !ip.is_private()
                && !ip.is_loopback()
                && !ip.is_link_local()
                && !ip.is_documentation()
                && a != 0
                && a < 224
                && !(a == 100 && b & 0xc0 == 64)
                && !(a == 198 && b & 0xfe == 18)
                && (!(a == 192 && b == 0 && c == 0) || protocol_exception)
        }
        IpAddr::V6(ip) => {
            // Deliberately narrower than donor global-scope classification:
            // ordinary global unicast only, excluding special/documentation ranges.
            let s = ip.segments();
            s[0] & 0xe000 == 0x2000
                && !(s[0] == 0x2001 && s[1] < 0x200)
                && !(s[0] == 0x2001 && s[1] == 0xdb8)
                && s[0] != 0x2002
                && !(s[0] == 0x3fff && s[1] < 0x1000)
        }
    }
}

fn listen_address(ip: IpAddr, transport: &str) -> Multiaddr {
    let addr = Multiaddr::empty().with(match ip {
        IpAddr::V4(ip) => Protocol::Ip4(ip),
        IpAddr::V6(ip) => Protocol::Ip6(ip),
    });
    if transport == "quic" {
        addr.with(Protocol::Udp(0)).with(Protocol::QuicV1)
    } else {
        addr.with(Protocol::Tcp(0))
    }
}

fn without_peer(mut addr: Multiaddr, expected: PeerId) -> Result<Multiaddr> {
    if let Some(Protocol::P2p(peer)) = addr.iter().last() {
        if peer != expected {
            return Err("multiaddr terminal peer differs from authenticated peer".into());
        }
        addr.pop();
    }
    Ok(addr)
}

// Only Swarm connection tasks are owned here. Quinn's internal runtime tasks
// are not exposed by the pinned public transport API and are not claimed joined.
#[derive(Clone, Default)]
struct TaskOwner(Arc<Mutex<TaskState>>);

#[derive(Default)]
struct TaskState {
    closed: bool,
    handles: Vec<JoinHandle<()>>,
    error: Option<String>,
}

impl Executor for TaskOwner {
    fn exec(&self, future: Pin<Box<dyn Future<Output = ()> + Send>>) {
        let mut state = self.0.lock().expect("task owner lock");
        if state.closed || state.handles.len() == TASK_LIMIT {
            state
                .error
                .get_or_insert("Swarm executor admission closed or task limit exceeded".into());
            return;
        }
        state.handles.push(tokio::spawn(future));
    }
}

impl TaskOwner {
    fn failure(&self) -> Option<String> {
        self.0.lock().expect("task owner lock").error.clone()
    }

    async fn join(&self, deadline: Instant) -> Result<()> {
        let handles = {
            let mut state = self.0.lock().expect("task owner lock");
            state.closed = true;
            std::mem::take(&mut state.handles)
        };
        let mut failure = self.failure();
        for mut task in handles {
            match timeout_at(deadline, &mut task).await {
                Ok(Ok(())) => {}
                Ok(Err(error)) => {
                    failure.get_or_insert(format!("Swarm task failed: {error}"));
                }
                Err(_) => {
                    // Abort is cleanup after a failed graceful join, never success evidence.
                    task.abort();
                    let _ = task.await;
                    failure.get_or_insert("Swarm executor join timed out".into());
                }
            }
        }
        match failure.or_else(|| self.failure()) {
            Some(error) => Err(error.into()),
            None => Ok(()),
        }
    }
}

// A configured listener is an unverified candidate, not ExternalAddrConfirmed.
#[derive(Default)]
struct CandidateReporter {
    pending: Option<Multiaddr>,
    waker: Option<Waker>,
}

impl CandidateReporter {
    fn report(&mut self, addr: Multiaddr) {
        self.pending = Some(addr);
        if let Some(waker) = self.waker.take() {
            waker.wake();
        }
    }
}

impl NetworkBehaviour for CandidateReporter {
    type ConnectionHandler = dummy::ConnectionHandler;
    type ToSwarm = Infallible;

    fn handle_established_inbound_connection(
        &mut self,
        _: ConnectionId,
        _: PeerId,
        _: &Multiaddr,
        _: &Multiaddr,
    ) -> std::result::Result<THandler<Self>, ConnectionDenied> {
        Ok(dummy::ConnectionHandler)
    }

    fn handle_established_outbound_connection(
        &mut self,
        _: ConnectionId,
        _: PeerId,
        _: &Multiaddr,
        _: Endpoint,
        _: PortUse,
    ) -> std::result::Result<THandler<Self>, ConnectionDenied> {
        Ok(dummy::ConnectionHandler)
    }

    fn on_swarm_event(&mut self, _: FromSwarm) {}
    fn on_connection_handler_event(&mut self, _: PeerId, _: ConnectionId, event: Infallible) {
        match event {}
    }
    fn poll(&mut self, cx: &mut Context<'_>) -> Poll<ToSwarm<Infallible, Infallible>> {
        if let Some(addr) = self.pending.take() {
            return Poll::Ready(ToSwarm::NewExternalAddrCandidate(addr));
        }
        self.waker = Some(cx.waker().clone());
        Poll::Pending
    }
}

#[derive(NetworkBehaviour)]
struct Protocols {
    reporter: CandidateReporter,
    identify: identify::Behaviour,
    v1: Toggle<autonat::v1::Behaviour>,
    v2_client: Toggle<autonat::v2::client::Behaviour>,
    v2_server: Toggle<autonat::v2::server::Behaviour>,
}

struct Behaviour {
    inner: Protocols,
    candidate: Option<Multiaddr>,
    closed: bool,
    connections: HashSet<ConnectionId>,
    error: Option<String>,
    transport: String,
}

impl Behaviour {
    fn admit(&mut self, id: ConnectionId) -> std::result::Result<(), ConnectionDenied> {
        if self.closed {
            return Err(ConnectionDenied::new(io::Error::other("fixture shutting down")));
        }
        if !self.connections.contains(&id) && self.connections.len() == CONNECTION_LIMIT {
            self.error
                .get_or_insert("AutoNAT connection admission overflow".into());
        }
        if let Some(error) = &self.error {
            return Err(ConnectionDenied::new(io::Error::other(error.clone())));
        }
        self.connections.insert(id);
        Ok(())
    }
}

impl NetworkBehaviour for Behaviour {
    type ConnectionHandler = <Protocols as NetworkBehaviour>::ConnectionHandler;
    type ToSwarm = ProtocolsEvent;

    fn handle_pending_inbound_connection(
        &mut self,
        id: ConnectionId,
        local: &Multiaddr,
        remote: &Multiaddr,
    ) -> std::result::Result<(), ConnectionDenied> {
        self.admit(id)?;
        let result = self.inner.handle_pending_inbound_connection(id, local, remote);
        if result.is_err() {
            self.connections.remove(&id);
        }
        result
    }

    fn handle_pending_outbound_connection(
        &mut self,
        id: ConnectionId,
        peer: Option<PeerId>,
        addresses: &[Multiaddr],
        role: Endpoint,
    ) -> std::result::Result<Vec<Multiaddr>, ConnectionDenied> {
        self.admit(id)?;
        for addr in addresses {
            let checked = peer
                .map_or_else(|| Ok(addr.clone()), |p| without_peer(addr.clone(), p))
                .and_then(|addr| address(&addr, &self.transport, false));
            if let Err(error) = checked {
                self.connections.remove(&id);
                return Err(ConnectionDenied::new(io::Error::other(error.to_string())));
            }
        }
        let result = self
            .inner
            .handle_pending_outbound_connection(id, peer, addresses, role)
            .and_then(|extra| {
                for addr in &extra {
                    peer.map_or_else(|| Ok(addr.clone()), |p| without_peer(addr.clone(), p))
                        .and_then(|addr| address(&addr, &self.transport, false))
                        .map_err(|error| ConnectionDenied::new(io::Error::other(error.to_string())))?;
                }
                Ok(extra)
            });
        if result.is_err() {
            self.connections.remove(&id);
        }
        result
    }

    fn handle_established_inbound_connection(
        &mut self,
        id: ConnectionId,
        peer: PeerId,
        local: &Multiaddr,
        remote: &Multiaddr,
    ) -> std::result::Result<THandler<Self>, ConnectionDenied> {
        self.admit(id)?;
        let result = self
            .inner
            .handle_established_inbound_connection(id, peer, local, remote);
        if result.is_err() {
            self.connections.remove(&id);
        }
        result
    }

    fn handle_established_outbound_connection(
        &mut self,
        id: ConnectionId,
        peer: PeerId,
        addr: &Multiaddr,
        role: Endpoint,
        port: PortUse,
    ) -> std::result::Result<THandler<Self>, ConnectionDenied> {
        self.admit(id)?;
        let result = self
            .inner
            .handle_established_outbound_connection(id, peer, addr, role, port);
        if result.is_err() {
            self.connections.remove(&id);
        }
        result
    }

    fn on_swarm_event(&mut self, event: FromSwarm) {
        match event {
            FromSwarm::ConnectionClosed(e) => {
                self.connections.remove(&e.connection_id);
            }
            FromSwarm::DialFailure(e) => {
                self.connections.remove(&e.connection_id);
            }
            FromSwarm::ListenFailure(e) => {
                self.connections.remove(&e.connection_id);
            }
            _ => {}
        }
        self.inner.on_swarm_event(event);
    }

    fn on_connection_handler_event(&mut self, peer: PeerId, id: ConnectionId, event: THandlerOutEvent<Self>) {
        self.inner.on_connection_handler_event(peer, id, event);
    }

    fn poll(&mut self, cx: &mut Context<'_>) -> Poll<ToSwarm<Self::ToSwarm, THandlerInEvent<Self>>> {
        for _ in 0..64 {
            match self.inner.poll(cx) {
                // Identify may also report an ephemeral/translated control address.
                // Tighten this fixture to its one requested listener, never mark a
                // candidate verified or change the donor's public-address policy.
                Poll::Ready(ToSwarm::NewExternalAddrCandidate(addr))
                    if self.closed || self.candidate.as_ref() != Some(&addr) =>
                {
                    continue;
                }
                Poll::Ready(ToSwarm::Dial { .. } | ToSwarm::ListenOn { .. }) if self.closed => continue,
                event => return event,
            }
        }
        cx.waker().wake_by_ref();
        Poll::Pending
    }
}

fn behaviour(key: &identity::Keypair, opts: &Options) -> Behaviour {
    let client = opts.command == "dial";
    let denied = opts.transport == "tcp-pnet" && opts.internet_egress == "deny";
    let v1_config = autonat::v1::Config {
        timeout: Duration::from_secs(10),
        boot_delay: Duration::from_secs(if client { 1 } else { 3600 }),
        retry_interval: Duration::from_secs(if client { 1 } else { 3600 }),
        refresh_interval: Duration::from_secs(3600),
        use_connected: false,
        only_global_ips: true,
        throttle_clients_period: Duration::from_secs(60),
        ..Default::default()
    };
    Behaviour {
        inner: Protocols {
            reporter: CandidateReporter::default(),
            identify: identify::Behaviour::new(
                identify::Config::new_with_signed_peer_record("/forge-interop/0.1.0".into(), key)
                    .with_cache_size(CONNECTION_LIMIT),
            ),
            v1: (!denied && opts.scenario == "autonat_v1")
                .then(|| autonat::v1::Behaviour::new(key.public().to_peer_id(), v1_config))
                .into(),
            v2_client: (!denied && client && opts.scenario == "autonat_v2")
                .then(|| {
                    autonat::v2::client::Behaviour::new(
                        OsRng,
                        autonat::v2::client::Config::default()
                            .with_max_candidates(1)
                            .with_probe_interval(Duration::from_secs(1)),
                    )
                })
                .into(),
            v2_server: (!denied && !client && opts.scenario == "autonat_v2")
                .then(|| autonat::v2::server::Behaviour::new(OsRng))
                .into(),
        },
        candidate: None,
        closed: false,
        connections: HashSet::new(),
        error: None,
        transport: opts.transport.clone(),
    }
}

fn build(opts: &Options, tasks: TaskOwner) -> Result<Swarm<Behaviour>> {
    let key = identity::Keypair::generate_ed25519();
    let config = move |_| {
        libp2p::swarm::Config::with_executor(tasks).with_idle_connection_timeout(Duration::from_secs(60))
    };
    let swarm = match opts.transport.as_str() {
        "quic" => SwarmBuilder::with_existing_identity(key)
            .with_tokio()
            .with_quic()
            .with_behaviour(|key| behaviour(key, opts))?
            .with_swarm_config(config)
            .build(),
        "tcp" => SwarmBuilder::with_existing_identity(key)
            .with_tokio()
            .with_tcp(
                tcp::Config::default().nodelay(true),
                noise::Config::new,
                yamux::Config::default,
            )?
            .with_behaviour(|key| behaviour(key, opts))?
            .with_swarm_config(config)
            .build(),
        "tcp-tls" => SwarmBuilder::with_existing_identity(key)
            .with_tokio()
            .with_tcp(
                tcp::Config::default().nodelay(true),
                tls::Config::new,
                yamux::Config::default,
            )?
            .with_behaviour(|key| behaviour(key, opts))?
            .with_swarm_config(config)
            .build(),
        "tcp-pnet" => {
            let psk = PreSharedKey::from_str(&std::fs::read_to_string(&opts.pnet_key_file)?)?;
            let security = tls::Config::new(&key)?;
            SwarmBuilder::with_existing_identity(key)
                .with_tokio()
                .with_other_transport(move |_| {
                    tcp::tokio::Transport::new(tcp::Config::default().nodelay(true))
                        .and_then(move |socket, _| PnetConfig::new(psk).handshake(socket))
                        .upgrade(Version::V1Lazy)
                        .authenticate(security)
                        .multiplex(yamux::Config::default())
                })?
                .with_behaviour(|key| behaviour(key, opts))?
                .with_swarm_config(config)
                .build()
        }
        _ => return Err("unsupported AutoNAT transport".into()),
    };
    Ok(swarm)
}

struct ConnectionRecord {
    id: ConnectionId,
    peer: PeerId,
    local: Option<Multiaddr>,
    remote: Multiaddr,
    inbound: bool,
    fresh: bool,
    closed: bool,
}

impl ConnectionRecord {
    fn json(&self) -> Value {
        json!({"connection_id": self.id.to_string(), "authenticated_peer": self.peer.to_string(),
            "local_addr": self.local.as_ref().map(ToString::to_string),
            "remote_addr": self.remote.to_string(),
            "direction": if self.inbound { "inbound" } else { "outbound" },
            "authenticated": true, "fresh_requested_inbound": self.fresh, "closed": self.closed})
    }
}

#[derive(Default)]
struct Trace {
    connections: Vec<ConnectionRecord>,
    live: HashMap<ConnectionId, PeerId>,
    incoming_after_arm: HashSet<ConnectionId>,
    listeners: HashSet<ListenerId>,
    events: Vec<Value>,
    error: Option<String>,
    requested: Option<Multiaddr>,
    control: Option<ConnectionId>,
    observer: Option<PeerId>,
    observed_control: Option<Multiaddr>,
    armed: bool,
    candidate_reported: bool,
    identified: bool,
    denied: bool,
    v1_probe: Option<autonat::v1::ProbeId>,
    response: Option<Multiaddr>,
    dial_data: Option<usize>,
    v1_requests: usize,
    v2_results: usize,
}

impl Trace {
    fn event(&mut self, event: Value) {
        if self.events.len() == EVENT_LIMIT || event.to_string().len() > 8192 {
            self.error.get_or_insert("AutoNAT event trace overflow".into());
        } else {
            self.events.push(event);
        }
    }

    fn check(&self, swarm: &Swarm<Behaviour>, tasks: &TaskOwner) -> Result<()> {
        match self
            .error
            .clone()
            .or_else(|| swarm.behaviour().error.clone())
            .or_else(|| tasks.failure())
        {
            Some(error) => Err(error.into()),
            None => Ok(()),
        }
    }

    fn observe(
        &mut self,
        event: &SwarmEvent<ProtocolsEvent>,
        local_peer: PeerId,
        closing: bool,
    ) -> Result<()> {
        match event {
            SwarmEvent::NewExternalAddrCandidate { address }
                if self.armed && self.requested.as_ref() == Some(address) =>
            {
                self.candidate_reported = true;
            }
            SwarmEvent::IncomingConnection { connection_id, .. } if self.armed && !closing => {
                if self.incoming_after_arm.len() == CONNECTION_LIMIT {
                    self.error.get_or_insert("AutoNAT incoming trace overflow".into());
                } else {
                    self.incoming_after_arm.insert(*connection_id);
                }
            }
            SwarmEvent::ConnectionEstablished {
                peer_id,
                connection_id,
                endpoint,
                ..
            } => {
                if self.connections.len() == CONNECTION_TRACE_LIMIT {
                    self.error
                        .get_or_insert("AutoNAT connection trace overflow".into());
                    return Err("AutoNAT connection trace overflow".into());
                }
                let (local, remote, inbound) = match endpoint {
                    ConnectedPoint::Listener {
                        local_addr,
                        send_back_addr,
                    } => (
                        Some(without_peer(local_addr.clone(), local_peer)?),
                        send_back_addr.clone(),
                        true,
                    ),
                    // The public API exposes no outbound local socket endpoint.
                    ConnectedPoint::Dialer { address, .. } => (None, address.clone(), false),
                };
                let new_incoming = self.incoming_after_arm.remove(connection_id);
                let fresh = new_incoming
                    && !closing
                    && inbound
                    && Some(*connection_id) != self.control
                    && local.is_some()
                    && local == self.requested;
                self.connections.push(ConnectionRecord {
                    id: *connection_id,
                    peer: *peer_id,
                    local,
                    remote,
                    inbound,
                    fresh,
                    closed: false,
                });
                self.live.insert(*connection_id, *peer_id);
            }
            SwarmEvent::ConnectionClosed { connection_id, .. } => {
                self.live.remove(connection_id);
                if let Some(record) = self.connections.iter_mut().find(|c| c.id == *connection_id) {
                    record.closed = true;
                }
            }
            SwarmEvent::ListenerClosed {
                listener_id, reason, ..
            } => {
                let owned = self.listeners.remove(listener_id);
                if let Err(error) = reason {
                    return Err(format!("AutoNAT listener closed: {error}").into());
                }
                if owned && !closing {
                    return Err("AutoNAT listener closed before shutdown".into());
                }
            }
            SwarmEvent::ListenerError { error, .. } => {
                return Err(format!("AutoNAT listener error: {error}").into());
            }
            SwarmEvent::Behaviour(ProtocolsEvent::V1(event)) => {
                // ProbeId is opaque in the donor API; its Debug value is only a
                // correlation token. All semantic fields come from typed events.
                let mut evidence = match event {
                    autonat::v1::Event::OutboundProbe(autonat::v1::OutboundProbeEvent::Request {
                        probe_id, peer,
                    }) => json!({"kind": "v1_outbound_request",
                        "probe_id": format!("{probe_id:?}"), "peer": peer.to_string()}),
                    autonat::v1::Event::OutboundProbe(autonat::v1::OutboundProbeEvent::Response {
                        probe_id, peer, address,
                    }) => json!({"kind": "v1_outbound_response",
                        "probe_id": format!("{probe_id:?}"), "peer": peer.to_string(),
                        "address": address.to_string(), "success": true}),
                    autonat::v1::Event::InboundProbe(autonat::v1::InboundProbeEvent::Request {
                        probe_id, peer, addresses,
                    }) => json!({"kind": "v1_inbound_request",
                        "probe_id": format!("{probe_id:?}"), "peer": peer.to_string(),
                        "addresses": addresses.iter().map(ToString::to_string).collect::<Vec<_>>()}),
                    autonat::v1::Event::InboundProbe(autonat::v1::InboundProbeEvent::Response {
                        probe_id, peer, address,
                    }) => json!({"kind": "v1_inbound_response",
                        "probe_id": format!("{probe_id:?}"), "peer": peer.to_string(),
                        "address": address.to_string(), "success": true}),
                    autonat::v1::Event::OutboundProbe(autonat::v1::OutboundProbeEvent::Error {
                        probe_id, peer, error,
                    }) => json!({"kind": "v1_outbound_error",
                        "probe_id": format!("{probe_id:?}"),
                        "peer": peer.map(|p| p.to_string()), "success": false,
                        "error_kind": match error {
                            autonat::v1::OutboundProbeError::NoServer => "NoServer",
                            autonat::v1::OutboundProbeError::NoAddresses => "NoAddresses",
                            autonat::v1::OutboundProbeError::OutboundRequest(_) => "OutboundRequest",
                            autonat::v1::OutboundProbeError::Response(_) => "Response",
                        }}),
                    autonat::v1::Event::InboundProbe(autonat::v1::InboundProbeEvent::Error {
                        probe_id, peer, ..
                    }) => json!({"kind": "v1_inbound_error",
                        "probe_id": format!("{probe_id:?}"), "peer": peer.to_string(),
                        "success": false}),
                    autonat::v1::Event::StatusChanged { .. } => json!({"kind": "v1_status_changed"}),
                };
                evidence["protocol"] = json!(V1);
                evidence["event"] = json!(format!("{event:?}"));
                self.event(evidence);
                if !closing && self.observer.is_some() {
                    match event {
                        autonat::v1::Event::OutboundProbe(autonat::v1::OutboundProbeEvent::Request {
                            probe_id,
                            peer,
                        }) => {
                            if !self.armed || Some(*peer) != self.observer || self.v1_probe.is_some() {
                                return Err("unexpected v1 outbound probe correlation".into());
                            }
                            self.v1_probe = Some(*probe_id);
                            self.v1_requests += 1;
                        }
                        autonat::v1::Event::OutboundProbe(autonat::v1::OutboundProbeEvent::Response {
                            probe_id,
                            peer,
                            address,
                        }) => {
                            let addr = without_peer(address.clone(), local_peer)?;
                            if Some(*probe_id) != self.v1_probe
                                || Some(*peer) != self.observer
                                || self.requested.as_ref() != Some(&addr)
                            {
                                return Err("v1 response peer/probe/address mismatch".into());
                            }
                            self.response = Some(addr);
                        }
                        autonat::v1::Event::OutboundProbe(autonat::v1::OutboundProbeEvent::Error {
                            peer: None,
                            error: autonat::v1::OutboundProbeError::NoServer,
                            ..
                        }) => {}
                        autonat::v1::Event::OutboundProbe(autonat::v1::OutboundProbeEvent::Error {
                            error,
                            ..
                        }) => return Err(format!("v1 probe failed: {error:?}").into()),
                        _ => {}
                    }
                }
            }
            SwarmEvent::Behaviour(ProtocolsEvent::V2Client(event)) => {
                self.event(
                    json!({"protocol": V2, "kind": "v2_client_result",
                    "tested_addr": event.tested_addr.to_string(), "success": event.result.is_ok(),
                    "server": event.server.to_string(), "bytes_sent": event.bytes_sent,
                    "result": format!("{:?}", event.result)}),
                );
                self.v2_results += 1;
                if !closing {
                    if !self.armed
                        || Some(event.server) != self.observer
                        || self.requested.as_ref() != Some(&event.tested_addr)
                    {
                        return Err("v2 result server/address mismatch".into());
                    }
                    if let Err(error) = &event.result {
                        return Err(format!("v2 probe failed: {error}").into());
                    }
                    self.response = Some(event.tested_addr.clone());
                    self.dial_data = Some(event.bytes_sent);
                }
            }
            SwarmEvent::Behaviour(ProtocolsEvent::V2Server(event)) => {
                self.event(json!({"protocol": V2, "kind": "v2_server_result",
                    "success": event.result.is_ok(), "client": event.client.to_string(),
                    "tested_addr": event.tested_addr.to_string(), "data_amount": event.data_amount,
                    "result": format!("{:?}", event.result)}));
            }
            SwarmEvent::IncomingConnectionError {
                connection_id, error, ..
            } => {
                self.incoming_after_arm.remove(connection_id);
                self.event(json!({"event": "incoming_connection_error", "error": format!("{error:?}")}));
            }
            SwarmEvent::OutgoingConnectionError { error, .. } => {
                self.event(json!({"event": "outgoing_connection_error", "error": format!("{error:?}")}));
            }
            _ => {}
        }
        Ok(())
    }

    fn reached(&self) -> bool {
        self.response.is_some() && self.connections.iter().any(|c| c.fresh)
    }
}

fn validate(opts: &Options) -> Result<(IpAddr, Multiaddr)> {
    if !matches!(opts.command.as_str(), "listen" | "dial") {
        return Err("AutoNAT command must be listen or dial".into());
    }
    if opts.result_file.as_os_str().is_empty()
        || (opts.command == "listen"
            && (opts.ready_file.as_os_str().is_empty() || opts.stop_file.as_os_str().is_empty()))
    {
        return Err("AutoNAT requires result-file and, for listen, ready-file/stop-file".into());
    }
    if !matches!(opts.internet_egress.as_str(), "" | "allow" | "deny") {
        return Err("internet-egress must be allow or deny".into());
    }
    if opts.transport == "tcp-pnet"
        && (opts.internet_egress.is_empty() || opts.pnet_key_file.as_os_str().is_empty())
    {
        return Err("tcp-pnet requires explicit internet-egress and pnet-key-file".into());
    }
    let bind: IpAddr = opts.bind_ip.parse()?;
    let default = listen_address(bind, &opts.transport);
    address(&default, &opts.transport, true)?;
    let requested = if opts.probe_addr.is_empty() {
        default
    } else {
        opts.probe_addr.parse()?
    };
    let probe_ip = address(&requested, &opts.transport, true)?;
    if (opts.scenario == "autonat_v1" || opts.command == "listen") && probe_ip != bind {
        return Err("v1 and service listeners require probe IP equal to bind IP".into());
    }
    Ok((bind, requested))
}

async fn exchange(
    opts: &Options,
    swarm: &mut Swarm<Behaviour>,
    trace: &mut Trace,
    tasks: &TaskOwner,
    result: &mut Value,
) -> Result<()> {
    let (bind, requested) = validate(opts)?;
    let client = opts.command == "dial";
    let denied = opts.transport == "tcp-pnet" && opts.internet_egress == "deny";
    let protocol = if opts.scenario == "autonat_v1" { V1 } else { V2 };
    let observer = if client {
        let peer: PeerId = opts.peer_id.parse()?;
        let addr = without_peer(opts.addr.parse()?, peer)?;
        address(&addr, &opts.transport, false)?;
        if peer == *swarm.local_peer_id() {
            return Err("AutoNAT observer cannot be self".into());
        }
        trace.observer = Some(peer);
        Some((peer, addr))
    } else {
        None
    };
    let listener = swarm.listen_on(requested)?;
    trace.listeners.insert(listener);
    let deadline = Instant::now()
        + if client {
            EXCHANGE_TIMEOUT
        } else {
            Duration::from_secs(120)
        };
    let mut tick = tokio::time::interval(Duration::from_millis(100));
    let mut ready = false;
    loop {
        if Instant::now() >= deadline {
            return Err("AutoNAT exchange deadline expired".into());
        }
        trace.check(swarm, tasks)?;
        if client && trace.reached() {
            return Ok(());
        }
        tokio::select! {
            _ = tokio::time::sleep_until(deadline) => return Err("AutoNAT exchange/stop-file deadline expired".into()),
            _ = tick.tick() => {
                if opts.stop_file.exists() {
                    if !client && ready { return Ok(()); }
                    return Err("AutoNAT client cancelled by stop-file".into());
                }
            }
            event = swarm.next() => {
                let event = event.ok_or("AutoNAT Swarm ended unexpectedly")?;
                trace.observe(&event, *swarm.local_peer_id(), false)?;
                match event {
                    SwarmEvent::NewListenAddr { listener_id, address: addr } if listener_id == listener && !ready => {
                        address(&addr, &opts.transport, false)?;
                        trace.requested = Some(addr.clone());
                        result["listen_addr"] = json!(addr.to_string());
                        ready = true;
                        if let Some((peer, observer_addr)) = &observer {
                            let dial = DialOpts::peer_id(*peer).addresses(vec![observer_addr.clone()])
                                .allocate_new_port().build();
                            trace.control = Some(dial.connection_id());
                            swarm.dial(dial)?;
                        } else {
                            result["donor_api_basis"] = json!("configured_donor_service_not_client_reachability");
                            let service_enabled = swarm.behaviour().inner.v1.is_enabled()
                                || swarm.behaviour().inner.v2_server.is_enabled();
                            write_json(&opts.ready_file, json!({"implementation": "rust", "role": "listener",
                                "scenario": opts.scenario, "peer_id": swarm.local_peer_id().to_string(),
                                "addr": addr.to_string(), "transport": opts.transport,
                                "service_enabled": service_enabled, "protocol": protocol,
                                "dialback_identity_basis": "same_swarm_identity_new_connection"}))?;
                        }
                    }
                    SwarmEvent::Behaviour(ProtocolsEvent::Identify(identify::Event::Received {
                        connection_id, peer_id, info, .. }))
                        if client && Some(connection_id) == trace.control && !trace.identified => {
                        if Some(peer_id) != trace.observer || info.public_key.to_peer_id() != peer_id
                            || trace.live.get(&connection_id) != Some(&peer_id) {
                            return Err("AutoNAT Identify/control authentication mismatch".into());
                        }
                        trace.identified = true;
                        trace.observed_control = Some(info.observed_addr.clone());
                        if denied {
                            trace.denied = true;
                            result["donor_api_basis"] = json!("fixtureegresspolicy_after_authenticated_control");
                            result["policy_basis"] = json!("fixture_policy_not_donor_feature");
                            return Ok(());
                        }
                        if address(&without_peer(info.observed_addr, *swarm.local_peer_id())?, &opts.transport, false)? != bind {
                            return Err("observer-reported control source differs from bind-ip; check namespace routing".into());
                        }
                        if !info.protocols.iter().any(|p| p.as_ref() == protocol) {
                            return Err("authenticated observer does not advertise requested AutoNAT protocol".into());
                        }
                        let candidate = trace.requested.clone().ok_or("missing actual listener")?;
                        trace.armed = true;
                        swarm.behaviour_mut().candidate = Some(candidate.clone());
                        if let Some(v1) = swarm.behaviour_mut().inner.v1.as_mut() {
                            // Explicit servers bypass donor client selection's IP filter;
                            // the direct control address was independently checked above.
                            v1.add_server(peer_id, None);
                            v1.probe_address(candidate);
                            result["donor_api_basis"] = json!("v1::Behaviour::OutboundProbe::Response: donor_verified_result");
                        } else {
                            swarm.behaviour_mut().inner.reporter.report(candidate);
                            result["donor_api_basis"] = json!("v2::client::Behaviour::Event: donor_verified_result");
                        }
                    }
                    SwarmEvent::OutgoingConnectionError { connection_id, error, .. }
                        if Some(connection_id) == trace.control => return Err(format!("control dial failed: {error}").into()),
                    SwarmEvent::ConnectionClosed { connection_id, .. }
                        if client && Some(connection_id) == trace.control && !trace.reached() =>
                            return Err("control connection closed before AutoNAT completion".into()),
                    _ => {}
                }
            }
        }
    }
}

async fn shutdown(
    mut swarm: Swarm<Behaviour>,
    trace: &mut Trace,
    tasks: &TaskOwner,
    result: &mut Value,
) -> Result<()> {
    let deadline = Instant::now() + DRAIN_TIMEOUT;
    swarm.behaviour_mut().closed = true;
    swarm.behaviour_mut().inner.reporter.pending = None;
    for id in trace.listeners.iter().copied().collect::<Vec<_>>() {
        swarm.remove_listener(id);
    }
    for peer in swarm.connected_peers().copied().collect::<Vec<_>>() {
        let _ = swarm.disconnect_peer_id(peer);
    }
    let mut failure = None;
    while swarm.network_info().connection_counters().num_connections() != 0
        || !trace.listeners.is_empty()
        || !trace.live.is_empty()
        || !trace.incoming_after_arm.is_empty()
    {
        if Instant::now() >= deadline {
            failure.get_or_insert("AutoNAT drain deadline expired".into());
            break;
        }
        match timeout_at(deadline, swarm.next()).await {
            Ok(Some(event)) => {
                if let SwarmEvent::ConnectionEstablished { connection_id, .. } = &event {
                    swarm.close_connection(*connection_id);
                }
                if let Err(error) = trace.observe(&event, *swarm.local_peer_id(), true) {
                    failure.get_or_insert(error.to_string());
                }
            }
            _ => {
                failure.get_or_insert("AutoNAT connection/listener drain timed out or ended".into());
                break;
            }
        }
    }
    result["connection_drain"] = json!(
        trace.listeners.is_empty()
            && trace.live.is_empty()
            && trace.incoming_after_arm.is_empty()
            && swarm.network_info().connection_counters().num_connections() == 0
    );
    if let Err(error) = trace.check(&swarm, tasks) {
        failure.get_or_insert(error.to_string());
    }
    drop(swarm);
    result["swarm_closed"] = json!(true);
    let joined = tasks.join(deadline).await;
    result["joined_swarm_executor"] = json!(joined.is_ok());
    if let Err(error) = joined {
        failure.get_or_insert(error.to_string());
    }
    if result["connection_drain"] != true {
        failure.get_or_insert("incomplete AutoNAT connection drain".into());
    }
    match failure {
        Some(error) => Err(error.into()),
        None => Ok(()),
    }
}

pub(super) async fn run(opts: Options) -> Result<()> {
    let tasks = TaskOwner::default();
    let mut trace = Trace::default();
    let client = opts.command == "dial";
    let mut result = json!({"implementation": "rust", "scenario": opts.scenario,
        "role": if client { "dialer" } else { "listener" }, "transport": opts.transport,
        "version": if opts.scenario == "autonat_v1" { 1 } else { 2 },
        "protocol": if opts.scenario == "autonat_v1" { V1 } else { V2 },
        "internet_egress": opts.internet_egress, "status": "error", "reached": false,
        "requested_addr": null, "response_addr": null,
        "nonce_proof": if opts.scenario == "autonat_v1" { "not_applicable_v1" } else { "not_exposed_by_donor_api" },
        "dialback_source_proof": "donor_verified_result_plus_fresh_requested_inbound; counterpart_pairing_required",
        "response_addr_basis": if opts.scenario == "autonat_v1" { "matched_response_event" } else { "tested_addr_from_verified_event_not_raw_wire_response" },
        "service_enabled": false,
        "dialback_identity_basis": "Rust same_swarm_identity_new_connection; different donor identity also accepted",
        "donor_revision": "22fb4c784fc55ad8b15d05fdc9f98d663107d4cb",
        "swarm_closed": false, "connection_drain": false, "joined_swarm_executor": false,
        "lifecycle_scope": "Swarm connections and fixture executor only; Quinn internal tasks not exposed; owner must join process"});
    let outcome = match validate(&opts).and_then(|_| build(&opts, tasks.clone())) {
        Ok(mut swarm) => {
            result["peer_id"] = json!(swarm.local_peer_id().to_string());
            result["service_enabled"] = json!(
                swarm.behaviour().inner.v1.is_enabled()
                    || swarm.behaviour().inner.v2_server.is_enabled()
            );
            let exchange = exchange(&opts, &mut swarm, &mut trace, &tasks, &mut result).await;
            let cleanup = shutdown(swarm, &mut trace, &tasks, &mut result).await;
            if let Err(error) = &cleanup {
                result["cleanup_error"] = json!(error.to_string());
            }
            exchange.and(cleanup)
        }
        Err(error) => {
            let joined = tasks.join(Instant::now() + DRAIN_TIMEOUT).await;
            result["joined_swarm_executor"] = json!(joined.is_ok());
            Err(error)
        }
    };
    let fresh: Vec<_> = trace
        .connections
        .iter()
        .filter(|c| c.fresh)
        .map(ConnectionRecord::json)
        .collect();
    result["status"] = json!(if outcome.is_err() {
        "error"
    } else if trace.denied {
        "rejected"
    } else {
        "ok"
    });
    result["reached"] = json!(outcome.is_ok() && trace.reached());
    result["requested_addr"] = json!(if client {
        trace.requested.as_ref().map(ToString::to_string)
    } else {
        None
    });
    result["response_addr"] = json!(trace.response.as_ref().map(ToString::to_string));
    result["authenticated_peer"] = json!(
        trace
            .identified
            .then(|| trace.observer.map(|p| p.to_string()))
            .flatten()
    );
    result["control_connection_id"] = json!(trace.control.map(|id| id.to_string()));
    result["observer_reported_control_addr"] =
        json!(trace.observed_control.as_ref().map(ToString::to_string));
    result["fresh_inbound_count"] = json!(fresh.len());
    result["fresh_inbound_connections"] = json!(fresh);
    result["actual_connections"] = json!(
        trace
            .connections
            .iter()
            .map(ConnectionRecord::json)
            .collect::<Vec<_>>()
    );
    result["donor_events"] = json!(trace.events);
    result["trace_overflow_error"] = json!(trace.error);
    result["trace_complete"] = json!(outcome.is_ok());
    result["dial_data_bytes"] = json!(trace.dial_data);
    result["v1_request_events"] = json!(trace.v1_requests);
    result["v2_result_events"] = json!(trace.v2_results);
    result["candidate_reported"] = json!(trace.candidate_reported);
    result["autonat_probe_api_calls"] = if trace.denied { json!(0) } else { Value::Null };
    result["probe_attempt_count_basis"] = json!("v1 request events exact; v2 internal attempts not exposed");
    if let Err(error) = &outcome {
        result["error"] = json!(error.to_string());
    }
    if !opts.result_file.as_os_str().is_empty() {
        write_json(&opts.result_file, result)?;
    }
    outcome
}
