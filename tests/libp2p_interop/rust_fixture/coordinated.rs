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
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
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
    swarm::{ConnectionError, ConnectionId, NetworkBehaviour, SwarmEvent, dial_opts::DialOpts},
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

fn unix_ns(time: SystemTime) -> io::Result<u64> {
    u64::try_from(
        time.duration_since(UNIX_EPOCH)
            .map_err(io::Error::other)?
            .as_nanos(),
    )
    .map_err(|_| invalid("coordinated observation clock overflow"))
}

fn complete_application_snapshot(
    raw: Value,
    socket: &Value,
    application: &Value,
    role: Role,
    actor_sequence: usize,
    local: PeerId,
    plan: &Plan,
    listener: &Multiaddr,
    id: ConnectionId,
) -> io::Result<Value> {
    if completed_socket(&raw, local, plan, listener, id, role)?.as_ref() != Some(socket) {
        return Err(invalid(
            "completed application lost its actual socket proof",
        ));
    }
    let binding = echo_binding(&raw, socket, application, role, true)?;
    if role == Role::Initiator {
        let attempts = raw["applications"]["attempts"]
            .as_array()
            .ok_or_else(|| invalid("missing independent application observation"))?;
        let [attempt] = attempts.as_slice() else {
            return Err(invalid("ambiguous independent application observation"));
        };
        if raw["applications"]["overflow"] != false
            || attempt["authenticated_remote_peer_id"] != plan.peer.to_string()
            || attempt["protocol"] != application_observer::ECHO
            || attempt["direction"] != "outbound"
            || [
                "opened",
                "application_io_complete",
                "attempt_ended",
                "stream_drop_returned",
                "write_close_returned",
            ]
            .iter()
            .any(|key| attempt[*key] != true)
            || attempt["overflow"] != false
            || attempt["errors"] != json!([])
            || attempt["read"] != application["read"]
            || attempt["write"] != application["write"]
        {
            return Err(invalid(
                "independent application observation is incomplete or failed",
            ));
        }
        let opened = attempt["preexisting_raw_streams"]
            .as_array()
            .ok_or_else(|| invalid("missing independent native stream open boundary"))?;
        let owners = opened
            .iter()
            .filter(|row| row["connection_trace_id"] == socket["connection_trace_id"])
            .collect::<Vec<_>>();
        if owners.len() != 1
            || owners[0]["stream_count"].as_u64().is_none_or(|count| {
                binding["stream_trace_id"]
                    .as_u64()
                    .is_none_or(|id| id <= count)
            })
        {
            return Err(invalid(
                "independent application did not open a new retained native stream",
            ));
        }
    }
    let events = raw["connections"][0]["events"]
        .as_array()
        .ok_or_else(|| invalid("missing completed native event boundary"))?;
    if events.is_empty()
        || events
            .iter()
            .enumerate()
            .any(|(index, event)| event["sequence"] != index + 1)
        || events.iter().any(|event| {
            !event["detail"]["error"].is_null()
                || event["kind"]
                    .as_str()
                    .is_some_and(|kind| kind.ends_with("_error"))
        })
    {
        return Err(invalid("invalid completed native event boundary"));
    }
    Ok(
        json!({"source": "rust.coordinated.immutable_completed_application.v1",
        "application_completed_sequence": actor_sequence, "captured_unix_ns": unix_ns(SystemTime::now())?,
        "native_connection_id": id.to_string(), "connection_trace_id": socket["connection_trace_id"],
        "raw_event_sequence": events.len(), "application": binding, "raw_upgrade_observations": raw}),
    )
}

fn observe_own_stop(
    path: &Path,
    proof: Option<&Value>,
    socket: Option<&Value>,
    peer: Option<PeerId>,
    retained: Option<ConnectionId>,
    current: &Value,
    close_observed_unix_ns: Option<u64>,
) -> io::Result<Option<Value>> {
    let metadata = match std::fs::metadata(path) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(error),
    };
    let mut marker = Vec::new();
    std::io::Read::read_to_end(
        &mut std::io::Read::take(std::fs::File::open(path)?, 6),
        &mut marker,
    )?;
    let proof =
        proof.ok_or_else(|| invalid("stop is cleanup only; coordinated proof is incomplete"))?;
    let socket = socket.ok_or_else(|| invalid("stop lacks the retained socket"))?;
    let cursor = current["connections"][0]["events"]
        .as_array()
        .ok_or_else(|| invalid("stop lacks its current native cursor"))?
        .len();
    let modified = unix_ns(metadata.modified()?)?;
    let observed = unix_ns(SystemTime::now())?;
    if !metadata.is_file() || marker != b"stop\n" {
        return Err(invalid("own Stop is not the actual regular-file marker"));
    }
    // Filesystem mtime can use a coarse kernel clock. Preserve it as an actual
    // observation, never as proof that Stop preceded an earlier native error.
    // The run loop has already frozen completion, and this read is the own-Stop
    // linearization point; a close handled without the marker remains fatal.
    if peer.map(|peer| peer.to_string()).as_deref()
        != socket["authenticated_remote_peer_id"].as_str()
        || retained.map(|id| id.to_string()).as_deref() != socket["native_connection_id"].as_str()
        || proof["native_connection_id"] != socket["native_connection_id"]
        || proof["connection_trace_id"] != socket["connection_trace_id"]
        || cursor < proof["raw_event_sequence"].as_u64().unwrap_or(u64::MAX) as usize
        || current["connections"]
            .as_array()
            .is_none_or(|rows| rows.len() != 1)
        || current["connections"][0]["connection_trace_id"] != socket["connection_trace_id"]
        || current["connections"][0]["authenticated_remote_peer_id"]
            != socket["authenticated_remote_peer_id"]
    {
        return Err(invalid(
            "own Stop is bound to a foreign retained owner or invalid current cursor",
        ));
    }
    Ok(Some(
        json!({"source": "rust.coordinated.own_stop_file_metadata_and_read.v1", "stop_file": path,
        "authorization_basis": "completed_retained_owner_and_actual_stop_read",
        "native_error_ordering": "not_inferred_from_stop_or_filesystem_time",
        "marker_hex": "73746f700a", "modified_unix_ns": modified, "observed_unix_ns": observed,
        "close_observed_unix_ns": close_observed_unix_ns,
        "authenticated_remote_peer_id": socket["authenticated_remote_peer_id"],
        "native_connection_id": socket["native_connection_id"], "connection_trace_id": socket["connection_trace_id"],
        "application_completed_sequence": proof["application_completed_sequence"],
        "raw_event_sequence": cursor}),
    ))
}

fn is_native_yamux_closed(error: io::Error) -> bool {
    let Some(inner) = error.into_inner() else {
        return false;
    };
    match inner.downcast::<yamux013::ConnectionError>() {
        Ok(native) => matches!(*native, yamux013::ConnectionError::Closed),
        Err(inner) => match inner.downcast::<libp2p::yamux::Error>() {
            // The pinned donor conversion unwraps Io and preserves other native
            // causes as typed io::Error payloads. Never authorize by Display.
            Ok(native) => is_native_yamux_closed(io::Error::from(*native)),
            Err(_) => false,
        },
    }
}

fn native_close_cause(cause: Option<ConnectionError>) -> Value {
    let raw = cause
        .as_ref()
        .map(|cause| json!({"display": cause.to_string(), "debug": format!("{cause:?}")}));
    let kind = match cause {
        None => "none",
        Some(ConnectionError::IO(error)) => {
            if is_native_yamux_closed(error) {
                "yamux013_closed"
            } else {
                "other"
            }
        }
        Some(_) => "other",
    };
    json!({"kind": kind, "source": "libp2p.swarm.ConnectionClosed.typed_native_cause", "raw": raw})
}

fn verify_identify_negotiation(stream: &Value, events: &[Value]) -> io::Result<()> {
    let protocol_frame = match stream["protocol"].as_str() {
        Some("/ipfs/id/1.0.0") => "0f2f697066732f69642f312e302e300a",
        Some("/ipfs/id/push/1.0.0") => "142f697066732f69642f707573682f312e302e300a",
        _ => return Err(invalid("unexpected native Identify protocol")),
    };
    let (proposal_direction, ack_direction) = match stream["direction"].as_str() {
        Some("outbound") => ("write", "read"),
        Some("inbound") => ("read", "write"),
        _ => return Err(invalid("missing native Identify stream direction")),
    };
    let id = stream["stream_trace_id"]
        .as_u64()
        .filter(|id| *id > 0)
        .ok_or_else(|| invalid("missing native Identify stream ID"))?;
    let scoped = events
        .iter()
        .filter(|event| event["stream_trace_id"] == id)
        .collect::<Vec<_>>();
    let opened = scoped
        .iter()
        .filter(|event| event["kind"] == "substream_opened")
        .copied()
        .collect::<Vec<_>>();
    let frames = scoped
        .iter()
        .filter(|event| event["kind"] == "negotiation_frame")
        .copied()
        .collect::<Vec<_>>();
    if scoped.iter().any(|event| event["phase"] != "application")
        || opened.len() != 1
        || opened[0]["detail"]["direction"] != stream["direction"]
        || frames.len() != 4
        || stream["negotiation_complete_frames"] != true
        || stream["proposed_protocol"] != stream["protocol"]
        || !stream["upgrade_completed_sequence"].is_null()
        || scoped
            .iter()
            .any(|event| event["kind"] == "upgrade_completed")
        || frames.iter().any(|event| {
            event["sequence"].as_u64().is_none_or(|sequence| {
                opened[0]["sequence"]
                    .as_u64()
                    .is_none_or(|open| sequence <= open)
            })
        })
    {
        return Err(invalid(
            "Identify lacks exact retained native negotiation ownership",
        ));
    }
    let mut terminal_frames = Vec::new();
    for direction in [proposal_direction, ack_direction] {
        let wire = frames
            .iter()
            .filter(|event| event["direction"] == direction)
            .copied()
            .collect::<Vec<_>>();
        if wire.len() != 2
            || wire[0]["frame_hex"] != "132f6d756c746973747265616d2f312e302e300a"
            || wire[1]["frame_hex"] != protocol_frame
        {
            return Err(invalid(
                "Identify multistream header, proposal or ACK differs",
            ));
        }
        terminal_frames.push(wire[1]);
    }
    // Captures in opposite wire directions may interleave. Selection belongs
    // to the last captured token of the pair, not necessarily the ACK.
    let selected = terminal_frames
        .iter()
        .max_by_key(|event| event["sequence"].as_u64())
        .unwrap();
    for frame in frames {
        if frame["sequence"] == selected["sequence"] {
            if frame["selection_outcome"] != "selected" || frame["protocol"] != stream["protocol"] {
                return Err(invalid(
                    "Identify lacks its actual matching native proposal/ACK selection",
                ));
            }
        } else if !frame["selection_outcome"].is_null() || !frame["protocol"].is_null() {
            return Err(invalid(
                "Identify selection annotation contradicts captured native frames",
            ));
        }
    }
    Ok(())
}

fn verify_cleanup_tail(
    raw: &Value,
    proof: &Value,
    stop: &Value,
    actor_events: &[Value],
) -> io::Result<()> {
    let before = &proof["raw_upgrade_observations"];
    let connections = raw["connections"]
        .as_array()
        .ok_or_else(|| invalid("missing final native owner"))?;
    let [connection] = connections.as_slice() else {
        return Err(invalid("ambiguous final native owner"));
    };
    let previous = &before["connections"][0];
    let prefix = previous["events"]
        .as_array()
        .ok_or_else(|| invalid("missing proof prefix"))?;
    let events = connection["events"]
        .as_array()
        .ok_or_else(|| invalid("missing final native events"))?;
    if raw["overflow"] != false
        || connection["overflow"] != false
        || events.len() < prefix.len()
        || events[..prefix.len()] != prefix[..]
        || events
            .iter()
            .enumerate()
            .any(|(index, event)| event["sequence"] != index + 1)
        || [
            "connection_trace_id",
            "authenticated_local_peer_id",
            "authenticated_remote_peer_id",
            "local_address",
            "remote_address",
            "endpoint",
            "transport_output_receipts",
        ]
        .iter()
        .any(|key| connection[*key] != previous[*key])
        || raw["swarm_events"] != before["swarm_events"]
    {
        return Err(invalid(
            "final cleanup lost the immutable completed native prefix",
        ));
    }
    let closes = actor_events
        .iter()
        .filter(|event| event["kind"] == "native_connection_closed")
        .collect::<Vec<_>>();
    let [close] = closes.as_slice() else {
        return Err(invalid("cleanup lacks a unique native close"));
    };
    let stop_cursor = stop["raw_event_sequence"]
        .as_u64()
        .ok_or_else(|| invalid("missing Stop cursor"))?;
    if close["native_connection_id"] != proof["native_connection_id"]
        || close["peer_id"] != previous["authenticated_remote_peer_id"]
        || close["remaining_established"] != 0
        || close["sequence"] != stop["native_close_sequence"]
        || stop_cursor < prefix.len() as u64
        || stop_cursor > events.len() as u64
        || close["raw_event_sequence"] != events.len()
    {
        return Err(invalid("native close or Stop cursor lost its exact owner"));
    }
    let closed = close["cause"]["kind"] == "yamux013_closed";
    if !closed && close["cause"]["kind"] != "none" {
        return Err(invalid("unrelated native close cause"));
    }
    let streams = connection["streams"]
        .as_array()
        .ok_or_else(|| invalid("missing final native streams"))?;
    let mut ids = BTreeSet::new();
    for stream in streams {
        let id = stream["stream_trace_id"]
            .as_u64()
            .ok_or_else(|| invalid("missing native stream ID"))?;
        if !ids.insert(id) || stream["io_failed"] != false || !stream["parser_error"].is_null() {
            return Err(invalid("failed or ambiguous final native stream"));
        }
        if stream["protocol"] != application_observer::ECHO {
            if stream["protocol"] != "/ipfs/id/1.0.0" && stream["protocol"] != "/ipfs/id/push/1.0.0"
            {
                return Err(invalid("unexpected native protocol during cleanup"));
            }
            verify_identify_negotiation(stream, events)?;
        }
    }
    for old in previous["streams"]
        .as_array()
        .ok_or_else(|| invalid("missing completed native streams"))?
    {
        let next = streams
            .iter()
            .find(|s| s["stream_trace_id"] == old["stream_trace_id"])
            .ok_or_else(|| invalid("completed native stream disappeared"))?;
        if old["protocol"] == application_observer::ECHO {
            if next != old {
                return Err(invalid("completed Echo changed during cleanup"));
            }
        } else if ["protocol", "direction", "upgrade_completed_sequence"]
            .iter()
            .any(|key| next[*key] != old[*key])
        {
            return Err(invalid("retained Identify stream changed identity"));
        }
    }
    let mut terminal = None;
    for event in &events[prefix.len()..] {
        if event["kind"] == "muxer_poll_error"
            && event["phase"] == "muxer"
            && event["stream_trace_id"].is_null()
            && event["detail"]["failure_stage"] == "post_upgrade"
            && event["detail"]["error"]
                .as_str()
                .is_some_and(|value| !value.is_empty())
        {
            if terminal.is_some()
                || !closed
                || event != events.last().unwrap()
                || close["raw_terminal_event"] != *event
            {
                return Err(invalid("unbound native terminal error during cleanup"));
            }
            terminal = Some(event);
        } else {
            let stream = streams
                .iter()
                .find(|s| s["stream_trace_id"] == event["stream_trace_id"])
                .ok_or_else(|| invalid("unrelated event after completed native proof"))?;
            if event["phase"] != "application"
                || (stream["protocol"] != "/ipfs/id/1.0.0"
                    && stream["protocol"] != "/ipfs/id/push/1.0.0")
                || !matches!(
                    event["kind"].as_str(),
                    Some("substream_opened" | "negotiation_frame" | "response_completion")
                )
                || !event["detail"]["error"].is_null()
            {
                return Err(invalid(
                    "unrelated native event or I/O failure during cleanup",
                ));
            }
        }
    }
    if closed != terminal.is_some() {
        return Err(invalid(
            "typed native close lacks its exact raw terminal cause",
        ));
    }
    let phases = connection["negotiations"]
        .as_array()
        .ok_or_else(|| invalid("missing final native negotiations"))?;
    if phases.len() != 2
        || phases[0]["io_failed"] != false
        || phases[1]["io_failed"] != closed
        || phases.iter().any(|phase| !phase["parser_error"].is_null())
        || connection["streams"].as_array().is_none_or(|streams| {
            streams
                .iter()
                .any(|stream| stream["io_failed"] != false || !stream["parser_error"].is_null())
        })
    {
        return Err(invalid(
            "final native cleanup flags do not match its retained raw cause",
        ));
    }
    for (index, phase) in phases.iter().enumerate() {
        let mut phase = phase.clone();
        // Compare immutable successful upgrade facts, not the sticky terminal
        // flag or natural drop/close facts. The original raw flag is untouched.
        let mut old = previous["negotiations"][index].clone();
        for key in [
            "io_failed",
            "drop_observed",
            "read_eof",
            "write_close_returned",
        ] {
            phase
                .as_object_mut()
                .ok_or_else(|| invalid("invalid native phase"))?
                .remove(key);
            old.as_object_mut()
                .ok_or_else(|| invalid("invalid completed native phase"))?
                .remove(key);
        }
        if phase != old {
            return Err(invalid("native negotiation changed after completed proof"));
        }
    }
    if connection["muxer_drop_observed"] != true {
        return Err(invalid("native muxer owner did not retire"));
    }
    Ok(())
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
    let mut completed_proof: Option<Value> = None;
    let mut cleanup_stop: Option<Value> = None;
    let mut cleanup_deadline: Option<Instant> = None;
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
                    SwarmEvent::ConnectionClosed { peer_id, connection_id, cause, num_established, .. } => {
                        let observed = unix_ns(SystemTime::now())?;
                        let current = observer.snapshot();
                        let cursor = current["connections"][0]["events"].as_array().map(Vec::len);
                        let cause = native_close_cause(cause);
                        capture.event("native_connection_closed", json!({"native_connection_id": connection_id.to_string(),
                            "peer_id": peer_id.to_string(), "remaining_established": num_established, "observed_unix_ns": observed,
                            "cause": cause, "raw_event_sequence": cursor,
                            "raw_terminal_event": current["connections"][0]["events"].as_array().and_then(|events| events.last())}))?;
                        let close_sequence = capture.events.len();
                        let mut stop = match cleanup_stop.take() {
                            Some(stop) => stop,
                            None => {
                                let mut stop = observe_own_stop(&config.stop, completed_proof.as_ref(), socket.as_ref(),
                                    Some(peer_id), Some(connection_id), &current, Some(observed))?
                                    .ok_or_else(|| invalid("retained native connection closed before own Stop"))?;
                                stop["native_close_sequence"] = json!(close_sequence);
                                stop["stop_observed_sequence"] = json!(capture.events.len() + 1);
                                capture.event("own_stop_observed", stop.clone())?;
                                stop
                            }
                        };
                        if stage != "exchanged" || retained != Some(connection_id) || num_established != 0
                            || plan.as_ref().is_none_or(|target| target.peer != peer_id)
                            || (cause["kind"] != "none" && cause["kind"] != "yamux013_closed") {
                            return Err(invalid("controlled native close lost its exact completed owner"));
                        }
                        stop["native_close_sequence"] = json!(close_sequence);
                        cleanup_stop = Some(stop);
                        break;
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
                    if cleanup_deadline.is_some_and(|deadline| Instant::now() >= deadline) {
                        return Err(io::Error::new(io::ErrorKind::TimedOut, "native close cleanup deadline"));
                    }
                    if capture.started.elapsed() >= RUN_BUDGET { return Err(io::Error::new(io::ErrorKind::TimedOut, "coordinated actor deadline")); }
                    if stage != "exchanged" && operation_deadline.is_some_and(|deadline| Instant::now() >= deadline) {
                        return Err(io::Error::new(io::ErrorKind::TimedOut, "coordinated operation deadline"));
                    }
                    if cleanup_stop.is_none() {
                        let current = observer.snapshot();
                        if let Some(mut stop) = observe_own_stop(&config.stop, completed_proof.as_ref(), socket.as_ref(),
                            plan.as_ref().map(|target| target.peer), retained, &current, None)? {
                            if stage != "exchanged" { return Err(invalid("stop is cleanup only; coordinated proof is incomplete")); }
                            stop["native_close_sequence"] = Value::Null;
                            stop["stop_observed_sequence"] = json!(capture.events.len() + 1);
                            capture.event("own_stop_observed", stop.clone())?;
                            cleanup_stop = Some(stop);
                            cleanup_deadline = Some(Instant::now() + JOIN_BUDGET);
                            // Poll the existing native owner to its actual close event;
                            // a queued peer close can win this ordinary disconnect race.
                            let _ = swarm.disconnect_peer_id(plan.as_ref().unwrap().peer);
                        }
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
                            let raw = observer.snapshot();
                            let snapshot = complete_application_snapshot(raw, socket, application, config.role,
                                capture.events.len() + 1, local, plan.as_ref().unwrap(), listener.as_ref().unwrap(), retained.unwrap())?;
                            let proof = snapshot["application"].clone();
                            if probe_dials_before != Some(audit.count()) || audit.count() != 1
                                || swarm.network_info().connection_counters().num_connections() != 1 || !swarm.is_connected(&plan.as_ref().unwrap().peer) { return Err(invalid("probe lost its retained connection")); }
                            capture.event("application_completed", json!({"native_connection_id": retained.map(|id| id.to_string()), "native_transport_dials_before": probe_dials_before, "native_transport_dials_after": audit.count()}))?;
                            completed_proof = Some(snapshot);
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
                        "application": stream_proof, "completed_proof": completed_proof,
                        "native_transport_dials": audit.snapshot(), "raw_upgrade_observations": observer.snapshot()}))?;
                }
            }
        }
        Ok(())
    }.await;
    drop(incoming);
    drop(control);
    drop(swarm);
    let join_budget = cleanup_deadline
        .map(|deadline| deadline.saturating_duration_since(Instant::now()))
        .unwrap_or(JOIN_BUDGET)
        .min(TOTAL_BUDGET.saturating_sub(capture.started.elapsed()));
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
        if let (Some(proof), Some(stop)) = (&completed_proof, &cleanup_stop) {
            if let Err(failure) = verify_cleanup_tail(&raw, proof, stop, &capture.events) {
                error = Some(failure.to_string());
            }
        } else {
            error = Some("cleanup lacks its own Stop and completed native proof".into());
        }
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
    }
    atomic(
        &config.result,
        &json!({"schema": "forge.p2p.evidence.coordinated.v1", "implementation": "rust",
        "scenario": if config.private { PRIVATE } else { NATIVE }, "transport": if config.private { "tcp-pnet-noise" } else { "tcp" },
        "case_token": config.token, "coord_role": config.role.name(), "peer_id": local.to_string(), "timeout_ms": config.operation_budget.as_millis(),
        "pnet_fingerprint": pnet_fingerprint, "pnet_fingerprint_basis": pnet_fingerprint.as_ref().map(|_| PNET_FINGERPRINT_BASIS),
        "status": if error.is_none() { "ok" } else { "error" }, "finalized": true, "joined": joined, "error": error,
        "events": capture.events, "socket": socket, "application": stream_proof, "completed_proof": completed_proof,
        "cleanup_stop": cleanup_stop, "native_transport_dials": audit.snapshot(),
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

    struct StopFile(PathBuf);
    impl StopFile {
        fn new() -> Self {
            let directory =
                std::env::temp_dir().join(format!("forge-coordinated-stop-{}", PeerId::random()));
            std::fs::create_dir(&directory).unwrap();
            Self(directory.join("own.stop"))
        }
        fn publish(&self) {
            std::fs::write(&self.0, b"stop\n").unwrap();
        }
    }
    impl Drop for StopFile {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(self.0.parent().unwrap());
        }
    }

    fn identify_negotiation_rows(
        id: u64,
        direction: &str,
        protocol: &str,
        sequences: [u64; 5],
        reverse_capture: bool,
    ) -> (Value, Vec<Value>) {
        let header = "132f6d756c746973747265616d2f312e302e300a";
        let token = match protocol {
            "/ipfs/id/1.0.0" => "0f2f697066732f69642f312e302e300a",
            "/ipfs/id/push/1.0.0" => "142f697066732f69642f707573682f312e302e300a",
            _ => panic!("unsupported test protocol"),
        };
        let (proposal, ack) = if direction == "outbound" {
            ("write", "read")
        } else {
            ("read", "write")
        };
        let wire = if reverse_capture {
            [
                (ack, header),
                (ack, token),
                (proposal, header),
                (proposal, token),
            ]
        } else if direction == "outbound" {
            [
                (proposal, header),
                (proposal, token),
                (ack, header),
                (ack, token),
            ]
        } else {
            [
                (proposal, header),
                (ack, header),
                (proposal, token),
                (ack, token),
            ]
        };
        let stream = json!({"protocol": protocol, "proposed_protocol": protocol,
            "direction": direction, "stream_trace_id": id, "io_failed": false, "parser_error": null,
            "upgrade_completed_sequence": null, "negotiation_complete_frames": true});
        let mut events = vec![json!({"sequence": sequences[0], "phase": "application",
            "stream_trace_id": id, "kind": "substream_opened", "detail": {"direction": direction}})];
        for (index, (side, frame)) in wire.into_iter().enumerate() {
            let mut event = json!({"sequence": sequences[index + 1], "phase": "application",
                "stream_trace_id": id, "kind": "negotiation_frame", "direction": side, "frame_hex": frame});
            if index == 3 {
                event["selection_outcome"] = json!("selected");
                event["protocol"] = json!(protocol);
            }
            events.push(event);
        }
        (stream, events)
    }

    fn completed_cleanup_owner() -> (Value, Value, Value, Plan, ConnectionId) {
        let (mut raw, local, plan, listener, id) = unit_socket(Role::Responder);
        let socket = completed_socket(&raw, local, &plan, &listener, id, Role::Responder)
            .unwrap()
            .unwrap();
        let body = body_receipt(&challenge("0123456789abcdef0123456789abcdef"));
        raw["connections"][0]["overflow"] = json!(false);
        raw["connections"][0]["muxer_drop_observed"] = json!(false);
        raw["connections"][0]["streams"] = json!([{"protocol": application_observer::ECHO,
            "direction": "inbound", "stream_trace_id": 4, "io_failed": false, "parser_error": null,
            "write_close_returned": true, "drop_observed": true, "read": body, "write": body}]);
        raw["connections"][0]["events"] = json!((1..=37).map(|index| json!({"sequence": index,
            "phase": "application", "stream_trace_id": 4, "kind": "response_completion", "detail": {}})).collect::<Vec<_>>());
        // Real 4cc retained Identify/Push layouts have null application upgrade
        // markers and interleaved proposal/ACK captures inside the same owner.
        for (id, direction, protocol, sequences) in [
            (1, "outbound", "/ipfs/id/1.0.0", [13, 14, 15, 22, 26]),
            (2, "inbound", "/ipfs/id/1.0.0", [19, 20, 21, 23, 24]),
            (3, "inbound", "/ipfs/id/push/1.0.0", [27, 28, 29, 30, 31]),
        ] {
            let (stream, events) =
                identify_negotiation_rows(id, direction, protocol, sequences, false);
            raw["connections"][0]["streams"]
                .as_array_mut()
                .unwrap()
                .push(stream);
            for event in events {
                let index = event["sequence"].as_u64().unwrap() as usize - 1;
                raw["connections"][0]["events"][index] = event;
            }
        }
        let proof = complete_application_snapshot(
            raw.clone(),
            &socket,
            &json!({"read": body, "write": body}),
            Role::Responder,
            1,
            local,
            &plan,
            &listener,
            id,
        )
        .unwrap();
        (raw, proof, socket, plan, id)
    }

    fn append_native_closed(raw: &mut Value) {
        let events = raw["connections"][0]["events"].as_array_mut().unwrap();
        events.push(json!({"sequence": events.len() + 1, "phase": "muxer", "stream_trace_id": null,
            "kind": "muxer_poll_error", "detail": {"error": "connection is closed", "failure_stage": "post_upgrade"}}));
        raw["connections"][0]["negotiations"][1]["io_failed"] = json!(true);
        raw["connections"][0]["muxer_drop_observed"] = json!(true);
    }

    #[test]
    fn cleanup_close_requires_native_typed_closed_not_display_or_io_timeout() {
        let closed = native_close_cause(Some(ConnectionError::IO(io::Error::other(
            yamux013::ConnectionError::Closed,
        ))));
        assert_eq!(closed["kind"], "yamux013_closed");
        assert!(closed["raw"]["display"].as_str().is_some());
        assert!(closed["raw"]["debug"].as_str().is_some());
        for error in [
            io::Error::other("connection is closed"),
            io::Error::from(io::ErrorKind::TimedOut),
            io::Error::other(yamux013::ConnectionError::Io(io::Error::from(
                io::ErrorKind::BrokenPipe,
            ))),
        ] {
            assert_eq!(
                native_close_cause(Some(ConnectionError::IO(error)))["kind"],
                "other"
            );
        }
        assert_eq!(
            native_close_cause(Some(ConnectionError::KeepAliveTimeout))["kind"],
            "other"
        );
        assert_eq!(native_close_cause(None)["kind"], "none");
    }

    #[tokio::test]
    async fn cleanup_closed_downcasts_actual_boxed_native_yamux_error() {
        use libp2p::core::{muxing::StreamMuxer, upgrade::OutboundConnectionUpgrade};
        let native = libp2p::yamux::Config::default()
            .upgrade_outbound(futures::io::Cursor::new(Vec::<u8>::new()), "/yamux/1.0.0")
            .await
            .unwrap();
        let mut boxed = StreamMuxerBox::new(native);
        let error = tokio::time::timeout(
            Duration::from_secs(1),
            futures::future::poll_fn(|cx| Pin::new(&mut boxed).poll(cx)),
        )
        .await
        .unwrap()
        .unwrap_err();
        let receipt = native_close_cause(Some(ConnectionError::IO(error)));
        assert_eq!(receipt["kind"], "yamux013_closed");
        assert!(receipt["raw"]["display"].as_str().is_some());
        assert!(receipt["raw"]["debug"].as_str().is_some());
    }

    #[test]
    fn own_stop_reads_real_marker_and_current_cursor_not_frozen_echo_boundary() {
        let stop = StopFile::new();
        let (mut raw, proof, socket, plan, id) = completed_cleanup_owner();
        assert!(
            observe_own_stop(
                &stop.0,
                Some(&proof),
                Some(&socket),
                Some(plan.peer),
                Some(id),
                &raw,
                None
            )
            .unwrap()
            .is_none()
        );
        stop.publish();
        let before = observe_own_stop(
            &stop.0,
            Some(&proof),
            Some(&socket),
            Some(plan.peer),
            Some(id),
            &raw,
            None,
        )
        .unwrap()
        .unwrap();
        assert_eq!(before["raw_event_sequence"], 37);
        assert_eq!(
            before["authorization_basis"],
            "completed_retained_owner_and_actual_stop_read"
        );
        assert_eq!(
            before["native_error_ordering"],
            "not_inferred_from_stop_or_filesystem_time"
        );
        append_native_closed(&mut raw);
        let time = unix_ns(SystemTime::now()).unwrap();
        let after = observe_own_stop(
            &stop.0,
            Some(&proof),
            Some(&socket),
            Some(plan.peer),
            Some(id),
            &raw,
            Some(time),
        )
        .unwrap()
        .unwrap();
        assert_eq!(after["raw_event_sequence"], 38);
        assert_eq!(proof["raw_event_sequence"], 37);
        assert_eq!(
            after["modified_unix_ns"],
            unix_ns(std::fs::metadata(&stop.0).unwrap().modified().unwrap()).unwrap()
        );
        assert!(
            observe_own_stop(
                &stop.0,
                None,
                Some(&socket),
                Some(plan.peer),
                Some(id),
                &raw,
                None
            )
            .is_err()
        );
        assert!(
            observe_own_stop(
                &stop.0,
                Some(&proof),
                Some(&socket),
                Some(PeerId::random()),
                Some(id),
                &raw,
                None
            )
            .is_err()
        );
        assert!(
            observe_own_stop(
                &stop.0,
                Some(&proof),
                Some(&socket),
                Some(plan.peer),
                Some(ConnectionId::new_unchecked(51)),
                &raw,
                None
            )
            .is_err()
        );
        std::fs::write(&stop.0, b"STOP\n").unwrap();
        assert!(
            observe_own_stop(
                &stop.0,
                Some(&proof),
                Some(&socket),
                Some(plan.peer),
                Some(id),
                &raw,
                None
            )
            .is_err()
        );
    }

    #[test]
    fn own_stop_observed_before_completed_owner_remains_fatal_without_clock_authority() {
        let stop = StopFile::new();
        stop.publish();
        let (raw, _proof, socket, plan, id) = completed_cleanup_owner();
        assert!(
            observe_own_stop(
                &stop.0,
                None,
                Some(&socket),
                Some(plan.peer),
                Some(id),
                &raw,
                None
            )
            .is_err()
        );
        assert_eq!(std::fs::read(&stop.0).unwrap(), b"stop\n");
    }

    #[test]
    fn identify_negotiation_binds_four_real_frames_with_null_upgrade_marker() {
        for protocol in ["/ipfs/id/1.0.0", "/ipfs/id/push/1.0.0"] {
            for direction in ["inbound", "outbound"] {
                for reverse_capture in [false, true] {
                    let (stream, events) = identify_negotiation_rows(
                        2,
                        direction,
                        protocol,
                        [38, 39, 40, 41, 42],
                        reverse_capture,
                    );
                    assert!(stream["upgrade_completed_sequence"].is_null());
                    assert!(verify_identify_negotiation(&stream, &events).is_ok());
                    for change in 0..10 {
                        let mut bad = events.clone();
                        match change {
                            0 => {
                                bad.pop();
                            }
                            1 => bad[4]["stream_trace_id"] = json!(99),
                            2 => bad[4]["phase"] = json!("muxer"),
                            3 => bad[4]["frame_hex"] = json!("036e610a"),
                            4 => bad[4]["direction"] = bad[1]["direction"].clone(),
                            5 => bad[4]["selection_outcome"] = Value::Null,
                            6 => bad[4]["protocol"] = json!("/foreign/1"),
                            7 => bad[1]["selection_outcome"] = json!("selected"),
                            8 => {
                                bad[2]["selection_outcome"] = json!("selected");
                                bad[2]["protocol"] = json!(protocol);
                                bad[4]["selection_outcome"] = Value::Null;
                                bad[4]["protocol"] = Value::Null;
                            }
                            _ => bad.push(
                                json!({"sequence": 43, "phase": "application", "stream_trace_id": 2,
                                "kind": "upgrade_completed", "detail": {"protocol": protocol}}),
                            ),
                        }
                        assert!(
                            verify_identify_negotiation(&stream, &bad).is_err(),
                            "{protocol} {direction} reverse={reverse_capture} change={change}"
                        );
                    }
                    let mut bad_stream = stream.clone();
                    bad_stream["upgrade_completed_sequence"] = json!(43);
                    assert!(verify_identify_negotiation(&bad_stream, &events).is_err());
                }
            }
        }
    }

    #[test]
    fn cleanup_tail_binds_typed_close_and_retains_bounded_late_identify() {
        for (late_identify, reverse_capture) in [(false, false), (true, false), (true, true)] {
            let stop_file = StopFile::new();
            let (mut raw, proof, socket, plan, id) = completed_cleanup_owner();
            assert_eq!(proof["raw_event_sequence"], 37);
            assert!(
                proof["raw_upgrade_observations"]["connections"][0]["streams"]
                    .as_array()
                    .unwrap()
                    .iter()
                    .filter(|stream| stream["protocol"] != application_observer::ECHO)
                    .all(|stream| stream["upgrade_completed_sequence"].is_null())
            );
            if late_identify {
                let (stream, events) = identify_negotiation_rows(
                    5,
                    "outbound",
                    "/ipfs/id/1.0.0",
                    [38, 39, 40, 41, 42],
                    reverse_capture,
                );
                raw["connections"][0]["streams"]
                    .as_array_mut()
                    .unwrap()
                    .push(stream);
                raw["connections"][0]["events"]
                    .as_array_mut()
                    .unwrap()
                    .extend(events);
            }
            stop_file.publish();
            let mut first_stop = observe_own_stop(
                &stop_file.0,
                Some(&proof),
                Some(&socket),
                Some(plan.peer),
                Some(id),
                &raw,
                None,
            )
            .unwrap()
            .unwrap();
            append_native_closed(&mut raw);
            let mut stop = observe_own_stop(
                &stop_file.0,
                Some(&proof),
                Some(&socket),
                Some(plan.peer),
                Some(id),
                &raw,
                Some(unix_ns(SystemTime::now()).unwrap()),
            )
            .unwrap()
            .unwrap();
            stop["native_close_sequence"] = json!(2);
            let close = json!({"sequence": 2, "kind": "native_connection_closed", "native_connection_id": id.to_string(),
                "peer_id": plan.peer.to_string(), "remaining_established": 0, "raw_event_sequence": raw["connections"][0]["events"].as_array().unwrap().len(),
                "raw_terminal_event": raw["connections"][0]["events"].as_array().unwrap().last(),
                "cause": native_close_cause(Some(ConnectionError::IO(io::Error::other(yamux013::ConnectionError::Closed))))});
            assert!(verify_cleanup_tail(&raw, &proof, &stop, &[close.clone()]).is_ok());
            first_stop["native_close_sequence"] = json!(2);
            assert!(verify_cleanup_tail(&raw, &proof, &first_stop, &[close.clone()]).is_ok());
            assert_eq!(raw["connections"][0]["negotiations"][1]["io_failed"], true);
            for change in 0..6 {
                let mut bad_raw = raw.clone();
                let mut bad_close = close.clone();
                match change {
                    0 => bad_close["cause"]["kind"] = json!("other"),
                    1 => bad_close["raw_terminal_event"]["sequence"] = json!(37),
                    2 => bad_close["native_connection_id"] = json!("foreign"),
                    3 => bad_raw["connections"][0]["negotiations"][1]["io_failed"] = json!(false),
                    4 => bad_raw["connections"][0]["streams"][0]["io_failed"] = json!(true),
                    _ => {
                        bad_raw["connections"][0]["events"][0]["detail"]["error"] =
                            json!("earlier error")
                    }
                }
                assert!(
                    verify_cleanup_tail(&bad_raw, &proof, &stop, &[bad_close]).is_err(),
                    "change {change}"
                );
            }
        }
    }
}
