//! Two fresh native TCP owners; the external network fixture holds actual SYNs.
//! No discovery, Relay, DCUtR, transport replacement or per-call cancel claim.
use std::{
    collections::{BTreeMap, BTreeSet},
    io,
    net::IpAddr,
    path::{Path, PathBuf},
    pin::Pin,
    str::FromStr,
    sync::{Arc, Mutex},
    task::{Context, Poll},
    time::{Duration, Instant},
};

use futures::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt, StreamExt};
use libp2p::{
    Multiaddr, PeerId, StreamProtocol, Swarm, Transport,
    core::{
        ConnectedPoint, Endpoint,
        muxing::StreamMuxerBox,
        transport::{Boxed, DialOpts as TransportDialOpts, ListenerId, TransportEvent},
    },
    identify, identity,
    multiaddr::Protocol,
    pnet::PreSharedKey,
    swarm::{ConnectionId, NetworkBehaviour, SwarmEvent, dial_opts::DialOpts},
};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};

use super::{application_observer, task_owner, upgrade_observer};

const NATIVE: &str = "coordinated_dial_port_reuse";
const PRIVATE: &str = "coordinated_dial_port_reuse_private_pnet";
const RUN_BUDGET: Duration = Duration::from_secs(40);
const JOIN_BUDGET: Duration = Duration::from_secs(5);
const TOTAL_BUDGET: Duration = Duration::from_secs(45);
const ECHO_BUDGET: Duration = Duration::from_secs(10);
const EVENT_LIMIT: usize = 64;
const PNET_FINGERPRINT_DOMAIN: &[u8] = b"forge.net.pnet.operational-fingerprint.v1\0";
const PNET_FINGERPRINT_BASIS: &str = "installed_native_pnet_psk_operational_sha256_v1";

#[derive(NetworkBehaviour)]
struct CoordinatedBehaviour {
    identify: identify::Behaviour,
    stream: libp2p_stream::Behaviour,
}

fn native_behaviour(key: &identity::Keypair) -> CoordinatedBehaviour {
    CoordinatedBehaviour {
        identify: identify::Behaviour::new(identify::Config::new_with_signed_peer_record(
            "/forge/interop/coordinated-identify/1".into(),
            key,
        )),
        stream: libp2p_stream::Behaviour::new(),
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Role {
    Initiator,
    Responder,
}
impl Role {
    fn name(self) -> &'static str {
        match self {
            Self::Initiator => "initiator",
            Self::Responder => "responder",
        }
    }
    fn upgrade_direction(self) -> &'static str {
        match self {
            Self::Initiator => "outbound",
            Self::Responder => "inbound",
        }
    }
}

#[derive(Debug)]
pub(crate) struct Config {
    role: Role,
    private: bool,
    token: String,
    bind: IpAddr,
    ready: PathBuf,
    result: PathBuf,
    stop: PathBuf,
    control: PathBuf,
    plan: PathBuf,
    key: Option<PathBuf>,
    fingerprint: Option<String>,
    operation_budget: Duration,
}

fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}
fn hex(value: &str, size: usize) -> bool {
    value.len() == size
        && value
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

fn verified_private_key(text: &str, expected: &str) -> io::Result<(PreSharedKey, String)> {
    // The pinned parser slices base16 text by bytes. Reject non-ASCII input
    // before it can panic, and never propagate an error containing key material.
    if !text.is_ascii() || text.lines().count() != 3 {
        return Err(invalid("invalid private key fixture"));
    }
    let psk = PreSharedKey::from_str(text).map_err(|_| invalid("invalid private key fixture"))?;
    // Serialize the parsed donor key, not the untrusted input. This computes
    // Forge/Go's operational hash of exactly the key passed to the protector;
    // the donor's fingerprint() uses a different, 16-byte fingerprint scheme.
    let canonical = psk.to_string();
    let material = super::decode_hex(
        canonical
            .lines()
            .nth(2)
            .ok_or_else(|| invalid("invalid parsed private key"))?,
    )
    .map_err(|_| invalid("invalid parsed private key"))?;
    if material.len() != 32 {
        return Err(invalid("invalid parsed private key"));
    }
    let mut digest = Sha256::new();
    digest.update(PNET_FINGERPRINT_DOMAIN);
    digest.update(&material);
    let fingerprint = format!("{:x}", digest.finalize());
    if !hex(expected, 64) || fingerprint != expected {
        return Err(invalid(
            "coordinated PSK fingerprint differs from installed key",
        ));
    }
    Ok((psk, fingerprint))
}

pub(crate) fn parse_args(argv: &[String]) -> io::Result<Option<Config>> {
    if argv.first().map(String::as_str) != Some("coordinated-live") {
        return Ok(None);
    }
    const REQUIRED: [&str; 10] = [
        "scenario",
        "transport",
        "coord-role",
        "case-token",
        "bind-ip",
        "ready-file",
        "result-file",
        "stop-file",
        "control-file",
        "plan-file",
    ];
    if argv.len() > 27 || (argv.len() - 1) % 2 != 0 {
        return Err(invalid("coordinated-live flag/value bound"));
    }
    let mut args = BTreeMap::new();
    for pair in argv[1..].chunks_exact(2) {
        let name = pair[0]
            .strip_prefix("--")
            .ok_or_else(|| invalid("unnamed coordinated-live flag"))?;
        if !REQUIRED.contains(&name)
            && !matches!(name, "pnet-key-file" | "pnet-fingerprint" | "timeout-ms")
            || pair[1].is_empty()
            || pair[1].len() > 4096
            || pair[1]
                .bytes()
                .any(|byte| matches!(byte, 0 | b'\n' | b'\r'))
            || pair[1].starts_with("--")
            || args.insert(name, pair[1].as_str()).is_some()
        {
            return Err(invalid("unknown/duplicate coordinated-live flag or value"));
        }
    }
    if REQUIRED.iter().any(|name| !args.contains_key(name)) {
        return Err(invalid("missing coordinated-live flag"));
    }
    let private = match (args["scenario"], args["transport"]) {
        (NATIVE, "tcp") => false,
        (PRIVATE, "tcp-pnet-noise") => true,
        _ => {
            return Err(invalid(
                "coordinated-live requires the exact native/private TCP Noise profile",
            ));
        }
    };
    let role = match args["coord-role"] {
        "initiator" => Role::Initiator,
        "responder" => Role::Responder,
        _ => return Err(invalid("invalid coordinated role")),
    };
    let bind: IpAddr = args["bind-ip"]
        .parse()
        .map_err(|_| invalid("numeric bind IP required"))?;
    let timeout_ms = args
        .get("timeout-ms")
        .copied()
        .unwrap_or("20000")
        .parse::<u64>()
        .map_err(|_| invalid("invalid coordinated operation budget"))?;
    if !(1000..=30000).contains(&timeout_ms) {
        return Err(invalid("coordinated timeout-ms must be 1000..30000"));
    }
    if bind.is_unspecified()
        || bind.is_multicast()
        || !hex(args["case-token"], 32)
        || private != args.contains_key("pnet-key-file")
        || private != args.contains_key("pnet-fingerprint")
        || private && !hex(args["pnet-fingerprint"], 64)
    {
        return Err(invalid("invalid coordinated identity/profile arguments"));
    }
    Ok(Some(Config {
        role,
        private,
        bind,
        token: args["case-token"].into(),
        ready: args["ready-file"].into(),
        result: args["result-file"].into(),
        stop: args["stop-file"].into(),
        control: args["control-file"].into(),
        plan: args["plan-file"].into(),
        key: args.get("pnet-key-file").map(|value| PathBuf::from(*value)),
        fingerprint: args.get("pnet-fingerprint").map(|s| (*s).into()),
        operation_budget: Duration::from_millis(timeout_ms),
    }))
}

fn fields(path: &Path) -> io::Result<BTreeMap<String, String>> {
    let mut bytes = Vec::new();
    std::io::Read::read_to_end(
        &mut std::io::Read::take(std::fs::File::open(path)?, 8193),
        &mut bytes,
    )?;
    if bytes.len() > 8192 {
        return Err(invalid("coordinated control file bound"));
    }
    let text = std::str::from_utf8(&bytes).map_err(|_| invalid("control must be UTF-8"))?;
    let mut result = BTreeMap::new();
    for line in text.lines() {
        let (key, value) = line
            .split_once('=')
            .ok_or_else(|| invalid("invalid control line"))?;
        if key.is_empty()
            || value.is_empty()
            || line.contains('\0')
            || result.len() == 8
            || result.insert(key.into(), value.into()).is_some()
        {
            return Err(invalid("invalid/duplicate control field"));
        }
    }
    Ok(result)
}
fn atomic(path: &Path, value: &Value) -> io::Result<()> {
    let temporary = path.with_extension("tmp");
    std::fs::write(&temporary, value.to_string())?;
    std::fs::rename(temporary, path)
}

fn numeric_tcp(address: &Multiaddr, peer: Option<PeerId>) -> io::Result<Multiaddr> {
    let mut address = address.clone();
    if let Some(Protocol::P2p(id)) = address.iter().last() {
        if Some(id) != peer {
            return Err(invalid("listener address peer mismatch"));
        }
        address.pop();
    }
    let valid = match address.iter().collect::<Vec<_>>().as_slice() {
        [Protocol::Ip4(ip), Protocol::Tcp(port)] => {
            !ip.is_unspecified() && !ip.is_multicast() && *port != 0
        }
        [Protocol::Ip6(ip), Protocol::Tcp(port)] => {
            !ip.is_unspecified() && !ip.is_multicast() && *port != 0
        }
        _ => false,
    };
    if !valid {
        return Err(invalid("one concrete numeric TCP listener required"));
    }
    Ok(address)
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct Plan {
    peer: PeerId,
    address: Multiaddr,
}
fn read_plan(path: &Path, token: &str, local: PeerId) -> io::Result<Plan> {
    let input = fields(path)?;
    if input.len() != 3
        || input.get("case-token").map(String::as_str) != Some(token)
        || !input.contains_key("peer-id")
        || !input.contains_key("addr")
        || input["addr"].len() > 512
    {
        return Err(invalid("invalid coordinated peer plan"));
    }
    let peer = input["peer-id"]
        .parse()
        .map_err(|_| invalid("invalid expected peer"))?;
    if peer == local {
        return Err(invalid("coordinated peer must be a fresh other actor"));
    }
    let address = input["addr"]
        .parse()
        .map_err(|_| invalid("invalid peer listener"))?;
    Ok(Plan {
        peer,
        address: numeric_tcp(&address, Some(peer))?,
    })
}
fn dial_options(plan: &Plan, role: Role) -> DialOpts {
    let options = DialOpts::peer_id(plan.peer).addresses(vec![plan.address.clone()]);
    // Default PortUse::Reuse is preserved. Only the coordinated security role changes.
    if role == Role::Responder {
        options.override_role().build()
    } else {
        options.build()
    }
}

#[derive(Clone, Default)]
struct DialAudit(Arc<Mutex<Vec<Value>>>);
impl DialAudit {
    fn count(&self) -> usize {
        self.0.lock().unwrap_or_else(|e| e.into_inner()).len()
    }
    fn snapshot(&self) -> Value {
        json!(self.0.lock().unwrap_or_else(|e| e.into_inner()).clone())
    }
}
struct AuditedTransport {
    inner: Boxed<(PeerId, StreamMuxerBox)>,
    audit: DialAudit,
}
impl Transport for AuditedTransport {
    type Output = (PeerId, StreamMuxerBox);
    type Error = io::Error;
    type ListenerUpgrade = <Boxed<Self::Output> as Transport>::ListenerUpgrade;
    type Dial = <Boxed<Self::Output> as Transport>::Dial;
    fn listen_on(
        &mut self,
        id: ListenerId,
        address: Multiaddr,
    ) -> Result<(), libp2p::core::transport::TransportError<Self::Error>> {
        self.inner.listen_on(id, address)
    }
    fn remove_listener(&mut self, id: ListenerId) -> bool {
        self.inner.remove_listener(id)
    }
    fn dial(
        &mut self,
        address: Multiaddr,
        options: TransportDialOpts,
    ) -> Result<Self::Dial, libp2p::core::transport::TransportError<Self::Error>> {
        let mut records = self.audit.0.lock().unwrap_or_else(|e| e.into_inner());
        // Record the unexpected invocation too; never suppress it to manufacture no-dial evidence.
        if records.len() < 4 {
            records.push(json!({"address": address.to_string(),
            "requested_role": if options.role == Endpoint::Dialer { "dialer" } else { "listener" },
            "port_use": format!("{:?}", options.port_use)}));
        }
        drop(records);
        self.inner.dial(address, options)
    }
    fn poll(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<TransportEvent<Self::ListenerUpgrade, Self::Error>> {
        Pin::new(&mut self.get_mut().inner).poll(cx)
    }
}

// Swarm IDs and raw IDs remain different domains. This is a strict unique-output
// correlation using the existing native output receipt, not a guessed ID mapping.
fn completed_socket(
    raw: &Value,
    local: PeerId,
    plan: &Plan,
    listener: &Multiaddr,
    id: ConnectionId,
    role: Role,
) -> io::Result<Option<Value>> {
    if raw["overflow"] != false {
        return Err(invalid("raw upgrade capture overflow"));
    }
    let connections = raw["connections"]
        .as_array()
        .ok_or_else(|| invalid("missing raw connections"))?;
    let events = raw["swarm_events"]
        .as_array()
        .ok_or_else(|| invalid("missing native connection events"))?;
    let ([connection], [event]) = (connections.as_slice(), events.as_slice()) else {
        return Err(invalid("coordinated owner is absent or ambiguous"));
    };
    let expected = plan.peer.to_string();
    let remote = application_observer::bound_remote_endpoint(connection, event, &expected)
        .map_err(invalid)?;
    if connection["authenticated_local_peer_id"] != local.to_string()
        || event["authenticated_remote_peer_id"] != expected
        || event["swarm_connection_id"] != id.to_string()
        || connection["local_address"] != listener.to_string()
        || remote != plan.address.to_string()
        || connection["direction"] != "outbound"
        || event["endpoint"]["direction"] != "outbound"
        || connection["selected_security"] != "/noise"
        || connection["selected_muxer"] != "/yamux/1.0.0"
        || connection["security_complete"] != true
        || connection["security_delegate_completed"] != true
        || connection["muxer_delegate_completed"] != true
        || connection["negotiations"]
            .as_array()
            .is_none_or(|phases| phases.len() != 2)
        || connection["negotiations"][0]["direction"] != role.upgrade_direction()
        || connection["negotiations"][1]["direction"] != role.upgrade_direction()
        || connection["negotiations"][0]["protocol"] != "/noise"
        || connection["negotiations"][0]["io_failed"] != false
        || connection["negotiations"][1]["io_failed"] != false
        || !connection["negotiations"][0]["parser_error"].is_null()
        || !connection["negotiations"][1]["parser_error"].is_null()
        || event["endpoint"]["upgrade_role"] != role.upgrade_direction()
    {
        return Err(invalid(
            "actual socket, outgoing owner, peer or completed security role mismatch",
        ));
    }
    // V1Lazy can yield the authenticated transport/muxer owner before the
    // peer's actual Yamux selection is read. Keep driving Swarm; no proof yet.
    if connection["muxer_complete"] == false {
        return Ok(None);
    }
    if connection["muxer_complete"] != true
        || connection["negotiations"][1]["protocol"] != "/yamux/1.0.0"
    {
        return Err(invalid("native muxer selection lacks a completed receipt"));
    }
    Ok(Some(
        json!({"basis": "unique_native_transport_output_and_swarm_endpoint",
        "socket_source": "rust.tcp.TcpStream.local_addr_peer_addr", "connection_trace_id": connection["connection_trace_id"],
        "native_connection_id": id.to_string(), "local_address": connection["local_address"], "remote_address": remote,
        "listener_address": listener.to_string(), "authenticated_local_peer_id": local.to_string(),
        "authenticated_remote_peer_id": expected, "connection_direction": "outbound",
        "security": "/noise", "muxer": "/yamux/1.0.0", "security_role": if role == Role::Initiator { "client" } else { "server" },
        "security_role_basis": "completed_native_security_and_muxer_delegates", "raw_upgrade_role": role.upgrade_direction()}),
    ))
}

fn challenge(token: &str) -> Vec<u8> {
    format!("coordinated:{token}").into_bytes()
}
async fn read_echo<T: AsyncRead + Unpin>(stream: &mut T) -> io::Result<Vec<u8>> {
    let mut size = [0];
    stream.read_exact(&mut size).await?;
    if size[0] == 0 || size[0] >= 128 {
        return Err(invalid("noncanonical or oversized echo frame"));
    }
    let mut body = vec![0; size[0] as usize];
    stream.read_exact(&mut body).await?;
    Ok(body)
}
async fn write_echo<T: AsyncWrite + Unpin>(stream: &mut T, body: &[u8]) -> io::Result<()> {
    if body.is_empty() || body.len() >= 128 {
        return Err(invalid("echo frame bound"));
    }
    stream.write_all(&[body.len() as u8]).await?;
    stream.write_all(body).await?;
    stream.flush().await
}
fn body_receipt(body: &[u8]) -> Value {
    let mut capture = upgrade_observer::Body::default();
    for byte in std::iter::once(body.len() as u8).chain(body.iter().copied()) {
        capture.byte(byte, 128);
    }
    capture.snapshot()
}
async fn responder_echo(mut stream: libp2p::Stream, token: String) -> io::Result<Value> {
    let request = read_echo(&mut stream).await?;
    if request != challenge(&token) {
        return Err(invalid("unexpected fresh connection challenge"));
    }
    write_echo(&mut stream, &request).await?;
    stream.close().await?;
    drop(stream);
    Ok(
        json!({"source": "rust.actual_stream.read_write", "read": body_receipt(&request), "write": body_receipt(&request)}),
    )
}
async fn initiator_echo(
    mut control: libp2p_stream::Control,
    observer: upgrade_observer::Observer,
    peer: PeerId,
    token: String,
) -> io::Result<Value> {
    let attempt = observer.application(peer, application_observer::ECHO);
    let stream = control
        .open_stream(peer, StreamProtocol::new(application_observer::ECHO))
        .await
        .map_err(|_| invalid("existing connection stream open failed"))?;
    let mut stream = attempt.wrap(stream);
    let request = challenge(&token);
    write_echo(&mut stream, &request).await?;
    let response = read_echo(&mut stream).await?;
    if response != request {
        return Err(invalid("coordinated echo mismatch"));
    }
    stream.close().await?;
    stream.complete();
    drop(stream);
    Ok(
        json!({"source": "rust.actual_stream.write_read", "read": body_receipt(&response), "write": body_receipt(&request)}),
    )
}
fn echo_binding(
    raw: &Value,
    socket: &Value,
    application: &Value,
    role: Role,
    require_drop: bool,
) -> io::Result<Value> {
    let connections = raw["connections"]
        .as_array()
        .ok_or_else(|| invalid("missing raw echo connection"))?;
    let [connection] = connections.as_slice() else {
        return Err(invalid("ambiguous echo connection"));
    };
    let streams = connection["streams"]
        .as_array()
        .ok_or_else(|| invalid("missing raw echo stream"))?;
    // Native Identify has its own observed substreams. It cannot stand in for
    // the unique challenge stream, and unknown/duplicate application streams fail.
    let mut stream_ids = BTreeSet::new();
    if streams.iter().any(|stream| {
        let Some(id) = stream["stream_trace_id"].as_u64().filter(|id| *id > 0) else {
            return true;
        };
        !stream_ids.insert(id)
            || stream["io_failed"] != false
            || !stream["parser_error"].is_null()
            || !matches!(stream["protocol"].as_str(), Some(protocol)
            if protocol == application_observer::ECHO
                || protocol == AsRef::<str>::as_ref(&identify::PROTOCOL_NAME)
                || protocol == AsRef::<str>::as_ref(&identify::PUSH_PROTOCOL_NAME))
    }) {
        return Err(invalid(
            "unexpected protocol beside coordinated echo/Identify",
        ));
    }
    let mut echoes = streams
        .iter()
        .filter(|stream| stream["protocol"] == application_observer::ECHO);
    let stream = echoes
        .next()
        .ok_or_else(|| invalid("missing actual coordinated echo stream"))?;
    if echoes.next().is_some() {
        return Err(invalid("coordinated echo must open exactly one new stream"));
    }
    let direction = if role == Role::Initiator {
        "outbound"
    } else {
        "inbound"
    };
    if raw["overflow"] != false
        || connection["connection_trace_id"] != socket["connection_trace_id"]
        || stream["protocol"] != application_observer::ECHO
        || stream["direction"] != direction
        || stream["io_failed"] != false
        || !stream["parser_error"].is_null()
        || stream["write_close_returned"] != true
        || require_drop && stream["drop_observed"] != true
        || stream["read"] != application["read"]
        || stream["write"] != application["write"]
        || application["read"] != application["write"]
        || application["read"]["frames"] != 1
        || application["read"]["complete_frames"] != true
    {
        return Err(invalid(
            "new echo stream lacks exact retained-owner framed I/O",
        ));
    }
    Ok(
        json!({"basis": "unique_retained_connection_and_actual_new_stream_framed_io", "protocol": application_observer::ECHO,
        "native_connection_id": socket["native_connection_id"], "connection_trace_id": connection["connection_trace_id"],
        "stream_trace_id": stream["stream_trace_id"], "stream_direction": direction,
        "request": application["write"], "response": application["read"], "write_close_returned": true}),
    )
}

struct Capture {
    started: Instant,
    events: Vec<Value>,
}
impl Capture {
    fn event(&mut self, kind: &str, fields: Value) -> io::Result<()> {
        if self.events.len() == EVENT_LIMIT {
            return Err(invalid("coordinated event bound"));
        }
        let mut row = fields;
        row["kind"] = json!(kind);
        row["sequence"] = json!(self.events.len() + 1);
        row["mono_ns"] = json!((self.started.elapsed().as_nanos() + 1) as u64);
        self.events.push(row);
        Ok(())
    }
}

pub(crate) async fn run(config: Config) -> io::Result<()> {
    let mut capture = Capture {
        started: Instant::now(),
        events: Vec::new(),
    };
    let key = identity::Keypair::generate_ed25519();
    let local = key.public().to_peer_id();
    let observer = upgrade_observer::Observer::default();
    let owner = task_owner::Owner::default();
    let (psk, pnet_fingerprint) = if let Some(path) = &config.key {
        let mut text = String::new();
        std::io::Read::read_to_string(
            &mut std::io::Read::take(std::fs::File::open(path)?, 2049),
            &mut text,
        )?;
        if text.len() > 2048 {
            return Err(invalid("private key fixture bound"));
        }
        let (psk, fingerprint) = verified_private_key(
            &text,
            config
                .fingerprint
                .as_deref()
                .ok_or_else(|| invalid("missing private key fingerprint"))?,
        )?;
        (Some(psk), Some(fingerprint))
    } else {
        (None, None)
    };
    let transport = if config.private {
        let psk =
            psk.ok_or_else(|| invalid("private coordinated transport requires a protector key"))?;
        upgrade_observer::native_private_transport(&key, false, Some(psk), observer.clone())
    } else {
        upgrade_observer::native_transport(&key, false, observer.clone(), None)
    }
    .map_err(|_| invalid("native TCP Noise transport construction failed"))?;
    let audit = DialAudit::default();
    let transport = AuditedTransport {
        inner: transport,
        audit: audit.clone(),
    }
    .boxed();
    let mut swarm = Swarm::new(
        transport,
        native_behaviour(&key),
        local,
        owner
            .swarm_config()
            .with_idle_connection_timeout(RUN_BUDGET),
    );
    let listen = match config.bind {
        IpAddr::V4(ip) => format!("/ip4/{ip}/tcp/0"),
        IpAddr::V6(ip) => format!("/ip6/{ip}/tcp/0"),
    }
    .parse()
    .map_err(|_| invalid("invalid numeric local listener"))?;
    let listener_id = swarm
        .listen_on(listen)
        .map_err(|_| invalid("native TCP listener failed"))?;
    let mut control = swarm.behaviour().stream.new_control();
    let mut incoming = control
        .accept(StreamProtocol::new(application_observer::ECHO))
        .map_err(|_| invalid("echo registration failed"))?;
    let mut listener: Option<Multiaddr> = None;
    let mut plan: Option<Plan> = None;
    let mut dial_id: Option<ConnectionId> = None;
    let mut operation_deadline: Option<Instant> = None;
    let mut retained: Option<ConnectionId> = None;
    let mut sequence = 0u64;
    let mut stage = "starting";
    let mut socket: Option<Value> = None;
    let mut application: Option<Value> = None;
    let mut stream_proof: Option<Value> = None;
    let mut pending: Option<futures::channel::oneshot::Receiver<io::Result<Value>>> = None;
    let mut probe_dials_before = None;
    let mut tick = tokio::time::interval(Duration::from_millis(10));
    let execution: io::Result<()> = async {
        loop {
            tokio::select! {
                event = swarm.select_next_some() => match event {
                    SwarmEvent::NewListenAddr { listener_id: actual, address } => {
                        if actual != listener_id || listener.is_some() { return Err(invalid("unexpected additional listener")); }
                        let address = numeric_tcp(&address, None)?;
                        let correct_ip = match (config.bind, address.iter().next()) {
                            (IpAddr::V4(a), Some(Protocol::Ip4(b))) => a == b,
                            (IpAddr::V6(a), Some(Protocol::Ip6(b))) => a == b,
                            _ => false,
                        };
                        if !correct_ip { return Err(invalid("actual listener IP mismatch")); }
                        if swarm.network_info().connection_counters().num_connections() != 0 || audit.count() != 0 || observer.snapshot()["connections"] != json!([]) {
                            return Err(invalid("coordinated actor has preexisting native connections"));
                        }
                        capture.event("native_listener_ready", json!({"listener_id": actual.to_string(), "listener_address": address.to_string(), "preexisting_connection_ids": []}))?;
                        atomic(&config.ready, &json!({"implementation": "rust", "status": "ready", "case_token": config.token,
                            "peer_id": local.to_string(), "coord_role": config.role.name(), "listener_address": address.to_string(),
                            "listener_port": address.iter().find_map(|part| if let Protocol::Tcp(port) = part { Some(port) } else { None }),
                            "listen_addrs": [address.clone().with(Protocol::P2p(local)).to_string()], "preexisting_connection_ids": []}))?;
                        listener = Some(address); stage = "ready";
                    }
                    SwarmEvent::ConnectionEstablished { peer_id, connection_id, endpoint, .. } => {
                        observer.established(connection_id, peer_id, &endpoint);
                        capture.event("native_connection_established", json!({"native_connection_id": connection_id.to_string(),
                            "peer_id": peer_id.to_string(), "endpoint": upgrade_observer::endpoint(&endpoint)}))?;
                        let target = plan.as_ref().ok_or_else(|| invalid("connection before PREPARE"))?;
                        if retained.is_some() || Some(connection_id) != dial_id || peer_id != target.peer || !matches!(endpoint, ConnectedPoint::Dialer { .. }) {
                            return Err(invalid("the planned native outgoing dial did not win uniquely"));
                        }
                        let proof = completed_socket(&observer.snapshot(), local, target, listener.as_ref().ok_or_else(|| invalid("missing listener"))?, connection_id, config.role)?;
                        if audit.count() != 1 || swarm.network_info().connection_counters().num_connections() != 1 { return Err(invalid("coordinated dial requested extra native sockets")); }
                        retained = Some(connection_id); socket = proof;
                        stage = if socket.is_some() { "connected" } else { "connecting" };
                    }
                    SwarmEvent::Behaviour(CoordinatedBehaviourEvent::Identify(event)) => {
                        let id = event.connection_id();
                        let (peer, kind, failure) = match event {
                            identify::Event::Received { peer_id, info, .. } => {
                                if info.public_key.to_peer_id() != peer_id { return Err(invalid("native Identify public key differs from authenticated peer")); }
                                (peer_id, "native_identify_received", None)
                            }
                            identify::Event::Sent { peer_id, .. } => (peer_id, "native_identify_sent", None),
                            identify::Event::Pushed { peer_id, .. } => (peer_id, "native_identify_pushed", None),
                            identify::Event::Error { peer_id, error, .. } => (peer_id, "native_identify_error", Some(error.to_string().chars().take(256).collect::<String>())),
                        };
                        if retained != Some(id) || plan.as_ref().is_none_or(|target| target.peer != peer) {
                            return Err(invalid("native Identify event lost the planned outgoing owner"));
                        }
                        capture.event(kind, json!({"source": "rust.identify.Event", "native_connection_id": id.to_string(),
                            "peer_id": peer.to_string(), "error": failure}))?;
                        if failure.is_some() { return Err(invalid("native coordinated Identify failed")); }
                    }
                    SwarmEvent::OutgoingConnectionError { connection_id, error, .. } => {
                        capture.event("native_dial_error", json!({"native_connection_id": connection_id.to_string(), "error": error.to_string().chars().take(256).collect::<String>()}))?;
                        return Err(invalid("native coordinated dial failed"));
                    }
                    SwarmEvent::ConnectionClosed { connection_id, .. } => {
                        capture.event("native_connection_closed", json!({"native_connection_id": connection_id.to_string()}))?;
                        return Err(invalid("retained native connection closed before cleanup"));
                    }
                    SwarmEvent::ListenerClosed { .. } | SwarmEvent::ListenerError { .. } => return Err(invalid("native TCP listener terminated before cleanup")),
                    _ => {}
                },
                next = incoming.next() => {
                    let (peer, stream) = next.ok_or_else(|| invalid("echo listener terminated"))?;
                    if config.role != Role::Responder || stage != "connected" || plan.as_ref().is_none_or(|target| target.peer != peer)
                        || pending.is_some() || application.is_some() { return Err(invalid("unexpected or duplicate incoming probe")); }
                    let token = config.token.clone();
                    probe_dials_before = Some(audit.count());
                    let echo_budget = ECHO_BUDGET.min(operation_deadline.ok_or_else(|| invalid("missing operation deadline"))?.saturating_duration_since(Instant::now()));
                    let (send, receive) = futures::channel::oneshot::channel();
                    owner.spawn("coordinated_echo", async move {
                        let answer = tokio::time::timeout(echo_budget, responder_echo(stream, token)).await
                            .map_err(|_| invalid("responder echo deadline")).and_then(|r| r);
                        let _ = send.send(answer);
                    })?;
                    pending = Some(receive);
                }
                _ = tick.tick() => {
                    if capture.started.elapsed() >= RUN_BUDGET { return Err(io::Error::new(io::ErrorKind::TimedOut, "coordinated actor deadline")); }
                    if stage != "exchanged" && operation_deadline.is_some_and(|deadline| Instant::now() >= deadline) {
                        return Err(io::Error::new(io::ErrorKind::TimedOut, "coordinated operation deadline"));
                    }
                    if config.stop.exists() {
                        if stage != "exchanged" { return Err(invalid("stop is cleanup only; coordinated proof is incomplete")); }
                        break;
                    }
                    if audit.count() > 1 { return Err(invalid("application or other behaviour requested a fresh dial")); }
                    if let Some(id) = retained.filter(|_| socket.is_none()) {
                        let target = plan.as_ref().ok_or_else(|| invalid("missing retained peer plan"))?;
                        if let Some(proof) = completed_socket(&observer.snapshot(), local, target,
                            listener.as_ref().ok_or_else(|| invalid("missing retained listener"))?, id, config.role)? {
                            if swarm.network_info().connection_counters().num_connections() != 1 || audit.count() != 1 {
                                return Err(invalid("native lazy upgrade lost its unique outgoing owner"));
                            }
                            socket = Some(proof); stage = "connected";
                            capture.event("native_muxer_confirmed", json!({"native_connection_id": id.to_string(), "source": "rust.native_upgrade_observer.completed_muxer"}))?;
                        }
                    }
                    if let Some(receiver) = pending.as_mut() {
                        if let Some(answer) = receiver.try_recv().map_err(|_| invalid("echo task did not return"))? {
                            application = Some(answer?); pending = None;
                        }
                    }
                    if let (Some(socket), Some(application)) = (&socket, &application) {
                        if stream_proof.is_none() {
                            let proof = echo_binding(&observer.snapshot(), socket, application, config.role, false)?;
                            if probe_dials_before != Some(audit.count()) || audit.count() != 1
                                || swarm.network_info().connection_counters().num_connections() != 1 || !swarm.is_connected(&plan.as_ref().unwrap().peer) { return Err(invalid("probe lost its retained connection")); }
                            capture.event("application_completed", json!({"native_connection_id": retained.map(|id| id.to_string()), "native_transport_dials_before": probe_dials_before, "native_transport_dials_after": audit.count()}))?;
                            stream_proof = Some(proof); stage = "exchanged";
                        }
                    }
                    if listener.is_some() && config.control.exists() {
                        let command = fields(&config.control)?;
                        if command.len() != 3 || command.get("case-token") != Some(&config.token) { return Err(invalid("invalid control token/fields")); }
                        let next: u64 = command.get("sequence").ok_or_else(|| invalid("missing sequence"))?.parse().map_err(|_| invalid("invalid sequence"))?;
                        if next > sequence {
                            if next != sequence + 1 { return Err(invalid("nonmonotonic control sequence")); }
                            let action = command.get("action").ok_or_else(|| invalid("missing action"))?;
                            match action.as_str() {
                                "prepare" | "start" => {
                                    if dial_id.is_some() || swarm.network_info().connection_counters().num_connections() != 0 || audit.count() != 0 { return Err(invalid("prepare/start requires a fresh actor")); }
                                    let target = read_plan(&config.plan, &config.token, local)?;
                                    if plan.as_ref().is_some_and(|previous| previous != &target) { return Err(invalid("prepared peer plan changed")); }
                                    if plan.is_none() { capture.event("prepared", json!({"expected_peer_id": target.peer.to_string(), "exact_peer_listener": target.address.to_string(), "preexisting_connection_ids": []}))?; }
                                    if action == "start" {
                                        operation_deadline = Some(Instant::now() + config.operation_budget);
                                        let options = dial_options(&target, config.role); let id = options.connection_id();
                                        swarm.dial(options).map_err(|_| invalid("native coordinated dial admission failed"))?;
                                        dial_id = Some(id); stage = "connecting";
                                        capture.event("native_dial_admitted", json!({"native_connection_id": id.to_string(), "expected_peer_id": target.peer.to_string(), "exact_peer_listener": target.address.to_string()}))?;
                                    } else { stage = "prepared"; }
                                    plan = Some(target);
                                }
                                "probe" => {
                                    if config.role != Role::Initiator || stage != "connected" || pending.is_some() || application.is_some() { return Err(invalid("probe requires the initiator's existing native connection")); }
                                    let peer = plan.as_ref().unwrap().peer;
                                    let mut peers = swarm.connected_peers();
                                    if peers.next() != Some(&peer) || peers.next().is_some() || audit.count() != 1
                                        || swarm.network_info().connection_counters().num_connections() != 1 { return Err(invalid("probe owner is not uniquely retained")); }
                                    probe_dials_before = Some(audit.count());
                                    let echo_budget = ECHO_BUDGET.min(operation_deadline.ok_or_else(|| invalid("missing operation deadline"))?.saturating_duration_since(Instant::now()));
                                    let token = config.token.clone(); let observer = observer.clone(); let app_control = control.clone();
                                    let (send, receive) = futures::channel::oneshot::channel();
                                    owner.spawn("coordinated_echo", async move {
                                        let answer = tokio::time::timeout(echo_budget, initiator_echo(app_control, observer, peer, token)).await
                                            .map_err(|_| invalid("initiator echo deadline")).and_then(|r| r);
                                        let _ = send.send(answer);
                                    })?;
                                    pending = Some(receive);
                                }
                                _ => return Err(invalid("unsupported coordinated control; stop is cleanup only")),
                            }
                            sequence = next;
                            capture.event("control_completed", json!({"control_sequence": next, "action": action,
                                "completion": match action.as_str() { "prepare" => "peer_listener_bound", "start" => "native_dial_admitted", _ => "application_admitted" }}))?;
                        }
                    }
                    atomic(&config.result, &json!({"implementation": "rust", "case_token": config.token, "coord_role": config.role.name(),
                        "status": stage, "joined": false, "finalized": false, "events": capture.events, "socket": socket,
                        "application": stream_proof, "native_transport_dials": audit.snapshot(), "raw_upgrade_observations": observer.snapshot()}))?;
                }
            }
        }
        Ok(())
    }.await;
    drop(incoming);
    drop(control);
    drop(swarm);
    let join_budget = JOIN_BUDGET.min(TOTAL_BUDGET.saturating_sub(capture.started.elapsed()));
    let report = tokio::time::timeout(join_budget, owner.close_and_join()).await;
    let join = report.map(|report| report.snapshot()).unwrap_or_else(
        |_| json!({"fixture_owned_tasks_joined": false, "error": "cleanup join deadline"}),
    );
    let joined = join["fixture_owned_tasks_joined"] == true
        && join["overflow"] == false
        && join["errors"].as_array().is_some_and(Vec::is_empty);
    let raw = observer.finalized(joined);
    let mut error = execution.as_ref().err().map(ToString::to_string);
    if error.is_none() && !joined {
        error = Some("native cleanup did not join".into());
    }
    if error.is_none() {
        match echo_binding(
            &raw,
            socket
                .as_ref()
                .ok_or_else(|| invalid("missing socket receipt"))?,
            application
                .as_ref()
                .ok_or_else(|| invalid("missing application receipt"))?,
            config.role,
            true,
        ) {
            Ok(proof) => stream_proof = Some(proof),
            Err(failure) => error = Some(failure.to_string()),
        }
        if config.role == Role::Initiator && raw["complete"] != true {
            error = Some("independent native application binding incomplete".into());
        }
    }
    atomic(
        &config.result,
        &json!({"schema": "forge.p2p.evidence.coordinated.v1", "implementation": "rust",
        "scenario": if config.private { PRIVATE } else { NATIVE }, "transport": if config.private { "tcp-pnet-noise" } else { "tcp" },
        "case_token": config.token, "coord_role": config.role.name(), "peer_id": local.to_string(), "timeout_ms": config.operation_budget.as_millis(),
        "pnet_fingerprint": pnet_fingerprint, "pnet_fingerprint_basis": pnet_fingerprint.as_ref().map(|_| PNET_FINGERPRINT_BASIS),
        "status": if error.is_none() { "ok" } else { "error" }, "finalized": true, "joined": joined, "error": error,
        "events": capture.events, "socket": socket, "application": stream_proof, "native_transport_dials": audit.snapshot(),
        "task_join": join, "raw_upgrade_observations": raw, "per_call_cancel_supported": false}),
    )?;
    if let Some(error) = error {
        Err(invalid(&error))
    } else {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use libp2p::{core::upgrade::UpgradeInfo, swarm::ConnectionHandler};

    #[tokio::test]
    async fn native_identify_and_echo_are_advertised_for_both_outgoing_upgrade_roles() {
        for role in [Endpoint::Dialer, Endpoint::Listener] {
            let key = identity::Keypair::generate_ed25519();
            let mut behaviour = native_behaviour(&key);
            let mut control = behaviour.stream.new_control();
            let _incoming = control
                .accept(StreamProtocol::new(application_observer::ECHO))
                .unwrap();
            let handler = behaviour
                .handle_established_outbound_connection(
                    ConnectionId::new_unchecked(1),
                    PeerId::random(),
                    &"/ip4/11.0.0.2/tcp/40100".parse().unwrap(),
                    role,
                    libp2p::core::transport::PortUse::Reuse,
                )
                .unwrap();
            let protocols = handler
                .listen_protocol()
                .upgrade()
                .protocol_info()
                .map(|protocol| AsRef::<str>::as_ref(&protocol).to_owned())
                .collect::<Vec<_>>();
            for protocol in [
                AsRef::<str>::as_ref(&identify::PROTOCOL_NAME),
                application_observer::ECHO,
            ] {
                assert!(
                    protocols.iter().any(|name| name == protocol),
                    "missing native {protocol} for {role:?}"
                );
            }
        }

        // The key type is private to the donor. Verify its actual Identify
        // output instead; this memory exchange is not coordinated path proof.
        fn memory_actor(
            key: &identity::Keypair,
            owner: &task_owner::Owner,
        ) -> Swarm<CoordinatedBehaviour> {
            let transport = libp2p::core::transport::MemoryTransport::default()
                .upgrade(libp2p::core::upgrade::Version::V1)
                .authenticate(libp2p::noise::Config::new(key).unwrap())
                .multiplex(libp2p::yamux::Config::default())
                .boxed();
            Swarm::new(
                transport,
                native_behaviour(key),
                key.public().to_peer_id(),
                owner
                    .swarm_config()
                    .with_idle_connection_timeout(Duration::from_secs(10)),
            )
        }
        let owner = task_owner::Owner::default();
        let sender_key = identity::Keypair::generate_ed25519();
        let receiver_key = identity::Keypair::generate_ed25519();
        let sender_peer = sender_key.public().to_peer_id();
        let mut sender = memory_actor(&sender_key, &owner);
        let mut receiver = memory_actor(&receiver_key, &owner);
        sender.listen_on("/memory/0".parse().unwrap()).unwrap();
        let exchange = tokio::time::timeout(Duration::from_secs(5), async {
            let listener = loop {
                if let SwarmEvent::NewListenAddr { address, .. } = sender.select_next_some().await {
                    break address;
                }
            };
            receiver.dial(DialOpts::peer_id(sender_peer).addresses(vec![listener.clone()]).build()).unwrap();
            loop {
                tokio::select! {
                    _ = sender.select_next_some() => {},
                    event = receiver.select_next_some() => {
                        if let SwarmEvent::Behaviour(CoordinatedBehaviourEvent::Identify(identify::Event::Received { peer_id, info, .. })) = event {
                            return (peer_id, info, listener);
                        }
                    }
                }
            }
        }).await;
        drop(sender);
        drop(receiver);
        let report = tokio::time::timeout(Duration::from_secs(2), owner.close_and_join())
            .await
            .unwrap()
            .snapshot();
        assert_eq!(report["fixture_owned_tasks_joined"], true);
        assert_eq!(report["errors"], json!([]));
        let (peer, info, listener) = exchange.expect("native Identify exchange deadline");
        assert_eq!(peer, sender_peer);
        assert_eq!(info.public_key.to_peer_id(), sender_peer);
        let signed = info
            .signed_peer_record
            .expect("native Identify must emit its signed peer record");
        let record = libp2p::core::PeerRecord::from_signed_envelope(signed)
            .expect("native Identify signature must verify");
        assert_eq!(record.peer_id(), sender_peer);
        assert!(record.addresses().contains(&listener));
        assert!(info.listen_addrs.contains(&listener));
    }

    fn argv(private: bool, role: &str) -> Vec<String> {
        let mut args = vec![
            "coordinated-live",
            "--scenario",
            if private { PRIVATE } else { NATIVE },
            "--transport",
            if private { "tcp-pnet-noise" } else { "tcp" },
            "--coord-role",
            role,
            "--case-token",
            "0123456789abcdef0123456789abcdef",
            "--bind-ip",
            "10.0.0.2",
            "--ready-file",
            "/case/ready",
            "--result-file",
            "/case/result",
            "--stop-file",
            "/case/stop",
            "--control-file",
            "/case/control",
            "--plan-file",
            "/case/plan",
        ]
        .into_iter()
        .map(String::from)
        .collect::<Vec<_>>();
        if private {
            args.extend(
                [
                    "--pnet-key-file",
                    "/case/key",
                    "--pnet-fingerprint",
                    &"a".repeat(64),
                ]
                .into_iter()
                .map(String::from),
            );
        }
        args
    }
    #[test]
    fn exact_cli_accepts_both_profiles_and_both_roles() {
        for private in [false, true] {
            for role in ["initiator", "responder"] {
                let config = parse_args(&argv(private, role)).unwrap().unwrap();
                assert_eq!(config.private, private);
                assert_eq!(config.role.name(), role);
                assert_eq!(config.operation_budget, Duration::from_millis(20000));
            }
        }
        assert!(parse_args(&["path-live".into()]).unwrap().is_none());
    }
    #[test]
    fn cli_rejects_noncanonical_or_unrelated_switches() {
        for additions in [
            vec!["--relay-addr", "/ip4/11.0.0.1/tcp/1"],
            vec!["--case-token", "duplicate"],
            vec!["--timeout-ms", "45001"],
            vec!["--pnet-key-file", "/case/key"],
        ] {
            let mut args = argv(false, "initiator");
            args.extend(additions.into_iter().map(String::from));
            assert!(parse_args(&args).is_err());
        }
        assert!(parse_args(&argv(false, "source")).is_err());
        let mut args = argv(true, "responder");
        args[4] = "tcp".into();
        assert!(parse_args(&args).is_err());
    }

    #[test]
    fn optional_timeout_sets_actual_bounded_operation_budget() {
        for budget in [1000, 1234, 30000] {
            for private in [false, true] {
                let mut args = argv(private, "initiator");
                args.extend(["--timeout-ms".into(), budget.to_string()]);
                assert_eq!(
                    parse_args(&args).unwrap().unwrap().operation_budget,
                    Duration::from_millis(budget)
                );
            }
        }
        for budget in ["999", "30001", "not-a-number", "18446744073709551616"] {
            let mut args = argv(false, "responder");
            args.extend(["--timeout-ms".into(), budget.into()]);
            assert!(parse_args(&args).is_err());
        }
    }

    #[test]
    fn loaded_psk_operational_fingerprint_matches_cross_language_vector() {
        let material = std::array::from_fn(|i| i as u8);
        let text = PreSharedKey::new(material).to_string();
        let uppercase = format!(
            "/key/swarm/psk/1.0.0/\n/base16/\n{}\n",
            text.lines().nth(2).unwrap().to_ascii_uppercase()
        );
        let expected = "7c291ef5c453de491f0a6a219ee8e3207767446da373dfbbbb91ce7d55418583";
        for text in [text.clone(), uppercase, text.replace('\n', "\r\n")] {
            let (installed, fingerprint) = verified_private_key(&text, expected).unwrap();
            assert!(installed == PreSharedKey::new(material));
            assert_eq!(fingerprint, expected);
            assert!(verified_private_key(&text, &installed.fingerprint().to_string()).is_err());
        }
    }

    #[test]
    fn private_fingerprint_mismatch_rejects_before_transport_construction() {
        let text = PreSharedKey::new([0; 32]).to_string();
        for expected in [
            "7c291ef5c453de491f0a6a219ee8e3207767446da373dfbbbb91ce7d55418583",
            &"a".repeat(64),
        ] {
            let error = verified_private_key(&text, expected).err().unwrap();
            assert_eq!(
                error.to_string(),
                "coordinated PSK fingerprint differs from installed key"
            );
        }
    }

    #[test]
    fn invalid_private_key_errors_are_bounded_and_do_not_expose_material() {
        for text in [
            "invalid key fixture".into(),
            format!("/key/swarm/psk/1.0.0/\n/base16/\n{}\n", "x".repeat(64)),
            format!("/key/swarm/psk/1.0.0/\n/base16/\n{}\n", "\u{e9}".repeat(32)),
            format!("{}unexpected\n", PreSharedKey::new([0; 32])),
        ] {
            let error = verified_private_key(&text, &"a".repeat(64)).err().unwrap();
            assert_eq!(error.to_string(), "invalid private key fixture");
        }
    }

    #[test]
    fn listener_grammar_rejects_relay_dns_zero_port_and_wrong_peer() {
        for text in [
            "/ip4/10.0.0.2/tcp/0",
            "/dns4/example.test/tcp/4001",
            "/ip4/10.0.0.2/udp/4001/quic-v1",
            "/ip4/10.0.0.2/tcp/4001/p2p-circuit",
        ] {
            assert!(numeric_tcp(&text.parse().unwrap(), None).is_err());
        }
        let peer = PeerId::random();
        let listener: Multiaddr = "/ip6/::1/tcp/4001".parse().unwrap();
        assert_eq!(
            numeric_tcp(&listener.clone().with(Protocol::P2p(peer)), Some(peer)).unwrap(),
            listener
        );
        assert!(numeric_tcp(&listener.with(Protocol::P2p(peer)), Some(PeerId::random())).is_err());
    }
    #[test]
    fn native_dial_addresses_are_explicit_and_identity_bound() {
        let target = Plan {
            peer: PeerId::random(),
            address: "/ip4/10.0.0.3/tcp/4001".parse().unwrap(),
        };
        for role in [Role::Initiator, Role::Responder] {
            assert_eq!(dial_options(&target, role).get_peer_id(), Some(target.peer));
        }
    }
    #[tokio::test]
    async fn echo_codec_is_bounded_and_matches_existing_varint_wire() {
        let token = "0123456789abcdef0123456789abcdef";
        let expected = challenge(token);
        assert_eq!(expected, b"coordinated:0123456789abcdef0123456789abcdef");
        let mut io = futures::io::Cursor::new(Vec::new());
        write_echo(&mut io, &expected).await.unwrap();
        assert_eq!(io.get_ref()[0] as usize, expected.len());
        io.set_position(0);
        assert_eq!(read_echo(&mut io).await.unwrap(), expected);
        for prefix in [0, 128, 255] {
            assert!(
                read_echo(&mut futures::io::Cursor::new(vec![prefix]))
                    .await
                    .is_err()
            );
        }
        assert_eq!(body_receipt(&expected)["complete_frames"], true);
    }
    #[test]
    fn empty_observations_never_prove_a_socket_or_application() {
        let local = PeerId::random();
        let peer = PeerId::random();
        let listener = "/ip4/10.0.0.2/tcp/4001".parse().unwrap();
        let plan = Plan {
            peer,
            address: "/ip4/10.0.0.3/tcp/4002".parse().unwrap(),
        };
        let raw = upgrade_observer::Observer::default().snapshot();
        assert!(
            completed_socket(
                &raw,
                local,
                &plan,
                &listener,
                ConnectionId::new_unchecked(1),
                Role::Initiator
            )
            .is_err()
        );
        assert!(echo_binding(&raw, &json!({}), &json!({}), Role::Initiator, false).is_err());
    }

    // These are validation-boundary unit inputs, never process or socket receipts.
    fn unit_socket(role: Role) -> (Value, PeerId, Plan, Multiaddr, ConnectionId) {
        let local = PeerId::random();
        let peer = PeerId::random();
        let listener: Multiaddr = "/ip4/10.0.0.2/tcp/4001".parse().unwrap();
        let plan = Plan {
            peer,
            address: "/ip4/10.0.0.3/tcp/4002".parse().unwrap(),
        };
        let id = ConnectionId::new_unchecked(50);
        let endpoint = json!({"direction": "outbound", "remote_address": plan.address.to_string(), "upgrade_role": role.upgrade_direction()});
        let connection = json!({"connection_trace_id": 1, "endpoint": endpoint,
            "direction": "outbound", "local_address": listener.to_string(), "remote_address": plan.address.to_string(),
            "authenticated_local_peer_id": local.to_string(), "authenticated_remote_peer_id": peer.to_string(),
            "security_complete": true, "muxer_complete": true, "security_delegate_completed": true, "muxer_delegate_completed": true,
            "selected_security": "/noise", "selected_muxer": "/yamux/1.0.0", "events": [], "streams": [],
            "negotiations": [{"direction": role.upgrade_direction(), "protocol": "/noise", "io_failed": false, "parser_error": null},
                {"direction": role.upgrade_direction(), "protocol": "/yamux/1.0.0", "io_failed": false, "parser_error": null}],
            "transport_output_receipts": [{"basis": "donor_transport_output_identity", "after_event_sequence": 0,
                "connection_trace_id": 1, "authenticated_remote_peer_id": peer.to_string(), "dns_wrapper_enabled": false,
                "request_endpoint": endpoint, "resolved_endpoint": endpoint, "local_address": listener.to_string(), "remote_address": plan.address.to_string()}]});
        (
            json!({"overflow": false, "connections": [connection], "swarm_events": [{"swarm_connection_id": id.to_string(),
            "authenticated_remote_peer_id": peer.to_string(), "endpoint": endpoint}]}),
            local,
            plan,
            listener,
            id,
        )
    }

    #[test]
    fn socket_binding_keeps_actual_ip_port_identity_and_completed_role_strict() {
        for role in [Role::Initiator, Role::Responder] {
            let (raw, local, plan, listener, id) = unit_socket(role);
            let bound = completed_socket(&raw, local, &plan, &listener, id, role)
                .unwrap()
                .unwrap();
            assert_eq!(bound["local_address"], listener.to_string());
            assert_eq!(
                bound["security_role"],
                if role == Role::Initiator {
                    "client"
                } else {
                    "server"
                }
            );
            for mutation in 0..10 {
                let mut bad = raw.clone();
                match mutation {
                    0 => bad["connections"][0]["local_address"] = json!("/ip4/10.0.0.2/tcp/4009"),
                    1 => bad["connections"][0]["local_address"] = json!("/ip4/10.0.0.9/tcp/4001"),
                    2 => {
                        bad["connections"][0]["authenticated_remote_peer_id"] =
                            json!(PeerId::random().to_string())
                    }
                    3 => bad["swarm_events"][0]["swarm_connection_id"] = json!("51"),
                    4 => bad["connections"][0]["security_delegate_completed"] = json!(false),
                    5 => {
                        bad["connections"][0]["negotiations"][0]["direction"] =
                            json!(if role == Role::Initiator {
                                "inbound"
                            } else {
                                "outbound"
                            })
                    }
                    6 => {
                        let copy = bad["connections"][0].clone();
                        bad["connections"].as_array_mut().unwrap().push(copy);
                    }
                    7 => bad["connections"][0]["negotiations"][1]["io_failed"] = json!(true),
                    8 => bad["connections"][0]["negotiations"][1]["protocol"] = Value::Null,
                    _ => bad["connections"][0]["muxer_complete"] = Value::Null,
                }
                assert!(
                    completed_socket(&bad, local, &plan, &listener, id, role).is_err(),
                    "mutation {mutation}"
                );
            }
        }
    }

    #[test]
    fn native_swarm_output_is_not_proof_of_lazy_muxer_completion() {
        for role in [Role::Initiator, Role::Responder] {
            let (mut raw, local, plan, listener, id) = unit_socket(role);
            let complete = raw.clone();
            raw["connections"][0]["muxer_complete"] = json!(false);
            raw["connections"][0]["negotiations"][1]["protocol"] = Value::Null;
            assert!(
                completed_socket(&raw, local, &plan, &listener, id, role)
                    .unwrap()
                    .is_none()
            );
            for mutation in 0..4 {
                let mut bad = raw.clone();
                match mutation {
                    0 => bad["connections"][0]["local_address"] = json!("/ip4/10.0.0.2/tcp/4999"),
                    1 => bad["connections"][0]["negotiations"][1]["io_failed"] = json!(true),
                    2 => {
                        bad["connections"][0]["negotiations"][1]["direction"] =
                            json!(if role == Role::Initiator {
                                "inbound"
                            } else {
                                "outbound"
                            })
                    }
                    _ => bad["connections"][0]["direction"] = json!("inbound"),
                }
                assert!(completed_socket(&bad, local, &plan, &listener, id, role).is_err());
            }
            assert!(
                completed_socket(&complete, local, &plan, &listener, id, role)
                    .unwrap()
                    .is_some()
            );
            assert_eq!(raw["connections"][0]["muxer_complete"], false);
        }
    }

    #[test]
    fn echo_binding_rejects_changed_connection_hash_or_multiple_streams() {
        let (mut raw, local, plan, listener, id) = unit_socket(Role::Initiator);
        let socket = completed_socket(&raw, local, &plan, &listener, id, Role::Initiator)
            .unwrap()
            .unwrap();
        let body = body_receipt(&challenge("0123456789abcdef0123456789abcdef"));
        let application = json!({"read": body, "write": body});
        let stream = json!({"protocol": application_observer::ECHO, "direction": "outbound", "stream_trace_id": 1,
            "io_failed": false, "parser_error": null, "write_close_returned": true, "drop_observed": true, "read": body, "write": body});
        raw["connections"][0]["streams"] = json!([stream]);
        assert!(echo_binding(&raw, &socket, &application, Role::Initiator, true).is_ok());
        let mut with_identify = raw.clone();
        with_identify["connections"][0]["streams"]
            .as_array_mut()
            .unwrap()
            .push(json!({
                "protocol": AsRef::<str>::as_ref(&identify::PROTOCOL_NAME), "stream_trace_id": 2,
                "io_failed": false, "parser_error": null
            }));
        let bound =
            echo_binding(&with_identify, &socket, &application, Role::Initiator, true).unwrap();
        assert_eq!(bound["stream_trace_id"], 1);
        let mut identify_only = with_identify.clone();
        identify_only["connections"][0]["streams"]
            .as_array_mut()
            .unwrap()
            .remove(0);
        assert!(
            echo_binding(&identify_only, &socket, &application, Role::Initiator, true).is_err()
        );
        for mutation in 0..7 {
            let mut bad = raw.clone();
            match mutation {
                0 => bad["connections"][0]["connection_trace_id"] = json!(2),
                1 => bad["connections"][0]["streams"][0]["read"]["framed_sha256"] = json!("bad"),
                2 => bad["connections"][0]["streams"][0]["drop_observed"] = json!(false),
                3 => {
                    let mut stream = bad["connections"][0]["streams"][0].clone();
                    stream["stream_trace_id"] = json!(3);
                    bad["connections"][0]["streams"]
                        .as_array_mut()
                        .unwrap()
                        .push(stream);
                }
                4 => bad["connections"][0]["streams"]
                    .as_array_mut()
                    .unwrap()
                    .push(json!({"protocol": "/unknown/application/1", "stream_trace_id": 3, "io_failed": false})),
                5 => {
                    bad = with_identify.clone();
                    bad["connections"][0]["streams"][1]["stream_trace_id"] = json!(1);
                }
                _ => {
                    bad = with_identify.clone();
                    bad["connections"][0]["streams"][1]["io_failed"] = json!(true);
                }
            }
            assert!(echo_binding(&bad, &socket, &application, Role::Initiator, true).is_err());
        }
    }
}
