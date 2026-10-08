//! PR11 control actor: the immutable donor Behaviour owns routing and scoring.
use futures::StreamExt;
use libp2p::{
    Multiaddr, PeerId, Swarm,
    gossipsub::{self, MessageAcceptance},
    identity,
    multiaddr::Protocol,
    swarm::{SwarmEvent, dial_opts::DialOpts},
};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeMap, BTreeSet, HashMap},
    error::Error,
    fs::{self, File},
    io::{self, Read},
    path::{Path, PathBuf},
    time::{Duration, Instant},
};

#[path = "pubsub_scoring/observer.rs"]
mod observer;
#[path = "pubsub_scoring/transport.rs"]
mod transport;
use observer::{Evidence, hex};
use transport::new_swarm;

const COMMAND_LIMIT: usize = 64;
const LINE_LIMIT: usize = 16 * 1024;
const PAYLOAD_LIMIT: usize = 1024;
const EVENT_LIMIT: usize = 2048;
const CONNECTION_LIMIT: usize = 16;
const STREAM_LIMIT: usize = 64;
const FRAME_LIMIT: usize = 16 * 1024;
const EVENT_BYTE_LIMIT: usize = 64 * 1024;
const TRACE_BYTE_LIMIT: usize = 16 * 1024 * 1024;
const BEHAVIOUR_SOURCE: &str = "rust.libp2p.gossipsub.public-Behaviour";

fn invalid(message: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message.into())
}

fn lower_hex(value: &str, length: usize) -> bool {
    value.len() == length
        && value
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

#[derive(Clone, Debug)]
pub(crate) struct Config {
    version: String,
    transport: String,
    actor: String,
    token: String,
    ready: PathBuf,
    control: PathBuf,
    result: PathBuf,
    stop: PathBuf,
    store: PathBuf,
    key_file: Option<PathBuf>,
    fingerprint: Option<String>,
}

impl Config {
    fn protocol(&self) -> &'static str {
        if self.version == "1.0" {
            "/meshsub/1.0.0"
        } else {
            "/meshsub/1.1.0"
        }
    }

    fn topic(&self) -> gossipsub::IdentTopic {
        gossipsub::IdentTopic::new(format!("forge-pr11:{}", self.token))
    }
}

pub(crate) fn parse_args(argv: &[String]) -> io::Result<Option<Config>> {
    if argv.first().map(String::as_str) != Some("pubsub-live") {
        return Ok(None);
    }
    const REQUIRED: [&str; 9] = [
        "version",
        "transport",
        "actor",
        "case-token",
        "ready-file",
        "control-file",
        "result-file",
        "stop-file",
        "store-dir",
    ];
    if (argv.len() - 1) % 2 != 0 {
        return Err(invalid("pubsub-live requires flag/value pairs"));
    }
    let mut args = BTreeMap::new();
    for pair in argv[1..].chunks_exact(2) {
        let name = pair[0]
            .strip_prefix("--")
            .ok_or_else(|| invalid("expected named flag"))?;
        if (!REQUIRED.contains(&name) && !matches!(name, "pnet-key-file" | "pnet-fingerprint"))
            || args.insert(name.to_owned(), pair[1].clone()).is_some()
            || pair[1].is_empty()
            || pair[1].starts_with("--")
        {
            return Err(invalid("unknown/duplicate flag or empty value"));
        }
    }
    if REQUIRED.iter().any(|name| !args.contains_key(*name)) {
        return Err(invalid("missing required pubsub-live flag"));
    }
    if !matches!(args["version"].as_str(), "1.0" | "1.1")
        || !matches!(
            args["transport"].as_str(),
            "quic" | "tcp" | "tcp-pnet-noise"
        )
        || !matches!(
            args["actor"].as_str(),
            "victim" | "offender" | "replacement" | "sink"
        )
        || !lower_hex(&args["case-token"], 32)
    {
        return Err(invalid(
            "invalid pubsub version, transport, actor or case token",
        ));
    }
    let private = args["transport"] == "tcp-pnet-noise";
    if private {
        if !args.contains_key("pnet-key-file")
            || !args
                .get("pnet-fingerprint")
                .is_some_and(|s| lower_hex(s, 64))
        {
            return Err(invalid(
                "private actor requires key file and operational fingerprint",
            ));
        }
    } else if args.contains_key("pnet-key-file") || args.contains_key("pnet-fingerprint") {
        return Err(invalid("public actor must not receive private-key flags"));
    }
    let paths = [
        "ready-file",
        "control-file",
        "result-file",
        "stop-file",
        "store-dir",
    ];
    if paths
        .iter()
        .map(|name| PathBuf::from(&args[*name]))
        .collect::<BTreeSet<_>>()
        .len()
        != paths.len()
    {
        return Err(invalid("actor files and store directory must be distinct"));
    }
    Ok(Some(Config {
        version: args["version"].clone(),
        transport: args["transport"].clone(),
        actor: args["actor"].clone(),
        token: args["case-token"].clone(),
        ready: args["ready-file"].clone().into(),
        control: args["control-file"].clone().into(),
        result: args["result-file"].clone().into(),
        stop: args["stop-file"].clone().into(),
        store: args["store-dir"].clone().into(),
        key_file: args.get("pnet-key-file").map(PathBuf::from),
        fingerprint: args.get("pnet-fingerprint").cloned(),
    }))
}

fn signed_message_id(message: &gossipsub::Message) -> gossipsub::MessageId {
    // Strict signed messages carry both fields; use their wire bytes, not text.
    let mut bytes = message
        .source
        .map(|peer| peer.to_bytes())
        .unwrap_or_default();
    if let Some(sequence) = message.sequence_number {
        bytes.extend_from_slice(&sequence.to_be_bytes());
    }
    gossipsub::MessageId::from(bytes)
}

fn router_config(config: &Config) -> io::Result<gossipsub::Config> {
    gossipsub::ConfigBuilder::default()
        .protocol_id(
            config.protocol(),
            if config.version == "1.0" {
                gossipsub::Version::V1_0
            } else {
                gossipsub::Version::V1_1
            },
        )
        .validation_mode(gossipsub::ValidationMode::Strict)
        .validate_messages()
        .message_id_fn(signed_message_id)
        .mesh_n(2)
        .mesh_n_low(1)
        .mesh_n_high(4)
        .retain_scores(1)
        .mesh_outbound_min(0)
        .heartbeat_initial_delay(Duration::from_millis(250))
        .heartbeat_interval(Duration::from_millis(250))
        .flood_publish(false)
        .prune_backoff(Duration::from_secs(1))
        .max_transmit_size(FRAME_LIMIT)
        // do_px defaults to false; its builder method only enables it.
        .build()
        .map_err(|e| invalid(e.to_string()))
}

fn score_params(topic: gossipsub::TopicHash) -> gossipsub::PeerScoreParams {
    let topic_params = gossipsub::TopicScoreParams {
        topic_weight: 1.0,
        time_in_mesh_weight: 0.0,
        first_message_deliveries_weight: 0.0,
        mesh_message_deliveries_weight: 0.0,
        mesh_failure_penalty_weight: 0.0,
        invalid_message_deliveries_weight: -100.0,
        invalid_message_deliveries_decay: 0.99,
        ..Default::default()
    };
    gossipsub::PeerScoreParams {
        topics: HashMap::from([(topic, topic_params)]),
        topic_score_cap: 0.0,
        app_specific_weight: 0.0,
        ip_colocation_factor_weight: 0.0,
        behaviour_penalty_weight: 0.0,
        slow_peer_weight: 0.0,
        decay_interval: Duration::from_secs(1),
        retain_score: Duration::from_secs(60),
        ..Default::default()
    }
}

fn behaviour(key: &identity::Keypair, config: &Config) -> io::Result<gossipsub::Behaviour> {
    let mut router = gossipsub::Behaviour::new(
        gossipsub::MessageAuthenticity::Signed(key.clone()),
        router_config(config)?,
    )
    .map_err(|e| invalid(e.to_string()))?;
    router
        .with_peer_score(
            score_params(config.topic().hash()),
            gossipsub::PeerScoreThresholds {
                gossip_threshold: -10.0,
                publish_threshold: -50.0,
                graylist_threshold: -80.0,
                accept_px_threshold: 10.0,
                opportunistic_graft_threshold: 20.0,
            },
        )
        .map_err(invalid)?;
    router
        .subscribe(&config.topic())
        .map_err(|e| invalid(e.to_string()))?;
    Ok(router)
}

fn validation(config: &Config, payload: &[u8]) -> MessageAcceptance {
    if config.actor == "victim" {
        if payload.starts_with(format!("reject:{}:", config.token).as_bytes()) {
            return MessageAcceptance::Reject;
        }
        if payload.starts_with(format!("ignore:{}:", config.token).as_bytes()) {
            return MessageAcceptance::Ignore;
        }
    }
    MessageAcceptance::Accept
}

#[derive(Debug)]
enum Command {
    Connect {
        sequence: u64,
        peer: PeerId,
        address: Multiaddr,
    },
    Publish {
        sequence: u64,
        payload: String,
    },
    Sample {
        sequence: u64,
        label: String,
    },
    PrepareShutdown {
        sequence: u64,
        actor: String,
        token: String,
        local: PeerId,
    },
}

impl Command {
    fn sequence(&self) -> u64 {
        match self {
            Self::Connect { sequence, .. }
            | Self::Publish { sequence, .. }
            | Self::Sample { sequence, .. }
            | Self::PrepareShutdown { sequence, .. } => *sequence,
        }
    }
}

#[derive(Default)]
struct Control {
    previous: Vec<u8>,
    parsed: usize,
    sequence: u64,
}

impl Control {
    fn ingest(&mut self, bytes: &[u8]) -> io::Result<Vec<Command>> {
        if bytes.len() > COMMAND_LIMIT * (LINE_LIMIT + 1) || !bytes.starts_with(&self.previous) {
            return Err(invalid(
                "control file overflow, truncation or non-append rewrite",
            ));
        }
        let mut commands = Vec::new();
        while let Some(end) = bytes[self.parsed..].iter().position(|b| *b == b'\n') {
            if end > LINE_LIMIT || self.sequence == COMMAND_LIMIT as u64 {
                return Err(invalid("control command limit exceeded"));
            }
            let row: Value = serde_json::from_slice(&bytes[self.parsed..self.parsed + end])
                .map_err(|_| invalid("invalid control JSON"))?;
            let object = row
                .as_object()
                .ok_or_else(|| invalid("control command is not an object"))?;
            let sequence = row["sequence"]
                .as_u64()
                .filter(|n| *n == self.sequence + 1)
                .ok_or_else(|| invalid("noncontiguous/duplicate command sequence"))?;
            let text = |name: &str| {
                row[name]
                    .as_str()
                    .filter(|s| !s.is_empty())
                    .ok_or_else(|| invalid("missing command field"))
            };
            let (command, fields): (Command, &[&str]) = match text("kind")? {
                "connect" => (
                    Command::Connect {
                        sequence,
                        peer: text("peer_id")?
                            .parse()
                            .map_err(|_| invalid("invalid peer ID"))?,
                        address: text("address")?
                            .parse()
                            .map_err(|_| invalid("invalid peer address"))?,
                    },
                    &["sequence", "kind", "peer_id", "address"],
                ),
                "publish" => {
                    let payload = text("payload")?;
                    if payload.len() > PAYLOAD_LIMIT {
                        return Err(invalid("publication exceeds 1 KiB"));
                    }
                    (
                        Command::Publish {
                            sequence,
                            payload: payload.to_owned(),
                        },
                        &["sequence", "kind", "payload"],
                    )
                }
                "sample" => {
                    let label = text("label")?;
                    if label.len() > 256 {
                        return Err(invalid("sample label too long"));
                    }
                    (
                        Command::Sample {
                            sequence,
                            label: label.to_owned(),
                        },
                        &["sequence", "kind", "label"],
                    )
                }
                "prepare_shutdown" => (
                    Command::PrepareShutdown {
                        sequence,
                        actor: text("actor")?.to_owned(),
                        token: text("case_token")?.to_owned(),
                        local: text("local_peer_id")?
                            .parse()
                            .map_err(|_| invalid("invalid prepare identity"))?,
                    },
                    &["sequence", "kind", "actor", "case_token", "local_peer_id"],
                ),
                _ => return Err(invalid("unknown control command")),
            };
            if object.len() != fields.len()
                || object.keys().any(|key| !fields.contains(&key.as_str()))
            {
                return Err(invalid("unexpected command field"));
            }
            commands.push(command);
            self.sequence = sequence;
            self.parsed += end + 1;
        }
        if bytes.len() - self.parsed > LINE_LIMIT {
            return Err(invalid("unterminated command exceeds line limit"));
        }
        self.previous = bytes.to_vec();
        Ok(commands)
    }

    fn poll(&mut self, path: &Path) -> io::Result<Vec<Command>> {
        let file = match File::open(path) {
            Ok(file) => file,
            Err(e) if e.kind() == io::ErrorKind::NotFound && self.previous.is_empty() => {
                return Ok(Vec::new());
            }
            Err(e) => return Err(e),
        };
        let mut bytes = Vec::new();
        file.take((COMMAND_LIMIT * (LINE_LIMIT + 1) + 1) as u64)
            .read_to_end(&mut bytes)?;
        self.ingest(&bytes)
    }

    fn finish(&self) -> io::Result<()> {
        if self.parsed != self.previous.len() {
            Err(invalid("stop with incomplete control command"))
        } else {
            Ok(())
        }
    }
}

fn replace_json(path: &Path, value: &Value) -> io::Result<()> {
    use std::io::Write;
    if let Some(parent) = path.parent().filter(|p| !p.as_os_str().is_empty()) {
        fs::create_dir_all(parent)?;
    }
    let name = path
        .file_name()
        .ok_or_else(|| invalid("evidence path lacks filename"))?;
    let mut temporary = name.to_os_string();
    temporary.push(".pubsub.tmp");
    let temporary = path.with_file_name(temporary);
    let bytes = serde_json::to_vec(value)?;
    let mut file = File::create(&temporary)?;
    file.write_all(&bytes)?;
    file.sync_all()?;
    drop(file);
    fs::rename(temporary, path)
}

fn result(
    config: &Config,
    local: Option<PeerId>,
    evidence: &Evidence,
    finalized: bool,
    joined: bool,
    task_join: Value,
    upgrades: Value,
) -> Value {
    let capture = evidence.lock();
    json!({"schema_version": 1, "implementation": "rust", "actor": config.actor, "case_token": config.token,
        "local_peer_id": local.map(|p| p.to_string()), "finalized": finalized, "joined": joined,
        "overflow": capture.overflow, "error": capture.error, "events": capture.events,
        "status": if capture.error.is_none() { "ok" } else { "error" },
        "evidence_gaps": ["pinned public Behaviour exposes peer_score, not the native per-topic invalid_deliveries counter"],
        "task_join": task_join, "upgrade_observation": upgrades,
        "native_close": {"live_muxers": capture.live_muxers, "live_streams": capture.live_streams,
            "connections": capture.connections.iter().enumerate().map(|(id, c)| json!({"connection_trace_id": id + 1,
                "connection_id": c.swarm_id, "peer_id": c.peer.to_string(), "close_returned": c.closed, "native_terminal_observed": c.terminal, "dropped": c.dropped})).collect::<Vec<_>>()},
        "join_scope": "fixture control/subscriber/observer are inline; public Swarm executor tasks joined; established native muxers closed and dropped; private transport-runtime tasks not individually joined"})
}

fn snapshot(
    router: &gossipsub::Behaviour,
    config: &Config,
    peers: &BTreeSet<PeerId>,
    label: &str,
    evidence: &Evidence,
) -> io::Result<()> {
    let mut mesh = router
        .mesh_peers(&config.topic().hash())
        .map(ToString::to_string)
        .collect::<Vec<_>>();
    mesh.sort();
    let mut scores = Vec::new();
    for peer in peers {
        if let Some(value) = router.peer_score(peer) {
            if !value.is_finite() {
                return Err(invalid("nonfinite native peer score"));
            }
            // Do not invert -100*x*x or count callbacks to fabricate a native counter.
            scores.push(json!({"peer_id": peer.to_string(), "value": value, "invalid_deliveries": null,
                "invalid_deliveries_available": false, "counter_limitation": "not exposed by pinned public Behaviour"}));
        }
    }
    evidence.emit(
        "snapshot",
        BEHAVIOUR_SOURCE,
        json!({"label": label, "mesh_peer_ids": mesh, "peer_scores": scores,
        "observation_basis": "public_mesh_peers_and_peer_score_at_observation_time"}),
    );
    Ok(())
}

fn command_done(evidence: &Evidence, sequence: u64, kind: &str, status: &str, detail: Value) {
    evidence.emit(
        "command_done",
        "rust.fixture.control-native-operation-completion",
        json!({"command_sequence": sequence,
        "command_kind": kind, "status": status, "native_result": detail}),
    );
}

struct PendingConnect {
    sequence: u64,
    started: Instant,
}

fn command(
    command: Command,
    swarm: &mut Swarm<gossipsub::Behaviour>,
    config: &Config,
    evidence: &Evidence,
    peers: &BTreeSet<PeerId>,
    pending: &mut BTreeMap<PeerId, PendingConnect>,
) -> io::Result<()> {
    evidence.admit()?;
    let sequence = command.sequence();
    match command {
        Command::Connect {
            peer, mut address, ..
        } => {
            if peer == *swarm.local_peer_id() || pending.contains_key(&peer) {
                return Err(invalid("self/duplicate inflight connect"));
            }
            if let Some(Protocol::P2p(address_peer)) = address.iter().last() {
                if address_peer != peer {
                    return Err(invalid("address terminal peer differs from requested peer"));
                }
                address.pop();
            }
            if swarm.is_connected(&peer) {
                command_done(
                    evidence,
                    sequence,
                    "connect",
                    "ok",
                    json!({"peer_id": peer.to_string(), "already_connected": true}),
                );
            } else {
                if peers.len() + pending.len() >= CONNECTION_LIMIT {
                    return Err(invalid("connected peer limit exceeded"));
                }
                swarm
                    .dial(DialOpts::peer_id(peer).addresses(vec![address]).build())
                    .map_err(|e| invalid(e.to_string()))?;
                pending.insert(
                    peer,
                    PendingConnect {
                        sequence,
                        started: Instant::now(),
                    },
                );
            }
        }
        Command::Publish { payload, .. } => {
            let published = swarm
                .behaviour_mut()
                .publish(config.topic(), payload.as_bytes());
            let detail = match &published {
                Ok(id) => json!({"message_id": hex(&id.0), "outcome": "published"}),
                Err(e) => json!({"outcome": "error", "error": e.to_string()}),
            };
            let mut event = detail.clone();
            event["command_sequence"] = json!(sequence);
            event["payload_sha256"] = json!(format!("{:x}", Sha256::digest(payload.as_bytes())));
            event["topic"] = json!(config.topic().hash().to_string());
            evidence.emit("publish", BEHAVIOUR_SOURCE, event);
            command_done(
                evidence,
                sequence,
                "publish",
                if published.is_ok() { "ok" } else { "error" },
                detail,
            );
            published.map_err(|e| invalid(e.to_string()))?;
        }
        Command::Sample { label, .. } => {
            snapshot(swarm.behaviour(), config, peers, &label, evidence)?;
            command_done(evidence, sequence, "sample", "ok", json!({"label": label}));
        }
        Command::PrepareShutdown {
            actor,
            token,
            local,
            ..
        } => {
            if actor != config.actor || token != config.token || local != *swarm.local_peer_id() {
                return Err(invalid("prepare_shutdown actor/token/identity mismatch"));
            }
            evidence.prepare(sequence, &actor, &token, local, pending.len())?;
            command_done(
                evidence,
                sequence,
                "prepare_shutdown",
                "ok",
                json!({"admission_closed": true}),
            );
        }
    }
    Ok(())
}

fn checked_native_event(
    event: SwarmEvent<gossipsub::Event>,
) -> io::Result<SwarmEvent<gossipsub::Event>> {
    match event {
        SwarmEvent::OutgoingConnectionError { error, .. } => Err(invalid(format!(
            "native outgoing connection error: {error}"
        ))),
        SwarmEvent::IncomingConnectionError { error, .. } => Err(invalid(format!(
            "native incoming connection error: {error}"
        ))),
        SwarmEvent::ListenerError { error, .. }
        | SwarmEvent::ListenerClosed {
            reason: Err(error), ..
        } => Err(error),
        other => Ok(other),
    }
}

fn handle_event(
    event: SwarmEvent<gossipsub::Event>,
    swarm: &mut Swarm<gossipsub::Behaviour>,
    config: &Config,
    evidence: &Evidence,
    upgrades: &crate::upgrade_observer::Observer,
    peers: &mut BTreeSet<PeerId>,
    pending: &mut BTreeMap<PeerId, PendingConnect>,
) -> io::Result<Option<Multiaddr>> {
    if let SwarmEvent::OutgoingConnectionError { peer_id, error, .. } = &event {
        if let Some(command) = peer_id.as_ref().and_then(|peer| pending.remove(peer)) {
            command_done(
                evidence,
                command.sequence,
                "connect",
                "error",
                json!({"error": error.to_string()}),
            );
        }
    }
    match checked_native_event(event)? {
        SwarmEvent::NewListenAddr { address, .. } => {
            evidence.emit(
                "listen",
                "rust.libp2p.Swarm.NewListenAddr",
                json!({"address": address.to_string()}),
            );
            return Ok(Some(address));
        }
        SwarmEvent::ConnectionEstablished {
            peer_id,
            connection_id,
            endpoint,
            ..
        } => {
            evidence.bind(peer_id, &endpoint, connection_id)?;
            upgrades.established(connection_id, peer_id, &endpoint);
            if peers.insert(peer_id) && peers.len() > CONNECTION_LIMIT {
                return Err(invalid("native peer limit exceeded"));
            }
            if let Some(command) = pending.remove(&peer_id) {
                command_done(
                    evidence,
                    command.sequence,
                    "connect",
                    "ok",
                    json!({"peer_id": peer_id.to_string(), "connection_id": connection_id.to_string()}),
                );
            }
        }
        SwarmEvent::Behaviour(gossipsub::Event::Message {
            propagation_source,
            message_id,
            message,
        }) => {
            if message.topic != config.topic().hash() || message.data.len() > PAYLOAD_LIMIT {
                return Err(invalid("native message outside actor topic/payload bounds"));
            }
            let author = message
                .source
                .ok_or_else(|| invalid("strict native message has no author"))?;
            let sequence = message
                .sequence_number
                .ok_or_else(|| invalid("strict native message has no author seqno"))?;
            let acceptance = validation(config, &message.data);
            let outcome = match &acceptance {
                MessageAcceptance::Accept => "accept",
                MessageAcceptance::Reject => "reject",
                MessageAcceptance::Ignore => "ignore",
            };
            let committed = swarm.behaviour_mut().report_message_validation_result(
                &message_id,
                &propagation_source,
                acceptance,
            );
            let detail = json!({"propagation_peer": propagation_source.to_string(), "author_peer": author.to_string(),
                "topic": message.topic.to_string(), "message_id": hex(&message_id.0), "seqno_hex": hex(&sequence.to_be_bytes()),
                "payload_sha256": format!("{:x}", Sha256::digest(&message.data)), "outcome": outcome,
                "report_message_validation_result": committed, "validation_commit": committed});
            if !committed {
                evidence.emit("validation_not_committed", BEHAVIOUR_SOURCE, detail);
                return Err(invalid(
                    "native report_message_validation_result returned false",
                ));
            }
            evidence.emit("validation", BEHAVIOUR_SOURCE, detail.clone());
            if outcome == "accept" {
                evidence.emit("delivery", BEHAVIOUR_SOURCE, detail);
            }
        }
        SwarmEvent::Behaviour(gossipsub::Event::Subscribed { peer_id, topic }) => {
            evidence.emit("subscription", BEHAVIOUR_SOURCE, json!({"peer_id": peer_id.to_string(), "topic": topic.to_string(), "subscribed": true}));
        }
        SwarmEvent::Behaviour(gossipsub::Event::Unsubscribed { peer_id, topic }) => {
            evidence.emit("subscription", BEHAVIOUR_SOURCE, json!({"peer_id": peer_id.to_string(), "topic": topic.to_string(), "subscribed": false}));
        }
        SwarmEvent::Behaviour(gossipsub::Event::GossipsubNotSupported { peer_id }) => {
            return Err(invalid(format!(
                "peer {} did not negotiate offered GossipSub protocol",
                peer_id
            )));
        }
        SwarmEvent::Behaviour(gossipsub::Event::SlowPeer {
            peer_id,
            failed_messages,
        }) => {
            return Err(invalid(format!(
                "native slow peer {}: {:?}",
                peer_id, failed_messages
            )));
        }
        SwarmEvent::ConnectionClosed {
            peer_id,
            connection_id,
            cause,
            ..
        } => {
            evidence.terminal(peer_id, connection_id, cause.map(|e| e.to_string()))?;
        }
        _ => {}
    }
    Ok(None)
}

async fn serve(
    config: &Config,
    swarm: &mut Swarm<gossipsub::Behaviour>,
    evidence: &Evidence,
    upgrades: &crate::upgrade_observer::Observer,
) -> Result<(), Box<dyn Error>> {
    let address: Multiaddr = if config.transport == "quic" {
        "/ip4/127.0.0.1/udp/0/quic-v1"
    } else {
        "/ip4/127.0.0.1/tcp/0"
    }
    .parse()?;
    swarm.listen_on(address)?;
    let mut peers = BTreeSet::new();
    let mut pending = BTreeMap::new();
    let listening = tokio::time::timeout(Duration::from_secs(10), async {
        loop {
            let event = swarm
                .next()
                .await
                .ok_or_else(|| invalid("native Swarm stopped before readiness"))?;
            if let Some(address) = handle_event(
                event,
                swarm,
                config,
                evidence,
                upgrades,
                &mut peers,
                &mut pending,
            )? {
                break Ok::<_, io::Error>(address);
            }
        }
    })
    .await
    .map_err(|_| invalid("native listener readiness deadline"))??;
    replace_json(
        &config.ready,
        &json!({"schema_version": 1, "implementation": "rust", "actor": config.actor,
        "case_token": config.token, "local_peer_id": swarm.local_peer_id().to_string(), "peer_id": swarm.local_peer_id().to_string(),
        "address": listening.to_string(), "listen_addr": listening.to_string(), "subscribed": true,
        "listen_addrs": [listening.to_string()],
        "ready": true, "subscription_created": true,
        "topic": config.topic().hash().to_string(), "source": "rust.libp2p.native_listener_and_successful_Behaviour_subscription"}),
    )?;
    let mut control = Control::default();
    let mut tick = tokio::time::interval(Duration::from_millis(25));
    tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    let mut observations = tokio::time::interval(Duration::from_millis(250));
    observations.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    let deadline = tokio::time::sleep(Duration::from_secs(180));
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            event = swarm.next() => {
                handle_event(event.ok_or_else(|| invalid("native Swarm stream ended"))?, swarm, config, evidence, upgrades, &mut peers, &mut pending)?;
            }
            _ = tick.tick() => {
                let commands = control.poll(&config.control)?;
                if evidence.lock().prepared { control.finish()?; }
                let count = commands.len();
                for (index, next) in commands.into_iter().enumerate() {
                    if matches!(next, Command::PrepareShutdown { .. }) {
                        control.finish()?;
                        if index + 1 != count || upgrades.snapshot()["overflow"] == true {
                            return Err(invalid("prepare_shutdown with queued commands or upgrade overflow").into());
                        }
                    }
                    command(next, swarm, config, evidence, &peers, &mut pending)?;
                }
                if pending.values().any(|command| command.started.elapsed() > Duration::from_secs(15)) { return Err(invalid("native connect completion deadline").into()); }
                if let Some(error) = evidence.lock().error.clone() { return Err(invalid(error).into()); }
                if upgrades.snapshot()["overflow"] == true { return Err(invalid("native upgrade observer overflow").into()); }
                replace_json(&config.result, &result(config, Some(*swarm.local_peer_id()), evidence, false, false, Value::Null, upgrades.snapshot()))?;
                if config.stop.exists() {
                    control.finish()?;
                    if !evidence.lock().prepared { return Err(invalid("stop before prepare_shutdown acknowledgement").into()); }
                    if !pending.is_empty() { return Err(invalid("stop before native connect command completion").into()); }
                    return Ok(());
                }
            }
            _ = observations.tick() => snapshot(swarm.behaviour(), config, &peers, "periodic", evidence)?,
            _ = &mut deadline => return Err(invalid("pubsub-live actor lifetime deadline").into()),
        }
    }
}

struct NativeClose {
    first_error: Option<io::Error>,
}

impl NativeClose {
    fn begin(swarm: &mut Swarm<gossipsub::Behaviour>, evidence: &Evidence) -> Self {
        let connections = evidence
            .lock()
            .connections
            .iter()
            .filter(|c| !c.terminal)
            .filter_map(|c| c.native_id)
            .collect::<Vec<_>>();
        for connection in connections {
            // Drive the pinned native handler/muxer close future.
            swarm.close_connection(connection);
        }
        let listeners = swarm.listeners().count();
        evidence.emit(
            "shutdown_requested",
            "rust.fixture.native_host_close",
            json!({"listeners": listeners}),
        );
        Self { first_error: None }
    }

    fn remember(&mut self, error: io::Error, evidence: &Evidence) {
        evidence.lock().fail(&error);
        if self.first_error.is_none() {
            self.first_error = Some(error);
        }
    }

    fn observe(&mut self, event: SwarmEvent<gossipsub::Event>, evidence: &Evidence) {
        let outcome = match checked_native_event(event) {
            Ok(SwarmEvent::ConnectionClosed {
                peer_id,
                connection_id,
                cause,
                ..
            }) => evidence.terminal(peer_id, connection_id, cause.map(|e| e.to_string())),
            Ok(_) => Ok(()),
            Err(error) => Err(error),
        };
        if let Err(error) = outcome {
            self.remember(error, evidence);
        }
    }

    async fn finish(
        mut self,
        swarm: &mut Swarm<gossipsub::Behaviour>,
        evidence: &Evidence,
    ) -> io::Result<()> {
        let drained = tokio::time::timeout(Duration::from_secs(5), async {
            loop {
                if swarm.connected_peers().next().is_none() && evidence.lock().live_muxers == 0 {
                    return Ok(());
                }
                tokio::select! {
                    event = swarm.next() => {
                        match event {
                            Some(event) => self.observe(event, evidence),
                            None => return Err(invalid("native Swarm ended during close")),
                        }
                    }
                    _ = tokio::time::sleep(Duration::from_millis(10)) => {},
                }
            }
        })
        .await;
        match drained {
            Ok(Ok(())) => {}
            Ok(Err(error)) => self.remember(error, evidence),
            Err(_) => self.remember(invalid("native host close deadline"), evidence),
        }
        match self.first_error {
            Some(error) => Err(error),
            None => Ok(()),
        }
    }
}

async fn close_native(
    swarm: &mut Swarm<gossipsub::Behaviour>,
    evidence: &Evidence,
) -> io::Result<()> {
    NativeClose::begin(swarm, evidence)
        .finish(swarm, evidence)
        .await
}

pub(crate) async fn run(config: Config) -> Result<(), Box<dyn Error>> {
    let tasks = crate::task_owner::Owner::default();
    let upgrades = crate::upgrade_observer::Observer::default();
    let evidence = Evidence::default();
    let mut local = None;
    let primary: Result<(), Box<dyn Error>> = async {
        fs::create_dir_all(&config.store)?;
        let mut swarm = new_swarm(&config, &evidence, &upgrades, &tasks)?;
        local = Some(*swarm.local_peer_id());
        let primary = serve(&config, &mut swarm, &evidence, &upgrades).await;
        if let Err(e) = &primary {
            evidence.lock().fail(e);
        }
        let close = close_native(&mut swarm, &evidence).await;
        if let Err(e) = &close {
            evidence.lock().fail(e);
        }
        drop(swarm);
        primary?;
        close?;
        Ok(())
    }
    .await;
    if let Err(e) = primary {
        evidence.lock().fail(e);
    }
    let report = tokio::time::timeout(Duration::from_secs(5), tasks.close_and_join()).await;
    let task_join = match report {
        Ok(report) => report.snapshot(),
        Err(_) => {
            json!({"fixture_owned_tasks_joined": false, "overflow": false, "errors": ["owned task join deadline"]})
        }
    };
    let joined = {
        let mut capture = evidence.lock();
        let joined = task_join["fixture_owned_tasks_joined"] == true
            && task_join["overflow"] == false
            && task_join["errors"].as_array().is_some_and(Vec::is_empty)
            && capture.live_muxers == 0
            && capture.live_streams == 0
            && capture
                .connections
                .iter()
                .all(|c| c.dropped && (c.swarm_id.is_none() || c.closed || c.terminal));
        if !joined {
            capture.fail("fixture workers or established native host did not close/join");
        }
        if task_join["overflow"] == true || upgrades.snapshot()["overflow"] == true {
            capture.overflow = true;
            capture.fail("native task/upgrade observation overflow");
        }
        joined
    };
    replace_json(
        &config.result,
        &result(
            &config,
            local,
            &evidence,
            true,
            joined,
            task_join,
            upgrades.finalized(joined),
        ),
    )?;
    if let Some(error) = evidence.lock().error.clone() {
        return Err(invalid(error).into());
    }
    Ok(())
}

#[cfg(test)]
#[path = "pubsub_scoring/tests.rs"]
mod tests;
