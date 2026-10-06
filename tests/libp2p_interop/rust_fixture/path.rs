//! Passive native path collection and actual stream exchange. DCUtR I/O,
//! application dial requests and authenticated muxer owners are captured here.
//! Successful Rust cases explicitly retire only the authenticated inner relay
//! before a fresh echo. This does NOT prove donor automatic direct preference:
//! pinned libp2p_stream chooses randomly among the peer's live connections.
//! A source-side native dial failure proves only its captured DCUtR wire wave,
//! never aggregate DCUtR completion or a join of the native Rust behaviour.
use std::{
    collections::BTreeMap,
    io,
    path::{Path, PathBuf},
    pin::Pin,
    sync::atomic::{AtomicU64, Ordering},
    sync::{Arc, Mutex},
    task::{Context, Poll},
    time::{Duration, Instant},
};

use futures::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt, StreamExt};
use libp2p::{
    Multiaddr, PeerId, StreamProtocol, Swarm, Transport,
    core::{
        ConnectedPoint, Endpoint,
        muxing::{StreamMuxer, StreamMuxerBox, StreamMuxerEvent},
        transport::{Boxed, DialOpts, ListenerId, PortUse, TransportEvent},
        upgrade::Version,
    },
    dcutr, identify, identity,
    multiaddr::Protocol,
    noise, ping, relay,
    swarm::{
        ConnectionDenied, ConnectionId, FromSwarm, NetworkBehaviour, SwarmEvent, THandler,
        THandlerInEvent, THandlerOutEvent, ToSwarm, behaviour::toggle::Toggle,
    },
    yamux,
};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};

#[path = "path_application.rs"]
mod application;

pub(crate) const ECHO_PROTOCOL: &str = "/forge/interop/path-echo/1";
const EVENT_LIMIT: usize = 256;
const WAVE_LIMIT: usize = 16;

#[derive(Clone, Debug)]
pub(crate) struct Observer(Arc<Inner>);

#[derive(Debug)]
struct Inner {
    token: String,
    local: PeerId,
    origin: Instant,
    dials: AtomicU64,
    application_dials: AtomicU64,
    transports: AtomicU64,
    streams: AtomicU64,
    state: Mutex<State>,
}

#[derive(Debug, Default)]
struct State {
    expected: Option<PeerId>,
    events: Vec<Value>,
    overflow: bool,
    sessions: Vec<(String, PeerId, ConnectedPoint, bool)>,
    retirement: Option<RelayRetirement>,
    application_error: Option<String>,
    waves: BTreeMap<ConnectionId, NativeWave>,
}

#[derive(Clone, Copy, Debug)]
struct EventStamp {
    sequence: u64,
    mono_ns: u64,
}

impl EventStamp {
    fn captured(event: &Value) -> io::Result<Self> {
        Ok(Self {
            sequence: event["sequence"]
                .as_u64()
                .filter(|n| *n > 0)
                .ok_or_else(|| io::Error::other("missing captured event sequence"))?,
            mono_ns: event["mono_ns"]
                .as_u64()
                .filter(|n| *n > 0)
                .ok_or_else(|| io::Error::other("missing captured event time"))?,
        })
    }
}

#[derive(Clone, Debug)]
struct LiveConnection {
    native: ConnectionId,
    transport: String,
    peer: PeerId,
    point: ConnectedPoint,
}

enum ApplicationOpening {
    Control(u64, u64, Vec<String>),
    ExactRelay {
        before: u64,
        after: u64,
        owner: LiveConnection,
        requested: EventStamp,
    },
}

#[derive(Clone, Debug)]
struct NativeWave {
    relay: LiveConnection,
    stream: String,
    frames: [EventStamp; 3],
    advertised: Vec<Multiaddr>,
    requested: Option<EventStamp>,
    pending: Option<EventStamp>,
    candidates: Vec<Multiaddr>,
    failed: bool,
    invalid: bool,
}

impl NativeWave {
    fn receipt(&self) -> Value {
        json!({
            "relay_native_connection_id": self.relay.native.to_string(),
            "relay_connection_id": self.relay.transport,
            "stream_id": self.stream, "remote_peer_id": self.relay.peer.to_string(),
            "connect_read_sequence": self.frames[0].sequence,
            "connect_read_mono_ns": self.frames[0].mono_ns,
            "connect_write_sequence": self.frames[1].sequence,
            "connect_write_mono_ns": self.frames[1].mono_ns,
            "sync_read_sequence": self.frames[2].sequence,
            "sync_read_mono_ns": self.frames[2].mono_ns,
            "advertised_addresses": self.advertised.iter().map(ToString::to_string).collect::<Vec<_>>(),
            "requested_sequence": self.requested.map(|stamp| stamp.sequence),
            "requested_mono_ns": self.requested.map(|stamp| stamp.mono_ns),
            "pending_sequence": self.pending.map(|stamp| stamp.sequence),
            "pending_mono_ns": self.pending.map(|stamp| stamp.mono_ns),
            "candidate_addresses": self.candidates.iter().map(ToString::to_string).collect::<Vec<_>>(),
            "binding_basis": "native_dcutr_poll_id_pending_callback_and_original_authenticated_relay_wire",
            "proof_scope": "native_wave_only",
            "aggregate_dcutr_completed": false, "rust_behaviour_joined": false,
        })
    }
}

#[derive(Clone, Debug)]
struct RelayRetirement {
    relay: LiveConnection,
    direct: LiveConnection,
    carrier: LiveConnection,
    stream: String,
    success: EventStamp,
    relay_before: EventStamp,
    requested: Option<EventStamp>,
    closed: Option<EventStamp>,
}

impl RelayRetirement {
    fn receipt(&self) -> Value {
        json!({
            "native_connection_id": self.relay.native.to_string(),
            "connection_id": self.relay.transport, "stream_id": self.stream,
            "remote_peer_id": self.relay.peer.to_string(),
            "direct_native_connection_id": self.direct.native.to_string(),
            "direct_connection_id": self.direct.transport,
            "carrier_native_connection_id": self.carrier.native.to_string(),
            "carrier_connection_id": self.carrier.transport,
            "carrier_remote_peer_id": self.carrier.peer.to_string(),
            "carrier_binding_basis": "unique_live_authenticated_carrier_for_native_circuit_route",
            "native_success_sequence": self.success.sequence,
            "native_success_mono_ns": self.success.mono_ns,
            "relay_before_sequence": self.relay_before.sequence,
            "relay_before_mono_ns": self.relay_before.mono_ns,
            "requested_sequence": self.requested.map(|stamp| stamp.sequence),
            "requested_mono_ns": self.requested.map(|stamp| stamp.mono_ns),
            "closed_sequence": self.closed.map(|stamp| stamp.sequence),
            "closed_mono_ns": self.closed.map(|stamp| stamp.mono_ns),
            "selection_policy": "success_only_explicit_inner_relay_retirement",
            "automatic_direct_preference_proven": false,
        })
    }
}

// Values come from native transport/muxer output and the actual opened stream,
// never from a configured address or an aggregate connections counter.
#[derive(Debug)]
pub(crate) struct StreamBinding {
    pub connection: String,
    pub stream: String,
    pub relayed: bool,
}

impl Observer {
    pub(crate) fn new(token: String, local: PeerId) -> Result<Self, &'static str> {
        if token.len() != 32
            || !token
                .bytes()
                .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
        {
            return Err("invalid path token");
        }
        Ok(Self(Arc::new(Inner {
            token,
            local,
            origin: Instant::now(),
            dials: AtomicU64::new(0),
            application_dials: AtomicU64::new(0),
            transports: AtomicU64::new(0),
            streams: AtomicU64::new(0),
            state: Mutex::new(State::default()),
        })))
    }

    pub(crate) fn record(&self, kind: &str, source: &str, fields: Value) {
        let _ = self.record_stamped(kind, source, fields);
    }

    fn record_stamped(&self, kind: &str, source: &str, mut fields: Value) -> Option<EventStamp> {
        let mut state = self.0.state.lock().unwrap_or_else(|e| e.into_inner());
        if !source.starts_with("rust.")
            || !fields.is_object()
            || fields.to_string().len() > 32 * 1024
            || state.events.len() == EVENT_LIMIT
        {
            state.overflow = true;
            return None;
        }
        let stamp = EventStamp {
            sequence: (state.events.len() + 1) as u64,
            mono_ns: (self.0.origin.elapsed().as_nanos() + 1) as u64,
        };
        fields["kind"] = json!(kind);
        fields["source"] = json!(source);
        fields["sequence"] = json!(stamp.sequence);
        fields["mono_ns"] = json!(stamp.mono_ns);
        if kind == "relay_retirement_requested" && source == "rust.swarm.close_connection" {
            fields["requested_sequence"] = json!(stamp.sequence);
            fields["requested_mono_ns"] = json!(stamp.mono_ns);
        }
        if kind == "native_dcutr_dial_requested"
            && source == "rust.dcutr.Behaviour.poll.ToSwarm.Dial"
        {
            fields["requested_sequence"] = json!(stamp.sequence);
            fields["requested_mono_ns"] = json!(stamp.mono_ns);
        }
        if kind == "native_dcutr_dial_options"
            && source == "rust.dcutr.Behaviour.handle_pending_outbound_connection"
        {
            fields["pending_sequence"] = json!(stamp.sequence);
            fields["pending_mono_ns"] = json!(stamp.mono_ns);
        }
        state.events.push(fields);
        Some(stamp)
    }

    pub(crate) fn bind(
        &self,
        expected: PeerId,
        actual_direct_ids: &[ConnectionId],
    ) -> Result<(), &'static str> {
        {
            let mut state = self.0.state.lock().unwrap_or_else(|e| e.into_inner());
            if state.expected.is_some() {
                return Err("path peer already bound");
            }
            state.expected = Some(expected);
        }
        self.record("baseline", "rust.swarm.connections", json!({
            "remote_peer_id": expected.to_string(),
            "direct_connection_ids": actual_direct_ids.iter().map(ToString::to_string).collect::<Vec<_>>()
        }));
        if !actual_direct_ids.is_empty() {
            return Err("preexisting direct connection");
        }
        Ok(())
    }

    pub(crate) fn native_event(&self, event: &dcutr::Event) {
        // No operation_finished/attempt_started/frame is manufactured here.
        // The pinned public event exposes completion/ConnectionId but not RTT,
        // frame bytes or the actual transport's coordinated listener ownership.
        let result = match &event.result {
            Ok(id) => json!({"connection_id": id.to_string(), "native_success": true}),
            Err(error) => {
                json!({"native_success": false, "error": error.to_string().chars().take(256).collect::<String>()})
            }
        };
        self.record(
            "native_dcutr_event",
            "rust.dcutr.behaviour",
            json!({
                "remote_peer_id": event.remote_peer_id.to_string(), "result": result
            }),
        );
    }

    pub(crate) fn result(&self, finalized: bool, joined: bool, error: Option<&str>) -> Value {
        let state = self.0.state.lock().unwrap_or_else(|e| e.into_inner());
        json!({"schema_version": 1, "implementation": "rust", "local_peer_id": self.0.local.to_string(),
               "case_token": self.0.token, "events": state.events, "overflow": state.overflow,
               "finalized": finalized, "joined": joined, "error": error})
    }
}

async fn read_frame<S: AsyncRead + Unpin>(io: &mut S) -> io::Result<Vec<u8>> {
    let mut header = [0u8; 4];
    io.read_exact(&mut header).await?;
    let size = u32::from_be_bytes(header) as usize;
    if size == 0 || size > 128 {
        return Err(io::Error::other("path frame bound"));
    }
    let mut value = vec![0; size];
    io.read_exact(&mut value).await?;
    Ok(value)
}

async fn write_frame<S: AsyncWrite + Unpin>(io: &mut S, value: &[u8]) -> io::Result<()> {
    if value.is_empty() || value.len() > 128 {
        return Err(io::Error::other("path frame bound"));
    }
    io.write_all(&(value.len() as u32).to_be_bytes()).await?;
    io.write_all(value).await?;
    io.flush().await
}

#[derive(NetworkBehaviour)]
struct PathBehaviour {
    relay_client: relay::client::Behaviour,
    relay_service: Toggle<relay::Behaviour>,
    dcutr: Toggle<PathDcutr>,
    identify: identify::Behaviour,
    ping: ping::Behaviour,
    stream: PathStreams,
    application: application::Behaviour,
}

fn native_behaviour(
    key: &identity::Keypair,
    role: &str,
    relay_client: relay::client::Behaviour,
    observer: Observer,
) -> PathBehaviour {
    let local = key.public().to_peer_id();
    PathBehaviour {
        relay_client,
        relay_service: Toggle::from(
            (role == "relay").then(|| relay::Behaviour::new(local, relay::Config::default())),
        ),
        dcutr: Toggle::from((role != "relay").then(|| PathDcutr {
            inner: dcutr::Behaviour::new(local),
            observer: observer.clone(),
            relays: BTreeMap::new(),
        })),
        identify: identify::Behaviour::new(identify::Config::new(
            "/forge/interop/path-identify/1".into(),
            key.public(),
        )),
        ping: ping::Behaviour::new(ping::Config::new()),
        stream: PathStreams {
            inner: libp2p_stream::Behaviour::new(),
            observer,
        },
        application: application::Behaviour::new(StreamProtocol::new(ECHO_PROTOCOL)),
    }
}

#[derive(Clone)]
struct Trace {
    observer: Observer,
    connection: String,
    remote: PeerId,
    relayed: bool,
}

impl Observer {
    fn output<M: StreamMuxer + Send + 'static>(
        &self,
        peer: PeerId,
        muxer: M,
        point: ConnectedPoint,
        relayed: bool,
    ) -> (PeerId, StreamMuxerBox)
    where
        M::Substream: Send + Unpin + 'static,
        M::Error: Send + Sync + 'static,
    {
        let mut state = self.0.state.lock().unwrap_or_else(|e| e.into_inner());
        if state.sessions.len() == 16 {
            state.overflow = true;
            return (peer, StreamMuxerBox::new(muxer));
        }
        // Closing a session must not make a later actual output reuse its ID.
        let id = format!(
            "transport-{}",
            self.0.transports.fetch_add(1, Ordering::SeqCst) + 1
        );
        state
            .sessions
            .push((id.clone(), peer, point.clone(), relayed));
        drop(state);
        let (local, remote, direction, role) = match &point {
            ConnectedPoint::Dialer { address, .. } => (
                None,
                address.to_string(),
                "outbound",
                // Pinned QUIC Listener + Reuse also calls Quinn connect_with.
                // The requested endpoint role is not the TLS handshake role.
                "client",
            ),
            ConnectedPoint::Listener {
                local_addr,
                send_back_addr,
            } => (
                Some(local_addr.to_string()),
                send_back_addr.to_string(),
                "inbound",
                "server",
            ),
        };
        let relay_peer = if relayed {
            [local.as_deref(), Some(remote.as_str())]
                .into_iter()
                .flatten()
                .find_map(|address| {
                    let address: Multiaddr = address.parse().ok()?;
                    let mut previous = None;
                    for p in address.iter() {
                        match p {
                            Protocol::P2p(peer) => previous = Some(peer.to_string()),
                            Protocol::P2pCircuit => return previous,
                            _ => {}
                        }
                    }
                    None
                })
        } else {
            None
        };
        self.record("authenticated_connection", "rust.native_transport.authenticated_output", json!({
            "connection_id": id, "remote_peer_id": peer.to_string(), "authenticated": true,
            "path": if relayed { "relay" } else { "direct" }, "local_address": local, "remote_address": remote,
            "direction": direction, "security": if relayed { "/noise" } else { "/tls/1.0.0" },
            "transport": if relayed { "circuit" } else { "quic" }, "muxer": if relayed { Some("/yamux/1.0.0") } else { None },
            "authentication_basis": if relayed { "native_relay_inner_upgrade" } else { "native_quic_authenticated_output" },
            "relay_peer_id": relay_peer
            , "endpoint": super::upgrade_observer::endpoint(&point)
        }));
        if !relayed {
            self.record("native_coordinated_authenticated", "rust.native_quic.output_role", json!({
                "connection_id": id, "remote_peer_id": peer.to_string(), "authenticated": true, "security_role": role,
                "security_role_basis": "pinned_quic_authenticated_transport_output",
                "requested_role": match &point { ConnectedPoint::Dialer { role_override, .. } =>
                    if *role_override == Endpoint::Dialer { "dialer" } else { "listener" },
                    ConnectedPoint::Listener { .. } => "listener" }
            }));
        }
        (
            peer,
            StreamMuxerBox::new(PathMuxer {
                inner: Box::pin(muxer),
                trace: Trace {
                    observer: self.clone(),
                    connection: id,
                    remote: peer,
                    relayed,
                },
            }),
        )
    }

    fn existing(&self, peer: PeerId, relayed: bool) -> Vec<String> {
        self.0
            .state
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .sessions
            .iter()
            .filter(|(_, remote, _, circuit)| *remote == peer && *circuit == relayed)
            .map(|(id, _, _, _)| id.clone())
            .collect()
    }

    fn authenticated_carrier(&self, peer: PeerId, point: &ConnectedPoint) -> io::Result<String> {
        let state = self.0.state.lock().unwrap_or_else(|e| e.into_inner());
        let mut matches = state
            .sessions
            .iter()
            .filter(|(_, remote, endpoint, relayed)| {
                *remote == peer && endpoint == point && !relayed
            });
        let connection = matches.next().ok_or_else(|| {
            io::Error::other("Identify lacks its actual authenticated QUIC carrier")
        })?;
        if matches.next().is_some() {
            return Err(io::Error::other("ambiguous authenticated Identify carrier"));
        }
        Ok(connection.0.clone())
    }

    fn closed(&self, peer: PeerId, point: &ConnectedPoint) {
        self.0
            .state
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .sessions
            .retain(|(_, remote, endpoint, _)| *remote != peer || endpoint != point);
    }

    fn resolve_echo(&self, peer: PeerId, phase: &str) -> io::Result<StreamBinding> {
        let state = self.0.state.lock().unwrap_or_else(|e| e.into_inner());
        let matches = state
            .events
            .iter()
            .filter(|e| {
                e["kind"] == "application_frame"
                    && e["remote_peer_id"] == peer.to_string()
                    && e["phase"] == phase
            })
            .collect::<Vec<_>>();
        if matches.len() != 2
            || matches[0]["stream_id"] != matches[1]["stream_id"]
            || matches[0]["connection_id"] != matches[1]["connection_id"]
            || matches[0]["direction"] == matches[1]["direction"]
        {
            return Err(io::Error::other(
                "ambiguous/missing native muxer application I/O binding",
            ));
        }
        Ok(StreamBinding {
            connection: matches[0]["connection_id"].as_str().unwrap().into(),
            stream: matches[0]["stream_id"].as_str().unwrap().into(),
            relayed: matches[0]["path"] == "relay",
        })
    }

    async fn transfer(
        &self,
        stream: &mut libp2p::Stream,
        peer: PeerId,
        phase: Option<&str>,
        server: bool,
        opening: Option<ApplicationOpening>,
    ) -> io::Result<String> {
        let before = self.0.dials.load(Ordering::SeqCst);
        let expected = phase.map(|phase| format!("path:{}:{phase}", self.0.token).into_bytes());
        let bytes = tokio::time::timeout(
            Duration::from_secs(if server && phase == Some("relay_after") {
                40
            } else {
                3
            }),
            async {
                if server {
                    let bytes = read_frame(stream).await?;
                    if expected.as_ref().is_some_and(|expected| *expected != bytes) {
                        return Err(io::Error::other("challenge mismatch"));
                    }
                    write_frame(stream, &bytes).await?;
                    Ok(bytes)
                } else {
                    let bytes = expected
                        .as_ref()
                        .ok_or_else(|| io::Error::other("missing application challenge"))?;
                    write_frame(stream, bytes).await?;
                    read_frame(stream).await
                }
            },
        )
        .await
        .map_err(|_| io::Error::new(io::ErrorKind::TimedOut, "path echo deadline"))??;
        let prefix = format!("path:{}:", self.0.token);
        let phase = std::str::from_utf8(&bytes)
            .ok()
            .and_then(|text| text.strip_prefix(&prefix))
            .filter(|phase| matches!(*phase, "relay_before" | "relay_after" | "direct_after"))
            .ok_or_else(|| io::Error::other("invalid native application challenge"))?;
        if expected.as_ref().is_some_and(|expected| *expected != bytes) {
            return Err(io::Error::other("challenge mismatch"));
        }
        let binding = self.resolve_echo(peer, phase)?;
        if (phase == "direct_after") == binding.relayed {
            return Err(io::Error::other("native stream used wrong path"));
        }
        let after = self.0.dials.load(Ordering::SeqCst);
        if let Some(opening) = opening {
            let (dial_before, dial_after, ids, exact) = match opening {
                ApplicationOpening::Control(before, after, ids) => (before, after, ids, None),
                ApplicationOpening::ExactRelay {
                    before,
                    after,
                    owner,
                    requested,
                } => (
                    before,
                    after,
                    vec![owner.transport.clone()],
                    Some((owner, requested)),
                ),
            };
            if dial_before != dial_after || !ids.contains(&binding.connection) {
                return Err(io::Error::other("application open dialed/replaced owner"));
            }
            let mut fields = json!({
                "connection_id": binding.connection, "stream_id": binding.stream, "connected_before": true,
                "opening_basis": "native_existing_connection", "phase": phase,
                "dial_counter_basis": "native_stream_behaviour_dial_requests",
                "dial_attempts_before": dial_before, "dial_attempts_after": dial_after
            });
            let source = if let Some((owner, requested)) = exact {
                if phase != "relay_before"
                    || owner.peer != peer
                    || !owner.point.is_relayed()
                    || !binding.relayed
                {
                    return Err(io::Error::other(
                        "exact application owner is not the original inner relay",
                    ));
                }
                fields["native_connection_id"] = json!(owner.native.to_string());
                fields["remote_peer_id"] = json!(peer.to_string());
                fields["requested_sequence"] = json!(requested.sequence);
                fields["requested_mono_ns"] = json!(requested.mono_ns);
                fields["open_api_basis"] = json!("native_notify_handler_one_no_dial_path");
                "rust.path_application.NotifyHandler.One.actual_owner"
            } else {
                "rust.stream.Control.open_stream.actual_owner"
            };
            self.record_stamped("application_open", source, fields)
                .ok_or_else(|| io::Error::other("application owner trace overflow"))?;
        }
        self.record("echo", "rust.path_echo.io", json!({
            "phase": phase, "protocol": ECHO_PROTOCOL, "connection_id": binding.connection, "stream_id": binding.stream,
            "remote_peer_id": peer.to_string(), "path": if binding.relayed { "relay" } else { "direct" },
            "server": server, "fresh_dial": false, "io_basis": "retained_native_stream", "read_bytes": bytes.len(), "write_bytes": bytes.len(),
            "read_sha256": format!("{:x}", Sha256::digest(&bytes)), "write_sha256": format!("{:x}", Sha256::digest(&bytes)),
            "dial_attempts_before": before, "dial_attempts_after": after
        }));
        Ok(phase.into())
    }
}

struct PathMuxer<M> {
    inner: Pin<Box<M>>,
    trace: Trace,
}
impl<M: StreamMuxer> StreamMuxer for PathMuxer<M>
where
    M::Substream: Unpin,
{
    type Substream = PathIo<M::Substream>;
    type Error = M::Error;
    fn poll_inbound(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<Result<Self::Substream, Self::Error>> {
        let this = self.get_mut();
        this.inner
            .as_mut()
            .poll_inbound(cx)
            .map_ok(|io| PathIo::new(io, this.trace.clone()))
    }
    fn poll_outbound(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<Result<Self::Substream, Self::Error>> {
        let this = self.get_mut();
        this.inner
            .as_mut()
            .poll_outbound(cx)
            .map_ok(|io| PathIo::new(io, this.trace.clone()))
    }
    fn poll_close(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<(), Self::Error>> {
        self.get_mut().inner.as_mut().poll_close(cx)
    }
    fn poll(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<Result<StreamMuxerEvent, Self::Error>> {
        self.get_mut().inner.as_mut().poll(cx)
    }
}

#[derive(Default)]
struct Decoder {
    bytes: Vec<u8>,
    selected: Option<String>,
    length: Option<(usize, usize)>,
    invalid: bool,
    completed: usize,
}
struct PathIo<T> {
    inner: T,
    trace: Trace,
    id: String,
    read: Decoder,
    write: Decoder,
    read_terminal: bool,
    write_terminal: bool,
}
impl<T> PathIo<T> {
    fn new(inner: T, trace: Trace) -> Self {
        let id = format!(
            "stream-{}",
            trace.observer.0.streams.fetch_add(1, Ordering::SeqCst) + 1
        );
        Self {
            inner,
            trace,
            id,
            read: Decoder::default(),
            write: Decoder::default(),
            read_terminal: false,
            write_terminal: false,
        }
    }

    fn record_terminal(&mut self, direction: &str, n: usize, error: Option<&io::Error>) {
        if self.read.selected.as_deref() != Some("/libp2p/dcutr")
            && self.write.selected.as_deref() != Some("/libp2p/dcutr")
        {
            return;
        }
        let (decoder, recorded) = match direction {
            "read" => (&self.read, &mut self.read_terminal),
            "write" => (&self.write, &mut self.write_terminal),
            _ => return,
        };
        if *recorded {
            return;
        }
        *recorded = true;
        let (kind, detail) = match error {
            None => ("eof", "EOF".to_owned()),
            Some(error) => (
                match error.kind() {
                    io::ErrorKind::UnexpectedEof => "eof",
                    io::ErrorKind::ConnectionReset => "reset",
                    io::ErrorKind::TimedOut => "deadline",
                    _ => "io_error",
                },
                error.to_string().chars().take(256).collect(),
            ),
        };
        self.trace.observer.record("dcutr_stream_terminal", "rust.native_muxer.dcutr.io", json!({
            "connection_id": self.trace.connection, "stream_id": self.id,
            "remote_peer_id": self.trace.remote.to_string(), "protocol": "/libp2p/dcutr",
            "direction": direction, "error": detail, "error_kind": kind, "io_bytes": n,
            "completed_frame_count": decoder.completed, "pending_frame_bytes": decoder.bytes.len(),
            "invalid_or_over_limit": decoder.invalid || decoder.bytes.len() > 4096 || n > 4096
        }));
    }
}

fn read_varint(bytes: &[u8]) -> Option<(usize, usize)> {
    let mut value = 0usize;
    for (i, byte) in bytes.iter().take(5).enumerate() {
        value |= usize::from(byte & 127) << (7 * i);
        if byte & 128 == 0 {
            return Some((value, i + 1));
        }
    }
    None
}

fn dcutr_fields(mut bytes: &[u8]) -> Option<(usize, Vec<String>)> {
    let mut kind = None;
    let mut addresses = Vec::new();
    while !bytes.is_empty() {
        let (key, n) = read_varint(bytes)?;
        bytes = &bytes[n..];
        match key {
            8 if kind.is_none() => {
                let (value, n) = read_varint(bytes)?;
                kind = Some(value);
                bytes = &bytes[n..];
            }
            18 if addresses.len() < 32 => {
                let (length, n) = read_varint(bytes)?;
                bytes = bytes.get(n..)?;
                let address = Multiaddr::try_from(bytes.get(..length)?.to_vec()).ok()?;
                addresses.push(address.to_string());
                bytes = bytes.get(length..)?;
            }
            _ => return None,
        }
    }
    Some((kind?, addresses))
}

impl Decoder {
    fn feed(&mut self, bytes: &[u8], direction: &str, trace: &Trace, stream: &str) {
        for byte in bytes {
            if self.invalid {
                return;
            }
            self.bytes.push(*byte);
            if self.length.is_none() {
                self.length = if self.selected.as_deref() == Some(ECHO_PROTOCOL) {
                    (self.bytes.len() == 4).then(|| {
                        (
                            u32::from_be_bytes(self.bytes[..4].try_into().unwrap()) as usize,
                            4,
                        )
                    })
                } else {
                    read_varint(&self.bytes)
                };
                if self.length.is_none() && self.bytes.len() > 4 {
                    self.invalid = true;
                }
            }
            let Some((length, prefix)) = self.length else {
                continue;
            };
            if length == 0 || length > 4096 {
                self.invalid = true;
                continue;
            }
            if self.bytes.len() < length + prefix {
                continue;
            }
            let body = &self.bytes[prefix..];
            if self.selected.is_none() {
                if let Ok(text) = std::str::from_utf8(body) {
                    if text == "/libp2p/dcutr\n" || text == format!("{ECHO_PROTOCOL}\n") {
                        self.selected = Some(text.trim_end().into());
                    }
                }
            } else if self.selected.as_deref() == Some("/libp2p/dcutr") {
                if let Some((kind, addresses)) = dcutr_fields(body) {
                    let hex = self
                        .bytes
                        .iter()
                        .map(|b| format!("{b:02x}"))
                        .collect::<String>();
                    let mut receipt = json!({"framed_hex": hex});
                    receipt[direction] = json!({"framed_bytes": self.bytes.len(), "framed_sha256": format!("{:x}", Sha256::digest(&self.bytes)),
                        "frames": 1, "complete_frames": true, "invalid_or_over_limit": false});
                    trace.observer.record("dcutr_frame", "rust.native_muxer.dcutr.io", json!({
                        "connection_id": trace.connection, "stream_id": stream, "remote_peer_id": trace.remote.to_string(),
                        "protocol": "/libp2p/dcutr", "direction": direction, "message_type": kind, "addresses": addresses, "receipt": receipt
                    }));
                    self.completed += 1;
                } else {
                    self.invalid = true;
                }
            } else if self.selected.as_deref() == Some(ECHO_PROTOCOL) {
                let prefix = format!("path:{}:", trace.observer.0.token);
                if let Some(phase) = std::str::from_utf8(body)
                    .ok()
                    .and_then(|text| text.strip_prefix(&prefix))
                {
                    trace.observer.record("application_frame", "rust.native_muxer.application.io", json!({
                        "connection_id": trace.connection, "stream_id": stream, "remote_peer_id": trace.remote.to_string(),
                        "direction": direction, "phase": phase, "path": if trace.relayed { "relay" } else { "direct" },
                        "payload_bytes": body.len(), "payload_sha256": format!("{:x}", Sha256::digest(body))
                    }));
                } else {
                    self.invalid = true;
                }
            }
            if self.invalid {
                trace.observer.record(
                    "capture_error",
                    "rust.native_muxer.io",
                    json!({"stream_id": stream}),
                );
            }
            self.bytes.clear();
            self.length = None;
        }
    }
}

impl<T: AsyncRead + Unpin> AsyncRead for PathIo<T> {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        bytes: &mut [u8],
    ) -> Poll<io::Result<usize>> {
        let this = self.get_mut();
        let result = Pin::new(&mut this.inner).poll_read(cx, bytes);
        match &result {
            Poll::Ready(Ok(n)) => {
                this.read.feed(&bytes[..*n], "read", &this.trace, &this.id);
                // A zero-capacity read is not an EOF observation.
                if *n == 0 && !bytes.is_empty() {
                    this.record_terminal("read", *n, None);
                }
            }
            Poll::Ready(Err(error)) => this.record_terminal("read", 0, Some(error)),
            Poll::Pending => {}
        }
        result
    }
}
impl<T: AsyncWrite + Unpin> AsyncWrite for PathIo<T> {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        bytes: &[u8],
    ) -> Poll<io::Result<usize>> {
        let this = self.get_mut();
        let result = Pin::new(&mut this.inner).poll_write(cx, bytes);
        match &result {
            Poll::Ready(Ok(n)) => {
                this.write
                    .feed(&bytes[..*n], "write", &this.trace, &this.id);
            }
            Poll::Ready(Err(error)) => this.record_terminal("write", 0, Some(error)),
            Poll::Pending => {}
        }
        result
    }
    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Pin::new(&mut self.get_mut().inner).poll_flush(cx)
    }
    fn poll_close(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Pin::new(&mut self.get_mut().inner).poll_close(cx)
    }
}

struct PathTransport {
    inner: Boxed<(PeerId, StreamMuxerBox)>,
    observer: Observer,
}

// The native behaviour owns all state/actions. This wrapper only associates its
// public dial ID and callbacks with actual muxer I/O; it never creates an action.
struct PathDcutr {
    inner: dcutr::Behaviour,
    observer: Observer,
    relays: NativeConnections,
}

impl NetworkBehaviour for PathDcutr {
    type ConnectionHandler = <dcutr::Behaviour as NetworkBehaviour>::ConnectionHandler;
    type ToSwarm = dcutr::Event;

    fn handle_pending_inbound_connection(
        &mut self,
        id: ConnectionId,
        local: &Multiaddr,
        remote: &Multiaddr,
    ) -> Result<(), ConnectionDenied> {
        self.inner
            .handle_pending_inbound_connection(id, local, remote)
    }
    fn handle_established_inbound_connection(
        &mut self,
        id: ConnectionId,
        peer: PeerId,
        local: &Multiaddr,
        remote: &Multiaddr,
    ) -> Result<THandler<Self>, ConnectionDenied> {
        self.inner
            .handle_established_inbound_connection(id, peer, local, remote)
    }
    fn handle_pending_outbound_connection(
        &mut self,
        id: ConnectionId,
        peer: Option<PeerId>,
        addresses: &[Multiaddr],
        role: Endpoint,
    ) -> Result<Vec<Multiaddr>, ConnectionDenied> {
        let result = self
            .inner
            .handle_pending_outbound_connection(id, peer, addresses, role);
        self.observer
            .native_wave_pending(id, peer, addresses, role, &result);
        result
    }
    fn handle_established_outbound_connection(
        &mut self,
        id: ConnectionId,
        peer: PeerId,
        address: &Multiaddr,
        role: Endpoint,
        port: PortUse,
    ) -> Result<THandler<Self>, ConnectionDenied> {
        self.inner
            .handle_established_outbound_connection(id, peer, address, role, port)
    }
    fn on_swarm_event(&mut self, event: FromSwarm) {
        self.inner.on_swarm_event(event);
        match event {
            FromSwarm::ConnectionEstablished(connection) if connection.endpoint.is_relayed() => {
                if self.relays.len() >= WAVE_LIMIT
                    || self.relays.contains_key(&connection.connection_id)
                {
                    self.observer
                        .0
                        .state
                        .lock()
                        .unwrap_or_else(|e| e.into_inner())
                        .overflow = true;
                } else {
                    self.relays.insert(
                        connection.connection_id,
                        (connection.peer_id, connection.endpoint.clone()),
                    );
                }
            }
            FromSwarm::ConnectionClosed(connection) => {
                self.relays.remove(&connection.connection_id);
            }
            FromSwarm::AddressChange(connection) => {
                if let Some((peer, point)) = self.relays.get_mut(&connection.connection_id) {
                    if *peer == connection.peer_id && *point == *connection.old {
                        *point = connection.new.clone();
                    } else {
                        self.observer
                            .0
                            .state
                            .lock()
                            .unwrap_or_else(|e| e.into_inner())
                            .overflow = true;
                    }
                }
            }
            _ => {}
        }
    }
    fn on_connection_handler_event(
        &mut self,
        peer: PeerId,
        id: ConnectionId,
        event: THandlerOutEvent<Self>,
    ) {
        self.inner.on_connection_handler_event(peer, id, event);
    }
    fn poll(
        &mut self,
        cx: &mut Context<'_>,
    ) -> Poll<ToSwarm<Self::ToSwarm, THandlerInEvent<Self>>> {
        let action = self.inner.poll(cx);
        if let Poll::Ready(ToSwarm::Dial { opts }) = &action {
            self.observer.native_wave_requested(opts, &self.relays);
        }
        action
    }
}

fn candidate_sockets(
    addresses: &[Multiaddr],
    peer: PeerId,
) -> io::Result<Vec<std::net::SocketAddr>> {
    if addresses.is_empty() || addresses.len() > 32 {
        return Err(io::Error::other("native wave candidate bound"));
    }
    let mut sockets = addresses
        .iter()
        .map(|addr| quic_socket(addr, Some(peer)))
        .collect::<io::Result<Vec<_>>>()?;
    sockets.sort_unstable();
    if sockets.windows(2).any(|pair| pair[0] == pair[1]) {
        return Err(io::Error::other("duplicate native wave candidate"));
    }
    Ok(sockets)
}

fn source_wave(state: &State, relay: LiveConnection, local: PeerId) -> io::Result<NativeWave> {
    let peer = relay.peer.to_string();
    let mut streams: BTreeMap<&str, Vec<&Value>> = BTreeMap::new();
    for event in &state.events {
        if event["kind"] == "dcutr_frame"
            && event["connection_id"] == relay.transport
            && event["remote_peer_id"] == peer
        {
            let stream = event["stream_id"]
                .as_str()
                .ok_or_else(|| io::Error::other("missing wave stream"))?;
            if !state
                .waves
                .values()
                .any(|wave| wave.relay.transport == relay.transport && wave.stream == stream)
            {
                streams.entry(stream).or_default().push(event);
            }
        }
    }
    let mut complete = streams.into_iter().filter(|(_, events)| events.len() == 3);
    let (stream, events) = complete
        .next()
        .ok_or_else(|| io::Error::other("no complete unclaimed source wave"))?;
    if complete.next().is_some() {
        return Err(io::Error::other("ambiguous unclaimed source wave"));
    }
    let mut stamps = Vec::new();
    for (event, (direction, kind)) in
        events
            .iter()
            .zip([("read", 100), ("write", 100), ("read", 300)])
    {
        if event["source"] != "rust.native_muxer.dcutr.io"
            || event["protocol"] != "/libp2p/dcutr"
            || event["direction"] != direction
            || event["message_type"] != kind
            || event["receipt"][direction]["complete_frames"] != true
            || event["receipt"][direction]["invalid_or_over_limit"] != false
            || event["receipt"][direction]["frames"] != 1
        {
            return Err(io::Error::other("incomplete or mismatched source wave"));
        }
        stamps.push(EventStamp::captured(event)?);
    }
    if stamps
        .windows(2)
        .any(|pair| pair[0].sequence >= pair[1].sequence || pair[0].mono_ns > pair[1].mono_ns)
        || events[2]["addresses"] != json!([])
    {
        return Err(io::Error::other("invalid source SYNC ordering"));
    }
    let addresses = |event: &Value| -> io::Result<Vec<Multiaddr>> {
        event["addresses"]
            .as_array()
            .filter(|values| !values.is_empty() && values.len() <= 32)
            .ok_or_else(|| io::Error::other("missing source CONNECT candidates"))?
            .iter()
            .map(|value| {
                value
                    .as_str()
                    .ok_or_else(|| io::Error::other("non-address CONNECT candidate"))?
                    .parse()
                    .map_err(io::Error::other)
            })
            .collect()
    };
    let advertised = addresses(events[0])?;
    candidate_sockets(&advertised, relay.peer)?;
    candidate_sockets(&addresses(events[1])?, local)?;
    Ok(NativeWave {
        relay,
        stream: stream.into(),
        frames: [stamps[0], stamps[1], stamps[2]],
        advertised,
        requested: None,
        pending: None,
        candidates: Vec::new(),
        failed: false,
        invalid: false,
    })
}

impl Observer {
    fn native_wave_requested(
        &self,
        opts: &libp2p::swarm::dial_opts::DialOpts,
        relays: &NativeConnections,
    ) {
        let id = opts.connection_id();
        let peer = opts.get_peer_id();
        let mut wave = (|| -> io::Result<NativeWave> {
            let peer = peer.ok_or_else(|| io::Error::other("native DCUtR dial lacks peer"))?;
            let mut owners = relays.iter().filter(|(_, (remote, point))| {
                *remote == peer && point.is_relayed() && point.is_dialer()
            });
            let (&relay_id, _) = owners
                .next()
                .ok_or_else(|| io::Error::other("no source relay owner"))?;
            if owners.next().is_some() {
                return Err(io::Error::other("ambiguous source relay owner"));
            }
            let relay = self.live_connection(relay_id, relays)?;
            let mut state = self.0.state.lock().unwrap_or_else(|e| e.into_inner());
            if state.waves.len() >= WAVE_LIMIT || state.waves.contains_key(&id) {
                state.overflow = true;
                return Err(io::Error::other("native wave ID/count bound"));
            }
            if state.overflow || state.expected != Some(peer) {
                return Err(io::Error::other("native wave peer/observation mismatch"));
            }
            source_wave(&state, relay, self.0.local)
        })()
        .ok();
        let mut fields = wave
            .as_ref()
            .map(NativeWave::receipt)
            .unwrap_or_else(|| json!({}));
        fields["native_connection_id"] = json!(id.to_string());
        fields["remote_peer_id"] = json!(peer.map(|peer| peer.to_string()));
        fields["source_wave_bound"] = json!(wave.is_some());
        fields["proof_scope"] = json!(if wave.is_some() {
            "native_wave_only"
        } else {
            "no_wave_proof"
        });
        fields["aggregate_dcutr_completed"] = json!(false);
        fields["rust_behaviour_joined"] = json!(false);
        fields["native_origin"] = json!("dcutr::Behaviour::poll::ToSwarm::Dial");
        if let Some(stamp) = self.record_stamped(
            "native_dcutr_dial_requested",
            "rust.dcutr.Behaviour.poll.ToSwarm.Dial",
            fields,
        ) {
            if let Some(mut wave) = wave.take() {
                wave.requested = Some(stamp);
                self.0
                    .state
                    .lock()
                    .unwrap_or_else(|e| e.into_inner())
                    .waves
                    .insert(id, wave);
            }
        }
    }

    fn wave_rejected(&self, source: &str, id: ConnectionId, peer: Option<PeerId>, reason: &str) {
        if let Some(wave) = self
            .0
            .state
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .waves
            .get_mut(&id)
        {
            wave.invalid = true;
        }
        self.record("native_dcutr_wave_binding_rejected", source, json!({
            "native_connection_id": id.to_string(), "remote_peer_id": peer.map(|peer| peer.to_string()),
            "reason": reason, "proof_scope": "no_wave_proof",
        }));
    }

    fn native_wave_pending(
        &self,
        id: ConnectionId,
        peer: Option<PeerId>,
        addresses: &[Multiaddr],
        role: Endpoint,
        result: &Result<Vec<Multiaddr>, ConnectionDenied>,
    ) {
        const SOURCE: &str = "rust.dcutr.Behaviour.handle_pending_outbound_connection";
        let wave = {
            let state = self.0.state.lock().unwrap_or_else(|e| e.into_inner());
            if state.overflow {
                return;
            }
            state.waves.get(&id).cloned()
        };
        let Some(wave) = wave else {
            return;
        };
        let matches = match (
            candidate_sockets(addresses, wave.relay.peer),
            candidate_sockets(&wave.advertised, wave.relay.peer),
        ) {
            (Ok(actual), Ok(advertised)) => actual == advertised,
            _ => false,
        };
        if wave.invalid
            || peer != Some(wave.relay.peer)
            || role != Endpoint::Dialer
            || !matches
            || wave.pending.is_some()
            || !result.as_ref().is_ok_and(Vec::is_empty)
        {
            self.wave_rejected(
                SOURCE,
                id,
                peer,
                "pending_peer_role_candidates_or_native_return_mismatch",
            );
            return;
        }
        let mut fields = wave.receipt();
        fields["native_connection_id"] = json!(id.to_string());
        fields["candidate_addresses"] = json!(
            addresses
                .iter()
                .map(ToString::to_string)
                .collect::<Vec<_>>()
        );
        fields["requested_role"] = json!("dialer");
        fields["candidate_match_basis"] = json!("exact_numeric_socket_optional_same_peer_suffix");
        fields["native_callback_accepted"] = json!(true);
        fields["native_returned_addresses"] = json!([]);
        if let Some(stamp) = self.record_stamped("native_dcutr_dial_options", SOURCE, fields) {
            let mut state = self.0.state.lock().unwrap_or_else(|e| e.into_inner());
            if let Some(wave) = state.waves.get_mut(&id) {
                wave.pending = Some(stamp);
                wave.candidates = addresses.to_vec();
            }
        }
    }

    fn native_wave_failure(
        &self,
        id: ConnectionId,
        peer: Option<PeerId>,
        error: &libp2p::swarm::DialError,
        connections: &NativeConnections,
        raw_error: EventStamp,
    ) {
        const SOURCE: &str = "rust.swarm.OutgoingConnectionError.native_dcutr_wave";
        let wave = {
            let state = self.0.state.lock().unwrap_or_else(|e| e.into_inner());
            if state.overflow {
                return;
            }
            state.waves.get(&id).cloned()
        };
        let Some(wave) = wave else {
            return;
        };
        let original_live = self
            .live_connection(wave.relay.native, connections)
            .is_ok_and(|live| {
                live.peer == wave.relay.peer
                    && live.transport == wave.relay.transport
                    && live.point == wave.relay.point
            });
        let ordered = match (wave.requested, wave.pending) {
            (Some(requested), Some(pending)) => {
                wave.frames[2].sequence < requested.sequence
                    && requested.sequence < pending.sequence
                    && pending.sequence < raw_error.sequence
                    && wave.frames[2].mono_ns <= requested.mono_ns
                    && requested.mono_ns <= pending.mono_ns
                    && pending.mono_ns <= raw_error.mono_ns
            }
            _ => false,
        };
        if wave.invalid
            || wave.failed
            || peer != Some(wave.relay.peer)
            || !original_live
            || !ordered
        {
            self.wave_rejected(
                SOURCE,
                id,
                peer,
                "error_peer_owner_order_or_duplicate_mismatch",
            );
            return;
        }
        let Some(errors) = native_quic_failures(error, &wave.candidates, wave.relay.peer) else {
            self.wave_rejected(
                SOURCE,
                id,
                peer,
                "unmatched_candidate_or_unavailable_typed_native_quic_error",
            );
            return;
        };
        let mut fields = wave.receipt();
        fields["native_connection_id"] = json!(id.to_string());
        fields["native_origin"] = json!("dcutr::Behaviour::poll::ToSwarm::Dial");
        fields["dial_error_variant"] = json!("Transport");
        fields["transport_errors"] = json!(errors);
        fields["raw_error_sequence"] = json!(raw_error.sequence);
        fields["raw_error_mono_ns"] = json!(raw_error.mono_ns);
        fields["original_relay_live"] = json!(true);
        if self
            .record_stamped("native_dcutr_wave_failed", SOURCE, fields)
            .is_some()
        {
            if let Some(wave) = self
                .0
                .state
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .waves
                .get_mut(&id)
            {
                wave.failed = true;
            }
        }
    }
}

// This is the exact boxed OrTransport error type used below. Walking io::Error's
// get_ref and Either's value preserves the typed root (source() may skip it).
type PathChoiceError = <libp2p::core::transport::OrTransport<
    Boxed<(PeerId, StreamMuxerBox)>,
    Boxed<(PeerId, StreamMuxerBox)>,
> as Transport>::Error;

fn native_quic_error(error: &io::Error) -> Option<Value> {
    let mut current: &(dyn std::error::Error + 'static) = error;
    let mut wrappers = Vec::new();
    for _ in 0..12 {
        if let Some(native) = current.downcast_ref::<libp2p::quic::Error>() {
            let variant = match native {
                libp2p::quic::Error::Reach(_) => "Reach",
                libp2p::quic::Error::Connection(_) => "Connection",
                libp2p::quic::Error::Io(_) => "Io",
                libp2p::quic::Error::HandshakeTimedOut => "HandshakeTimedOut",
                libp2p::quic::Error::NoActiveListenerForDialAsListener => {
                    "NoActiveListenerForDialAsListener"
                }
                libp2p::quic::Error::HolePunchInProgress(_) => "HolePunchInProgress",
            };
            return Some(
                json!({"quic_error_type": "libp2p_quic::Error", "quic_error_variant": variant,
                "classification_basis": "typed_downcast_and_public_native_enum", "wrapper_types": wrappers,
                "error": native.to_string().chars().take(256).collect::<String>()}),
            );
        }
        current = if let Some(boxed) = current.downcast_ref::<io::Error>() {
            wrappers.push("std::io::Error");
            boxed.get_ref()?
        } else if let Some(choice) = current.downcast_ref::<PathChoiceError>() {
            wrappers.push("native_path_OrTransport::Error");
            choice.as_ref().into_inner()
        } else {
            return None;
        };
    }
    None
}

fn native_quic_failures(
    error: &libp2p::swarm::DialError,
    candidates: &[Multiaddr],
    peer: PeerId,
) -> Option<Vec<Value>> {
    let libp2p::swarm::DialError::Transport(errors) = error else {
        return None;
    };
    if errors.is_empty() || errors.len() > 32 {
        return None;
    }
    let actual = errors
        .iter()
        .map(|(address, _)| address.clone())
        .collect::<Vec<_>>();
    if candidate_sockets(&actual, peer).ok()? != candidate_sockets(candidates, peer).ok()? {
        return None;
    }
    errors
        .iter()
        .map(|(address, error)| {
            let libp2p::core::transport::TransportError::Other(error) = error else {
                return None;
            };
            let mut receipt = native_quic_error(error)?;
            receipt["address"] = json!(address.to_string());
            receipt["transport_error_variant"] = json!("Other");
            Some(receipt)
        })
        .collect()
}

// Delegate every public native stream behaviour method unchanged. This observes
// only application-originated ToSwarm::Dial, separately from DCUtR's workers.
struct PathStreams {
    inner: libp2p_stream::Behaviour,
    observer: Observer,
}
impl PathStreams {
    fn new_control(&self) -> libp2p_stream::Control {
        self.inner.new_control()
    }
}
impl NetworkBehaviour for PathStreams {
    type ConnectionHandler = <libp2p_stream::Behaviour as NetworkBehaviour>::ConnectionHandler;
    type ToSwarm = ();
    fn handle_pending_inbound_connection(
        &mut self,
        id: ConnectionId,
        local: &Multiaddr,
        remote: &Multiaddr,
    ) -> Result<(), ConnectionDenied> {
        self.inner
            .handle_pending_inbound_connection(id, local, remote)
    }
    fn handle_established_inbound_connection(
        &mut self,
        id: ConnectionId,
        peer: PeerId,
        local: &Multiaddr,
        remote: &Multiaddr,
    ) -> Result<THandler<Self>, ConnectionDenied> {
        self.inner
            .handle_established_inbound_connection(id, peer, local, remote)
    }
    fn handle_pending_outbound_connection(
        &mut self,
        id: ConnectionId,
        peer: Option<PeerId>,
        addresses: &[Multiaddr],
        role: Endpoint,
    ) -> Result<Vec<Multiaddr>, ConnectionDenied> {
        self.inner
            .handle_pending_outbound_connection(id, peer, addresses, role)
    }
    fn handle_established_outbound_connection(
        &mut self,
        id: ConnectionId,
        peer: PeerId,
        address: &Multiaddr,
        role: Endpoint,
        port: PortUse,
    ) -> Result<THandler<Self>, ConnectionDenied> {
        self.inner
            .handle_established_outbound_connection(id, peer, address, role, port)
    }
    fn on_swarm_event(&mut self, event: FromSwarm) {
        self.inner.on_swarm_event(event);
    }
    fn on_connection_handler_event(
        &mut self,
        peer: PeerId,
        id: ConnectionId,
        event: THandlerOutEvent<Self>,
    ) {
        self.inner.on_connection_handler_event(peer, id, event);
    }
    fn poll(
        &mut self,
        cx: &mut Context<'_>,
    ) -> Poll<ToSwarm<Self::ToSwarm, THandlerInEvent<Self>>> {
        let event = self.inner.poll(cx);
        if let Poll::Ready(ToSwarm::Dial { opts }) = &event {
            self.observer
                .0
                .application_dials
                .fetch_add(1, Ordering::SeqCst);
            self.observer.record(
                "application_dial_requested",
                "rust.native_stream_behaviour.ToSwarm.Dial",
                json!({
                    "remote_peer_id": opts.get_peer_id().map(|peer| peer.to_string()),
                    "native_connection_id": opts.connection_id().to_string()
                }),
            );
        }
        event
    }
}
impl Transport for PathTransport {
    type Output = (PeerId, StreamMuxerBox);
    type Error = io::Error;
    type ListenerUpgrade = <Boxed<Self::Output> as Transport>::ListenerUpgrade;
    type Dial = <Boxed<Self::Output> as Transport>::Dial;
    fn listen_on(
        &mut self,
        id: ListenerId,
        addr: Multiaddr,
    ) -> Result<(), libp2p::core::transport::TransportError<Self::Error>> {
        self.inner.listen_on(id, addr)
    }
    fn remove_listener(&mut self, id: ListenerId) -> bool {
        self.inner.remove_listener(id)
    }
    fn dial(
        &mut self,
        addr: Multiaddr,
        opts: DialOpts,
    ) -> Result<Self::Dial, libp2p::core::transport::TransportError<Self::Error>> {
        self.observer.0.dials.fetch_add(1, Ordering::SeqCst);
        let expected = self
            .observer
            .0
            .state
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .expected;
        self.observer.record("native_coordinated_dial", "rust.native_transport.dial_options", json!({
            "address": addr.to_string(), "remote_peer_id": expected.map(|p| p.to_string()),
            "requested_role": if opts.role == Endpoint::Dialer { "dialer" } else { "listener" }, "port_use": format!("{:?}", opts.port_use)
        }));
        self.inner.dial(addr, opts)
    }
    fn poll(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<TransportEvent<Self::ListenerUpgrade, Self::Error>> {
        Pin::new(&mut self.get_mut().inner).poll(cx)
    }
}

fn fields(path: &Path) -> io::Result<BTreeMap<String, String>> {
    let bytes = std::fs::read(path)?;
    if bytes.len() > 8192 {
        return Err(io::Error::other("control bound"));
    }
    let text = std::str::from_utf8(&bytes).map_err(io::Error::other)?;
    let mut fields = BTreeMap::new();
    for line in text.lines() {
        let (key, value) = line
            .split_once('=')
            .ok_or_else(|| io::Error::other("invalid control"))?;
        if key.is_empty()
            || value.is_empty()
            || line.contains('\0')
            || fields.len() >= 16
            || fields.insert(key.into(), value.into()).is_some()
        {
            return Err(io::Error::other("duplicate/invalid control"));
        }
    }
    Ok(fields)
}
fn atomic(path: &Path, value: &Value) -> io::Result<()> {
    let tmp = path.with_extension("tmp");
    std::fs::write(&tmp, value.to_string())?;
    std::fs::rename(tmp, path)
}

fn quic_socket(address: &Multiaddr, peer: Option<PeerId>) -> io::Result<std::net::SocketAddr> {
    let mut parts = address.iter();
    let ip = match parts.next() {
        Some(Protocol::Ip4(ip)) => std::net::IpAddr::V4(ip),
        Some(Protocol::Ip6(ip)) => std::net::IpAddr::V6(ip),
        _ => return Err(io::Error::other("QUIC candidate requires a numeric IP")),
    };
    let port = match parts.next() {
        Some(Protocol::Udp(port)) if port != 0 => port,
        _ => return Err(io::Error::other("QUIC candidate requires a bound UDP port")),
    };
    if ip.is_unspecified() || ip.is_multicast() || parts.next() != Some(Protocol::QuicV1) {
        return Err(io::Error::other("invalid direct QUIC candidate endpoint"));
    }
    match parts.next() {
        None => {}
        Some(Protocol::P2p(actual)) if Some(actual) == peer && parts.next().is_none() => {}
        _ => {
            return Err(io::Error::other(
                "QUIC candidate has an unexpected peer or suffix",
            ));
        }
    }
    Ok(std::net::SocketAddr::new(ip, port))
}

struct RelayCandidate {
    native_connection_id: ConnectionId,
    carrier_connection_id: String,
    listener_id: ListenerId,
    listener_address: Multiaddr,
    observed_address: Multiaddr,
    propagated: bool,
}

impl RelayCandidate {
    fn new(
        local: PeerId,
        native_connection_id: ConnectionId,
        carrier_connection_id: String,
        listener_id: ListenerId,
        listener_address: Multiaddr,
        observed_address: Multiaddr,
    ) -> io::Result<Self> {
        let listener = quic_socket(&listener_address, None)?;
        let observed = quic_socket(&observed_address, Some(local))?;
        if listener.port() != observed.port() || listener.is_ipv4() != observed.is_ipv4() {
            return Err(io::Error::other(
                "native Identify candidate does not match the bound QUIC listener port/family",
            ));
        }
        Ok(Self {
            native_connection_id,
            carrier_connection_id,
            listener_id,
            listener_address,
            observed_address,
            propagated: false,
        })
    }

    fn accept_native_candidate(&mut self, address: &Multiaddr) -> bool {
        if address != &self.observed_address || self.propagated {
            return false;
        }
        self.propagated = true;
        true
    }

    fn receipt(&self, relay_peer: PeerId) -> Value {
        json!({
            "native_connection_id": self.native_connection_id.to_string(),
            "carrier_connection_id": self.carrier_connection_id,
            "observer_peer_id": relay_peer.to_string(),
            "listener_id": self.listener_id.to_string(),
            "local_listener_address": self.listener_address.to_string(),
            "observed_address": self.observed_address.to_string(),
            "candidate_basis": "native_identify_then_swarm_candidate",
            "listener_binding_basis": "native_listener_and_observed_udp_port_match",
            "candidate_propagated": self.propagated,
        })
    }
}

fn relay_listener_address(role: &str, address: &Multiaddr) -> io::Result<Option<Multiaddr>> {
    if role != "relay" {
        return Ok(None);
    }
    // Only the topology's directly bound, unrouted WAN relay may confirm its
    // actual listener. NAT endpoints still need native Identify observations.
    let mut parts = address.iter();
    if !matches!(parts.next(), Some(Protocol::Ip4(ip)) if ip.octets() == [11, 0, 0, 1])
        || !matches!(parts.next(), Some(Protocol::Udp(port)) if port != 0)
        || !matches!(parts.next(), Some(Protocol::QuicV1))
        || parts.next().is_some()
    {
        return Err(io::Error::other(
            "relay external address requires its actual owned WAN QUIC listener",
        ));
    }
    Ok(Some(address.clone()))
}

fn circuit_listener_address(
    address: &Multiaddr,
    relay_peer: PeerId,
    local: PeerId,
) -> io::Result<Multiaddr> {
    let mut suffix = address
        .iter()
        .skip_while(|part| *part != Protocol::P2p(relay_peer));
    if suffix.next() != Some(Protocol::P2p(relay_peer))
        || suffix.next() != Some(Protocol::P2pCircuit)
        || suffix.next() != Some(Protocol::P2p(local))
        || suffix.next().is_some()
    {
        return Err(io::Error::other(
            "native reservation listener address peer mismatch",
        ));
    }
    // The native reservation handler already appends circuit and local peer.
    Ok(address.clone())
}

type NativeConnections = BTreeMap<ConnectionId, (PeerId, ConnectedPoint)>;
type ApplicationResult = futures::channel::oneshot::Receiver<io::Result<Option<libp2p::Stream>>>;

fn circuit_carrier(point: &ConnectedPoint) -> io::Result<(PeerId, Multiaddr)> {
    let addresses = match point {
        ConnectedPoint::Dialer { address, .. } => vec![address],
        ConnectedPoint::Listener {
            local_addr,
            send_back_addr,
        } => vec![local_addr, send_back_addr],
    };
    let mut carrier = None;
    for address in addresses {
        let mut prefix = Multiaddr::empty();
        let mut peer = None;
        for protocol in address.iter() {
            if protocol == Protocol::P2pCircuit {
                let peer =
                    peer.ok_or_else(|| io::Error::other("native circuit lacks relay identity"))?;
                let candidate = (peer, prefix.clone());
                if carrier
                    .as_ref()
                    .is_some_and(|previous| previous != &candidate)
                {
                    return Err(io::Error::other("ambiguous native circuit carrier route"));
                }
                carrier = Some(candidate);
                break;
            }
            peer = match protocol {
                Protocol::P2p(peer) => Some(peer),
                _ => None,
            };
            prefix.push(protocol);
        }
    }
    carrier.ok_or_else(|| io::Error::other("missing actual inner circuit route"))
}

impl Observer {
    fn live_connection(
        &self,
        native: ConnectionId,
        connections: &NativeConnections,
    ) -> io::Result<LiveConnection> {
        let (peer, point) = connections
            .get(&native)
            .ok_or_else(|| io::Error::other("required native connection is not live"))?;
        let state = self.0.state.lock().unwrap_or_else(|e| e.into_inner());
        let mut outputs = state
            .sessions
            .iter()
            .filter(|(_, remote, endpoint, relayed)| {
                remote == peer && endpoint == point && *relayed == point.is_relayed()
            });
        let output = outputs.next().ok_or_else(|| {
            io::Error::other("native connection lacks authenticated transport output")
        })?;
        if outputs.next().is_some() {
            return Err(io::Error::other(
                "ambiguous native connection/authenticated output binding",
            ));
        }
        Ok(LiveConnection {
            native,
            transport: output.0.clone(),
            peer: *peer,
            point: point.clone(),
        })
    }

    fn prepare_retirement(
        &self,
        peer: PeerId,
        relay_peer: PeerId,
        outcome: &str,
        retained: bool,
        connections: &NativeConnections,
    ) -> io::Result<RelayRetirement> {
        if outcome != "success" || !retained {
            return Err(io::Error::other(
                "relay retirement requires success and the original retained stream",
            ));
        }
        let binding = self.resolve_echo(peer, "relay_before")?;
        let (success_id, success, relay_before) = {
            let state = self.0.state.lock().unwrap_or_else(|e| e.into_inner());
            if state.expected != Some(peer) || state.retirement.is_some() {
                return Err(io::Error::other(
                    "retirement peer is unbound or already retired",
                ));
            }
            let success = state
                .events
                .iter()
                .rev()
                .find(|event| {
                    event["kind"] == "native_dcutr_event"
                        && event["source"] == "rust.dcutr.behaviour"
                        && event["remote_peer_id"] == peer.to_string()
                        && event["result"]["native_success"] == true
                })
                .ok_or_else(|| {
                    io::Error::other("relay retirement lacks actual native DCUtR success")
                })?;
            let native = connections
                .keys()
                .copied()
                .find(|id| success["result"]["connection_id"] == id.to_string())
                .ok_or_else(|| {
                    io::Error::other("native DCUtR success connection is no longer live")
                })?;
            let echoes = state
                .events
                .iter()
                .filter(|event| {
                    event["kind"] == "echo"
                        && event["source"] == "rust.path_echo.io"
                        && event["phase"] == "relay_before"
                        && event["remote_peer_id"] == peer.to_string()
                        && event["connection_id"] == binding.connection
                        && event["stream_id"] == binding.stream
                        && event["path"] == "relay"
                        && event["server"] == false
                })
                .collect::<Vec<_>>();
            if echoes.len() != 1 || !binding.relayed {
                return Err(io::Error::other(
                    "retirement lacks completed retained relay-before I/O",
                ));
            }
            (
                native,
                EventStamp::captured(success)?,
                EventStamp::captured(echoes[0])?,
            )
        };
        let direct = self.live_connection(success_id, connections)?;
        if direct.peer != peer || direct.point.is_relayed() {
            return Err(io::Error::other(
                "DCUtR success is not authenticated direct to the expected peer",
            ));
        }
        let relays = connections
            .iter()
            .filter(|(_, (remote, point))| *remote == peer && point.is_relayed())
            .map(|(id, _)| *id)
            .collect::<Vec<_>>();
        if relays.len() != 1
            || connections
                .values()
                .filter(|(remote, _)| *remote == peer)
                .count()
                != 2
        {
            return Err(io::Error::other(
                "retirement requires exactly one inner relay and the actual successful direct",
            ));
        }
        let relay = self.live_connection(relays[0], connections)?;
        if relay.transport != binding.connection {
            return Err(io::Error::other(
                "retained stream belongs to a different inner relay",
            ));
        }
        let (actual_relay, route) = circuit_carrier(&relay.point)?;
        if actual_relay != relay_peer {
            return Err(io::Error::other(
                "native inner circuit relay identity mismatch",
            ));
        }
        let carriers = connections
            .iter()
            .filter(|(_, (remote, point))| *remote == actual_relay && !point.is_relayed())
            .map(|(id, _)| *id)
            .collect::<Vec<_>>();
        if carriers.len() != 1 {
            return Err(io::Error::other(
                "retirement lacks a unique live outer relay carrier",
            ));
        }
        let carrier = self.live_connection(carriers[0], connections)?;
        let carrier_remote = match &carrier.point {
            ConnectedPoint::Dialer { address, .. } => address,
            ConnectedPoint::Listener { send_back_addr, .. } => send_back_addr,
        };
        if quic_socket(&route, Some(relay_peer))? != quic_socket(carrier_remote, Some(relay_peer))?
        {
            return Err(io::Error::other(
                "authenticated outer carrier differs from the native circuit route",
            ));
        }
        Ok(RelayRetirement {
            relay,
            direct,
            carrier,
            stream: binding.stream,
            success,
            relay_before,
            requested: None,
            closed: None,
        })
    }

    fn retirement_requested(
        &self,
        mut retirement: RelayRetirement,
        accepted: bool,
    ) -> io::Result<()> {
        let mut fields = retirement.receipt();
        fields["native_close_accepted"] = json!(accepted);
        let stamp = self
            .record_stamped(
                "relay_retirement_requested",
                "rust.swarm.close_connection",
                fields,
            )
            .ok_or_else(|| io::Error::other("retirement request trace overflow"))?;
        if !accepted {
            return Err(io::Error::other(
                "native inner relay close request was not accepted",
            ));
        }
        retirement.requested = Some(stamp);
        self.0
            .state
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .retirement = Some(retirement);
        Ok(())
    }

    fn retirement_survivors(
        &self,
        retirement: &RelayRetirement,
        connections: &NativeConnections,
    ) -> io::Result<()> {
        for original in [&retirement.direct, &retirement.carrier] {
            let live = self.live_connection(original.native, connections)?;
            if live.peer != original.peer
                || live.point != original.point
                || live.transport != original.transport
            {
                return Err(io::Error::other(
                    "native direct or outer carrier owner changed during retirement",
                ));
            }
        }
        if connections.contains_key(&retirement.relay.native)
            || connections.iter().any(|(id, (peer, _))| {
                *peer == retirement.relay.peer && *id != retirement.direct.native
            })
        {
            return Err(io::Error::other(
                "inner relay still live or peer gained an ambiguous stream owner",
            ));
        }
        Ok(())
    }

    fn retirement_closed(
        &self,
        native: ConnectionId,
        peer: PeerId,
        point: &ConnectedPoint,
        cause: Option<&str>,
        actual_closed: EventStamp,
        connections: &NativeConnections,
    ) -> io::Result<()> {
        let retirement = self
            .0
            .state
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .retirement
            .clone();
        let Some(mut retirement) =
            retirement.filter(|retirement| retirement.relay.native == native)
        else {
            return Ok(());
        };
        let requested = retirement
            .requested
            .ok_or_else(|| io::Error::other("closure lacks an accepted native close request"))?;
        if retirement.closed.is_some()
            || retirement.relay.peer != peer
            || retirement.relay.point != *point
            || !point.is_relayed()
            || cause.is_some()
            || actual_closed.sequence <= requested.sequence
            || actual_closed.mono_ns < requested.mono_ns
        {
            return Err(io::Error::other(
                "unexpected native inner relay closure after retirement request",
            ));
        }
        self.retirement_survivors(&retirement, connections)?;
        let mut fields = retirement.receipt();
        fields["native_closed_sequence"] = json!(actual_closed.sequence);
        fields["native_closed_mono_ns"] = json!(actual_closed.mono_ns);
        fields["cause"] = json!(cause);
        fields["direct_still_live"] = json!(true);
        fields["carrier_still_live"] = json!(true);
        fields["inner_relay_still_live"] = json!(false);
        retirement.closed = Some(actual_closed);
        fields["closed_sequence"] = json!(actual_closed.sequence);
        fields["closed_mono_ns"] = json!(actual_closed.mono_ns);
        self.record_stamped(
            "relay_retirement_closed",
            "rust.swarm.ConnectionClosed",
            fields,
        )
        .ok_or_else(|| io::Error::other("retirement closure trace overflow"))?;
        self.0
            .state
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .retirement = Some(retirement);
        Ok(())
    }

    fn application_failure(&self, peer: PeerId, phase: &str, error: &io::Error) {
        self.record("application_error", "rust.path_echo.native_failure", json!({
            "remote_peer_id": peer.to_string(), "phase": phase,
            "error": error.to_string().chars().take(256).collect::<String>(), "error_kind": format!("{:?}", error.kind())
        }));
        let mut state = self.0.state.lock().unwrap_or_else(|e| e.into_inner());
        state
            .application_error
            .get_or_insert_with(|| format!("{phase}: {error}").chars().take(256).collect());
    }

    fn expected_handler_terminal(
        &self,
        peer: PeerId,
        binding: &StreamBinding,
        error: &io::Error,
    ) -> bool {
        if !matches!(
            error.kind(),
            io::ErrorKind::UnexpectedEof | io::ErrorKind::ConnectionReset
        ) {
            return false;
        }
        let retirement = self
            .0
            .state
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .retirement
            .clone();
        let Some(retirement) = retirement.filter(|retirement| {
            retirement.requested.is_some()
                && retirement.relay.peer == peer
                && retirement.relay.transport == binding.connection
                && retirement.stream == binding.stream
                && binding.relayed
        }) else {
            return false;
        };
        let mut fields = retirement.receipt();
        fields["phase"] = json!("relay_after");
        fields["terminal_basis"] = json!("retained_handler_actual_read_error");
        fields["direction"] = json!("read");
        fields["io_bytes"] = Value::Null;
        fields["error_kind"] = json!(if error.kind() == io::ErrorKind::UnexpectedEof {
            "eof"
        } else {
            "reset"
        });
        fields["error"] = json!(error.to_string().chars().take(256).collect::<String>());
        fields["expected_retirement"] = json!(true);
        self.record(
            "retained_stream_terminal",
            "rust.path_echo.retained_stream.native_io",
            fields,
        );
        true
    }

    async fn retired_stream_terminal<S: AsyncRead + Unpin>(
        &self,
        stream: &mut S,
        retirement: &RelayRetirement,
    ) -> io::Result<()> {
        if retirement.requested.is_none() || retirement.closed.is_none() {
            return Err(io::Error::other(
                "retained stream terminal observation precedes actual relay closure",
            ));
        }
        let mut byte = [0];
        let result = tokio::time::timeout(Duration::from_secs(3), stream.read(&mut byte)).await;
        let (bytes, kind, error, expected) = match &result {
            Ok(Ok(0)) => (Some(0), "eof", "EOF".to_owned(), true),
            Ok(Ok(n)) => (
                Some(*n),
                "unexpected_data",
                "data arrived after native inner relay closure".to_owned(),
                false,
            ),
            Ok(Err(error)) => (
                None,
                match error.kind() {
                    io::ErrorKind::ConnectionReset => "reset",
                    io::ErrorKind::UnexpectedEof => "eof",
                    _ => "io_error",
                },
                error.to_string().chars().take(256).collect(),
                matches!(
                    error.kind(),
                    io::ErrorKind::ConnectionReset | io::ErrorKind::UnexpectedEof
                ),
            ),
            Err(_) => (
                None,
                "observation_timeout",
                "retained stream terminal observation deadline".to_owned(),
                false,
            ),
        };
        let mut fields = retirement.receipt();
        fields["phase"] = json!("relay_before");
        fields["terminal_basis"] = json!("retained_initiator_actual_read_after_native_close");
        fields["direction"] = json!("read");
        fields["io_bytes"] = json!(bytes);
        fields["error_kind"] = json!(kind);
        fields["error"] = json!(error);
        fields["expected_retirement"] = json!(expected);
        self.record(
            "retained_stream_terminal",
            "rust.path_echo.retained_stream.native_io",
            fields,
        );
        if expected {
            return Ok(());
        }
        match result {
            Ok(Err(error)) => Err(error),
            _ => Err(io::Error::other(error)),
        }
    }

    fn relay_application_owner(
        &self,
        peer: PeerId,
        connections: &NativeConnections,
    ) -> io::Result<LiveConnection> {
        let mut relays = connections
            .iter()
            .filter(|(_, (remote, point))| *remote == peer && point.is_relayed());
        let (&id, _) = relays
            .next()
            .ok_or_else(|| io::Error::other("no live inner relay application owner"))?;
        if relays.next().is_some() {
            return Err(io::Error::other("ambiguous inner relay application owner"));
        }
        self.live_connection(id, connections)
    }

    fn record_relay_open_request(
        &self,
        owner: &LiveConnection,
        connections: &NativeConnections,
        before: u64,
    ) -> io::Result<EventStamp> {
        let actual = self.relay_application_owner(owner.peer, connections)?;
        if actual.native != owner.native
            || actual.transport != owner.transport
            || actual.point != owner.point
        {
            return Err(io::Error::other(
                "inner relay application owner changed before request",
            ));
        }
        let fields = {
            let state = self.0.state.lock().unwrap_or_else(|e| e.into_inner());
            if state.overflow || state.expected != Some(owner.peer) || state.retirement.is_some() {
                return Err(io::Error::other(
                    "unbound, retired or overflowing inner relay application request",
                ));
            }
            let direct_ids = state
                .sessions
                .iter()
                .filter(|(_, peer, _, relayed)| *peer == owner.peer && !relayed)
                .map(|(id, _, _, _)| id.clone())
                .collect::<Vec<_>>();
            json!({
                "phase": "relay_before", "remote_peer_id": owner.peer.to_string(), "protocol": ECHO_PROTOCOL,
                "native_connection_id": owner.native.to_string(), "connection_id": owner.transport,
                "path": "relay", "endpoint": super::upgrade_observer::endpoint(&owner.point),
                "existing_connection_ids": [owner.transport.clone()], "existing_direct_connection_ids": direct_ids,
                "connected_before": true,
                "live_state_basis": "native_authenticated_outputs_retained_until_ConnectionClosed",
                "target_binding_basis": "exact_live_native_ConnectionId_authenticated_inner_relay",
                "open_api_basis": "native_notify_handler_one_no_dial_path",
                "dial_attempts_before": before, "dial_counter_basis": "native_stream_behaviour_dial_requests",
            })
        };
        self.record_stamped(
            "application_open_requested",
            "rust.path_application.NotifyHandler.One.request",
            fields,
        )
        .ok_or_else(|| io::Error::other("exact relay application request trace overflow"))
    }

    fn record_open_request(
        &self,
        peer: PeerId,
        phase: &str,
        ids: &[String],
        application_dials: u64,
        retirement: Option<&RelayRetirement>,
    ) -> io::Result<EventStamp> {
        let fields = {
            let state = self.0.state.lock().unwrap_or_else(|e| e.into_inner());
            if state.overflow || !matches!(phase, "relay_before" | "direct_after") {
                return Err(io::Error::other(
                    "invalid or overflowing application open request",
                ));
            }
            let direct_ids = state
                .sessions
                .iter()
                .filter(|(_, remote, _, relayed)| *remote == peer && !relayed)
                .map(|(id, _, _, _)| id.clone())
                .collect::<Vec<_>>();
            let available = state
                .sessions
                .iter()
                .filter(|(_, remote, _, relayed)| {
                    *remote == peer && *relayed == (phase == "relay_before")
                })
                .map(|(id, _, _, _)| id.clone())
                .collect::<Vec<_>>();
            if ids.is_empty() || ids != available.as_slice() {
                return Err(io::Error::other(
                    "application open request lacks its live authenticated owners",
                ));
            }
            let mut fields = json!({});
            if phase == "direct_after" {
                let retirement = retirement
                    .ok_or_else(|| io::Error::other("direct open request lacks retirement"))?;
                let stored = state.retirement.as_ref().ok_or_else(|| {
                    io::Error::other("direct open request has no accepted native retirement")
                })?;
                if state.expected != Some(peer)
                    || retirement.relay.peer != peer
                    || retirement.requested.is_none()
                    || retirement.closed.is_none()
                    || stored.receipt() != retirement.receipt()
                {
                    return Err(io::Error::other(
                        "direct open request retirement is unbound, unrequested or not actually closed",
                    ));
                }
                for original in [&retirement.direct, &retirement.carrier] {
                    let matches = state
                        .sessions
                        .iter()
                        .filter(|(id, remote, point, relayed)| {
                            *id == original.transport
                                && *remote == original.peer
                                && *point == original.point
                                && !relayed
                        })
                        .count();
                    if matches != 1 {
                        return Err(io::Error::other(
                            "direct or carrier is no longer authenticated/live before open_stream",
                        ));
                    }
                }
                if direct_ids != [retirement.direct.transport.clone()]
                    || state
                        .sessions
                        .iter()
                        .any(|(_, remote, _, relayed)| *remote == peer && *relayed)
                {
                    return Err(io::Error::other(
                        "direct open request has an ambiguous or relayed peer owner",
                    ));
                }
                let terminals = state
                    .events
                    .iter()
                    .filter(|event| {
                        event["kind"] == "retained_stream_terminal"
                            && event["source"] == "rust.path_echo.retained_stream.native_io"
                            && event["terminal_basis"]
                                == "retained_initiator_actual_read_after_native_close"
                            && event["remote_peer_id"] == peer.to_string()
                            && event["connection_id"] == retirement.relay.transport
                            && event["stream_id"] == retirement.stream
                            && event["expected_retirement"] == true
                            && event["requested_sequence"] == retirement.requested.unwrap().sequence
                            && event["closed_sequence"] == retirement.closed.unwrap().sequence
                    })
                    .collect::<Vec<_>>();
                if terminals.len() != 1 {
                    return Err(io::Error::other(
                        "direct open request lacks its actual retained terminal",
                    ));
                }
                let terminal = EventStamp::captured(terminals[0])?;
                if terminal.sequence <= retirement.closed.unwrap().sequence {
                    return Err(io::Error::other(
                        "retained terminal precedes actual native close",
                    ));
                }
                fields = retirement.receipt();
                // These identify the retired stream, not an unopened stream owner.
                let object = fields.as_object_mut().unwrap();
                for (old, new) in [
                    ("native_connection_id", "retired_relay_native_connection_id"),
                    ("connection_id", "retired_relay_connection_id"),
                    ("stream_id", "retired_relay_stream_id"),
                ] {
                    let value = object.remove(old).unwrap();
                    object.insert(new.into(), value);
                }
                fields["retained_terminal_sequence"] = json!(terminal.sequence);
                fields["retained_terminal_mono_ns"] = json!(terminal.mono_ns);
                fields["direct_still_live"] = json!(true);
                fields["carrier_still_live"] = json!(true);
                fields["inner_relay_still_live"] = json!(false);
            } else if retirement.is_some() {
                return Err(io::Error::other(
                    "relay-before open must not carry a retirement",
                ));
            }
            fields["phase"] = json!(phase);
            fields["remote_peer_id"] = json!(peer.to_string());
            fields["protocol"] = json!(ECHO_PROTOCOL);
            fields["existing_connection_ids"] = json!(ids);
            fields["existing_direct_connection_ids"] = json!(direct_ids);
            fields["connected_before"] = json!(true);
            fields["live_state_basis"] =
                json!("native_authenticated_outputs_retained_until_ConnectionClosed");
            fields["dial_attempts_before"] = json!(application_dials);
            fields["dial_counter_basis"] = json!("native_stream_behaviour_dial_requests");
            fields
        };
        self.record_stamped(
            "application_open_requested",
            "rust.libp2p_stream.Control.open_stream.request",
            fields,
        )
        .ok_or_else(|| io::Error::other("application open request trace overflow"))
    }
}

fn start_application(
    tasks: &super::task_owner::Owner,
    observer: Observer,
    mut control: libp2p_stream::Control,
    peer: PeerId,
    phase: String,
    old: Option<libp2p::Stream>,
    ids: Vec<String>,
    retirement: Option<RelayRetirement>,
) -> io::Result<ApplicationResult> {
    if !matches!(phase.as_str(), "relay_after" | "direct_after") {
        return Err(io::Error::other(
            "relay-before must use its exact native application owner",
        ));
    }
    let (send, receive) = futures::channel::oneshot::channel();
    tasks.spawn("path_application_control", async move {
        let result = async {
            let mut old = old;
            if let Some(retirement) = &retirement {
                observer
                    .retired_stream_terminal(
                        old.as_mut()
                            .ok_or_else(|| io::Error::other("missing retired raw relay stream"))?,
                        retirement,
                    )
                    .await?;
                drop(old.take());
                if observer.existing(peer, false) != ids
                    || !observer.existing(peer, true).is_empty()
                {
                    return Err(io::Error::other(
                        "direct owner changed before application open",
                    ));
                }
            }
            let before = observer.0.application_dials.load(Ordering::SeqCst);
            let mut stream = if phase == "relay_after" {
                old.ok_or_else(|| io::Error::other("missing retained relay"))?
            } else {
                observer.record_open_request(peer, &phase, &ids, before, retirement.as_ref())?;
                control
                    .open_stream(peer, StreamProtocol::new(ECHO_PROTOCOL))
                    .await
                    .map_err(io::Error::other)?
            };
            let after = observer.0.application_dials.load(Ordering::SeqCst);
            observer
                .transfer(
                    &mut stream,
                    peer,
                    Some(&phase),
                    false,
                    (phase != "relay_after")
                        .then_some(ApplicationOpening::Control(before, after, ids)),
                )
                .await?;
            if phase == "relay_before" || phase == "relay_after" {
                Ok(Some(stream))
            } else {
                stream.close().await?;
                Ok(None)
            }
        }
        .await;
        if let Err(error) = &result {
            observer.application_failure(peer, &phase, error);
        }
        let _ = send.send(result);
    })?;
    Ok(receive)
}

fn start_relay_application(
    tasks: &super::task_owner::Owner,
    observer: Observer,
    owner: LiveConnection,
    opened: application::Opened,
    requested: EventStamp,
    before: u64,
    deadline: tokio::time::Instant,
) -> io::Result<ApplicationResult> {
    let (send, receive) = futures::channel::oneshot::channel();
    tasks.spawn("path_application_exact_relay", async move {
        let peer = owner.peer;
        let result = async {
            let mut stream = tokio::time::timeout_at(deadline, opened)
                .await
                .map_err(|_| {
                    io::Error::new(
                        io::ErrorKind::TimedOut,
                        "exact relay application open deadline",
                    )
                })?
                .map_err(io::Error::other)??;
            let after = observer.0.application_dials.load(Ordering::SeqCst);
            observer
                .transfer(
                    &mut stream,
                    peer,
                    Some("relay_before"),
                    false,
                    Some(ApplicationOpening::ExactRelay {
                        before,
                        after,
                        owner,
                        requested,
                    }),
                )
                .await?;
            Ok(Some(stream))
        }
        .await;
        if let Err(error) = &result {
            observer.application_failure(peer, "relay_before", error);
        }
        let _ = send.send(result);
    })?;
    Ok(receive)
}

/// Shared main calls this after parsing path-live key/value flags, before its
/// legacy Options parser. All behaviour, candidate discovery and DCUtR is native.
pub(crate) async fn run_live(args: BTreeMap<String, String>) -> io::Result<()> {
    let arg = |key: &str| {
        args.get(key)
            .cloned()
            .ok_or_else(|| io::Error::other(format!("missing --{key}")))
    };
    if arg("transport")? != "quic"
        || arg("scenario")? != "dcutr"
        || args.contains_key("pnet-key-file")
    {
        return Err(io::Error::other("path-live is native QUIC only"));
    }
    let role = arg("path-role")?;
    if !matches!(role.as_str(), "relay" | "source" | "destination") {
        return Err(io::Error::other("invalid path role"));
    }
    let token = arg("case-token")?;
    let key = identity::Keypair::generate_ed25519();
    let local = key.public().to_peer_id();
    let observer = Observer::new(token.clone(), local).map_err(io::Error::other)?;
    let tasks = super::task_owner::Owner::default();
    let (relay_transport, relay_client) = relay::client::new(local);
    let circuit_observer = observer.clone();
    let circuit = relay_transport
        .upgrade(Version::V1)
        .authenticate(noise::Config::new(&key).map_err(io::Error::other)?)
        .multiplex(yamux::Config::default())
        .map(move |(peer, muxer), point| circuit_observer.output(peer, muxer, point, true))
        .boxed();
    let direct_observer = observer.clone();
    let direct = libp2p::quic::tokio::Transport::new(libp2p::quic::Config::new(&key))
        .map(move |(peer, muxer), point| direct_observer.output(peer, muxer, point, false))
        .boxed();
    let transport = PathTransport {
        inner: circuit
            .or_transport(direct)
            .map(|output, _| output.into_inner())
            .boxed(),
        observer: observer.clone(),
    }
    .boxed();
    let behaviour = native_behaviour(&key, &role, relay_client, observer.clone());
    let mut swarm = Swarm::new(transport, behaviour, local, tasks.swarm_config());
    let ready = PathBuf::from(arg("ready-file")?);
    let result = PathBuf::from(arg("result-file")?);
    let stop = PathBuf::from(arg("stop-file")?);
    let control_file = PathBuf::from(arg("control-file")?);
    let plan_file = PathBuf::from(arg("plan-file")?);
    let listen: Multiaddr = format!("/ip4/{}/udp/0/quic-v1", arg("bind-ip")?)
        .parse()
        .map_err(io::Error::other)?;
    let direct_listener_id = swarm.listen_on(listen).map_err(io::Error::other)?;
    let mut control = swarm.behaviour().stream.new_control();
    let mut incoming = control
        .accept(StreamProtocol::new(ECHO_PROTOCOL))
        .map_err(io::Error::other)?;
    let incoming_observer = observer.clone();
    let incoming_owner = tasks.clone();
    tasks.spawn("path_echo_accept", async move {
        while let Some((peer, mut stream)) = incoming.next().await {
            let observer = incoming_observer.clone();
            let errors = incoming_owner.spawner();
            let admission_observer = observer.clone();
            if let Err(error) = incoming_owner.spawn("path_echo_exchange", async move {
                let result: io::Result<()> = async {
                    let phase = observer
                        .transfer(&mut stream, peer, None, true, None)
                        .await?;
                    if phase == "relay_before" {
                        let binding = observer.resolve_echo(peer, "relay_before")?;
                        if let Err(error) = observer
                            .transfer(&mut stream, peer, Some("relay_after"), true, None)
                            .await
                        {
                            if observer.expected_handler_terminal(peer, &binding, &error) {
                                return Ok(());
                            }
                            return Err(error);
                        }
                    }
                    stream.close().await
                }
                .await;
                if let Err(error) = result {
                    observer.application_failure(peer, "incoming_handler", &error);
                    errors.record_error("path_echo_exchange", error);
                }
            }) {
                admission_observer.application_failure(peer, "incoming_handler_admission", &error);
                break;
            }
        }
    })?;
    let mut listen_addr = None;
    let mut local_listener = None;
    let mut relay_candidate: Option<RelayCandidate> = None;
    let mut circuit_addr = None;
    let mut reserved = false;
    let mut relay_listener_id = None;
    let mut relay_connected = role == "relay";
    let mut relay_advertised = role != "relay";
    let relay_peer = args
        .get("relay-peer-id")
        .map(|p| p.parse::<PeerId>())
        .transpose()
        .map_err(io::Error::other)?;
    if role != "relay" {
        let relay_addr: Multiaddr = arg("relay-addr")?.parse().map_err(io::Error::other)?;
        if !relay_addr
            .iter()
            .any(|p| matches!(p, Protocol::P2p(p) if Some(p) == relay_peer))
        {
            return Err(io::Error::other("relay identity mismatch"));
        }
        if role == "destination" {
            // The native circuit listener owns its relay dial and Reserve
            // command. A competing explicit dial can discard that command.
            relay_listener_id = Some(
                swarm
                    .listen_on(relay_addr.with(Protocol::P2pCircuit))
                    .map_err(io::Error::other)?,
            );
        } else {
            swarm.dial(relay_addr).map_err(io::Error::other)?;
        }
    }
    let started = Instant::now();
    let mut published = false;
    let mut sequence = 0u64;
    let mut retained: Option<libp2p::Stream> = None;
    let mut pending: Option<ApplicationResult> = None;
    let mut retiring: Option<(u64, PeerId, Instant)> = None;
    let mut connections = NativeConnections::new();
    let mut tick = tokio::time::interval(Duration::from_millis(25));
    let execution: io::Result<()> = async {
        loop {
            tokio::select! {
                event = swarm.select_next_some() => match event {
                    SwarmEvent::NewListenAddr { listener_id, address } => {
                        if Some(listener_id) == relay_listener_id {
                            let address = circuit_listener_address(&address, relay_peer.ok_or_else(|| io::Error::other("missing relay peer"))?, local)?;
                            observer.record("relay_listener_address", "rust.swarm.NewListenAddr.native_reservation", json!({
                                "listener_id": listener_id.to_string(), "address": address.to_string()
                            }));
                            circuit_addr = Some(address);
                        }
                        else {
                            if listener_id != direct_listener_id {
                                return Err(io::Error::other("unexpected native QUIC listener"));
                            }
                            quic_socket(&address, None)?;
                            observer.record("local_listener_address", "rust.swarm.NewListenAddr.actual_udp_listener", json!({
                                "listener_id": listener_id.to_string(), "local_listener_address": address.to_string()
                            }));
                            local_listener = Some(address.clone());
                            if let Some(external) = relay_listener_address(&role, &address)? {
                                observer.record("relay_external_address", "rust.swarm.NewListenAddr.actual_relay_wan", json!({
                                    "listener_id": listener_id.to_string(), "local_address": external.to_string(),
                                    "confirmation_basis": "owned_unnatted_wan_listener"
                                }));
                                swarm.add_external_address(external);
                            }
                            listen_addr = Some(address.with(Protocol::P2p(local)));
                        }
                    }
                    SwarmEvent::ListenerClosed { listener_id, reason, .. } => {
                        observer.record("native_listener_closed", "rust.swarm.ListenerClosed", json!({
                            "listener_id": listener_id.to_string(), "reservation_listener": Some(listener_id) == relay_listener_id,
                            "error": reason.as_ref().err().map(ToString::to_string)
                        }));
                        let detail = reason.err().map(|error| error.to_string()).unwrap_or_else(|| "native listener channel closed".into());
                        return Err(io::Error::other(format!("native path listener {listener_id} closed: {detail}")));
                    }
                    SwarmEvent::ListenerError { listener_id, error } => {
                        observer.record("native_listener_error", "rust.swarm.ListenerError", json!({
                            "listener_id": listener_id.to_string(), "reservation_listener": Some(listener_id) == relay_listener_id,
                            "error": error.to_string()
                        }));
                    }
                    SwarmEvent::OutgoingConnectionError { peer_id, connection_id, error } => {
                        let stamp = observer.record_stamped("native_dial_error", "rust.swarm.OutgoingConnectionError", json!({
                            "native_connection_id": connection_id.to_string(), "remote_peer_id": peer_id.map(|peer| peer.to_string()),
                            "error": error.to_string()
                        }));
                        if let Some(stamp) = stamp {
                            observer.native_wave_failure(connection_id, peer_id, &error, &connections, stamp);
                        }
                    }
                    SwarmEvent::ConnectionEstablished { peer_id, connection_id, endpoint, .. } => {
                        relay_connected |= Some(peer_id) == relay_peer;
                        observer.record("swarm_connection", "rust.swarm.ConnectionEstablished", json!({
                            "native_connection_id": connection_id.to_string(), "remote_peer_id": peer_id.to_string(),
                            "endpoint": super::upgrade_observer::endpoint(&endpoint)
                        }));
                        connections.insert(connection_id, (peer_id, endpoint));
                    }
                    SwarmEvent::ConnectionClosed { connection_id, peer_id, endpoint, cause, num_established } => {
                        let owner = observer.live_connection(connection_id, &connections)?;
                        let cause = cause.as_ref().map(|error| error.to_string().chars().take(256).collect::<String>());
                        let stamp = observer.record_stamped("native_connection_closed", "rust.swarm.ConnectionClosed", json!({
                            "native_connection_id": connection_id.to_string(), "connection_id": owner.transport,
                            "remote_peer_id": peer_id.to_string(), "endpoint": super::upgrade_observer::endpoint(&endpoint),
                            "cause": cause, "remaining_established": num_established
                        })).ok_or_else(|| io::Error::other("native closure trace overflow"))?;
                        if owner.peer != peer_id || owner.point != endpoint { return Err(io::Error::other("native closed event differs from its authenticated live owner")); }
                        if let Some((peer, point)) = connections.remove(&connection_id) { observer.closed(peer, &point); }
                        observer.retirement_closed(connection_id, peer_id, &endpoint, cause.as_deref(), stamp, &connections)?;
                        relay_connected = role == "relay" || connections.values().any(|(peer, point)|
                            Some(*peer) == relay_peer && !point.is_relayed());
                    }
                    SwarmEvent::Behaviour(PathBehaviourEvent::Dcutr(event)) => observer.native_event(&event),
                    SwarmEvent::Behaviour(PathBehaviourEvent::Ping(event)) => {
                        let result = match event.result {
                            Ok(rtt) => json!({"success": true, "rtt_seconds": rtt.as_secs_f64()}),
                            Err(error) => json!({"success": false, "error": error.to_string()}),
                        };
                        observer.record("native_ping_result", "rust.ping.Event", json!({
                            "native_connection_id": event.connection.to_string(), "remote_peer_id": event.peer.to_string(),
                            "result": result
                        }));
                    }
                    SwarmEvent::Behaviour(PathBehaviourEvent::RelayService(relay::Event::StatusChanged { status })) => {
                        relay_advertised = status == relay::Status::Enable;
                        observer.record("native_relay_advertisement", "rust.relay.Event.StatusChanged", json!({
                            "hop_advertised": relay_advertised, "protocol": "/libp2p/circuit/relay/0.2.0/hop"
                        }));
                    }
                    SwarmEvent::Behaviour(PathBehaviourEvent::RelayClient(relay::client::Event::ReservationReqAccepted { relay_peer_id, renewal, .. })) => {
                        observer.record("relay_reservation_accepted", "rust.relay.client.Event.ReservationReqAccepted", json!({
                            "relay_peer_id": relay_peer_id.to_string(), "renewal": renewal,
                            "listener_id": relay_listener_id.map(|id| id.to_string())
                        }));
                        if role != "destination" || Some(relay_peer_id) != relay_peer {
                            return Err(io::Error::other("native reservation accepted by unexpected relay"));
                        }
                        reserved = true;
                    }
                    SwarmEvent::Behaviour(PathBehaviourEvent::Identify(identify::Event::Received { peer_id, connection_id, info })) => {
                        if Some(peer_id) == relay_peer {
                            let (actual_peer, point) = connections.get(&connection_id)
                                .ok_or_else(|| io::Error::other("Identify carrier is not established"))?;
                            if *actual_peer != peer_id || info.public_key.to_peer_id() != peer_id || point.is_relayed() {
                                return Err(io::Error::other("Identify does not belong to the authenticated relay carrier"));
                            }
                            let carrier = observer.authenticated_carrier(peer_id, point)?;
                            let listener = local_listener.as_ref()
                                .ok_or_else(|| io::Error::other("Identify precedes actual QUIC listener receipt"))?;
                            let mut candidate = RelayCandidate::new(local, connection_id, carrier, direct_listener_id,
                                listener.clone(), info.observed_addr)?;
                            if let Some(previous) = relay_candidate.as_ref() {
                                if previous.native_connection_id == candidate.native_connection_id
                                    && previous.carrier_connection_id == candidate.carrier_connection_id
                                    && previous.listener_id == candidate.listener_id
                                    && previous.listener_address == candidate.listener_address
                                    && previous.observed_address == candidate.observed_address {
                                    candidate.propagated = previous.propagated;
                                }
                            }
                            observer.record("identify_observed_address", "rust.identify.Event.Received", candidate.receipt(peer_id));
                            // Identify queues its candidate after Received. Confirming here
                            // suppresses that native event, leaving DCUtR with no addresses.
                            relay_candidate = Some(candidate);
                        }
                    }
                    SwarmEvent::NewExternalAddrCandidate { address } => {
                        if let Some(candidate) = relay_candidate.as_mut() {
                            if connections.get(&candidate.native_connection_id).is_some_and(|(peer, point)|
                                Some(*peer) == relay_peer && !point.is_relayed())
                                && candidate.accept_native_candidate(&address) {
                                // Swarm broadcasts FromSwarm to every behaviour before
                                // yielding this event. This is native DCUtR admission,
                                // not a declaration that unilateral NAT ingress works.
                                observer.record("native_dcutr_candidate_ready", "rust.swarm.NewExternalAddrCandidate", candidate.receipt(
                                    relay_peer.ok_or_else(|| io::Error::other("missing candidate observer peer"))?));
                            }
                        }
                    }
                    _ => {}
                },
                _ = tick.tick() => {
                    let application_error = observer.0.state.lock().unwrap_or_else(|e| e.into_inner()).application_error.clone();
                    if let Some(error) = application_error { return Err(io::Error::other(error)); }
                    if stop.exists() {
                        if !published { return Err(io::Error::new(io::ErrorKind::Interrupted, "path fixture stopped before native readiness")); }
                        if retiring.is_some() { return Err(io::Error::new(io::ErrorKind::Interrupted, "path fixture stopped before actual inner relay closure")); }
                        break;
                    }
                    if started.elapsed() > Duration::from_secs(55) { return Err(io::Error::new(io::ErrorKind::TimedOut, "path fixture deadline")); }
                    if !published && listen_addr.is_some() && relay_connected && relay_advertised
                        && (role == "relay" || relay_candidate.as_ref().is_some_and(|candidate| candidate.propagated))
                        && (role != "destination" || reserved && circuit_addr.is_some()) {
                        observer.record("native_ready", "rust.swarm.native_composition", json!({"role": role}));
                        atomic(&ready, &json!({"status": "ready", "implementation": "rust", "peer_id": local.to_string(), "case_token": token,
                            "listen_addrs": [listen_addr.as_ref().unwrap().to_string()], "circuit_addr": circuit_addr.as_ref().map(ToString::to_string),
                            "path_bindings": "actual_io_and_native_attempt_v1",
                            "dcutr_candidate": relay_candidate.as_ref().zip(relay_peer).map(|(candidate, peer)| candidate.receipt(peer))}))?;
                        published = true;
                    }
                    if let Some(receiver) = pending.as_mut() {
                        match receiver.try_recv().map_err(io::Error::other)? {
                            Some(value) => { retained = value?; pending = None; }, None => {}
                        }
                    }
                    if let Some((control_sequence, peer, requested_at)) = retiring {
                        if requested_at.elapsed() > Duration::from_secs(3) { return Err(io::Error::new(io::ErrorKind::TimedOut, "native inner relay close deadline")); }
                        let retirement = observer.0.state.lock().unwrap_or_else(|e| e.into_inner()).retirement.clone()
                            .ok_or_else(|| io::Error::other("missing accepted retirement state"))?;
                        if retirement.closed.is_some() {
                            observer.retirement_survivors(&retirement, &connections)?;
                            let ids = vec![retirement.direct.transport.clone()];
                            pending = Some(start_application(&tasks, observer.clone(), control.clone(), peer,
                                "direct_after".into(), retained.take(), ids, Some(retirement))?);
                            retiring = None;
                            observer.record("control_completed", "rust.path_control.native_call", json!({
                                "control_sequence": control_sequence, "action": "direct_after"
                            }));
                        }
                    }
                    if published && control_file.exists() {
                        let command = fields(&control_file)?;
                        let next: u64 = command.get("sequence").ok_or_else(|| io::Error::other("missing sequence"))?.parse().map_err(io::Error::other)?;
                        if next > sequence {
                            if next != sequence + 1 || command.get("case-token") != Some(&token) || pending.is_some() || retiring.is_some() { return Err(io::Error::other("control sequence/token/overlap")); }
                            let plan = fields(&plan_file)?;
                            if plan.get("case-token") != Some(&token) { return Err(io::Error::other("plan token mismatch")); }
                            let peer: PeerId = plan.get("peer-id").ok_or_else(|| io::Error::other("missing expected peer"))?.parse().map_err(io::Error::other)?;
                            let action = command.get("action").ok_or_else(|| io::Error::other("missing action"))?;
                            match action.as_str() {
                                "bind" => {
                                    let direct_ids = connections.iter().filter(|(_, (remote, point))| *remote == peer && !point.is_relayed()).map(|(id, _)| *id).collect::<Vec<_>>();
                                    observer.bind(peer, &direct_ids).map_err(io::Error::other)?;
                                }
                                "connect" => {
                                    let address: Multiaddr = plan.get("circuit-addr").ok_or_else(|| io::Error::other("missing circuit"))?.parse().map_err(io::Error::other)?;
                                    if !address.iter().any(|p| matches!(p, Protocol::P2p(p) if p == peer)) { return Err(io::Error::other("circuit peer mismatch")); }
                                    swarm.dial(address).map_err(io::Error::other)?;
                                }
                                "barrier" => {
                                    if retained.is_some() { return Err(io::Error::other("original relay application stream already retained")); }
                                    let owner = observer.relay_application_owner(peer, &connections)?;
                                    let before = observer.0.application_dials.load(Ordering::SeqCst);
                                    let requested = observer.record_relay_open_request(&owner, &connections, before)?;
                                    let admitted = tokio::time::Instant::now();
                                    let opened = swarm.behaviour_mut().application.open(peer, owner.native)?;
                                    pending = Some(start_relay_application(&tasks, observer.clone(), owner, opened, requested, before,
                                        admitted + Duration::from_secs(3))?);
                                }
                                "relay_after" => {
                                    let ids = observer.existing(peer, true);
                                    if ids.is_empty() { return Err(io::Error::other("no existing required native path")); }
                                    pending = Some(start_application(&tasks, observer.clone(), control.clone(), peer,
                                        action.clone(), retained.take(), ids, None)?);
                                }
                                "direct_after" => {
                                    // Native peer-only stream Control has no direct preference.
                                    // Retire only the proven inner relay, never the carrier or a
                                    // failed/cancelled path, and drive Swarm to actual closure.
                                    let retirement = observer.prepare_retirement(peer,
                                        relay_peer.ok_or_else(|| io::Error::other("missing relay identity for retirement"))?,
                                        plan.get("outcome").map(String::as_str).unwrap_or(""), retained.is_some(), &connections)?;
                                    let accepted = swarm.close_connection(retirement.relay.native);
                                    observer.retirement_requested(retirement, accepted)?;
                                    retiring = Some((next, peer, Instant::now()));
                                }
                                "release" => { observer.record("barrier_observed", "rust.path_echo.control", json!({"case_token": token})); }
                                "cancel" => {
                                    return Err(io::Error::other("pinned DCUtR behaviour has no public per-upgrade cancellation/join; dropping Swarm would destroy relay"));
                                }
                                _ => return Err(io::Error::other("unsupported path control")),
                            }
                            sequence = next;
                            if action != "direct_after" {
                                observer.record("control_completed", "rust.path_control.native_call", json!({"control_sequence": next, "action": action}));
                            }
                        }
                    }
                    atomic(&result, &observer.result(false, false, None))?;
                }
            }
        }
        Ok(())
    }.await;
    drop(retained);
    drop(control);
    drop(swarm);
    let report = tasks.close_and_join().await;
    let report_value = report.snapshot();
    let joined = report_value["fixture_owned_tasks_joined"] == true
        && report_value["overflow"] == false
        && report_value["errors"]
            .as_array()
            .is_some_and(|errors| errors.is_empty());
    let error = execution.as_ref().err().map(ToString::to_string);
    let mut final_result = observer.result(true, joined, error.as_deref());
    final_result["task_join"] = report_value;
    atomic(&result, &final_result)?;
    execution
}

#[cfg(test)]
mod tests {
    use super::*;
    use libp2p::{
        core::upgrade::UpgradeInfo,
        swarm::{
            ConnectionHandler, ConnectionHandlerEvent, DialError,
            behaviour::{DialFailure, ExternalAddrConfirmed, ExternalAddrExpired},
        },
    };
    use std::collections::VecDeque;

    fn composition(role: &str) -> (relay::client::Transport, PathBehaviour, PeerId) {
        let key = identity::Keypair::generate_ed25519();
        let local = key.public().to_peer_id();
        let observer = Observer::new("0123456789abcdef0123456789abcdef".into(), local).unwrap();
        let (transport, client) = relay::client::new(local);
        (
            transport,
            native_behaviour(&key, role, client, observer),
            local,
        )
    }

    fn inbound_protocols(behaviour: &mut PathBehaviour) -> Vec<String> {
        let handler = behaviour
            .handle_established_inbound_connection(
                ConnectionId::new_unchecked(1),
                PeerId::random(),
                &"/ip4/11.0.0.1/udp/40100/quic-v1".parse().unwrap(),
                &"/ip4/11.0.0.2/udp/40200/quic-v1".parse().unwrap(),
            )
            .unwrap();
        handler
            .listen_protocol()
            .upgrade()
            .protocol_info()
            .map(|protocol| AsRef::<str>::as_ref(&protocol).to_owned())
            .collect()
    }

    fn reservation_dial(
        transport: &mut relay::client::Transport,
        behaviour: &mut PathBehaviour,
        cx: &mut Context<'_>,
    ) -> libp2p::swarm::dial_opts::DialOpts {
        assert!(Pin::new(transport).poll(cx).is_pending());
        match behaviour.poll(cx) {
            Poll::Ready(ToSwarm::Dial { opts }) => opts,
            _ => panic!("native circuit listener did not request its relay dial"),
        }
    }

    #[derive(Default)]
    struct ScriptedIo {
        reads: VecDeque<Poll<io::Result<Vec<u8>>>>,
        writes: VecDeque<Poll<io::Result<usize>>>,
        written: Vec<u8>,
        flushes: usize,
        closes: usize,
    }

    impl AsyncRead for ScriptedIo {
        fn poll_read(
            self: Pin<&mut Self>,
            _: &mut Context<'_>,
            bytes: &mut [u8],
        ) -> Poll<io::Result<usize>> {
            if bytes.is_empty() {
                return Poll::Ready(Ok(0));
            }
            match self
                .get_mut()
                .reads
                .pop_front()
                .unwrap_or(Poll::Ready(Ok(Vec::new())))
            {
                Poll::Ready(Ok(data)) => {
                    assert!(data.len() <= bytes.len());
                    bytes[..data.len()].copy_from_slice(&data);
                    Poll::Ready(Ok(data.len()))
                }
                Poll::Ready(Err(error)) => Poll::Ready(Err(error)),
                Poll::Pending => Poll::Pending,
            }
        }
    }

    impl AsyncWrite for ScriptedIo {
        fn poll_write(
            self: Pin<&mut Self>,
            _: &mut Context<'_>,
            bytes: &[u8],
        ) -> Poll<io::Result<usize>> {
            let this = self.get_mut();
            let result = this
                .writes
                .pop_front()
                .unwrap_or(Poll::Ready(Ok(bytes.len())));
            if let Poll::Ready(Ok(n)) = &result {
                assert!(*n <= bytes.len());
                this.written.extend_from_slice(&bytes[..*n]);
            }
            result
        }
        fn poll_flush(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
            self.get_mut().flushes += 1;
            Poll::Ready(Ok(()))
        }
        fn poll_close(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
            self.get_mut().closes += 1;
            Poll::Ready(Ok(()))
        }
    }

    fn observed_io(inner: ScriptedIo) -> (PathIo<ScriptedIo>, Observer) {
        let observer =
            Observer::new("0123456789abcdef0123456789abcdef".into(), PeerId::random()).unwrap();
        let trace = Trace {
            observer: observer.clone(),
            connection: "unit-muxer".into(),
            remote: PeerId::random(),
            relayed: true,
        };
        (PathIo::new(inner, trace), observer)
    }

    fn selected_wire(protocol: &str) -> Vec<u8> {
        let mut bytes = Vec::new();
        for name in ["/multistream/1.0.0\n", protocol] {
            assert!(name.len() < 128);
            bytes.push(name.len() as u8);
            bytes.extend_from_slice(name.as_bytes());
        }
        bytes
    }

    fn connect_wire() -> Vec<u8> {
        let address = "/ip4/11.0.0.3/udp/44503/quic-v1"
            .parse::<Multiaddr>()
            .unwrap()
            .to_vec();
        let mut body = vec![8, 100, 18, address.len() as u8];
        body.extend(address);
        assert!(body.len() < 128);
        let mut wire = vec![body.len() as u8];
        wire.extend(body);
        wire
    }

    fn io_events(observer: &Observer, kind: &str) -> Vec<Value> {
        observer.result(false, false, None)["events"]
            .as_array()
            .unwrap()
            .iter()
            .filter(|event| event["kind"] == kind)
            .cloned()
            .collect()
    }

    fn native_wave_unit() -> (Observer, NativeConnections, PeerId, Multiaddr) {
        // Scripted bytes and callback inputs exercise receipts, NOT live proof.
        let peer = PeerId::random();
        let observer =
            Observer::new("0123456789abcdef0123456789abcdef".into(), PeerId::random()).unwrap();
        observer.bind(peer, &[]).unwrap();
        let point = ConnectedPoint::Dialer {
            address: format!(
                "/ip4/11.0.0.1/udp/40100/quic-v1/p2p/{}/p2p-circuit/p2p/{peer}",
                PeerId::random()
            )
            .parse()
            .unwrap(),
            role_override: Endpoint::Dialer,
            port_use: PortUse::Reuse,
        };
        let connections =
            NativeConnections::from([(ConnectionId::new_unchecked(2), (peer, point.clone()))]);
        observer.0.state.lock().unwrap().sessions.push((
            "unit-wave-relay".into(),
            peer,
            point,
            true,
        ));
        let trace = Trace {
            observer: observer.clone(),
            connection: "unit-wave-relay".into(),
            remote: peer,
            relayed: true,
        };
        let mut stream = PathIo::new(ScriptedIo::default(), trace);
        let mut cx = Context::from_waker(futures::task::noop_waker_ref());
        let mut connect = selected_wire("/libp2p/dcutr\n");
        connect.extend(connect_wire());
        stream
            .inner
            .reads
            .push_back(Poll::Ready(Ok(connect.clone())));
        assert!(
            matches!(Pin::new(&mut stream).poll_read(&mut cx, &mut vec![0; connect.len()]), Poll::Ready(Ok(n)) if n == connect.len())
        );
        assert!(
            matches!(Pin::new(&mut stream).poll_write(&mut cx, &connect), Poll::Ready(Ok(n)) if n == connect.len())
        );
        let sync = vec![3, 8, 172, 2];
        stream.inner.reads.push_back(Poll::Ready(Ok(sync.clone())));
        assert!(
            matches!(Pin::new(&mut stream).poll_read(&mut cx, &mut vec![0; sync.len()]), Poll::Ready(Ok(n)) if n == sync.len())
        );
        assert_eq!(io_events(&observer, "dcutr_frame").len(), 3);
        (
            observer,
            connections,
            peer,
            "/ip4/11.0.0.3/udp/44503/quic-v1".parse().unwrap(),
        )
    }

    fn unit_native_request(
        observer: &Observer,
        connections: &NativeConnections,
        peer: PeerId,
        address: &Multiaddr,
    ) -> ConnectionId {
        let opts = libp2p::swarm::dial_opts::DialOpts::peer_id(peer)
            .addresses(vec![address.clone()])
            .build();
        let id = opts.connection_id();
        observer.native_wave_requested(&opts, connections);
        assert_eq!(opts.connection_id(), id);
        assert_eq!(opts.get_peer_id(), Some(peer));
        id
    }

    #[test]
    fn relay_application_request_targets_inner_owner_after_concurrent_native_success() {
        // Constructed callback/session state tests composition, not live DCUtR.
        let (observer, mut connections, peer, _) = native_wave_unit();
        let direct_id = ConnectionId::new_unchecked(333);
        let direct = ConnectedPoint::Dialer {
            address: format!("/ip4/11.0.0.3/udp/44503/quic-v1/p2p/{peer}")
                .parse()
                .unwrap(),
            role_override: Endpoint::Dialer,
            port_use: PortUse::Reuse,
        };
        connections.insert(direct_id, (peer, direct.clone()));
        observer.0.state.lock().unwrap().sessions.push((
            "unit-concurrent-direct".into(),
            peer,
            direct,
            false,
        ));
        observer.native_event(&dcutr::Event {
            remote_peer_id: peer,
            result: Ok(direct_id),
        });
        let owner = observer
            .relay_application_owner(peer, &connections)
            .unwrap();
        let before = observer.0.application_dials.load(Ordering::SeqCst);
        let requested = observer
            .record_relay_open_request(&owner, &connections, before)
            .unwrap();
        let mut behaviour = composition("source").1;
        behaviour
            .application
            .on_swarm_event(FromSwarm::ConnectionEstablished(
                libp2p::swarm::behaviour::ConnectionEstablished {
                    peer_id: peer,
                    connection_id: owner.native,
                    endpoint: &owner.point,
                    failed_addresses: &[],
                    other_established: 1,
                },
            ));
        let _receiver = behaviour.application.open(peer, owner.native).unwrap();
        let mut cx = Context::from_waker(futures::task::noop_waker_ref());
        match behaviour.application.poll(&mut cx) {
            Poll::Ready(ToSwarm::NotifyHandler {
                peer_id,
                handler: libp2p::swarm::NotifyHandler::One(id),
                ..
            }) => {
                assert_eq!(peer_id, peer);
                assert_eq!(id, owner.native);
                assert_ne!(id, direct_id);
            }
            _ => panic!("exact relay application request did not preserve native owner"),
        }
        assert!(behaviour.application.poll(&mut cx).is_pending());
        let receipts = io_events(&observer, "application_open_requested");
        assert_eq!(receipts.len(), 1);
        let receipt = &receipts[0];
        assert_eq!(
            receipt["source"],
            "rust.path_application.NotifyHandler.One.request"
        );
        assert_eq!(receipt["native_connection_id"], owner.native.to_string());
        assert_eq!(receipt["connection_id"], "unit-wave-relay");
        assert_eq!(
            receipt["existing_direct_connection_ids"],
            json!(["unit-concurrent-direct"])
        );
        assert_eq!(receipt["phase"], "relay_before");
        assert_eq!(receipt["sequence"], requested.sequence);
        assert!(
            requested.sequence
                > io_events(&observer, "native_dcutr_event")[0]["sequence"]
                    .as_u64()
                    .unwrap()
        );
        assert_eq!(observer.0.application_dials.load(Ordering::SeqCst), before);
        assert!(io_events(&observer, "application_dial_requested").is_empty());
    }

    #[test]
    fn exact_relay_pre_call_requires_bound_unique_live_authenticated_owner_and_trace_capacity() {
        for mismatch in [
            "closed",
            "native_id",
            "peer",
            "auth_output",
            "ambiguous",
            "baseline",
            "overflow",
            "capacity",
        ] {
            let (observer, mut connections, peer, _) = native_wave_unit();
            let mut owner = observer
                .relay_application_owner(peer, &connections)
                .unwrap();
            match mismatch {
                "closed" => {
                    connections.remove(&owner.native);
                }
                "native_id" => owner.native = ConnectionId::new_unchecked(888),
                "peer" => owner.peer = PeerId::random(),
                "auth_output" => {
                    observer.0.state.lock().unwrap().sessions[0].0 = "other-output".into()
                }
                "ambiguous" => {
                    connections.insert(
                        ConnectionId::new_unchecked(888),
                        (peer, owner.point.clone()),
                    );
                }
                "baseline" => observer.0.state.lock().unwrap().expected = Some(PeerId::random()),
                "overflow" => observer.0.state.lock().unwrap().overflow = true,
                "capacity" => {
                    while observer.0.state.lock().unwrap().events.len() < EVENT_LIMIT {
                        observer.record("unit", "rust.unit", json!({}));
                    }
                }
                _ => unreachable!(),
            }
            assert!(
                observer
                    .record_relay_open_request(&owner, &connections, 0)
                    .is_err(),
                "{mismatch}"
            );
            assert!(io_events(&observer, "application_open_requested").is_empty());
        }
    }

    #[tokio::test]
    async fn relay_before_cannot_fall_back_to_random_stream_control() {
        let (_transport, behaviour, local) = composition("source");
        let observer = Observer::new("0123456789abcdef0123456789abcdef".into(), local).unwrap();
        let tasks = crate::task_owner::Owner::default();
        assert!(
            start_application(
                &tasks,
                observer.clone(),
                behaviour.stream.new_control(),
                PeerId::random(),
                "relay_before".into(),
                None,
                vec!["unit-relay".into()],
                None
            )
            .is_err()
        );
        assert!(io_events(&observer, "application_open_requested").is_empty());
        let joined = tasks.close_and_join().await.snapshot();
        assert_eq!(joined["fixture_owned_tasks_joined"], true);
        assert_eq!(joined["errors"], json!([]));
    }

    fn unit_quic_failure(address: &Multiaddr) -> libp2p::swarm::DialError {
        let choice =
            PathChoiceError::Right(io::Error::other(libp2p::quic::Error::HandshakeTimedOut));
        libp2p::swarm::DialError::Transport(vec![(
            address.clone(),
            libp2p::core::transport::TransportError::Other(io::Error::other(io::Error::other(
                choice,
            ))),
        )])
    }

    fn unit_report_failure(
        observer: &Observer,
        id: ConnectionId,
        peer: Option<PeerId>,
        error: &libp2p::swarm::DialError,
        connections: &NativeConnections,
    ) {
        let stamp = observer.record_stamped("native_dial_error", "rust.swarm.OutgoingConnectionError", json!({
            "native_connection_id": id.to_string(), "remote_peer_id": peer.map(|peer| peer.to_string()), "error": error.to_string()
        }));
        if let Some(stamp) = stamp {
            observer.native_wave_failure(id, peer, error, connections, stamp);
        }
    }

    #[test]
    fn source_wave_failure_binds_actual_receipt_refs_and_never_aggregate_completion() {
        let (observer, connections, peer, address) = native_wave_unit();
        let id = unit_native_request(&observer, &connections, peer, &address);
        let candidates = vec![address.with(Protocol::P2p(peer))];
        observer.native_wave_pending(id, Some(peer), &candidates, Endpoint::Dialer, &Ok(vec![]));
        let error = unit_quic_failure(&candidates[0]);
        let original = error.to_string();
        unit_report_failure(&observer, id, Some(peer), &error, &connections);
        assert_eq!(error.to_string(), original);
        let failed = io_events(&observer, "native_dcutr_wave_failed");
        assert_eq!(failed.len(), 1);
        let failed = &failed[0];
        assert_eq!(failed["native_connection_id"], id.to_string());
        assert_eq!(failed["relay_native_connection_id"], "2");
        assert_eq!(failed["relay_connection_id"], "unit-wave-relay");
        assert_eq!(failed["proof_scope"], "native_wave_only");
        assert_eq!(failed["aggregate_dcutr_completed"], false);
        assert_eq!(failed["rust_behaviour_joined"], false);
        let frames = io_events(&observer, "dcutr_frame");
        for (name, frame) in ["connect_read", "connect_write", "sync_read"]
            .into_iter()
            .zip(&frames)
        {
            assert_eq!(failed[format!("{name}_sequence")], frame["sequence"]);
            assert_eq!(failed[format!("{name}_mono_ns")], frame["mono_ns"]);
            assert_eq!(failed["stream_id"], frame["stream_id"]);
        }
        assert!(
            failed["sync_read_sequence"].as_u64().unwrap()
                < failed["requested_sequence"].as_u64().unwrap()
        );
        assert!(
            failed["requested_sequence"].as_u64().unwrap()
                < failed["pending_sequence"].as_u64().unwrap()
        );
        assert!(
            failed["pending_sequence"].as_u64().unwrap()
                < failed["raw_error_sequence"].as_u64().unwrap()
        );
        assert_eq!(
            failed["transport_errors"][0]["quic_error_variant"],
            "HandshakeTimedOut"
        );
        assert_eq!(
            failed["transport_errors"][0]["classification_basis"],
            "typed_downcast_and_public_native_enum"
        );
        for kind in [
            "native_dcutr_event",
            "operation_finished",
            "relay_retirement_requested",
        ] {
            assert!(io_events(&observer, kind).is_empty());
        }
        unit_report_failure(&observer, id, Some(peer), &error, &connections);
        assert_eq!(io_events(&observer, "native_dcutr_wave_failed").len(), 1);
    }

    #[test]
    fn source_wave_requires_complete_original_peer_stream_and_sync() {
        for mismatch in [
            "missing_sync",
            "direction",
            "stream",
            "transport",
            "peer",
            "sync_addresses",
            "incomplete",
            "source",
            "protocol",
        ] {
            let (observer, connections, peer, address) = native_wave_unit();
            {
                let mut state = observer.0.state.lock().unwrap();
                let sync = state.events.last_mut().unwrap();
                match mismatch {
                    "missing_sync" => {
                        state.events.pop();
                    }
                    "direction" => sync["direction"] = json!("write"),
                    "stream" => sync["stream_id"] = json!("different"),
                    "transport" => sync["connection_id"] = json!("different"),
                    "peer" => sync["remote_peer_id"] = json!(PeerId::random().to_string()),
                    "sync_addresses" => sync["addresses"] = json!([address.to_string()]),
                    "incomplete" => sync["receipt"]["read"]["complete_frames"] = json!(false),
                    "source" => sync["source"] = json!("rust.not_native_muxer"),
                    "protocol" => sync["protocol"] = json!(ECHO_PROTOCOL),
                    _ => unreachable!(),
                }
            }
            let id = unit_native_request(&observer, &connections, peer, &address);
            observer.native_wave_pending(
                id,
                Some(peer),
                &[address.clone()],
                Endpoint::Dialer,
                &Ok(vec![]),
            );
            unit_report_failure(
                &observer,
                id,
                Some(peer),
                &unit_quic_failure(&address),
                &connections,
            );
            assert!(
                io_events(&observer, "native_dcutr_wave_failed").is_empty(),
                "{mismatch}"
            );
            assert_eq!(
                io_events(&observer, "native_dcutr_dial_requested")[0]["source_wave_bound"],
                false
            );
        }
    }

    #[test]
    fn native_pending_mismatch_does_not_change_return_or_admit_later_wave_failure() {
        for mismatch in [
            "peer",
            "role",
            "port",
            "peer_suffix",
            "returned_addresses",
            "denied",
            "duplicate",
        ] {
            let (observer, connections, peer, address) = native_wave_unit();
            let id = unit_native_request(&observer, &connections, peer, &address);
            let mut wrapper = PathDcutr {
                inner: dcutr::Behaviour::new(observer.0.local),
                observer: observer.clone(),
                relays: connections.clone(),
            };
            let candidate: Multiaddr = match mismatch {
                "port" => "/ip4/11.0.0.3/udp/44504/quic-v1".parse().unwrap(),
                "peer_suffix" => address.clone().with(Protocol::P2p(PeerId::random())),
                _ => address.clone(),
            };
            let callback_peer = Some(if mismatch == "peer" {
                PeerId::random()
            } else {
                peer
            });
            let role = if mismatch == "role" {
                Endpoint::Listener
            } else {
                Endpoint::Dialer
            };
            let result =
                wrapper.handle_pending_outbound_connection(id, callback_peer, &[candidate], role);
            assert!(
                result.as_ref().is_ok_and(Vec::is_empty),
                "wrapper changed native result"
            );
            match mismatch {
                "returned_addresses" => observer.native_wave_pending(
                    id,
                    Some(peer),
                    &[address.clone()],
                    Endpoint::Dialer,
                    &Ok(vec![address.clone()]),
                ),
                "denied" => observer.native_wave_pending(
                    id,
                    Some(peer),
                    &[address.clone()],
                    Endpoint::Dialer,
                    &Err(ConnectionDenied::new(io::Error::other("unit denied"))),
                ),
                "duplicate" => observer.native_wave_pending(
                    id,
                    Some(peer),
                    &[address.clone()],
                    Endpoint::Dialer,
                    &Ok(vec![]),
                ),
                _ => {}
            }
            unit_report_failure(
                &observer,
                id,
                Some(peer),
                &unit_quic_failure(&address),
                &connections,
            );
            assert!(
                io_events(&observer, "native_dcutr_wave_failed").is_empty(),
                "{mismatch}"
            );
            assert!(!io_events(&observer, "native_dcutr_wave_binding_rejected").is_empty());
        }
    }

    #[test]
    fn wave_error_requires_same_id_peer_pending_original_owner_and_order() {
        for mismatch in [
            "id",
            "peer",
            "no_pending",
            "closed",
            "transport",
            "before_pending",
        ] {
            let (observer, mut connections, peer, address) = native_wave_unit();
            let id = unit_native_request(&observer, &connections, peer, &address);
            let error = unit_quic_failure(&address);
            let early = observer
                .record_stamped(
                    "native_dial_error",
                    "rust.swarm.OutgoingConnectionError",
                    json!({}),
                )
                .unwrap();
            if mismatch != "no_pending" {
                observer.native_wave_pending(
                    id,
                    Some(peer),
                    &[address.clone()],
                    Endpoint::Dialer,
                    &Ok(vec![]),
                );
            }
            match mismatch {
                "closed" => {
                    connections.remove(&ConnectionId::new_unchecked(2));
                }
                "transport" => observer.0.state.lock().unwrap().sessions[0].0 = "different".into(),
                _ => {}
            }
            if mismatch == "before_pending" {
                observer.native_wave_failure(id, Some(peer), &error, &connections, early);
            } else {
                unit_report_failure(
                    &observer,
                    if mismatch == "id" {
                        ConnectionId::new_unchecked(999)
                    } else {
                        id
                    },
                    Some(if mismatch == "peer" {
                        PeerId::random()
                    } else {
                        peer
                    }),
                    &error,
                    &connections,
                );
            }
            assert!(
                io_events(&observer, "native_dcutr_wave_failed").is_empty(),
                "{mismatch}"
            );
        }
    }

    #[test]
    fn typed_quic_error_survives_boxed_native_choice_without_text_classification() {
        let error = io::Error::other(io::Error::other(PathChoiceError::Right(io::Error::other(
            libp2p::quic::Error::HandshakeTimedOut,
        ))));
        assert_eq!(
            native_quic_error(&error).unwrap()["quic_error_variant"],
            "HandshakeTimedOut"
        );
        assert_eq!(
            native_quic_error(&io::Error::other(
                libp2p::quic::Error::NoActiveListenerForDialAsListener
            ))
            .unwrap()["quic_error_variant"],
            "NoActiveListenerForDialAsListener"
        );
        for message in [
            "Handshake with the remote timed out.",
            "HandshakeTimedOut",
            "QUIC Connection timed out",
        ] {
            assert!(native_quic_error(&io::Error::other(message)).is_none());
        }
        let mut too_deep = io::Error::other(libp2p::quic::Error::HandshakeTimedOut);
        for _ in 0..12 {
            too_deep = io::Error::other(too_deep);
        }
        assert!(native_quic_error(&too_deep).is_none());
    }

    #[test]
    fn typed_wave_failure_rejects_unsupported_untyped_or_changed_candidate_errors() {
        use libp2p::core::transport::TransportError;
        let (_, _, peer, address) = native_wave_unit();
        let changed: Multiaddr = "/ip4/11.0.0.3/udp/44504/quic-v1".parse().unwrap();
        for error in [
            libp2p::swarm::DialError::Aborted,
            libp2p::swarm::DialError::Transport(vec![]),
            libp2p::swarm::DialError::Transport(vec![(
                address.clone(),
                TransportError::MultiaddrNotSupported(address.clone()),
            )]),
            libp2p::swarm::DialError::Transport(vec![(
                address.clone(),
                TransportError::Other(io::Error::other("HandshakeTimedOut")),
            )]),
            unit_quic_failure(&changed),
        ] {
            assert!(native_quic_failures(&error, &[address.clone()], peer).is_none());
        }
        let duplicate = libp2p::swarm::DialError::Transport(vec![
            (
                address.clone(),
                TransportError::Other(io::Error::other(libp2p::quic::Error::HandshakeTimedOut)),
            ),
            (
                address.clone(),
                TransportError::Other(io::Error::other(libp2p::quic::Error::HandshakeTimedOut)),
            ),
        ]);
        assert!(native_quic_failures(&duplicate, &[address], peer).is_none());
    }

    #[test]
    fn native_wave_cannot_claim_an_old_sync_twice_or_ignore_observation_overflow() {
        for mismatch in ["claimed", "overflow", "capacity"] {
            let (observer, connections, peer, address) = native_wave_unit();
            let id = unit_native_request(&observer, &connections, peer, &address);
            observer.native_wave_pending(
                id,
                Some(peer),
                &[address.clone()],
                Endpoint::Dialer,
                &Ok(vec![]),
            );
            if mismatch == "claimed" {
                let other = unit_native_request(&observer, &connections, peer, &address);
                observer.native_wave_pending(
                    other,
                    Some(peer),
                    &[address.clone()],
                    Endpoint::Dialer,
                    &Ok(vec![]),
                );
                unit_report_failure(
                    &observer,
                    other,
                    Some(peer),
                    &unit_quic_failure(&address),
                    &connections,
                );
            } else {
                if mismatch == "overflow" {
                    observer.0.state.lock().unwrap().overflow = true;
                } else {
                    while observer.0.state.lock().unwrap().events.len() < EVENT_LIMIT {
                        observer.record("unit", "rust.unit", json!({}));
                    }
                }
                unit_report_failure(
                    &observer,
                    id,
                    Some(peer),
                    &unit_quic_failure(&address),
                    &connections,
                );
            }
            assert!(io_events(&observer, "native_dcutr_wave_failed").is_empty());
        }
    }

    #[test]
    fn native_wave_count_limit_fails_closed_without_altering_dial_opts() {
        let (observer, connections, peer, address) = native_wave_unit();
        let id = unit_native_request(&observer, &connections, peer, &address);
        let wave = observer.0.state.lock().unwrap().waves[&id].clone();
        for index in 1000..1000 + WAVE_LIMIT {
            observer
                .0
                .state
                .lock()
                .unwrap()
                .waves
                .insert(ConnectionId::new_unchecked(index), wave.clone());
        }
        let other = unit_native_request(&observer, &connections, peer, &address);
        assert!(observer.0.state.lock().unwrap().overflow);
        assert!(!observer.0.state.lock().unwrap().waves.contains_key(&other));
        unit_report_failure(
            &observer,
            id,
            Some(peer),
            &unit_quic_failure(&address),
            &connections,
        );
        assert!(io_events(&observer, "native_dcutr_wave_failed").is_empty());
    }

    #[tokio::test]
    async fn passive_dcutr_delegates_native_handlers_candidates_and_failure_events() {
        let local = PeerId::random();
        let peer = PeerId::random();
        let observer = Observer::new("0123456789abcdef0123456789abcdef".into(), local).unwrap();
        let mut wrapper = PathDcutr {
            inner: dcutr::Behaviour::new(local),
            observer: observer.clone(),
            relays: BTreeMap::new(),
        };
        let mut native = dcutr::Behaviour::new(local);
        let candidate: Multiaddr = "/ip4/11.0.0.2/udp/40200/quic-v1".parse().unwrap();
        let discovered = FromSwarm::NewExternalAddrCandidate(
            libp2p::swarm::behaviour::NewExternalAddrCandidate { addr: &candidate },
        );
        native.on_swarm_event(discovered);
        wrapper.on_swarm_event(discovered);
        let addr: Multiaddr = format!(
            "/ip4/11.0.0.1/udp/40100/quic-v1/p2p/{}/p2p-circuit/p2p/{peer}",
            PeerId::random()
        )
        .parse()
        .unwrap();
        let id = ConnectionId::new_unchecked(123);
        let mut expected_handler = native
            .handle_established_inbound_connection(id, peer, &addr, &candidate)
            .unwrap();
        let mut actual_handler = wrapper
            .handle_established_inbound_connection(id, peer, &addr, &candidate)
            .unwrap();
        let mut cx = Context::from_waker(futures::task::noop_waker_ref());
        match (expected_handler.poll(&mut cx), actual_handler.poll(&mut cx)) {
            (
                Poll::Ready(ConnectionHandlerEvent::OutboundSubstreamRequest {
                    protocol: expected,
                }),
                Poll::Ready(ConnectionHandlerEvent::OutboundSubstreamRequest { protocol: actual }),
            ) => {
                let expected: Vec<String> = expected
                    .upgrade()
                    .protocol_info()
                    .map(|p| AsRef::<str>::as_ref(&p).to_owned())
                    .collect();
                let actual: Vec<String> = actual
                    .upgrade()
                    .protocol_info()
                    .map(|p| AsRef::<str>::as_ref(&p).to_owned())
                    .collect();
                assert_eq!(actual, expected);
            }
            _ => panic!("passive wrapper changed native inbound DCUtR handler request"),
        }
        let error = unit_quic_failure(&candidate);
        let failed = FromSwarm::DialFailure(DialFailure {
            peer_id: Some(peer),
            error: &error,
            connection_id: id,
        });
        native.on_swarm_event(failed);
        wrapper.on_swarm_event(failed);
        assert!(native.poll(&mut cx).is_pending());
        assert!(wrapper.poll(&mut cx).is_pending());
        assert!(io_events(&observer, "native_dcutr_event").is_empty());
        assert!(io_events(&observer, "native_dcutr_wave_failed").is_empty());
    }

    fn retirement_unit() -> (Observer, NativeConnections, PeerId, PeerId) {
        // Constructed branch-test inputs only; never live DCUtR evidence.
        let peer = PeerId::random();
        let carrier_peer = PeerId::random();
        let observer =
            Observer::new("0123456789abcdef0123456789abcdef".into(), PeerId::random()).unwrap();
        observer.bind(peer, &[]).unwrap();
        let carrier = ConnectedPoint::Dialer {
            address: format!("/ip4/11.0.0.1/udp/40001/quic-v1/p2p/{carrier_peer}")
                .parse()
                .unwrap(),
            role_override: Endpoint::Dialer,
            port_use: PortUse::Reuse,
        };
        let relay = ConnectedPoint::Dialer {
            address: format!(
                "/ip4/11.0.0.1/udp/40001/quic-v1/p2p/{carrier_peer}/p2p-circuit/p2p/{peer}"
            )
            .parse()
            .unwrap(),
            role_override: Endpoint::Dialer,
            port_use: PortUse::Reuse,
        };
        let direct = ConnectedPoint::Dialer {
            address: format!("/ip4/11.0.0.3/udp/40003/quic-v1/p2p/{peer}")
                .parse()
                .unwrap(),
            role_override: Endpoint::Listener,
            port_use: PortUse::Reuse,
        };
        let connections = NativeConnections::from([
            (
                ConnectionId::new_unchecked(1),
                (carrier_peer, carrier.clone()),
            ),
            (ConnectionId::new_unchecked(2), (peer, relay.clone())),
            (ConnectionId::new_unchecked(3), (peer, direct.clone())),
        ]);
        observer.0.state.lock().unwrap().sessions = vec![
            ("transport-1".into(), carrier_peer, carrier, false),
            ("transport-2".into(), peer, relay, true),
            ("transport-3".into(), peer, direct, false),
        ];
        observer.native_event(&dcutr::Event {
            remote_peer_id: peer,
            result: Ok(ConnectionId::new_unchecked(3)),
        });
        for direction in ["read", "write"] {
            observer.record("application_frame", "rust.native_muxer.application.io", json!({
                "remote_peer_id": peer.to_string(), "phase": "relay_before", "direction": direction,
                "connection_id": "transport-2", "stream_id": "unit-retained", "path": "relay"
            }));
        }
        observer.record(
            "echo",
            "rust.path_echo.io",
            json!({
                "remote_peer_id": peer.to_string(), "phase": "relay_before", "server": false,
                "connection_id": "transport-2", "stream_id": "unit-retained", "path": "relay"
            }),
        );
        (observer, connections, peer, carrier_peer)
    }

    fn requested_retirement_unit() -> (Observer, NativeConnections, RelayRetirement) {
        let (observer, connections, peer, carrier_peer) = retirement_unit();
        let retirement = observer
            .prepare_retirement(peer, carrier_peer, "success", true, &connections)
            .unwrap();
        observer.retirement_requested(retirement, true).unwrap();
        let retirement = observer.0.state.lock().unwrap().retirement.clone().unwrap();
        (observer, connections, retirement)
    }

    fn closed_retirement_unit() -> (Observer, RelayRetirement) {
        let (observer, mut connections, retirement) = requested_retirement_unit();
        connections.remove(&retirement.relay.native);
        observer.closed(retirement.relay.peer, &retirement.relay.point);
        let stamp = observer.record_stamped("native_connection_closed", "rust.swarm.ConnectionClosed", json!({
            "native_connection_id": retirement.relay.native.to_string(), "connection_id": retirement.relay.transport,
            "cause": null
        })).unwrap();
        observer
            .retirement_closed(
                retirement.relay.native,
                retirement.relay.peer,
                &retirement.relay.point,
                None,
                stamp,
                &connections,
            )
            .unwrap();
        let retirement = observer.0.state.lock().unwrap().retirement.clone().unwrap();
        (observer, retirement)
    }

    #[test]
    fn retirement_never_admits_failure_cancel_missing_raw_stream_or_success() {
        for outcome in ["failed", "cancelled", "", "success"] {
            let (observer, connections, peer, carrier_peer) = retirement_unit();
            assert!(
                observer
                    .prepare_retirement(
                        peer,
                        carrier_peer,
                        outcome,
                        outcome != "success",
                        &connections
                    )
                    .is_err()
            );
            assert!(io_events(&observer, "relay_retirement_requested").is_empty());
            assert!(connections.contains_key(&ConnectionId::new_unchecked(2)));
        }
        for missing in ["native_dcutr_event", "echo", "application_frame"] {
            let (observer, connections, peer, carrier_peer) = retirement_unit();
            observer
                .0
                .state
                .lock()
                .unwrap()
                .events
                .retain(|event| event["kind"] != missing);
            assert!(
                observer
                    .prepare_retirement(peer, carrier_peer, "success", true, &connections)
                    .is_err()
            );
        }
    }

    #[test]
    fn retirement_requires_same_live_authenticated_success_peer_inner_owner_and_carrier() {
        for defect in [
            "peer",
            "dead_direct",
            "missing_auth",
            "ambiguous_auth",
            "wrong_success",
            "wrong_stream_owner",
            "dead_carrier",
            "carrier_port",
            "extra_owner",
        ] {
            let (observer, mut connections, peer, carrier_peer) = retirement_unit();
            match defect {
                "peer" => observer.0.state.lock().unwrap().expected = Some(PeerId::random()),
                "dead_direct" => {
                    connections.remove(&ConnectionId::new_unchecked(3));
                }
                "missing_auth" => observer
                    .0
                    .state
                    .lock()
                    .unwrap()
                    .sessions
                    .retain(|(id, _, _, _)| id != "transport-3"),
                "ambiguous_auth" => {
                    let mut state = observer.0.state.lock().unwrap();
                    let duplicate = state.sessions[2].clone();
                    state.sessions.push(duplicate);
                }
                "wrong_success" => observer.native_event(&dcutr::Event {
                    remote_peer_id: peer,
                    result: Ok(ConnectionId::new_unchecked(2)),
                }),
                "wrong_stream_owner" => {
                    observer.0.state.lock().unwrap().sessions[1].0 = "different-owner".into()
                }
                "dead_carrier" => {
                    connections.remove(&ConnectionId::new_unchecked(1));
                }
                "carrier_port" => {
                    let point = ConnectedPoint::Dialer {
                        address: format!("/ip4/11.0.0.1/udp/40009/quic-v1/p2p/{carrier_peer}")
                            .parse()
                            .unwrap(),
                        role_override: Endpoint::Dialer,
                        port_use: PortUse::Reuse,
                    };
                    connections
                        .get_mut(&ConnectionId::new_unchecked(1))
                        .unwrap()
                        .1 = point.clone();
                    observer.0.state.lock().unwrap().sessions[0].2 = point;
                }
                "extra_owner" => {
                    connections.insert(
                        ConnectionId::new_unchecked(4),
                        connections[&ConnectionId::new_unchecked(3)].clone(),
                    );
                }
                _ => unreachable!(),
            }
            assert!(
                observer
                    .prepare_retirement(peer, carrier_peer, "success", true, &connections)
                    .is_err(),
                "{defect}"
            );
        }
    }

    #[test]
    fn rejected_close_and_request_alone_never_become_live_closed_receipts() {
        let (observer, connections, peer, carrier_peer) = retirement_unit();
        let retirement = observer
            .prepare_retirement(peer, carrier_peer, "success", true, &connections)
            .unwrap();
        assert!(observer.retirement_requested(retirement, false).is_err());
        assert_eq!(
            io_events(&observer, "relay_retirement_requested")[0]["native_close_accepted"],
            false
        );
        assert!(observer.0.state.lock().unwrap().retirement.is_none());
        let (observer, connections, retirement) = requested_retirement_unit();
        assert!(
            observer
                .retirement_survivors(&retirement, &connections)
                .is_err()
        );
        assert!(retirement.closed.is_none());
        assert!(io_events(&observer, "relay_retirement_closed").is_empty());
    }

    #[test]
    fn actual_close_requires_exact_inner_endpoint_and_live_unchanged_survivors() {
        for defect in [
            "peer",
            "endpoint",
            "cause",
            "direct",
            "carrier",
            "still_relay",
        ] {
            let (observer, mut connections, retirement) = requested_retirement_unit();
            if defect != "still_relay" {
                connections.remove(&retirement.relay.native);
                observer.closed(retirement.relay.peer, &retirement.relay.point);
            }
            let peer = if defect == "peer" {
                PeerId::random()
            } else {
                retirement.relay.peer
            };
            let point = if defect == "endpoint" {
                &retirement.direct.point
            } else {
                &retirement.relay.point
            };
            let cause = (defect == "cause").then_some("unit unexpected native close error");
            if defect == "direct" {
                connections.remove(&retirement.direct.native);
            }
            if defect == "carrier" {
                connections.remove(&retirement.carrier.native);
            }
            let stamp = observer
                .record_stamped(
                    "native_connection_closed",
                    "rust.swarm.ConnectionClosed",
                    json!({"cause": cause}),
                )
                .unwrap();
            assert!(
                observer
                    .retirement_closed(
                        retirement.relay.native,
                        peer,
                        point,
                        cause,
                        stamp,
                        &connections
                    )
                    .is_err(),
                "{defect}"
            );
            assert!(io_events(&observer, "relay_retirement_closed").is_empty());
        }
        let (observer, retirement) = closed_retirement_unit();
        let requested = &io_events(&observer, "relay_retirement_requested")[0];
        let closed = &io_events(&observer, "relay_retirement_closed")[0];
        for field in [
            "native_connection_id",
            "connection_id",
            "stream_id",
            "remote_peer_id",
            "direct_native_connection_id",
            "direct_connection_id",
            "carrier_native_connection_id",
            "carrier_connection_id",
        ] {
            assert_eq!(requested[field], closed[field], "{field}");
        }
        assert_eq!(closed["requested_sequence"], requested["sequence"]);
        assert_eq!(closed["requested_mono_ns"], requested["mono_ns"]);
        assert_eq!(
            closed["closed_sequence"],
            retirement.closed.unwrap().sequence
        );
        assert_eq!(closed["direct_still_live"], true);
        assert_eq!(closed["carrier_still_live"], true);
        assert_eq!(closed["inner_relay_still_live"], false);
        assert_eq!(closed["automatic_direct_preference_proven"], false);
        assert!(retirement.closed.unwrap().sequence > retirement.requested.unwrap().sequence);
    }

    #[tokio::test]
    async fn retained_terminal_records_actual_eof_reset_and_rejects_other_results() {
        for outcome in ["eof", "reset", "unexpected_eof", "other", "data"] {
            let (observer, retirement) = closed_retirement_unit();
            let read = match outcome {
                "eof" => Ok(Vec::new()),
                "data" => Ok(vec![1]),
                "reset" => Err(io::Error::new(
                    io::ErrorKind::ConnectionReset,
                    "unit actual reset",
                )),
                "unexpected_eof" => Err(io::Error::new(
                    io::ErrorKind::UnexpectedEof,
                    "unit actual EOF",
                )),
                _ => Err(io::Error::new(
                    io::ErrorKind::PermissionDenied,
                    "unit original unexpected error",
                )),
            };
            let mut io = ScriptedIo {
                reads: VecDeque::from([Poll::Ready(read)]),
                ..Default::default()
            };
            let result = observer.retired_stream_terminal(&mut io, &retirement).await;
            assert_eq!(
                result.is_ok(),
                matches!(outcome, "eof" | "reset" | "unexpected_eof")
            );
            if outcome == "other" {
                let error = result.unwrap_err();
                assert_eq!(error.kind(), io::ErrorKind::PermissionDenied);
                assert_eq!(error.to_string(), "unit original unexpected error");
            }
            let terminals = io_events(&observer, "retained_stream_terminal");
            assert_eq!(terminals.len(), 1);
            assert_eq!(terminals[0]["stream_id"], retirement.stream);
            assert_eq!(
                terminals[0]["closed_sequence"],
                retirement.closed.unwrap().sequence
            );
            assert_eq!(
                terminals[0]["expected_retirement"],
                matches!(outcome, "eof" | "reset" | "unexpected_eof")
            );
            if outcome == "eof" {
                assert_eq!(terminals[0]["io_bytes"], 0);
            }
            if outcome == "reset" {
                assert_eq!(terminals[0]["error"], "unit actual reset");
            }
        }
        let (observer, _, retirement) = requested_retirement_unit();
        assert!(
            observer
                .retired_stream_terminal(&mut ScriptedIo::default(), &retirement)
                .await
                .is_err()
        );
        assert!(io_events(&observer, "retained_stream_terminal").is_empty());
    }

    #[test]
    fn handler_terminal_is_bound_to_requested_retained_stream_not_any_peer_error() {
        let (observer, _, retirement) = requested_retirement_unit();
        let mut binding = StreamBinding {
            connection: retirement.relay.transport.clone(),
            stream: retirement.stream.clone(),
            relayed: true,
        };
        let reset = io::Error::new(io::ErrorKind::ConnectionReset, "unit handler reset");
        assert!(!observer.expected_handler_terminal(PeerId::random(), &binding, &reset));
        binding.stream = "different-stream".into();
        assert!(!observer.expected_handler_terminal(retirement.relay.peer, &binding, &reset));
        binding.stream = retirement.stream.clone();
        assert!(!observer.expected_handler_terminal(
            retirement.relay.peer,
            &binding,
            &io::Error::from(io::ErrorKind::PermissionDenied)
        ));
        assert!(observer.expected_handler_terminal(retirement.relay.peer, &binding, &reset));
        assert_eq!(io_events(&observer, "retained_stream_terminal").len(), 1);
        observer.application_failure(
            retirement.relay.peer,
            "incoming_handler",
            &io::Error::from(io::ErrorKind::PermissionDenied),
        );
        assert!(observer.0.state.lock().unwrap().application_error.is_some());
    }

    #[tokio::test]
    async fn open_request_receipt_follows_actual_terminal_and_rechecks_live_owners() {
        let (observer, retirement) = closed_retirement_unit();
        observer
            .retired_stream_terminal(&mut ScriptedIo::default(), &retirement)
            .await
            .unwrap();
        let stamp = observer
            .record_open_request(
                retirement.direct.peer,
                "direct_after",
                &[retirement.direct.transport.clone()],
                0,
                Some(&retirement),
            )
            .unwrap();
        let request = &io_events(&observer, "application_open_requested")[0];
        let terminal = &io_events(&observer, "retained_stream_terminal")[0];
        assert_eq!(
            request["source"],
            "rust.libp2p_stream.Control.open_stream.request"
        );
        assert_eq!(request["phase"], "direct_after");
        assert_eq!(
            request["existing_direct_connection_ids"],
            json!([retirement.direct.transport])
        );
        assert_eq!(
            request["direct_native_connection_id"],
            retirement.direct.native.to_string()
        );
        assert_eq!(
            request["carrier_native_connection_id"],
            retirement.carrier.native.to_string()
        );
        assert_eq!(
            request["retired_relay_native_connection_id"],
            retirement.relay.native.to_string()
        );
        assert_eq!(request["retired_relay_stream_id"], retirement.stream);
        assert_eq!(
            request["requested_sequence"],
            retirement.requested.unwrap().sequence
        );
        assert_eq!(
            request["closed_sequence"],
            retirement.closed.unwrap().sequence
        );
        assert_eq!(request["retained_terminal_sequence"], terminal["sequence"]);
        assert_eq!(request["dial_attempts_before"], 0);
        assert_eq!(request["direct_still_live"], true);
        assert_eq!(request["carrier_still_live"], true);
        assert!(stamp.sequence > terminal["sequence"].as_u64().unwrap());
        assert!(terminal["sequence"].as_u64().unwrap() > retirement.closed.unwrap().sequence);
    }

    #[tokio::test]
    async fn open_request_rejects_unrequested_unclosed_missing_terminal_dead_carrier_and_overflow()
    {
        for defect in [
            "no_retirement",
            "unrequested",
            "unclosed",
            "no_terminal",
            "direct",
            "carrier",
            "overflow",
            "capacity",
        ] {
            let (observer, mut retirement) = closed_retirement_unit();
            if defect != "no_terminal" {
                observer
                    .retired_stream_terminal(&mut ScriptedIo::default(), &retirement)
                    .await
                    .unwrap();
            }
            match defect {
                "unrequested" => retirement.requested = None,
                "unclosed" => retirement.closed = None,
                "direct" => observer.closed(retirement.direct.peer, &retirement.direct.point),
                "carrier" => observer.closed(retirement.carrier.peer, &retirement.carrier.point),
                "overflow" => observer.0.state.lock().unwrap().overflow = true,
                "capacity" => observer
                    .0
                    .state
                    .lock()
                    .unwrap()
                    .events
                    .resize(EVENT_LIMIT, Value::Null),
                _ => {}
            }
            assert!(
                observer
                    .record_open_request(
                        retirement.direct.peer,
                        "direct_after",
                        &[retirement.direct.transport.clone()],
                        0,
                        (defect != "no_retirement").then_some(&retirement)
                    )
                    .is_err(),
                "{defect}"
            );
            assert!(
                io_events(&observer, "application_open_requested").is_empty(),
                "{defect}"
            );
            assert_eq!(observer.0.application_dials.load(Ordering::SeqCst), 0);
        }
    }

    #[tokio::test]
    async fn public_swarm_close_updates_control_before_fresh_echo_on_existing_survivor() {
        // Actual localhost QUIC/Swarm/stream test, NOT a native DCUtR or NAT proof.
        fn actor(owner: &super::super::task_owner::Owner) -> (Swarm<PathStreams>, Observer) {
            let key = identity::Keypair::generate_ed25519();
            let local = key.public().to_peer_id();
            let observer = Observer::new("0123456789abcdef0123456789abcdef".into(), local).unwrap();
            let capture = observer.clone();
            let transport = libp2p::quic::tokio::Transport::new(libp2p::quic::Config::new(&key))
                .map(move |(peer, muxer), point| capture.output(peer, muxer, point, false))
                .boxed();
            let behaviour = PathStreams {
                inner: libp2p_stream::Behaviour::new(),
                observer: observer.clone(),
            };
            (
                Swarm::new(
                    transport,
                    behaviour,
                    local,
                    owner
                        .swarm_config()
                        .with_idle_connection_timeout(Duration::from_secs(10)),
                ),
                observer,
            )
        }
        let owner = super::super::task_owner::Owner::default();
        let (mut sender, observer) = actor(&owner);
        let (mut receiver, receiver_observer) = actor(&owner);
        let peer = *receiver.local_peer_id();
        let mut receiver_control = receiver.behaviour().new_control();
        let mut incoming = receiver_control
            .accept(StreamProtocol::new(ECHO_PROTOCOL))
            .unwrap();
        for _ in 0..2 {
            receiver
                .listen_on("/ip4/127.0.0.1/udp/0/quic-v1".parse().unwrap())
                .unwrap();
        }
        let exchange = tokio::time::timeout(Duration::from_secs(8), async {
            let mut addresses = Vec::new();
            while addresses.len() < 2 {
                if let SwarmEvent::NewListenAddr { address, .. } = receiver.select_next_some().await { addresses.push(address); }
            }
            let mut connections = NativeConnections::new();
            for address in addresses {
                sender.dial(libp2p::swarm::dial_opts::DialOpts::peer_id(peer)
                    .condition(libp2p::swarm::dial_opts::PeerCondition::Always).addresses(vec![address]).build()).unwrap();
                let before = connections.len();
                while connections.len() == before {
                    tokio::select! {
                        _ = receiver.select_next_some() => {},
                        event = sender.select_next_some() => if let SwarmEvent::ConnectionEstablished { connection_id, peer_id, endpoint, .. } = event {
                            assert_eq!(peer_id, peer); connections.insert(connection_id, (peer_id, endpoint));
                        },
                    }
                }
            }
            let close_id = *connections.keys().next().unwrap();
            let survivor_id = *connections.keys().next_back().unwrap();
            assert!(sender.close_connection(close_id));
            loop {
                tokio::select! {
                    _ = receiver.select_next_some() => {},
                    event = sender.select_next_some() => if let SwarmEvent::ConnectionClosed { connection_id, peer_id, endpoint, cause, .. } = event {
                        assert_eq!(connection_id, close_id); assert_eq!(peer_id, peer); assert!(cause.is_none());
                        connections.remove(&connection_id); observer.closed(peer_id, &endpoint); break;
                    },
                }
            }
            assert!(!sender.close_connection(close_id));
            let survivor = observer.live_connection(survivor_id, &connections).unwrap();
            // Exercise the public primitive directly: this test has no DCUtR or
            // relay retirement and must not fabricate a path-live open request.
            let (send, mut application) = futures::channel::oneshot::channel();
            let mut control = sender.behaviour().new_control();
            let capture = observer.clone();
            let ids = vec![survivor.transport.clone()];
            owner.spawn("unit_existing_open", async move {
                let result: io::Result<Option<libp2p::Stream>> = async {
                    let before = capture.0.application_dials.load(Ordering::SeqCst);
                    let mut stream = control.open_stream(peer, StreamProtocol::new(ECHO_PROTOCOL)).await.map_err(io::Error::other)?;
                    let after = capture.0.application_dials.load(Ordering::SeqCst);
                    capture.transfer(&mut stream, peer, Some("direct_after"), false, Some(ApplicationOpening::Control(before, after, ids))).await?;
                    stream.close().await?;
                    Ok(None)
                }.await;
                let _ = send.send(result);
            }).unwrap();
            loop {
                tokio::select! {
                    result = &mut application => { assert!(result.unwrap().unwrap().is_none()); break; },
                    _ = receiver.select_next_some() => {},
                    event = sender.select_next_some() => {
                        assert!(!matches!(event, SwarmEvent::ConnectionEstablished { .. } | SwarmEvent::Dialing { .. }));
                    },
                    received = incoming.next() => {
                        let (remote, mut stream) = received.unwrap();
                        let capture = receiver_observer.clone(); let errors = owner.spawner();
                        owner.spawn("unit_existing_echo", async move {
                            let result: io::Result<()> = async {
                                capture.transfer(&mut stream, remote, None, true, None).await?;
                                stream.close().await
                            }.await;
                            if let Err(error) = result { errors.record_error("unit_existing_echo", error); }
                        }).unwrap();
                    },
                }
            }
            let opening = &io_events(&observer, "application_open")[0];
            assert_eq!(opening["connection_id"], survivor.transport);
            assert_eq!(opening["dial_attempts_before"], 0);
            assert_eq!(opening["dial_attempts_after"], 0);
            assert!(io_events(&observer, "native_dcutr_event").is_empty());
        }).await;
        drop(incoming);
        drop(receiver_control);
        drop(sender);
        drop(receiver);
        let report = owner.close_and_join().await.snapshot();
        assert_eq!(report["fixture_owned_tasks_joined"], true);
        assert_eq!(report["errors"], json!([]));
        exchange.expect("public native close / existing stream echo deadline");
    }

    #[test]
    fn read_eof_retains_actual_stream_binding_counts_and_partial_bytes() {
        // Scripted I/O tests capture wrapper receipts, not live DCUtR proof.
        let mut bytes = selected_wire("/libp2p/dcutr\n");
        bytes.extend(connect_wire());
        bytes.extend([3, 8]);
        let inner = ScriptedIo {
            reads: VecDeque::from([Poll::Ready(Ok(bytes.clone()))]),
            ..Default::default()
        };
        let (mut stream, observer) = observed_io(inner);
        let mut cx = Context::from_waker(futures::task::noop_waker_ref());
        let mut output = vec![0; bytes.len()];
        match Pin::new(&mut stream).poll_read(&mut cx, &mut output) {
            Poll::Ready(Ok(n)) => {
                assert_eq!(n, bytes.len());
                assert_eq!(output, bytes);
            }
            _ => panic!("actual read result changed"),
        }
        assert!(io_events(&observer, "dcutr_stream_terminal").is_empty());
        for _ in 0..2 {
            assert!(matches!(
                Pin::new(&mut stream).poll_read(&mut cx, &mut output),
                Poll::Ready(Ok(0))
            ));
        }
        let frames = io_events(&observer, "dcutr_frame");
        let terminal = io_events(&observer, "dcutr_stream_terminal");
        assert_eq!(frames.len(), 1);
        assert_eq!(terminal.len(), 1);
        let terminal = &terminal[0];
        assert_eq!(terminal["source"], "rust.native_muxer.dcutr.io");
        for field in ["connection_id", "stream_id", "remote_peer_id", "protocol"] {
            assert_eq!(terminal[field], frames[0][field]);
        }
        assert!(terminal["sequence"].as_u64().unwrap() > frames[0]["sequence"].as_u64().unwrap());
        assert_eq!(terminal["direction"], "read");
        assert_eq!(terminal["error_kind"], "eof");
        assert_eq!(terminal["error"], "EOF");
        assert_eq!(terminal["io_bytes"], 0);
        assert_eq!(terminal["completed_frame_count"], 1);
        assert_eq!(terminal["pending_frame_bytes"], 2);
        assert_eq!(terminal["invalid_or_over_limit"], false);
    }

    #[test]
    fn retry_read_error_after_written_connect_preserves_original_error() {
        let inner = ScriptedIo {
            reads: VecDeque::from([Poll::Ready(Err(io::Error::new(
                io::ErrorKind::ConnectionReset,
                "unit native reset",
            )))]),
            ..Default::default()
        };
        let (mut stream, observer) = observed_io(inner);
        let mut cx = Context::from_waker(futures::task::noop_waker_ref());
        let mut bytes = selected_wire("/libp2p/dcutr\n");
        bytes.extend(connect_wire());
        assert!(
            matches!(Pin::new(&mut stream).poll_write(&mut cx, &bytes), Poll::Ready(Ok(n)) if n == bytes.len())
        );
        let mut output = [0; 64];
        match Pin::new(&mut stream).poll_read(&mut cx, &mut output) {
            Poll::Ready(Err(error)) => {
                assert_eq!(error.kind(), io::ErrorKind::ConnectionReset);
                assert_eq!(error.to_string(), "unit native reset");
            }
            _ => panic!("native read error was replaced"),
        }
        let frames = io_events(&observer, "dcutr_frame");
        let terminal = io_events(&observer, "dcutr_stream_terminal");
        assert_eq!(frames.len(), 1);
        assert_eq!(frames[0]["direction"], "write");
        assert_eq!(terminal.len(), 1);
        assert_eq!(terminal[0]["stream_id"], frames[0]["stream_id"]);
        assert_eq!(terminal[0]["direction"], "read");
        assert_eq!(terminal[0]["error_kind"], "reset");
        assert_eq!(terminal[0]["error"], "unit native reset");
        assert_eq!(terminal[0]["completed_frame_count"], 0);
        assert_eq!(terminal[0]["pending_frame_bytes"], 0);
        assert_eq!(terminal[0]["io_bytes"], 0);
    }

    #[test]
    fn partial_write_error_preserves_counts_pending_and_terminal_deduplication() {
        let (mut stream, observer) = observed_io(ScriptedIo::default());
        let mut cx = Context::from_waker(futures::task::noop_waker_ref());
        let selected = selected_wire("/libp2p/dcutr\n");
        assert!(
            matches!(Pin::new(&mut stream).poll_write(&mut cx, &selected), Poll::Ready(Ok(n)) if n == selected.len())
        );
        stream.inner.writes = VecDeque::from([
            Poll::Ready(Ok(2)),
            Poll::Ready(Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "unit native deadline",
            ))),
            Poll::Ready(Err(io::Error::other("unit later error"))),
        ]);
        let frame = connect_wire();
        assert!(matches!(
            Pin::new(&mut stream).poll_write(&mut cx, &frame),
            Poll::Ready(Ok(2))
        ));
        for message in ["unit native deadline", "unit later error"] {
            match Pin::new(&mut stream).poll_write(&mut cx, &frame[2..]) {
                Poll::Ready(Err(error)) => assert_eq!(error.to_string(), message),
                _ => panic!("native write error was replaced"),
            }
        }
        assert!(io_events(&observer, "dcutr_frame").is_empty());
        let terminal = io_events(&observer, "dcutr_stream_terminal");
        assert_eq!(terminal.len(), 1);
        assert_eq!(terminal[0]["direction"], "write");
        assert_eq!(terminal[0]["error_kind"], "deadline");
        assert_eq!(terminal[0]["error"], "unit native deadline");
        assert_eq!(terminal[0]["completed_frame_count"], 0);
        assert_eq!(terminal[0]["pending_frame_bytes"], 2);
        assert_eq!(terminal[0]["invalid_or_over_limit"], false);
        assert_eq!(
            stream.inner.written,
            [selected, frame[..2].to_vec()].concat()
        );
    }

    #[test]
    fn pending_empty_read_zero_write_and_close_do_not_manufacture_terminal() {
        let (mut stream, observer) = observed_io(ScriptedIo::default());
        let mut cx = Context::from_waker(futures::task::noop_waker_ref());
        let selected = selected_wire("/libp2p/dcutr\n");
        assert!(
            matches!(Pin::new(&mut stream).poll_write(&mut cx, &selected), Poll::Ready(Ok(n)) if n == selected.len())
        );
        stream.inner.reads = VecDeque::from([Poll::Pending]);
        stream.inner.writes = VecDeque::from([Poll::Pending, Poll::Ready(Ok(0))]);
        let mut output = [0; 64];
        assert!(matches!(
            Pin::new(&mut stream).poll_read(&mut cx, &mut []),
            Poll::Ready(Ok(0))
        ));
        assert!(
            Pin::new(&mut stream)
                .poll_read(&mut cx, &mut output)
                .is_pending()
        );
        assert!(
            Pin::new(&mut stream)
                .poll_write(&mut cx, &selected)
                .is_pending()
        );
        assert!(matches!(
            Pin::new(&mut stream).poll_write(&mut cx, &selected),
            Poll::Ready(Ok(0))
        ));
        assert!(matches!(
            Pin::new(&mut stream).poll_flush(&mut cx),
            Poll::Ready(Ok(()))
        ));
        assert!(matches!(
            Pin::new(&mut stream).poll_close(&mut cx),
            Poll::Ready(Ok(()))
        ));
        assert_eq!(stream.inner.flushes, 1);
        assert_eq!(stream.inner.closes, 1);
        drop(stream);
        assert!(io_events(&observer, "dcutr_stream_terminal").is_empty());
    }

    #[test]
    fn unrelated_protocol_error_and_eof_are_not_dcutr_terminal() {
        for selected in [Vec::new(), selected_wire(&format!("{ECHO_PROTOCOL}\n"))] {
            let inner = ScriptedIo {
                reads: VecDeque::from([Poll::Ready(Err(io::Error::other(
                    "unit application error",
                )))]),
                ..Default::default()
            };
            let (mut stream, observer) = observed_io(inner);
            let mut cx = Context::from_waker(futures::task::noop_waker_ref());
            assert!(
                matches!(Pin::new(&mut stream).poll_write(&mut cx, &selected), Poll::Ready(Ok(n)) if n == selected.len())
            );
            let mut output = [0; 64];
            assert!(matches!(
                Pin::new(&mut stream).poll_read(&mut cx, &mut output),
                Poll::Ready(Err(_))
            ));
            assert!(matches!(
                Pin::new(&mut stream).poll_read(&mut cx, &mut output),
                Poll::Ready(Ok(0))
            ));
            assert!(io_events(&observer, "dcutr_stream_terminal").is_empty());
        }
    }

    #[test]
    fn invalid_capture_and_long_native_error_remain_fail_closed_and_bounded() {
        let original = "e".repeat(512);
        let inner = ScriptedIo {
            reads: VecDeque::from([Poll::Ready(Err(io::Error::other(original.clone())))]),
            ..Default::default()
        };
        let (mut stream, observer) = observed_io(inner);
        let mut cx = Context::from_waker(futures::task::noop_waker_ref());
        let mut bytes = selected_wire("/libp2p/dcutr\n");
        bytes.extend([1, 0]);
        assert!(
            matches!(Pin::new(&mut stream).poll_write(&mut cx, &bytes), Poll::Ready(Ok(n)) if n == bytes.len())
        );
        let mut output = [0; 64];
        match Pin::new(&mut stream).poll_read(&mut cx, &mut output) {
            Poll::Ready(Err(error)) => assert_eq!(error.to_string(), original),
            _ => panic!("bounded capture changed the original error"),
        }
        let mut bytes = [0];
        assert!(matches!(
            Pin::new(&mut stream).poll_read(&mut cx, &mut bytes),
            Poll::Ready(Ok(0))
        ));
        stream.inner.writes = VecDeque::from([Poll::Ready(Err(io::Error::other(
            "unit native write error",
        )))]);
        assert!(matches!(
            Pin::new(&mut stream).poll_write(&mut cx, &bytes),
            Poll::Ready(Err(_))
        ));
        let terminal = io_events(&observer, "dcutr_stream_terminal");
        assert_eq!(terminal.len(), 2);
        assert_eq!(terminal[0]["error"].as_str().unwrap().len(), 256);
        assert_eq!(terminal[0]["error_kind"], "io_error");
        assert_eq!(terminal[1]["direction"], "write");
        assert_eq!(terminal[1]["invalid_or_over_limit"], true);
        assert!(io_events(&observer, "dcutr_frame").is_empty());
    }

    #[test]
    fn relay_candidate_requires_exact_listener_port_and_native_propagation() {
        let local = PeerId::random();
        let observed: Multiaddr = "/ip4/11.0.0.3/udp/55210/quic-v1".parse().unwrap();
        let mut candidate = RelayCandidate::new(
            local,
            ConnectionId::new_unchecked(1),
            "unit-carrier".into(),
            ListenerId::next(),
            "/ip4/10.2.0.2/udp/55210/quic-v1".parse().unwrap(),
            observed.clone(),
        )
        .unwrap();
        assert!(!candidate.propagated);
        assert!(
            !candidate.accept_native_candidate(&"/ip4/11.0.0.3/udp/55211/quic-v1".parse().unwrap())
        );
        assert!(!candidate.propagated);
        assert!(candidate.accept_native_candidate(&observed));
        assert!(candidate.propagated);
        assert!(!candidate.accept_native_candidate(&observed));
        assert_eq!(
            candidate.receipt(PeerId::random())["candidate_propagated"],
            true
        );

        for invalid in [
            "/ip4/11.0.0.3/udp/55211/quic-v1",
            "/ip4/11.0.0.3/udp/0/quic-v1",
            "/ip4/0.0.0.0/udp/55210/quic-v1",
            "/ip4/224.0.0.1/udp/55210/quic-v1",
            "/ip6/::1/udp/55210/quic-v1",
            "/ip4/11.0.0.3/tcp/55210",
            "/ip4/11.0.0.3/udp/55210",
            "/ip4/11.0.0.3/udp/55210/quic-v1/p2p-circuit",
            "/dns4/relay.invalid/udp/55210/quic-v1",
        ] {
            assert!(
                RelayCandidate::new(
                    local,
                    ConnectionId::new_unchecked(1),
                    "unit-carrier".into(),
                    ListenerId::next(),
                    "/ip4/10.2.0.2/udp/55210/quic-v1".parse().unwrap(),
                    invalid.parse().unwrap(),
                )
                .is_err(),
                "accepted {invalid}"
            );
        }
        assert!(
            quic_socket(
                &observed.clone().with(Protocol::P2p(PeerId::random())),
                Some(local)
            )
            .is_err()
        );
        assert_eq!(
            quic_socket(&observed.with(Protocol::P2p(local)), Some(local))
                .unwrap()
                .port(),
            55210
        );
    }

    #[test]
    fn configured_endpoint_alone_is_not_an_authenticated_identify_carrier() {
        let local = PeerId::random();
        let remote = PeerId::random();
        let observer = Observer::new("0123456789abcdef0123456789abcdef".into(), local).unwrap();
        let point = ConnectedPoint::Dialer {
            address: "/ip4/11.0.0.1/udp/40100/quic-v1".parse().unwrap(),
            role_override: Endpoint::Dialer,
            port_use: PortUse::Reuse,
        };
        assert!(observer.authenticated_carrier(remote, &point).is_err());
        assert!(
            observer.result(false, false, None)["events"]
                .as_array()
                .unwrap()
                .is_empty()
        );
    }

    #[tokio::test]
    async fn actual_identify_candidate_precedes_readiness_without_early_confirmation() {
        fn actor(
            key: &identity::Keypair,
            owner: &super::super::task_owner::Owner,
        ) -> (Swarm<PathBehaviour>, Observer) {
            let local = key.public().to_peer_id();
            let observer = Observer::new("0123456789abcdef0123456789abcdef".into(), local).unwrap();
            let (relay_transport, relay_client) = relay::client::new(local);
            let circuit_capture = observer.clone();
            let circuit = relay_transport
                .upgrade(Version::V1)
                .authenticate(noise::Config::new(key).unwrap())
                .multiplex(yamux::Config::default())
                .map(move |(peer, muxer), point| circuit_capture.output(peer, muxer, point, true))
                .boxed();
            let direct_capture = observer.clone();
            let direct = libp2p::quic::tokio::Transport::new(libp2p::quic::Config::new(key))
                .map(move |(peer, muxer), point| direct_capture.output(peer, muxer, point, false))
                .boxed();
            let transport = circuit
                .or_transport(direct)
                .map(|output, _| output.into_inner())
                .boxed();
            let swarm = Swarm::new(
                transport,
                native_behaviour(key, "source", relay_client, observer.clone()),
                local,
                owner
                    .swarm_config()
                    .with_idle_connection_timeout(Duration::from_secs(10)),
            );
            (swarm, observer)
        }

        // These are actual localhost QUIC/Identify exchanges, not live NAT or
        // DCUtR success receipts. The negative reproduces the old Swarm filter.
        for confirm_early in [true, false] {
            let owner = super::super::task_owner::Owner::default();
            let relay_key = identity::Keypair::generate_ed25519();
            let client_key = identity::Keypair::generate_ed25519();
            let relay_peer = relay_key.public().to_peer_id();
            let local = client_key.public().to_peer_id();
            let (mut relay, _relay_observer) = actor(&relay_key, &owner);
            let (mut client, observer) = actor(&client_key, &owner);
            relay
                .listen_on("/ip4/127.0.0.1/udp/0/quic-v1".parse().unwrap())
                .unwrap();
            let listener_id = client
                .listen_on("/ip4/127.0.0.1/udp/0/quic-v1".parse().unwrap())
                .unwrap();
            let exchange = tokio::time::timeout(Duration::from_secs(5), async {
                let remote = loop {
                    if let SwarmEvent::NewListenAddr { address, .. } = relay.select_next_some().await {
                        break address;
                    }
                };
                let listener = loop {
                    if let SwarmEvent::NewListenAddr { listener_id: actual, address } = client.select_next_some().await {
                        assert_eq!(actual, listener_id);
                        break address;
                    }
                };
                client.dial(libp2p::swarm::dial_opts::DialOpts::peer_id(relay_peer)
                    .addresses(vec![remote]).build()).unwrap();
                let mut connections = BTreeMap::new();
                let mut candidate: Option<RelayCandidate> = None;
                let mut candidate_deadline = None;
                loop {
                    let deadline = candidate_deadline.unwrap_or_else(|| tokio::time::Instant::now() + Duration::from_secs(5));
                    tokio::select! {
                        _ = relay.select_next_some() => {},
                        _ = tokio::time::sleep_until(deadline), if candidate_deadline.is_some() => {
                            return candidate.unwrap();
                        },
                        event = client.select_next_some() => match event {
                            SwarmEvent::ConnectionEstablished { connection_id, peer_id, endpoint, .. } => {
                                assert_eq!(peer_id, relay_peer);
                                connections.insert(connection_id, endpoint);
                            }
                            SwarmEvent::Behaviour(PathBehaviourEvent::Identify(identify::Event::Received { peer_id, connection_id, info })) => {
                                assert_eq!(peer_id, relay_peer);
                                assert_eq!(info.public_key.to_peer_id(), relay_peer);
                                let carrier = observer.authenticated_carrier(peer_id, connections.get(&connection_id).unwrap()).unwrap();
                                let observed = info.observed_addr;
                                let actual = RelayCandidate::new(local, connection_id, carrier, listener_id, listener.clone(), observed.clone()).unwrap();
                                observer.record("identify_observed_address", "rust.identify.Event.Received", actual.receipt(peer_id));
                                assert!(!actual.propagated);
                                if confirm_early { client.add_external_address(observed); }
                                candidate = Some(actual);
                                candidate_deadline = Some(tokio::time::Instant::now() + Duration::from_millis(150));
                            }
                            SwarmEvent::NewExternalAddrCandidate { address } => {
                                let candidate = candidate.as_mut().expect("native candidate must follow Identify Received");
                                if candidate.accept_native_candidate(&address) {
                                    observer.record("native_dcutr_candidate_ready", "rust.swarm.NewExternalAddrCandidate", candidate.receipt(relay_peer));
                                }
                            }
                            _ => {},
                        },
                    }
                }
            }).await;
            drop(client);
            drop(relay);
            let report = tokio::time::timeout(Duration::from_secs(2), owner.close_and_join())
                .await
                .unwrap()
                .snapshot();
            assert_eq!(report["fixture_owned_tasks_joined"], true);
            assert_eq!(report["errors"], json!([]));
            let candidate = exchange.expect("native QUIC/Identify candidate deadline");
            assert_eq!(candidate.propagated, !confirm_early);
            let events = observer.result(false, false, None)["events"]
                .as_array()
                .unwrap()
                .clone();
            let received = events
                .iter()
                .find(|event| event["kind"] == "identify_observed_address")
                .unwrap();
            let admitted = events
                .iter()
                .find(|event| event["kind"] == "native_dcutr_candidate_ready");
            if confirm_early {
                assert!(admitted.is_none());
            } else {
                let admitted = admitted.unwrap();
                assert!(
                    admitted["sequence"].as_u64().unwrap() > received["sequence"].as_u64().unwrap()
                );
                assert_eq!(
                    admitted["native_connection_id"],
                    received["native_connection_id"]
                );
                assert_eq!(
                    admitted["carrier_connection_id"],
                    received["carrier_connection_id"]
                );
                assert_eq!(
                    admitted["local_listener_address"],
                    candidate.listener_address.to_string()
                );
            }
        }
    }

    #[test]
    fn only_actual_unnatted_relay_listener_can_be_confirmed() {
        let actual: Multiaddr = "/ip4/11.0.0.1/udp/40100/quic-v1".parse().unwrap();
        assert_eq!(
            relay_listener_address("relay", &actual).unwrap(),
            Some(actual.clone())
        );
        for role in ["source", "destination"] {
            assert_eq!(relay_listener_address(role, &actual).unwrap(), None);
        }
        for address in [
            "/ip4/10.1.0.2/udp/40100/quic-v1",
            "/ip4/127.0.0.1/udp/40100/quic-v1",
            "/ip4/11.0.0.2/udp/40100/quic-v1",
            "/ip4/11.0.0.1/udp/0/quic-v1",
            "/ip4/11.0.0.1/tcp/40100",
            "/ip4/11.0.0.1/udp/40100/quic-v1/p2p-circuit",
        ] {
            assert!(relay_listener_address("relay", &address.parse().unwrap()).is_err());
        }
        assert!(
            relay_listener_address("relay", &actual.with(Protocol::P2p(PeerId::random()))).is_err()
        );
    }

    #[tokio::test]
    async fn composed_roles_advertise_native_ping_alongside_path_echo() {
        for role in ["relay", "source", "destination"] {
            let (_transport, mut behaviour, _local) = composition(role);
            let mut control = behaviour.stream.new_control();
            let _incoming = control.accept(StreamProtocol::new(ECHO_PROTOCOL)).unwrap();
            let protocols = inbound_protocols(&mut behaviour);
            assert!(
                protocols
                    .iter()
                    .any(|protocol| protocol == AsRef::<str>::as_ref(&ping::PROTOCOL_NAME)),
                "native Ping missing for {role}"
            );
            assert!(protocols.iter().any(|protocol| protocol == ECHO_PROTOCOL));
        }
    }

    #[tokio::test]
    async fn composed_stream_handler_keeps_native_hop_advertisement_policy() {
        let (_transport, mut behaviour, _local) = composition("relay");
        let mut control = behaviour.stream.new_control();
        let _incoming = control.accept(StreamProtocol::new(ECHO_PROTOCOL)).unwrap();
        let protocols = inbound_protocols(&mut behaviour);
        assert!(protocols.iter().any(|protocol| protocol == ECHO_PROTOCOL));
        assert!(
            !protocols
                .iter()
                .any(|protocol| protocol == relay::HOP_PROTOCOL_NAME.as_ref())
        );

        let external =
            relay_listener_address("relay", &"/ip4/11.0.0.1/udp/40100/quic-v1".parse().unwrap())
                .unwrap()
                .unwrap();
        behaviour.on_swarm_event(FromSwarm::ExternalAddrConfirmed(ExternalAddrConfirmed {
            addr: &external,
        }));
        let protocols = inbound_protocols(&mut behaviour);
        assert!(
            protocols
                .iter()
                .any(|protocol| protocol == relay::HOP_PROTOCOL_NAME.as_ref())
        );
        assert!(protocols.iter().any(|protocol| protocol == ECHO_PROTOCOL));
        assert!(
            protocols
                .iter()
                .any(|protocol| protocol == "/ipfs/id/1.0.0")
        );

        behaviour.on_swarm_event(FromSwarm::ExternalAddrExpired(ExternalAddrExpired {
            addr: &external,
        }));
        let protocols = inbound_protocols(&mut behaviour);
        assert!(
            !protocols
                .iter()
                .any(|protocol| protocol == relay::HOP_PROTOCOL_NAME.as_ref())
        );
        assert!(protocols.iter().any(|protocol| protocol == ECHO_PROTOCOL));
    }

    #[tokio::test]
    async fn native_listener_owned_dial_preserves_reserve_in_composed_handler() {
        let (mut transport, mut behaviour, _local) = composition("destination");
        let relay = PeerId::random();
        let address: Multiaddr = "/ip4/11.0.0.1/udp/40100/quic-v1".parse().unwrap();
        transport
            .listen_on(
                ListenerId::next(),
                address
                    .clone()
                    .with(Protocol::P2p(relay))
                    .with(Protocol::P2pCircuit),
            )
            .unwrap();
        let mut cx = Context::from_waker(futures::task::noop_waker_ref());
        let opts = reservation_dial(&mut transport, &mut behaviour, &mut cx);
        assert_eq!(opts.get_peer_id(), Some(relay));
        let mut handler = behaviour
            .handle_established_outbound_connection(
                opts.connection_id(),
                relay,
                &address,
                Endpoint::Dialer,
                PortUse::Reuse,
            )
            .unwrap();
        match handler.poll(&mut cx) {
            Poll::Ready(ConnectionHandlerEvent::OutboundSubstreamRequest { protocol }) => {
                assert!(
                    protocol.upgrade().protocol_info().any(
                        |name| AsRef::<str>::as_ref(&name) == relay::HOP_PROTOCOL_NAME.as_ref()
                    )
                );
            }
            _ => panic!("native reservation owner lost its HOP request"),
        }
        assert_eq!(
            behaviour
                .stream
                .observer
                .0
                .application_dials
                .load(Ordering::SeqCst),
            0
        );
    }

    #[tokio::test]
    async fn denied_reservation_dial_closes_native_listener_even_without_error() {
        let (mut transport, mut behaviour, _local) = composition("destination");
        let relay = PeerId::random();
        let listener = ListenerId::next();
        let address: Multiaddr = "/ip4/11.0.0.1/udp/40100/quic-v1".parse().unwrap();
        transport
            .listen_on(
                listener,
                address
                    .with(Protocol::P2p(relay))
                    .with(Protocol::P2pCircuit),
            )
            .unwrap();
        let mut cx = Context::from_waker(futures::task::noop_waker_ref());
        let opts = reservation_dial(&mut transport, &mut behaviour, &mut cx);
        // Simulate Swarm's native notification when a competing dial exists.
        // This is a unit regression, not a captured live reservation receipt.
        let error = DialError::DialPeerConditionFalse(Default::default());
        behaviour.on_swarm_event(FromSwarm::DialFailure(DialFailure {
            peer_id: Some(relay),
            error: &error,
            connection_id: opts.connection_id(),
        }));
        match Pin::new(&mut transport).poll(&mut cx) {
            Poll::Ready(TransportEvent::ListenerClosed {
                listener_id,
                reason,
            }) => {
                assert_eq!(listener_id, listener);
                assert!(reason.is_ok());
            }
            _ => panic!("native failed reservation owner did not close its listener"),
        }
    }

    #[test]
    fn circuit_listener_address_is_preserved_without_duplicate_peer_suffix() {
        let relay = PeerId::random();
        let local = PeerId::random();
        let address = "/ip4/11.0.0.1/udp/40100/quic-v1"
            .parse::<Multiaddr>()
            .unwrap()
            .with(Protocol::P2p(relay))
            .with(Protocol::P2pCircuit)
            .with(Protocol::P2p(local));
        assert_eq!(
            circuit_listener_address(&address, relay, local).unwrap(),
            address
        );
        assert!(
            circuit_listener_address(&address.clone().with(Protocol::P2p(local)), relay, local)
                .is_err()
        );
        assert!(circuit_listener_address(&address, relay, PeerId::random()).is_err());
        assert!(circuit_listener_address(&address, PeerId::random(), local).is_err());
    }

    #[test]
    fn native_completion_does_not_manufacture_a_path_proof() {
        let local = PeerId::random();
        let observer = Observer::new("0123456789abcdef0123456789abcdef".into(), local).unwrap();
        observer.native_event(&dcutr::Event {
            remote_peer_id: PeerId::random(),
            result: Ok(ConnectionId::new_unchecked(1)),
        });
        let result = observer.result(false, false, None);
        assert_eq!(result["events"][0]["kind"], "native_dcutr_event");
        assert_eq!(result["joined"], false);
        assert!(observer.0.state.lock().unwrap().sessions.is_empty());
    }

    #[test]
    fn preexisting_direct_connection_is_rejected() {
        let observer =
            Observer::new("0123456789abcdef0123456789abcdef".into(), PeerId::random()).unwrap();
        assert!(
            observer
                .bind(PeerId::random(), &[ConnectionId::new_unchecked(1)])
                .is_err()
        );
    }

    #[test]
    fn observer_is_bounded() {
        let observer =
            Observer::new("0123456789abcdef0123456789abcdef".into(), PeerId::random()).unwrap();
        for _ in 0..=EVENT_LIMIT {
            observer.record("capture", "rust.native", json!({}));
        }
        let result = observer.result(false, false, None);
        assert_eq!(result["overflow"], true);
        assert_eq!(result["events"].as_array().unwrap().len(), EVENT_LIMIT);
    }

    #[tokio::test]
    async fn frames_are_actual_bounded_io() {
        let mut io = futures::io::Cursor::new(Vec::new());
        write_frame(&mut io, b"challenge").await.unwrap();
        io.set_position(0);
        assert_eq!(read_frame(&mut io).await.unwrap(), b"challenge");
        assert!(
            read_frame(&mut futures::io::Cursor::new(vec![0, 0, 0, 129]))
                .await
                .is_err()
        );
    }
}
