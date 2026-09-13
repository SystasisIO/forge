use std::{
    collections::HashSet,
    error::Error,
    fs,
    net::{Ipv4Addr, SocketAddr},
    num::NonZeroUsize,
    path::{Path, PathBuf},
    str::FromStr,
    sync::{
        Arc, Mutex,
        atomic::{AtomicU64, Ordering},
    },
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

use futures::{AsyncReadExt, AsyncWriteExt, StreamExt};
use hickory_resolver::config::{NameServerConfig, ResolveHosts};
use hickory_resolver::proto::xfer::Protocol as DnsProtocol;
use libp2p::kad::store::RecordStore;
use libp2p::{
    Multiaddr, PeerId, StreamProtocol, SwarmBuilder, Transport,
    core::transport::upgrade::Version,
    dcutr, gossipsub, identify, identity, kad,
    multiaddr::Protocol,
    noise, ping,
    pnet::{PnetConfig, PreSharedKey},
    relay, rendezvous,
    swarm::{NetworkBehaviour, SwarmEvent, behaviour::toggle::Toggle, dial_opts::DialOpts},
    tcp, yamux,
};
use libp2p_stream as raw_stream;
use quick_protobuf::BytesReader;
use serde_json::json;
use sha2::{Digest, Sha256};

mod provider;
mod task_owner;
mod upgrade_observer;
mod application_observer;
#[path = "autonat.rs"]
mod autonat_fixture;

const KAD_PROTOCOL: &str = "/ipfs/kad/1.0.0";
const PUBSUB_TOPIC: &str = "forge.pubsub.interop";
const PUBSUB_PAYLOAD: &[u8] = b"forge-gossipsub-live";

#[derive(Debug, Default)]
struct Options {
    command: String,
    scenario: String,
    peer_id: String,
    addr: String,
    relay_addr: String,
    relay_peer_id: String,
    ready_file: PathBuf,
    stop_file: PathBuf,
    result_file: PathBuf,
    seed_file: PathBuf,
    seed_peer_id: String,
    seed_addr: String,
    target_peer_id: String,
    payload: String,
    transport: String,
    dns_server: Option<SocketAddr>,
    bind_ip: String,
    probe_addr: String,
    internet_egress: String,
    tcp_upgrade_observation: Arc<Mutex<Option<TcpUpgradeObservation>>>,
    upgrade_observer: upgrade_observer::Observer,
    tasks: task_owner::Owner,
    expected_messages: usize,
    pnet_key_file: PathBuf,
    pnet_fingerprint: String,
    pnet_control: String,
    pnet_correlation: String,
    features: HashSet<String>,
}

#[derive(Clone, Debug)]
struct TcpUpgradeObservation {
    peer: PeerId,
    remote_address: Multiaddr,
    security: &'static str,
    muxer: &'static str,
}

#[derive(Clone, Copy, Debug, Default)]
struct PnetObservation {
    attempted_connections: u64,
    established_connections: u64,
    identify_streams: u64,
    application_streams: u64,
}

#[derive(NetworkBehaviour)]
struct Behaviour {
    // Native AutoNAT owns a separate behaviour in autonat_fixture.
    relay: Toggle<relay::Behaviour>,
    relay_client: Toggle<relay::client::Behaviour>,
    kad: kad::Behaviour<kad::store::MemoryStore>,
    rendezvous_server: rendezvous::server::Behaviour,
    rendezvous_client: rendezvous::client::Behaviour,
    gossipsub: gossipsub::Behaviour,
    ping: ping::Behaviour,
    identify: identify::Behaviour,
    dcutr: Toggle<dcutr::Behaviour>,
    stream: raw_stream::Behaviour,
}

#[derive(Debug)]
struct RendezvousRegisterDiscoverEvidence {
    wire_registration_count: usize,
    record_sequence: u64,
    record_address_count: usize,
    registered_ttl_seconds: u64,
    discovered_ttl_seconds: u64,
    cookie_bytes: usize,
}

#[derive(Debug)]
struct RendezvousLifecycleEvidence {
    initial_ttl_seconds: u64,
    updated_ttl_seconds: u64,
    renewed_ttl_seconds: u64,
    initial_record_sequence: u64,
    updated_record_sequence: u64,
    renewed_record_sequence: u64,
    pre_unregister_record_sequence: u64,
    initial_cookie_bytes: usize,
    delta_cookie_bytes: usize,
    initial_visible_count: usize,
    updated_visible_count: usize,
    renewed_visible_after_original_expiry: bool,
    renewed_visible_count: usize,
    expired_registration_count: usize,
    pre_unregister_count: usize,
    final_registration_count: usize,
}

#[derive(Debug)]
struct RelayReservationEvidence {
    accepted: bool,
    relay_peer_id: String,
    client_peer_id: String,
    circuit_addr: String,
    renewal: bool,
}

#[derive(Debug, PartialEq, Eq)]
enum RelayAddressError {
    TerminalPeerMismatch,
}

impl RelayAddressError {
    fn status_code(&self) -> &'static str {
        match self {
            Self::TerminalPeerMismatch => "relay_endpoint_peer_mismatch",
        }
    }
}

impl std::fmt::Display for RelayAddressError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::TerminalPeerMismatch => {
                formatter.write_str("relay endpoint terminal peer does not match requested peer")
            }
        }
    }
}

impl Error for RelayAddressError {}

#[derive(Debug, Default)]
struct RelayReservationProgress {
    accepted_renewal: Option<bool>,
    circuit_addr: Option<String>,
}

impl RelayReservationProgress {
    fn record_acceptance(&mut self, renewal: bool) {
        self.accepted_renewal = Some(renewal);
    }

    fn record_circuit_addr(&mut self, circuit_addr: String) {
        self.circuit_addr = Some(circuit_addr);
    }

    fn complete(&mut self) -> Option<(bool, String)> {
        if self.accepted_renewal.is_none() || self.circuit_addr.is_none() {
            return None;
        }
        Some((
            self.accepted_renewal.take().expect("checked above"),
            self.circuit_addr.take().expect("checked above"),
        ))
    }
}

const RENDEZVOUS_LIFECYCLE_MIN_TTL_SECONDS: u64 = 2;
const RENDEZVOUS_LIFECYCLE_MAX_TTL_SECONDS: u64 = 3;
const RENDEZVOUS_LIFECYCLE_TIMING_MARGIN: Duration = Duration::from_millis(200);

fn parse_args() -> Result<Options, Box<dyn Error>> {
    let mut args = std::env::args().skip(1);
    let mut out = Options::default();
    out.command = args.next().ok_or("missing command")?;
    while let Some(key) = args.next() {
        let value = args
            .next()
            .ok_or_else(|| format!("missing value for {key}"))?;
        match key.as_str() {
            "--scenario" => out.scenario = value,
            "--peer-id" => out.peer_id = value,
            "--addr" => out.addr = value,
            "--relay-addr" => out.relay_addr = value,
            "--relay-peer-id" => out.relay_peer_id = value,
            "--ready-file" => out.ready_file = PathBuf::from(value),
            "--stop-file" => out.stop_file = PathBuf::from(value),
            "--result-file" => out.result_file = PathBuf::from(value),
            "--seed-file" => out.seed_file = PathBuf::from(value),
            "--seed-peer-id" => out.seed_peer_id = value,
            "--seed-addr" => out.seed_addr = value,
            "--target-peer-id" => out.target_peer_id = value,
            "--payload" => out.payload = value,
            "--transport" => out.transport = value,
            "--bind-ip" => out.bind_ip = value,
            "--probe-addr" => out.probe_addr = value,
            "--internet-egress" => out.internet_egress = value,
            "--dns-server" => {
                let address: SocketAddr = value.parse()?;
                if address.port() == 0 {
                    return Err("--dns-server port must be 1..65535".into());
                }
                out.dns_server = Some(address);
            }
            "--expected-messages" => out.expected_messages = value.parse()?,
            "--pnet-key-file" => out.pnet_key_file = PathBuf::from(value),
            "--pnet-fingerprint" => out.pnet_fingerprint = value,
            "--pnet-control" => out.pnet_control = value,
            "--pnet-correlation" => out.pnet_correlation = value,
            "--features" => {
                out.features.extend(
                    value
                        .split(',')
                        .filter(|feature| !feature.is_empty())
                        .map(str::to_owned),
                );
            }
            "--store-dir" => {}
            _ => return Err(format!("unknown argument {key}").into()),
        }
    }
    if out.payload.is_empty() {
        out.payload = String::from_utf8_lossy(PUBSUB_PAYLOAD).to_string();
    }
    if out.transport.is_empty() {
        out.transport = "quic".to_string();
    }
    if out.expected_messages == 0 {
        out.expected_messages = 1;
    }
    if out.transport == "tcp-pnet"
        && !autonat_fixture::is_scenario(&out.scenario)
        && let Some(feature) = ["relay", "dcutr"]
            .into_iter()
            .find(|feature| out.features.contains(*feature))
    {
        return Err(format!("tcp-pnet does not permit feature {feature}").into());
    }
    Ok(out)
}

fn write_json(path: &Path, value: serde_json::Value) -> Result<(), Box<dyn Error>> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::write(path, serde_json::to_vec(&value)?)?;
    Ok(())
}

fn behaviour_for(
    key: &identity::Keypair,
    relay_client: Option<relay::client::Behaviour>,
    opts: &Options,
) -> Behaviour {
    let peer = key.public().to_peer_id();
    let private_network = opts.transport == "tcp-pnet";
    let mut kad_config = kad::Config::new(StreamProtocol::new(KAD_PROTOCOL));
    kad_config.set_query_timeout(Duration::from_secs(10));
    let mut kad_behaviour =
        kad::Behaviour::with_config(peer, kad::store::MemoryStore::new(peer), kad_config);
    kad_behaviour.set_mode(Some(kad::Mode::Server));
    Behaviour {
        relay: (!private_network)
            .then(|| relay::Behaviour::new(peer, Default::default()))
            .into(),
        relay_client: relay_client.filter(|_| !private_network).into(),
        kad: kad_behaviour,
        rendezvous_server: rendezvous::server::Behaviour::new(
            if opts.scenario == "rendezvous_lifecycle" {
                rendezvous::server::Config::default()
                    .with_min_ttl(1)
                    .with_max_ttl(3)
            } else {
                rendezvous::server::Config::default()
            },
        ),
        rendezvous_client: rendezvous::client::Behaviour::new(key.clone()),
        gossipsub: gossipsub::Behaviour::new(
            gossipsub::MessageAuthenticity::Signed(key.clone()),
            gossipsub::ConfigBuilder::default()
                .protocol_id_prefix("/meshsub")
                .validation_mode(gossipsub::ValidationMode::Strict)
                .build()
                .expect("valid gossipsub config"),
        )
        .expect("valid gossipsub behaviour"),
        ping: ping::Behaviour::new(ping::Config::new()),
        identify: identify::Behaviour::new(identify::Config::new_with_signed_peer_record(
            "/forge-interop/0.1.0".into(),
            key,
        )),
        dcutr: (!private_network)
            .then(|| dcutr::Behaviour::new(peer))
            .into(),
        stream: raw_stream::Behaviour::new(),
    }
}

async fn new_swarm(opts: &Options) -> Result<libp2p::Swarm<Behaviour>, Box<dyn Error>> {
    let transport = opts.transport.as_str();
    let key = identity::Keypair::generate_ed25519();
    let configure_tasks = |_| opts.tasks.swarm_config();
    let resolver = opts.dns_server.map(|address| {
        let mut config = libp2p::dns::ResolverConfig::new();
        config.add_name_server(NameServerConfig::new(address, DnsProtocol::Udp));
        config.add_name_server(NameServerConfig::new(address, DnsProtocol::Tcp));
        let mut options = libp2p::dns::ResolverOpts::default();
        options.use_hosts_file = ResolveHosts::Never;
        (config, options)
    });
    let mut swarm = match transport {
        "quic" | "" => {
            let builder = SwarmBuilder::with_existing_identity(key)
                .with_tokio()
                .with_quic();
            if let Some((config, options)) = resolver.clone() {
                builder
                    .with_dns_config(config, options)
                    .with_relay_client(noise::Config::new, yamux::Config::default)?
                    .with_behaviour(|key, relay_client| {
                        behaviour_for(key, Some(relay_client), opts)
                    })?
                    .with_swarm_config(configure_tasks)
                    .build()
            } else {
                builder
                    .with_relay_client(noise::Config::new, yamux::Config::default)?
                    .with_behaviour(|key, relay_client| {
                        behaviour_for(key, Some(relay_client), opts)
                    })?
                    .with_swarm_config(configure_tasks)
                    .build()
            }
        }
        "tcp" => {
            let observation = Arc::clone(&opts.tcp_upgrade_observation);
            let upgrade_observer = opts.upgrade_observer.clone();
            let expected_peer = if opts.command == "dial" {
                Some(opts.peer_id.parse::<PeerId>()?)
            } else {
                None
            };
            let builder = SwarmBuilder::with_existing_identity(key)
                .with_tokio()
                .with_other_transport(move |key| {
                    upgrade_observer::native_transport(key, false, upgrade_observer)
                        .map(|transport| {
                            Transport::map(transport, move |(peer, muxer), endpoint| {
                                // This runs only after both upgrades, below the DNS wrapper.
                                if expected_peer == Some(peer) && endpoint.is_dialer() {
                                    *observation.lock().expect("TCP upgrade observation lock") =
                                        Some(TcpUpgradeObservation {
                                            peer,
                                            remote_address: endpoint.get_remote_address().clone(),
                                            security: "/noise",
                                            muxer: "/yamux/1.0.0",
                                        });
                                }
                                (peer, muxer)
                            })
                        })
                })?;
            if let Some((config, options)) = resolver.clone() {
                builder
                    .with_dns_config(config, options)
                    .with_relay_client(noise::Config::new, yamux::Config::default)?
                    .with_behaviour(|key, relay_client| {
                        behaviour_for(key, Some(relay_client), opts)
                    })?
                    .with_swarm_config(configure_tasks)
                    .build()
            } else {
                builder
                    .with_relay_client(noise::Config::new, yamux::Config::default)?
                    .with_behaviour(|key, relay_client| {
                        behaviour_for(key, Some(relay_client), opts)
                    })?
                    .with_swarm_config(configure_tasks)
                    .build()
            }
        }
        "tcp-tls" => {
            let upgrade_observer = opts.upgrade_observer.clone();
            let builder = SwarmBuilder::with_existing_identity(key)
                .with_tokio()
                .with_other_transport(move |key| {
                    upgrade_observer::native_transport(key, true, upgrade_observer)
                })?;
            if let Some((config, options)) = resolver.clone() {
                builder
                    .with_dns_config(config, options)
                    .with_relay_client(noise::Config::new, yamux::Config::default)?
                    .with_behaviour(|key, relay_client| {
                        behaviour_for(key, Some(relay_client), opts)
                    })?
                    .with_swarm_config(configure_tasks)
                    .build()
            } else {
                builder
                    .with_relay_client(noise::Config::new, yamux::Config::default)?
                    .with_behaviour(|key, relay_client| {
                        behaviour_for(key, Some(relay_client), opts)
                    })?
                    .with_swarm_config(configure_tasks)
                    .build()
            }
        }
        "tcp-pnet" if opts.pnet_key_file.as_os_str().is_empty() => {
            let builder = SwarmBuilder::with_existing_identity(key)
                .with_tokio()
                .with_tcp(
                    tcp::Config::default().nodelay(true),
                    noise::Config::new,
                    yamux::Config::default,
                )?;
            if let Some((config, options)) = resolver.clone() {
                builder
                    .with_dns_config(config, options)
                    .with_behaviour(|key| behaviour_for(key, None, opts))?
                    .with_swarm_config(configure_tasks)
                    .build()
            } else {
                builder
                    .with_behaviour(|key| behaviour_for(key, None, opts))?
                    .with_swarm_config(configure_tasks)
                    .build()
            }
        }
        "tcp-pnet" => {
            let text = fs::read_to_string(&opts.pnet_key_file)?;
            let psk = PreSharedKey::from_str(&text)?;
            let builder = SwarmBuilder::with_existing_identity(key)
                .with_tokio()
                .with_other_transport(move |key| {
                    tcp::tokio::Transport::new(tcp::Config::default().nodelay(true))
                        .and_then(move |socket, _| PnetConfig::new(psk).handshake(socket))
                        .upgrade(Version::V1Lazy)
                        .authenticate(noise::Config::new(key).expect("valid noise identity"))
                        .multiplex(yamux::Config::default())
                })?;
            if let Some((config, options)) = resolver.clone() {
                builder
                    .with_dns_config(config, options)
                    .with_behaviour(|key| behaviour_for(key, None, opts))?
                    .with_swarm_config(configure_tasks)
                    .build()
            } else {
                builder
                    .with_behaviour(|key| behaviour_for(key, None, opts))?
                    .with_swarm_config(configure_tasks)
                    .build()
            }
        }
        other => return Err(format!("unsupported transport {other}").into()),
    };

    let listen_addr = if transport == "tcp" || transport == "tcp-tls" || transport == "tcp-pnet" {
        Multiaddr::empty()
            .with(Protocol::Ip4(Ipv4Addr::LOCALHOST))
            .with(Protocol::Tcp(0))
    } else {
        Multiaddr::empty()
            .with(Protocol::Ip4(Ipv4Addr::LOCALHOST))
            .with(Protocol::Udp(0))
            .with(Protocol::QuicV1)
    };
    swarm.listen_on(listen_addr)?;
    Ok(swarm)
}

fn pnet_rejection(
    opts: &Options,
    role: &str,
    expected_peer: &str,
    observation: PnetObservation,
) -> serde_json::Value {
    json!({
        "implementation": "rust",
        "role": role,
        "scenario": "pnet",
        "status": "rejected",
        "control_kind": opts.pnet_control,
        "correlation_token": opts.pnet_correlation,
        "expected_peer_id": expected_peer,
        "counter_source": "rust-libp2p.swarm-events",
        "attempted_connections": observation.attempted_connections,
        "established_connections": observation.established_connections,
        "identify_streams": observation.identify_streams,
        "application_streams": observation.application_streams,
        "rejected_before_identify": observation.established_connections == 0
            && observation.identify_streams == 0
            && observation.application_streams == 0
    })
}

fn decode_hex(value: &str) -> Result<Vec<u8>, Box<dyn Error>> {
    if value.len() % 2 != 0 {
        return Err("hex fixture has odd length".into());
    }
    value
        .as_bytes()
        .chunks_exact(2)
        .map(|pair| {
            let text = std::str::from_utf8(pair)?;
            Ok(u8::from_str_radix(text, 16)?)
        })
        .collect()
}

fn is_dht_value_scenario(scenario: &str) -> bool {
    scenario == "dht_pk_put_get" || scenario == "dht_ipns_put_get"
}

fn dht_value_fixture(scenario: &str) -> Result<(kad::RecordKey, Vec<u8>), Box<dyn Error>> {
    const IDENTITY_MULTIHASH: &str =
        "00240801122079b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664";
    let (prefix, value) = match scenario {
        "dht_pk_put_get" => (
            "2f706b2f",
            "0801122079b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664".to_string(),
        ),
        "dht_ipns_put_get" => (
            "2f69706e732f",
            [
                "0a1f2f697066732f6261666b716163336a6f627868676964736e3572777734796b1240b7be19b36e1955d2e1ccddd889d25c",
                "4eaef61aa72763bc44db9696697be7587e35d2efb2a625e7ac19b05f8c348086114103ee042a5a4041683e39c4ac0c460118",
                "00221e323033302d30312d30325430333a30343a30352e3132333435363738395a28073080f092cbdd0842408904024a1b09",
                "b52636334f17b9098f648f9a00214e6c6c89bb954c01300b00f54d085ddcacbe42952f2f819d70a48ff453d13329bb775d66",
                "e5a4b6165c38a40a4a76a56354544c1b00000045d964b8006556616c7565581f2f697066732f6261666b716163336a6f6278",
                "68676964736e3572777734796b6853657175656e6365076856616c6964697479581e323033302d30312d30325430333a30343a",
                "30352e3132333435363738395a6c56616c69646974795479706500",
            ]
            .concat(),
        ),
        other => return Err(format!("unknown DHT value fixture {other}").into()),
    };
    let key = decode_hex(&format!("{prefix}{IDENTITY_MULTIHASH}"))?;
    Ok((kad::RecordKey::new(&key), decode_hex(&value)?))
}

fn transport_addr(mut address: Multiaddr) -> Multiaddr {
    if matches!(address.iter().last(), Some(Protocol::P2p(_))) {
        let _ = address.pop();
    }
    address
}

fn relay_transport_addr(
    mut address: Multiaddr,
    relay_peer: PeerId,
) -> Result<Multiaddr, RelayAddressError> {
    if let Some(Protocol::P2p(endpoint_peer)) = address.iter().last() {
        if endpoint_peer != relay_peer {
            return Err(RelayAddressError::TerminalPeerMismatch);
        }
        let _ = address.pop();
    }
    Ok(address)
}

fn observed_quic_transport(endpoint: &libp2p::core::ConnectedPoint) -> Option<&'static str> {
    endpoint
        .get_remote_address()
        .iter()
        .any(|protocol| matches!(protocol, Protocol::QuicV1))
        .then_some("/quic-v1")
}

async fn reserve_relay_address(
    swarm: &mut libp2p::Swarm<Behaviour>,
    relay_peer: PeerId,
    relay_addr: Multiaddr,
) -> Result<RelayReservationEvidence, Box<dyn Error>> {
    let client_peer = *swarm.local_peer_id();
    let base_circuit_addr = relay_addr
        .with(Protocol::P2p(relay_peer))
        .with(Protocol::P2pCircuit);
    let expected_circuit_addr = base_circuit_addr.clone().with(Protocol::P2p(client_peer));
    let relay_listener_id = swarm.listen_on(base_circuit_addr)?;
    let mut progress = RelayReservationProgress::default();
    let deadline = tokio::time::sleep(Duration::from_secs(20));
    tokio::pin!(deadline);

    loop {
        tokio::select! {
            _ = &mut deadline => {
                return Err("timed out waiting for relay reservation acceptance and circuit address".into());
            }
            event = swarm.select_next_some() => match event {
                SwarmEvent::NewListenAddr { listener_id, address, .. }
                    if listener_id == relay_listener_id && address == expected_circuit_addr =>
                {
                    swarm.add_external_address(address.clone());
                    progress.record_circuit_addr(address.to_string());
                }
                SwarmEvent::Behaviour(BehaviourEvent::RelayClient(
                    relay::client::Event::ReservationReqAccepted {
                        relay_peer_id,
                        renewal,
                        ..
                    },
                )) if relay_peer_id == relay_peer => {
                    if renewal {
                        return Err("initial relay reservation unexpectedly reported renewal=true".into());
                    }
                    progress.record_acceptance(renewal);
                }
                SwarmEvent::ListenerClosed { listener_id, reason: Err(error), .. }
                    if listener_id == relay_listener_id =>
                {
                    return Err(format!("relay reservation listener closed: {error}").into());
                }
                other => {
                    eprintln!("rust relay reservation event: {other:?}");
                }
            },
        }
        if let Some((renewal, circuit_addr)) = progress.complete() {
            return Ok(RelayReservationEvidence {
                accepted: true,
                relay_peer_id: relay_peer.to_string(),
                client_peer_id: client_peer.to_string(),
                circuit_addr,
                renewal,
            });
        }
    }
}

async fn read_frame<S>(stream: &mut S) -> Result<Vec<u8>, Box<dyn Error>>
where
    S: futures::AsyncRead + Unpin,
{
    let mut shift = 0u32;
    let mut size = 0usize;
    loop {
        let mut single = [0u8; 1];
        stream.read_exact(&mut single).await?;
        let byte = single[0];
        size |= usize::from(byte & 0x7f) << shift;
        if byte & 0x80 == 0 {
            break;
        }
        shift += 7;
        if shift > 28 {
            return Err("frame varint is too large".into());
        }
    }
    if size == 0 || size > 256 * 1024 {
        return Err(format!("invalid frame size {size}").into());
    }
    let mut payload = vec![0; size];
    stream.read_exact(&mut payload).await?;
    Ok(payload)
}

async fn write_frame<S>(stream: &mut S, payload: &[u8]) -> Result<(), Box<dyn Error>>
where
    S: futures::AsyncWrite + Unpin,
{
    let mut value = payload.len();
    let mut prefix = Vec::new();
    loop {
        let mut byte = (value & 0x7f) as u8;
        value >>= 7;
        if value != 0 {
            byte |= 0x80;
        }
        prefix.push(byte);
        if value == 0 {
            break;
        }
    }
    stream.write_all(&prefix).await?;
    stream.write_all(payload).await?;
    stream.flush().await?;
    Ok(())
}

fn spawn_incoming_stream_echo(
    swarm: &mut libp2p::Swarm<Behaviour>,
    protocol: &'static str,
    opened_streams: Option<Arc<AtomicU64>>,
    tasks: &task_owner::Owner,
) -> Result<(), Box<dyn Error>> {
    let mut control = swarm.behaviour().stream.new_control();
    let mut incoming = control.accept(StreamProtocol::new(protocol))?;
    let errors = tasks.spawner();
    tasks.spawn("echo_handler", async move {
        while let Some((_, mut stream)) = incoming.next().await {
            if let Some(counter) = &opened_streams {
                counter.fetch_add(1, Ordering::Relaxed);
            }
            let payload = match read_frame(&mut stream).await.map_err(|error| error.to_string()) {
                Ok(value) => value,
                Err(error) => {
                    errors.record_error("echo_read_frame", error);
                    if let Err(error) = stream.close().await {
                        errors.record_error("echo_close_after_read_failure", error);
                    }
                    continue;
                }
            };
            if let Err(error) = write_frame(&mut stream, &payload).await {
                errors.record_error("echo_write_frame", error);
            }
            if let Err(error) = stream.close().await {
                errors.record_error("echo_close_stream", error);
            }
        }
    })?;
    Ok(())
}

async fn open_required_stream(
    swarm: &mut libp2p::Swarm<Behaviour>,
    peer: PeerId,
    protocol: &'static str,
) -> Result<(), Box<dyn Error>> {
    let mut control = swarm.behaviour().stream.new_control();
    let mut open = Box::pin(control.open_stream(peer, StreamProtocol::new(protocol)));
    let deadline = tokio::time::sleep(Duration::from_secs(15));
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            result = &mut open => {
                let mut stream = result?;
                stream.close().await?;
                return Ok(());
            }
            _ = &mut deadline => {
                return Err(format!("timed out opening {protocol}").into());
            }
            event = swarm.select_next_some() => {
                if let SwarmEvent::NewListenAddr { address, .. } = event {
                    swarm.add_external_address(address);
                }
            }
        }
    }
}

const RAW_IDENTIFY_LIMIT: usize = 4096;

async fn read_raw_identify_frame<S>(stream: &mut S) -> Result<Vec<u8>, Box<dyn Error>>
where
    S: futures::AsyncRead + Unpin,
{
    let mut size = 0usize;
    // Every permitted length fits in two varint bytes. Bound before allocating
    // or reading a body; do not reuse the larger application echo-frame limit.
    for shift in [0, 7] {
        let mut byte = [0u8];
        stream.read_exact(&mut byte).await?;
        size |= usize::from(byte[0] & 0x7f) << shift;
        if byte[0] & 0x80 == 0 {
            if size == 0 || size > RAW_IDENTIFY_LIMIT || (shift != 0 && byte[0] == 0) {
                return Err("Identify frame length violates canonical 4096-byte limit".into());
            }
            let mut bytes = vec![0; size];
            stream.read_exact(&mut bytes).await?;
            return Ok(bytes);
        }
    }
    Err("Identify frame length exceeds 4096-byte limit".into())
}

fn raw_identify_fields(bytes: &[u8]) -> Result<(&[u8], &[u8]), Box<dyn Error>> {
    if bytes.is_empty() || bytes.len() > RAW_IDENTIFY_LIMIT {
        return Err("invalid bounded Identify protobuf".into());
    }
    let mut reader = BytesReader::from_bytes(bytes);
    let mut public_key = None;
    let mut signed_record = None;
    while !reader.is_eof() {
        let tag = reader.next_tag(bytes)?;
        match tag >> 3 {
            0 => return Err("Identify field zero is invalid".into()),
            field @ (1 | 8) => {
                if tag & 7 != 2 {
                    return Err("Identify key/record has wrong protobuf wire type".into());
                }
                let slot = if field == 1 { &mut public_key } else { &mut signed_record };
                if slot.is_some() {
                    return Err("duplicate Identify key/record field".into());
                }
                *slot = Some(reader.read_bytes(bytes)?);
            }
            _ => reader.read_unknown(bytes, tag)?,
        }
    }
    Ok((public_key.ok_or("Identify public key missing")?,
        signed_record.ok_or("Identify signed peer record missing")?))
}

fn raw_identify_evidence(bytes: &[u8], authenticated_peer: PeerId) -> serde_json::Value {
    use libp2p::core::{PeerRecord, SignedEnvelope};
    const STANDARD_PAYLOAD_TYPE: &[u8] = &[0x03, 0x01];

    let hex = |value: &[u8]| value.iter().map(|byte| format!("{byte:02x}")).collect::<String>();
    let captured = &bytes[..bytes.len().min(RAW_IDENTIFY_LIMIT)];
    // Capture before any fallible decoding or signature/identity validation.
    let mut evidence = json!({
        "basis": "fixture_separate_authenticated_identify_exchange",
        "protocol": "/ipfs/id/1.0.0",
        "authenticated_remote_peer_id": authenticated_peer.to_string(),
        "status": "failed",
        "signed_peer_record_verified": false,
        "legacy_validation": {"status": "not_attempted"},
        "interop_validation": {"status": "not_attempted"},
        "raw_protobuf_hex": hex(captured),
        "raw_protobuf_sha256": format!("{:x}", Sha256::digest(captured)),
        "raw_protobuf_bytes": captured.len(),
        "raw_capture_truncated": captured.len() != bytes.len(),
    });
    let validation = (|| -> Result<(), Box<dyn Error>> {
        let (key_bytes, envelope_bytes) = raw_identify_fields(bytes)?;
        evidence["signed_envelope_hex"] = json!(hex(envelope_bytes));
        evidence["signed_envelope_sha256"] = json!(format!("{:x}", Sha256::digest(envelope_bytes)));
        let key = identity::PublicKey::try_decode_protobuf(key_bytes)?;
        let envelope = SignedEnvelope::from_protobuf_encoding(envelope_bytes)?;
        let (record, domain, payload_type, format) = match PeerRecord::from_signed_envelope(envelope.clone()) {
            Ok(record) => {
                evidence["legacy_validation"] = json!({"status": "verified"});
                (record, "libp2p-routing-state", b"/libp2p/routing-state-record".as_slice(), "legacy")
            }
            Err(error) => {
                evidence["legacy_validation"] = json!({"status": "rejected", "error": format!("{error:?}")});
                let record = match PeerRecord::from_signed_envelope_interop(envelope.clone()) {
                    Ok(record) => {
                        evidence["interop_validation"] = json!({"status": "verified"});
                        record
                    }
                    Err(error) => {
                        evidence["interop_validation"] = json!({"status": "rejected", "error": format!("{error:?}")});
                        return Err(error.into());
                    }
                };
                (record, "libp2p-peer-record", STANDARD_PAYLOAD_TYPE, "standard")
            }
        };
        let (_, signer) = envelope.payload_and_signing_key(domain.to_owned(), payload_type)?;
        evidence["identify_public_key_peer_id"] = json!(key.to_peer_id().to_string());
        evidence["signer_peer_id"] = json!(signer.to_peer_id().to_string());
        evidence["record_peer_id"] = json!(record.peer_id().to_string());
        if signer != &key || key.to_peer_id() != authenticated_peer || record.peer_id() != authenticated_peer {
            return Err("Identify signer, record, public key and authenticated peer disagree".into());
        }
        evidence["envelope_format"] = json!(format);
        evidence["domain"] = json!(domain);
        evidence["payload_type_hex"] = json!(hex(payload_type));
        evidence["record_sequence"] = json!(record.seq());
        evidence["record_addresses"] = json!(record.addresses().iter().map(ToString::to_string).collect::<Vec<_>>());
        Ok(())
    })();
    match validation {
        Ok(()) => {
            evidence["status"] = json!("verified");
            evidence["signed_peer_record_verified"] = json!(true);
            evidence["error"] = serde_json::Value::Null;
        }
        Err(error) => evidence["error"] = json!(format!("{error:?}")),
    }
    evidence
}

async fn exchange_raw_identify(
    swarm: &mut libp2p::Swarm<Behaviour>, peer: PeerId,
    observer: &upgrade_observer::Observer,
) -> Result<serde_json::Value, Box<dyn Error>> {
    if !swarm.is_connected(&peer) {
        return Err("raw Identify requires an established authenticated connection".into());
    }
    let mut control = swarm.behaviour().stream.new_control();
    let attempt = observer.application(peer, application_observer::IDENTIFY);
    let exchange = async {
        let stream = control.open_stream(peer, StreamProtocol::new("/ipfs/id/1.0.0")).await?;
        let mut stream = attempt.wrap(stream);
        let bytes = read_raw_identify_frame(&mut stream).await?;
        // Like pinned recv_identify, drop after the frame rather than waiting
        // for a close ACK from a peer that may already have dropped its writer.
        stream.complete();
        Ok::<_, Box<dyn Error>>(raw_identify_evidence(&bytes, peer))
    };
    tokio::pin!(exchange);
    let deadline = tokio::time::sleep(Duration::from_secs(15));
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            biased;
            _ = &mut deadline => return Err("raw Identify exchange deadline expired".into()),
            result = &mut exchange => return result,
            event = swarm.select_next_some() => match event {
                SwarmEvent::NewListenAddr { address, .. } => swarm.add_external_address(address),
                SwarmEvent::ConnectionClosed { peer_id, num_established: 0, .. } if peer_id == peer => {
                    return Err("authenticated connection closed during raw Identify".into());
                }
                _ => {}
            },
        }
    }
}

async fn open_echo_stream_direct(
    swarm: &mut libp2p::Swarm<Behaviour>,
    peer: PeerId,
    payload: &[u8],
    observer: &upgrade_observer::Observer,
) -> Result<usize, Box<dyn Error>> {
    let mut control = swarm.behaviour().stream.new_control();
    let attempt = observer.application(peer, application_observer::ECHO);
    let exchange = async {
        let stream = control.open_stream(peer, StreamProtocol::new(application_observer::ECHO)).await?;
        let mut stream = attempt.wrap(stream);
        write_frame(&mut stream, payload).await?;
        let echoed = read_frame(&mut stream).await?;
        stream.close().await?;
        if echoed != payload { return Err("echo mismatch".into()); }
        stream.complete();
        Ok::<_, Box<dyn Error>>(echoed.len())
    };
    tokio::pin!(exchange);
    let deadline = tokio::time::sleep(Duration::from_secs(15));
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            result = &mut exchange => return result,
            _ = &mut deadline => {
                return Err("timed out completing echo stream".into());
            }
            event = swarm.select_next_some() => {
                if let SwarmEvent::NewListenAddr { address, .. } = event {
                    swarm.add_external_address(address);
                }
            }
        }
    }
}

async fn expect_unknown_stream_rejection(
    swarm: &mut libp2p::Swarm<Behaviour>,
    peer: PeerId,
    protocol: &'static str,
) -> Result<String, Box<dyn Error>> {
    let mut control = swarm.behaviour().stream.new_control();
    let mut open = Box::pin(control.open_stream(peer, StreamProtocol::new(protocol)));
    let deadline = tokio::time::sleep(Duration::from_secs(15));
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            result = &mut open => {
                match result {
                    Ok(mut stream) => {
                        let _ = stream.close().await;
                        return Err(format!("{protocol} unexpectedly opened").into());
                    }
                    Err(error) => return Ok(error.to_string()),
                }
            }
            _ = &mut deadline => {
                return Err(format!("timed out waiting for {protocol} rejection").into());
            }
            event = swarm.select_next_some() => {
                if let SwarmEvent::NewListenAddr { address, .. } = event {
                    swarm.add_external_address(address);
                }
            }
        }
    }
}

struct DhtFindPeerEvidence {
    closest_peers: usize,
    requests: u32,
    successes: u32,
    failures: u32,
}

async fn wait_dht_remote_ready(
    swarm: &mut libp2p::Swarm<Behaviour>,
    remote_peer: PeerId,
) -> Result<(), Box<dyn Error>> {
    let mut routing_admitted = false;
    let mut amino_advertised = false;
    let deadline = tokio::time::sleep(Duration::from_secs(20));
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            _ = &mut deadline => {
                return Err(format!(
                    "timed out waiting for Kademlia readiness: routing_admitted={routing_admitted}, \
                     amino_advertised={amino_advertised}"
                ).into());
            }
            event = swarm.select_next_some() => {
                match event {
                    SwarmEvent::Behaviour(BehaviourEvent::Kad(kad::Event::RoutingUpdated {
                        peer,
                        ..
                    })) if peer == remote_peer => {
                        routing_admitted = true;
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Identify(identify::Event::Received {
                        peer_id,
                        info,
                        ..
                    })) if peer_id == remote_peer => {
                        amino_advertised = info
                            .protocols
                            .iter()
                            .any(|protocol| protocol.as_ref() == "/ipfs/kad/1.0.0");
                    }
                    SwarmEvent::NewListenAddr { address, .. } => swarm.add_external_address(address),
                    _ => {}
                }
                if routing_admitted && amino_advertised {
                    return Ok(());
                }
            }
        }
    }
}

async fn wait_dht_find_peer(
    swarm: &mut libp2p::Swarm<Behaviour>,
    remote_peer: PeerId,
) -> Result<DhtFindPeerEvidence, Box<dyn Error>> {
    let mut query = None;
    let mut routing_admitted = false;
    let mut amino_advertised = false;
    let deadline = tokio::time::sleep(Duration::from_secs(20));
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            _ = &mut deadline => return Err("timed out waiting for Kademlia routing admission or closest peers".into()),
            event = swarm.select_next_some() => {
                match event {
                    SwarmEvent::Behaviour(BehaviourEvent::Kad(kad::Event::RoutingUpdated {
                        peer,
                        ..
                    })) if peer == remote_peer => {
                        routing_admitted = true;
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Identify(identify::Event::Received {
                        peer_id,
                        info,
                        ..
                    })) if peer_id == remote_peer => {
                        amino_advertised = info
                            .protocols
                            .iter()
                            .any(|protocol| protocol.as_ref() == "/ipfs/kad/1.0.0");
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Kad(kad::Event::OutboundQueryProgressed {
                        id: event_id,
                        result: kad::QueryResult::GetClosestPeers(Ok(ok)),
                        stats,
                        ..
                    })) if Some(event_id) == query => {
                        if !ok.peers.iter().any(|peer| peer.peer_id == remote_peer) {
                            return Err("Kademlia closest peers result omitted the queried Forge peer".into());
                        }
                        if stats.num_requests() == 0 || stats.num_successes() == 0 || stats.num_failures() != 0 {
                            return Err(format!(
                                "Kademlia query evidence was incomplete: requests={}, successes={}, failures={}",
                                stats.num_requests(),
                                stats.num_successes(),
                                stats.num_failures(),
                            ).into());
                        }
                        return Ok(DhtFindPeerEvidence {
                            closest_peers: ok.peers.len(),
                            requests: stats.num_requests(),
                            successes: stats.num_successes(),
                            failures: stats.num_failures(),
                        });
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Kad(kad::Event::OutboundQueryProgressed {
                        id: event_id,
                        result: kad::QueryResult::GetClosestPeers(Err(error)),
                        ..
                    })) if Some(event_id) == query => {
                        return Err(format!("Kademlia closest peers failed: {error:?}").into());
                    }
                    SwarmEvent::NewListenAddr { address, .. } => swarm.add_external_address(address),
                    _ => {}
                }
                if routing_admitted && amino_advertised && query.is_none() {
                    query = Some(swarm.behaviour_mut().kad.get_closest_peers(remote_peer));
                }
            }
        }
    }
}

struct DhtHiddenPeerEvidence {
    target_dialed: bool,
    target_connected: bool,
    routing_updated: bool,
    address_count: usize,
    followup_exact_rpc: bool,
    closest_peers: usize,
}

async fn wait_dht_hidden_peer(
    swarm: &mut libp2p::Swarm<Behaviour>,
    target_peer: PeerId,
) -> Result<DhtHiddenPeerEvidence, Box<dyn Error>> {
    if swarm.is_connected(&target_peer) {
        return Err("hidden target was connected before the Kademlia query".into());
    }
    if swarm.behaviour_mut().kad.kbuckets().any(|bucket| {
        bucket
            .iter()
            .any(|entry| entry.node.key.preimage() == &target_peer)
    }) {
        return Err("hidden target was present in Kademlia kbuckets before the query".into());
    }

    let initial_query = swarm.behaviour_mut().kad.get_closest_peers(target_peer);
    let deadline = tokio::time::sleep(Duration::from_secs(20));
    tokio::pin!(deadline);
    let mut initial_complete = false;
    let mut target_dial_connections = Vec::new();
    let mut target_dialed = false;
    let mut target_connected = false;
    let mut routing_updated = false;
    let mut address_count = 0usize;
    let mut closest_peers = 0usize;
    let mut followup_query = None;

    loop {
        tokio::select! {
            _ = &mut deadline => return Err(format!(
                "timed out waiting for hidden Kademlia peer: initial_complete={initial_complete}, \
                 target_dialed={target_dialed}, target_connected={target_connected}, \
                 routing_updated={routing_updated}, address_count={address_count}, \
                 followup_query_started={}, closest_peers={closest_peers}",
                followup_query.is_some(),
            ).into()),
            event = swarm.select_next_some() => {
                match event {
                    SwarmEvent::Dialing {
                        peer_id,
                        connection_id,
                    } => {
                        eprintln!(
                            "rust-hidden-dht dialing: peer_id={peer_id:?}, connection_id={connection_id:?}"
                        );
                        if peer_id.as_ref() == Some(&target_peer) {
                            target_dialed = true;
                            if !target_dial_connections.contains(&connection_id) {
                                target_dial_connections.push(connection_id);
                            }
                        }
                    }
                    SwarmEvent::ConnectionEstablished {
                        peer_id,
                        connection_id,
                        endpoint,
                        ..
                    } if peer_id == target_peer => {
                        const EVENT: &str = "rust-hidden-dht target connected";
                        let observed_target_dial = target_dial_connections.contains(&connection_id);
                        eprintln!(
                            "{EVENT}: connection_id={connection_id:?}, endpoint={endpoint:?}, \
                             observed_target_dial={observed_target_dial}"
                        );
                        if observed_target_dial {
                            target_connected = true;
                        }
                    }
                    SwarmEvent::OutgoingConnectionError {
                        peer_id,
                        connection_id,
                        error,
                    } if peer_id.as_ref() == Some(&target_peer)
                        || target_dial_connections.contains(&connection_id) =>
                    {
                        eprintln!(
                            "rust-hidden-dht target dial failed: peer_id={peer_id:?}, \
                             connection_id={connection_id:?}, error={error:?}"
                        );
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Kad(kad::Event::RoutingUpdated {
                        peer,
                        addresses,
                        ..
                    })) if peer == target_peer => {
                        address_count = addresses.len();
                        routing_updated = address_count > 0;
                        eprintln!(
                            "rust-hidden-dht target routing updated: address_count={address_count}"
                        );
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Kad(kad::Event::OutboundQueryProgressed {
                        id,
                        result: kad::QueryResult::GetClosestPeers(Ok(ok)),
                        step,
                        ..
                    })) if id == initial_query && step.last => {
                        initial_complete = true;
                        closest_peers = ok.peers.len();
                        eprintln!(
                            "rust-hidden-dht initial query completed: id={id:?}, \
                             closest_peers={closest_peers}"
                        );
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Kad(kad::Event::OutboundQueryProgressed {
                        id,
                        result: kad::QueryResult::GetClosestPeers(Err(error)),
                        ..
                    })) if id == initial_query => {
                        eprintln!("rust-hidden-dht initial query failed: id={id:?}, error={error:?}");
                        return Err(format!("hidden Kademlia lookup failed: {error:?}").into());
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Kad(kad::Event::OutboundQueryProgressed {
                        id,
                        result: kad::QueryResult::GetClosestPeers(Ok(ok)),
                        ..
                    })) if Some(id) == followup_query => {
                        let found_target = ok.peers.iter().any(|peer| peer.peer_id == target_peer);
                        eprintln!(
                            "rust-hidden-dht follow-up query completed: id={id:?}, \
                             closest_peers={}, found_target={found_target}",
                            ok.peers.len(),
                        );
                        if !found_target {
                            return Err("follow-up Kademlia query did not contain the hidden target".into());
                        }
                        return Ok(DhtHiddenPeerEvidence {
                            target_dialed,
                            target_connected,
                            routing_updated,
                            address_count,
                            followup_exact_rpc: true,
                            closest_peers,
                        });
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Kad(kad::Event::OutboundQueryProgressed {
                        id,
                        result: kad::QueryResult::GetClosestPeers(Err(error)),
                        ..
                    })) if Some(id) == followup_query => {
                        eprintln!("rust-hidden-dht follow-up query failed: id={id:?}, error={error:?}");
                        return Err(format!("follow-up Kademlia query failed: {error:?}").into());
                    }
                    SwarmEvent::NewListenAddr { address, .. } => swarm.add_external_address(address),
                    _ => {}
                }

                if initial_complete
                    && target_dialed
                    && target_connected
                    && routing_updated
                    && followup_query.is_none()
                {
                    followup_query = Some(
                        swarm
                            .behaviour_mut()
                            .kad
                            .get_n_closest_peers(target_peer, NonZeroUsize::new(1).unwrap()),
                    );
                }
            }
        }
    }
}

async fn wait_dht_put_get(
    swarm: &mut libp2p::Swarm<Behaviour>,
    scenario: &str,
    operation: &str,
) -> Result<usize, Box<dyn Error>> {
    let (key, expected) = dht_value_fixture(scenario)?;
    let mut put_id = None;
    let mut get_id = None;
    if operation == "get_only" {
        get_id = Some(swarm.behaviour_mut().kad.get_record(key.clone()));
    } else {
        put_id = Some(swarm.behaviour_mut().kad.put_record(
            kad::Record::new(key.clone(), expected.clone()),
            kad::Quorum::One,
        )?);
    }
    let deadline = tokio::time::sleep(Duration::from_secs(30));
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            _ = &mut deadline => return Err("timed out waiting for Kademlia value proof".into()),
            event = swarm.select_next_some() => {
                match event {
                    SwarmEvent::Behaviour(BehaviourEvent::Kad(kad::Event::OutboundQueryProgressed {
                        id,
                        result: kad::QueryResult::PutRecord(result),
                        ..
                    })) if Some(id) == put_id => {
                        result?;
                        if operation == "put_only" {
                            return Ok(expected.len());
                        }
                        get_id = Some(swarm.behaviour_mut().kad.get_record(key.clone()));
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Kad(kad::Event::OutboundQueryProgressed {
                        id,
                        result: kad::QueryResult::GetRecord(Ok(kad::GetRecordOk::FoundRecord(found))),
                        ..
                    })) if Some(id) == get_id => {
                        if found.record.key != key || found.record.value != expected {
                            return Err("Kademlia GetRecord returned a different value".into());
                        }
                        return Ok(found.record.value.len());
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Kad(kad::Event::OutboundQueryProgressed {
                        id,
                        result: kad::QueryResult::GetRecord(Err(error)),
                        ..
                    })) if Some(id) == get_id => {
                        return Err(format!("Kademlia GetRecord failed: {error:?}").into());
                    }
                    SwarmEvent::NewListenAddr { address, .. } => swarm.add_external_address(address),
                    _ => {}
                }
            }
        }
    }
}

async fn wait_rendezvous_register_discover(
    swarm: &mut libp2p::Swarm<Behaviour>,
    remote_peer: PeerId,
) -> Result<RendezvousRegisterDiscoverEvidence, Box<dyn Error>> {
    let namespace = rendezvous::Namespace::new("forge.discovery".to_string())?;
    let local_peer = *swarm.local_peer_id();
    let mut expected_addresses = swarm
        .external_addresses()
        .map(ToString::to_string)
        .collect::<Vec<_>>();
    if expected_addresses.is_empty() {
        let deadline = tokio::time::sleep(Duration::from_secs(20));
        tokio::pin!(deadline);
        while expected_addresses.is_empty() {
            tokio::select! {
                _ = &mut deadline => return Err("rendezvous registration has no confirmed local address".into()),
                event = swarm.select_next_some() => match event {
                    SwarmEvent::NewListenAddr { address, .. } => {
                        swarm.add_external_address(address);
                        expected_addresses = swarm
                            .external_addresses()
                            .map(ToString::to_string)
                            .collect();
                    }
                    _ => {}
                }
            }
        }
    }
    expected_addresses.sort();
    swarm
        .behaviour_mut()
        .rendezvous_client
        .register(namespace.clone(), remote_peer, None)?;
    let deadline = tokio::time::sleep(Duration::from_secs(30));
    tokio::pin!(deadline);
    let mut registered = false;
    loop {
        tokio::select! {
            _ = &mut deadline => return Err("timed out waiting for rendezvous register/discover".into()),
            event = swarm.select_next_some() => {
                match event {
                    SwarmEvent::Behaviour(BehaviourEvent::RendezvousClient(
                        rendezvous::client::Event::Registered { rendezvous_node, ttl, .. },
                    )) if rendezvous_node == remote_peer => {
                        if ttl != 7_200 {
                            return Err(format!("rendezvous register returned TTL {ttl}, expected 7200").into());
                        }
                        registered = true;
                        swarm.behaviour_mut().rendezvous_client.discover(
                            Some(namespace.clone()),
                            None,
                            Some(10),
                            remote_peer,
                        );
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::RendezvousClient(
                        rendezvous::client::Event::Discovered { rendezvous_node, registrations, cookie },
                    )) if registered && rendezvous_node == remote_peer => {
                        if registrations.len() != 1 {
                            return Err(format!("rendezvous discover returned {} registrations, expected one", registrations.len()).into());
                        }
                        let registration = registrations.into_iter().next().expect("one registration is present");
                        let mut record_addresses = registration
                            .record
                            .addresses()
                            .iter()
                            .map(ToString::to_string)
                            .collect::<Vec<_>>();
                        record_addresses.sort();
                        if registration.namespace.to_string() != "forge.discovery"
                            || registration.record.peer_id() != local_peer
                            || registration.record.seq() == 0
                            || record_addresses != expected_addresses
                            || registration.ttl != 7_200
                        {
                            return Err("rendezvous signed record did not match local peer, namespace, addresses or TTL".into());
                        }
                        let cookie_bytes = cookie.into_wire_encoding().len();
                        if cookie_bytes == 0 {
                            return Err("rendezvous discover returned an empty cookie".into());
                        }
                        return Ok(RendezvousRegisterDiscoverEvidence {
                            wire_registration_count: 1,
                            record_sequence: registration.record.seq(),
                            record_address_count: record_addresses.len(),
                            registered_ttl_seconds: 7_200,
                            discovered_ttl_seconds: registration.ttl,
                            cookie_bytes,
                        });
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::RendezvousClient(
                        rendezvous::client::Event::RegisterFailed { error, .. },
                    )) => return Err(format!("rendezvous register failed: {error:?}").into()),
                    SwarmEvent::NewListenAddr { address, .. } => swarm.add_external_address(address),
                    _ => {}
                }
            }
        }
    }
}

async fn wait_rendezvous_registered(
    swarm: &mut libp2p::Swarm<Behaviour>,
    remote_peer: PeerId,
) -> Result<u64, Box<dyn Error>> {
    let deadline = tokio::time::sleep(Duration::from_secs(20));
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            _ = &mut deadline => return Err("timed out waiting for rendezvous registration".into()),
            event = swarm.select_next_some() => match event {
                SwarmEvent::Behaviour(BehaviourEvent::RendezvousClient(
                    rendezvous::client::Event::Registered { rendezvous_node, ttl, .. },
                )) if rendezvous_node == remote_peer => {
                    if ttl == 0 || ttl > 3 {
                        return Err(format!("rendezvous server returned unbounded TTL {ttl}").into());
                    }
                    return Ok(ttl);
                }
                SwarmEvent::Behaviour(BehaviourEvent::RendezvousClient(
                    rendezvous::client::Event::RegisterFailed { error, .. },
                )) => return Err(format!("rendezvous register failed: {error:?}").into()),
                SwarmEvent::NewListenAddr { address, .. } => swarm.add_external_address(address),
                _ => {}
            }
        }
    }
}

async fn wait_rendezvous_discovered(
    swarm: &mut libp2p::Swarm<Behaviour>,
    remote_peer: PeerId,
) -> Result<(Vec<rendezvous::Registration>, rendezvous::Cookie), Box<dyn Error>> {
    let deadline = tokio::time::sleep(Duration::from_secs(20));
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            _ = &mut deadline => return Err("timed out waiting for rendezvous discovery".into()),
            event = swarm.select_next_some() => match event {
                SwarmEvent::Behaviour(BehaviourEvent::RendezvousClient(
                    rendezvous::client::Event::Discovered { rendezvous_node, registrations, cookie },
                )) if rendezvous_node == remote_peer => return Ok((registrations, cookie)),
                SwarmEvent::Behaviour(BehaviourEvent::RendezvousClient(
                    rendezvous::client::Event::DiscoverFailed { error, .. },
                )) => return Err(format!("rendezvous discover failed: {error:?}").into()),
                SwarmEvent::NewListenAddr { address, .. } => swarm.add_external_address(address),
                _ => {}
            }
        }
    }
}

fn rendezvous_lifecycle_ttl(ttl: u64, stage: &str) -> Result<Duration, Box<dyn Error>> {
    if !(RENDEZVOUS_LIFECYCLE_MIN_TTL_SECONDS..=RENDEZVOUS_LIFECYCLE_MAX_TTL_SECONDS).contains(&ttl)
    {
        return Err(format!(
            "rendezvous {stage} TTL {ttl} cannot support bounded renewal evidence"
        )
        .into());
    }
    Ok(Duration::from_secs(ttl))
}

fn rendezvous_expiry_deadline(
    registered_at: Instant,
    ttl: Duration,
    stage: &str,
) -> Result<Instant, Box<dyn Error>> {
    registered_at
        .checked_add(ttl)
        .and_then(|expiry| expiry.checked_sub(RENDEZVOUS_LIFECYCLE_TIMING_MARGIN))
        .ok_or_else(|| -> Box<dyn Error> {
            format!("rendezvous {stage} expiry deadline overflowed").into()
        })
}

async fn sleep_until_monotonic(deadline: Instant) {
    let now = Instant::now();
    if now < deadline {
        tokio::time::sleep(deadline.duration_since(now)).await;
    }
}

fn rendezvous_registration_count(
    registrations: &[rendezvous::Registration],
    peer: PeerId,
    sequence: u64,
    endpoint: Option<&Multiaddr>,
) -> usize {
    registrations
        .iter()
        .filter(|registration| {
            registration.record.peer_id() == peer
                && registration.record.seq() == sequence
                && endpoint
                    .is_none_or(|expected| registration.record.addresses().contains(expected))
        })
        .count()
}

fn rendezvous_peer_registration_count(
    registrations: &[rendezvous::Registration],
    peer: PeerId,
) -> usize {
    registrations
        .iter()
        .filter(|registration| registration.record.peer_id() == peer)
        .count()
}

async fn wait_rendezvous_lifecycle(
    swarm: &mut libp2p::Swarm<Behaviour>,
    remote_peer: PeerId,
) -> Result<RendezvousLifecycleEvidence, Box<dyn Error>> {
    let namespace = rendezvous::Namespace::new("forge.discovery".to_string())?;
    let local_peer = *swarm.local_peer_id();
    swarm
        .behaviour_mut()
        .rendezvous_client
        .register(namespace.clone(), remote_peer, Some(3))?;
    let initial_ttl = wait_rendezvous_registered(swarm, remote_peer).await?;
    let _ = rendezvous_lifecycle_ttl(initial_ttl, "initial")?;
    swarm.behaviour_mut().rendezvous_client.discover(
        Some(namespace.clone()),
        None,
        Some(10),
        remote_peer,
    );
    let (initial_registrations, initial_cookie) =
        wait_rendezvous_discovered(swarm, remote_peer).await?;
    let initial = initial_registrations
        .iter()
        .find(|registration| registration.record.peer_id() == local_peer)
        .ok_or("initial signed legacy peer record was not discoverable")?;
    let initial_sequence = initial.record.seq();
    let initial_visible_count =
        rendezvous_registration_count(&initial_registrations, local_peer, initial_sequence, None);
    if initial_visible_count != 1 {
        return Err(
            "rendezvous initial discovery did not contain exactly one matching record".into(),
        );
    }

    tokio::time::sleep(Duration::from_secs(1)).await;
    let updated_address = Multiaddr::empty()
        .with(Protocol::Ip4(Ipv4Addr::LOCALHOST))
        .with(Protocol::Udp(20_001))
        .with(Protocol::QuicV1);
    swarm.add_external_address(updated_address.clone());
    let updated_requested_at = Instant::now();
    swarm.behaviour_mut().rendezvous_client.register(
        namespace.clone(),
        remote_peer,
        Some(initial_ttl),
    )?;
    let updated_ttl = wait_rendezvous_registered(swarm, remote_peer).await?;
    let updated_confirmed_at = Instant::now();
    let updated_ttl_duration = rendezvous_lifecycle_ttl(updated_ttl, "updated")?;
    swarm.behaviour_mut().rendezvous_client.discover(
        Some(namespace.clone()),
        Some(initial_cookie.clone()),
        Some(10),
        remote_peer,
    );
    let (delta_registrations, delta_cookie) =
        wait_rendezvous_discovered(swarm, remote_peer).await?;
    let updated = delta_registrations
        .iter()
        .find(|registration| registration.record.peer_id() == local_peer)
        .ok_or("rendezvous cookie delta did not contain the updated peer record")?;
    let updated_sequence = updated.record.seq();
    if updated_sequence <= initial_sequence
        || !updated.record.addresses().contains(&updated_address)
        || delta_cookie == initial_cookie
    {
        return Err(
            "rendezvous update did not advance sequence, address and cookie together".into(),
        );
    }
    let updated_visible_count = rendezvous_registration_count(
        &delta_registrations,
        local_peer,
        updated_sequence,
        Some(&updated_address),
    );
    if updated_visible_count != 1 {
        return Err("rendezvous cookie delta did not contain exactly one updated record".into());
    }

    let original_renewal_deadline =
        rendezvous_expiry_deadline(updated_requested_at, updated_ttl_duration, "original")?;
    let renewal_due = updated_confirmed_at + updated_ttl_duration / 2;
    if renewal_due >= original_renewal_deadline {
        return Err("rendezvous original TTL left no bounded renewal window".into());
    }
    sleep_until_monotonic(renewal_due).await;
    let renewed_requested_at = Instant::now();
    if renewed_requested_at >= original_renewal_deadline {
        return Err("rendezvous renewal started after original expiry window".into());
    }
    swarm.behaviour_mut().rendezvous_client.register(
        namespace.clone(),
        remote_peer,
        Some(updated_ttl),
    )?;
    let renewed_ttl = wait_rendezvous_registered(swarm, remote_peer).await?;
    let renewed_confirmed_at = Instant::now();
    let renewed_ttl_duration = rendezvous_lifecycle_ttl(renewed_ttl, "renewed")?;
    if renewed_confirmed_at >= original_renewal_deadline {
        return Err("rendezvous renewal was not confirmed before original expiry".into());
    }

    let original_expiry_latest = updated_confirmed_at + updated_ttl_duration;
    let renewed_visibility_time = original_expiry_latest + RENDEZVOUS_LIFECYCLE_TIMING_MARGIN;
    let renewed_visibility_deadline =
        rendezvous_expiry_deadline(renewed_requested_at, renewed_ttl_duration, "renewed")?;
    if renewed_visibility_time >= renewed_visibility_deadline {
        return Err("rendezvous TTL left no bounded post-expiry renewal proof window".into());
    }
    sleep_until_monotonic(renewed_visibility_time).await;
    let renewed_visibility_started_at = Instant::now();
    if renewed_visibility_started_at >= renewed_visibility_deadline {
        return Err("rendezvous renewal visibility probe started after renewed expiry".into());
    }
    swarm.behaviour_mut().rendezvous_client.discover(
        Some(namespace.clone()),
        None,
        Some(10),
        remote_peer,
    );
    let (renewed_registrations, _) = wait_rendezvous_discovered(swarm, remote_peer).await?;
    let renewed_visibility_completed_at = Instant::now();
    if renewed_visibility_completed_at >= renewed_visibility_deadline {
        return Err("rendezvous renewal visibility probe completed after renewed expiry".into());
    }
    let renewed = renewed_registrations
        .iter()
        .find(|registration| {
            registration.record.peer_id() == local_peer
                && registration.record.addresses().contains(&updated_address)
        })
        .ok_or("rendezvous renewed record was not visible after original expiry")?;
    let renewed_sequence = renewed.record.seq();
    if renewed_sequence <= updated_sequence {
        return Err("rendezvous renewal did not advance the signed peer record sequence".into());
    }
    let renewed_visible_count = rendezvous_registration_count(
        &renewed_registrations,
        local_peer,
        renewed_sequence,
        Some(&updated_address),
    );
    if renewed_visible_count != 1 {
        return Err(
            "rendezvous renewal discovery did not contain exactly one renewed record".into(),
        );
    }

    sleep_until_monotonic(
        renewed_confirmed_at + renewed_ttl_duration + RENDEZVOUS_LIFECYCLE_TIMING_MARGIN,
    )
    .await;
    swarm.behaviour_mut().rendezvous_client.discover(
        Some(namespace.clone()),
        None,
        Some(10),
        remote_peer,
    );
    let (expired_registrations, _) = wait_rendezvous_discovered(swarm, remote_peer).await?;
    let expired_registration_count =
        rendezvous_peer_registration_count(&expired_registrations, local_peer);
    if expired_registration_count != 0 {
        return Err("rendezvous registration survived its returned TTL".into());
    }

    let pre_unregister_requested_at = Instant::now();
    swarm.behaviour_mut().rendezvous_client.register(
        namespace.clone(),
        remote_peer,
        Some(renewed_ttl),
    )?;
    let pre_unregister_ttl = wait_rendezvous_registered(swarm, remote_peer).await?;
    let pre_unregister_ttl_duration =
        rendezvous_lifecycle_ttl(pre_unregister_ttl, "pre-unregister")?;
    let pre_unregister_deadline = rendezvous_expiry_deadline(
        pre_unregister_requested_at,
        pre_unregister_ttl_duration,
        "pre-unregister",
    )?;
    swarm.behaviour_mut().rendezvous_client.discover(
        Some(namespace.clone()),
        None,
        Some(10),
        remote_peer,
    );
    let (pre_unregister_registrations, _) = wait_rendezvous_discovered(swarm, remote_peer).await?;
    let pre_unregister = pre_unregister_registrations
        .iter()
        .find(|registration| {
            registration.record.peer_id() == local_peer
                && registration.record.addresses().contains(&updated_address)
        })
        .ok_or("rendezvous registration was not visible before unregister")?;
    let pre_unregister_sequence = pre_unregister.record.seq();
    if pre_unregister_sequence <= renewed_sequence {
        return Err(
            "rendezvous pre-unregister registration did not advance the signed peer record sequence".into(),
        );
    }
    let pre_unregister_count = rendezvous_registration_count(
        &pre_unregister_registrations,
        local_peer,
        pre_unregister_sequence,
        Some(&updated_address),
    );
    if pre_unregister_count != 1 {
        return Err(
            "rendezvous pre-unregister discovery did not contain exactly one matching record"
                .into(),
        );
    }
    swarm
        .behaviour_mut()
        .rendezvous_client
        .unregister(namespace.clone(), remote_peer);
    swarm
        .behaviour_mut()
        .rendezvous_client
        .discover(Some(namespace), None, Some(10), remote_peer);
    let (final_registrations, _) = wait_rendezvous_discovered(swarm, remote_peer).await?;
    if Instant::now() >= pre_unregister_deadline {
        return Err("rendezvous unregister proof exceeded the registration TTL window".into());
    }
    let final_registration_count =
        rendezvous_peer_registration_count(&final_registrations, local_peer);
    if final_registration_count != 0 {
        return Err("rendezvous unregister did not remove the confirmed registration".into());
    }
    Ok(RendezvousLifecycleEvidence {
        initial_ttl_seconds: initial_ttl,
        updated_ttl_seconds: updated_ttl,
        renewed_ttl_seconds: renewed_ttl,
        initial_record_sequence: initial_sequence,
        updated_record_sequence: updated_sequence,
        renewed_record_sequence: renewed_sequence,
        pre_unregister_record_sequence: pre_unregister_sequence,
        initial_cookie_bytes: initial_cookie.into_wire_encoding().len(),
        delta_cookie_bytes: delta_cookie.into_wire_encoding().len(),
        initial_visible_count,
        updated_visible_count,
        renewed_visible_after_original_expiry: true,
        renewed_visible_count,
        expired_registration_count,
        pre_unregister_count,
        final_registration_count,
    })
}

async fn wait_gossipsub_peer_and_publish(
    swarm: &mut libp2p::Swarm<Behaviour>,
    remote_peer: PeerId,
    payload: &[u8],
) -> Result<(), Box<dyn Error>> {
    let topic = gossipsub::IdentTopic::new(PUBSUB_TOPIC);
    swarm.behaviour_mut().gossipsub.subscribe(&topic)?;
    let deadline = tokio::time::sleep(Duration::from_secs(10));
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            _ = &mut deadline => return Err("timed out waiting for gossipsub subscription exchange".into()),
            event = swarm.select_next_some() => {
                match event {
                    SwarmEvent::Behaviour(BehaviourEvent::Gossipsub(
                        gossipsub::Event::Subscribed { peer_id, topic: subscribed_topic },
                    )) if peer_id == remote_peer && subscribed_topic == topic.hash() => {
                        swarm.behaviour_mut().gossipsub.publish(topic.clone(), payload)?;
                        tokio::time::sleep(Duration::from_secs(2)).await;
                        return Ok(());
                    }
                    SwarmEvent::NewListenAddr { address, .. } => swarm.add_external_address(address),
                    _ => {}
                }
            }
        }
    }
}

fn write_relay_listener_result(
    path: &PathBuf,
    peer: PeerId,
    addresses: &[String],
    acceptances: &[serde_json::Value],
    overflow: bool,
    started: u64,
    complete: bool,
) -> Result<(), Box<dyn Error>> {
    let temporary = path.with_extension("relay.tmp");
    write_json(&temporary, json!({
        "implementation": "rust", "role": "listener", "scenario": "relay_reserve",
        "status": if overflow { "overflow" } else { "ok" },
        "peer_id": peer.to_string(), "listen_addrs": addresses,
        "reservation_basis": "libp2p.relay.server.ReservationReqAccepted",
        "reservation_acceptances": acceptances, "trace_overflow": overflow,
        "trace_complete": complete, "started_at_unix_seconds": started,
        "observed_at_unix_seconds": SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs(),
    }))?;
    // Both provisional and final snapshots are read by another process.
    fs::rename(temporary, path)?;
    Ok(())
}

async fn listen(opts: Options) -> Result<(), Box<dyn Error>> {
    let mut swarm = new_swarm(&opts).await?;
    let pnet_application_streams = Arc::new(AtomicU64::new(0));
    spawn_incoming_stream_echo(
        &mut swarm,
        "/forge/interop/relay-echo/1",
        (opts.scenario == "pnet").then(|| Arc::clone(&pnet_application_streams)),
        &opts.tasks,
    )?;
    if opts.scenario == "gossipsub_publish" || opts.scenario == "gossipsub_mixed_mesh_stress" {
        let topic = gossipsub::IdentTopic::new(PUBSUB_TOPIC);
        swarm.behaviour_mut().gossipsub.subscribe(&topic)?;
    }
    let peer = *swarm.local_peer_id();
    let mut ready = false;
    let mut seeded = false;
    let mut dht_seed_dial_started = false;
    let mut dht_seed_query = None;
    let dht_seed_peer = if opts.scenario == "dht_hidden_find_peer" && !opts.seed_peer_id.is_empty()
    {
        Some(opts.seed_peer_id.parse::<PeerId>()?)
    } else {
        None
    };
    let mut payloads = HashSet::<String>::new();
    let mut duplicates = 0usize;
    let expected_record = if is_dht_value_scenario(&opts.scenario) {
        Some(dht_value_fixture(&opts.scenario)?)
    } else {
        None
    };
    let mut record_reported = false;
    let mut pnet_reported = false;
    let mut pnet_observation = PnetObservation::default();
    let relay_started = SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs();
    let mut relay_addresses = Vec::new();
    let mut relay_acceptances = Vec::new();
    let mut relay_trace_overflow = false;
    loop {
        tokio::select! {
            _ = tokio::time::sleep(Duration::from_millis(100)) => {
                if ready && !record_reported {
                    if let Some((key, expected)) = &expected_record {
                        let found = swarm
                            .behaviour_mut()
                            .kad
                            .store_mut()
                            .get(key)
                            .is_some_and(|record| record.value == *expected);
                        if found {
                            write_json(
                                &opts.result_file,
                                json!({
                                    "implementation": "rust",
                                    "role": "listener",
                                    "scenario": opts.scenario,
                                    "status": "ok",
                                    "record_persisted": true
                                }),
                            )?;
                            record_reported = true;
                        }
                    }
                }
                if ready && !seeded && opts.scenario == "gossipsub_mixed_mesh_stress" && opts.seed_file.exists() {
                    let seeds = fs::read_to_string(&opts.seed_file)?;
                    for line in seeds.lines().filter(|line| !line.is_empty()) {
                        if let Ok(address) = line.parse::<Multiaddr>() {
                            if !address.to_string().contains(&peer.to_string()) {
                                let _ = swarm.dial(address);
                            }
                        }
                    }
                    seeded = true;
                }
                if ready && !seeded && !dht_seed_dial_started && opts.scenario == "dht_hidden_find_peer" {
                    let seed_peer = dht_seed_peer.ok_or("dht_hidden_find_peer routing listener requires --seed-peer-id")?;
                    if opts.seed_addr.is_empty() || opts.result_file.as_os_str().is_empty() {
                        return Err("dht_hidden_find_peer routing listener requires --seed-addr and --result-file".into());
                    }
                    let seed_addr = opts.seed_addr.parse::<Multiaddr>()?;
                    if !seed_addr.to_string().contains(&seed_peer.to_string()) {
                        return Err("dht_hidden_find_peer seed address does not contain the supplied peer id".into());
                    }
                    swarm.dial(seed_addr)?;
                    dht_seed_dial_started = true;
                }
                if ready && opts.stop_file.exists() {
                    if opts.scenario == "relay_reserve" {
                        write_relay_listener_result(
                            &opts.result_file, peer, &relay_addresses, &relay_acceptances,
                            relay_trace_overflow, relay_started, true,
                        )?;
                    }
                    if !pnet_reported && opts.scenario == "pnet" && !opts.pnet_control.is_empty() {
                        pnet_observation.application_streams = pnet_application_streams.load(Ordering::Relaxed);
                        write_json(
                            &opts.result_file,
                            pnet_rejection(&opts, "listener", "", pnet_observation),
                        )?;
                    }
                    if opts.scenario == "gossipsub_mixed_mesh_stress" {
                        let status = if payloads.len() >= opts.expected_messages && duplicates == 0 {
                            "ok"
                        } else {
                            "mismatch"
                        };
                        write_json(
                            &opts.result_file,
                            json!({
                                "implementation": "rust",
                                "scenario": "gossipsub_mixed_mesh_stress",
                                "status": status,
                                "received": payloads.len(),
                                "expected": opts.expected_messages,
                                "duplicates": duplicates,
                                "payloads": payloads.iter().cloned().collect::<Vec<_>>()
                            }),
                        )?;
                    }
                    return Ok(());
                }
            }
            event = swarm.select_next_some() => {
                eprintln!("rust-listen event: {event:?}");
                if let SwarmEvent::ConnectionEstablished { connection_id, peer_id, endpoint, .. } = &event {
                    opts.upgrade_observer.established(connection_id, *peer_id, endpoint);
                }
                match event {
                    SwarmEvent::Behaviour(BehaviourEvent::Relay(
                        relay::Event::ReservationReqAccepted { src_peer_id, renewed },
                    )) if opts.scenario == "relay_reserve" => {
                        if relay_acceptances.len() < 16 {
                            relay_acceptances.push(json!({
                                "src_peer_id": src_peer_id.to_string(), "renewed": renewed,
                                "observed_at_unix_seconds": SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs(),
                            }));
                        } else {
                            relay_trace_overflow = true;
                        }
                        // The runner waits for this event, then joins us and reads the final snapshot.
                        write_relay_listener_result(
                            &opts.result_file, peer, &relay_addresses, &relay_acceptances,
                            relay_trace_overflow, relay_started, false,
                        )?;
                    }
                    SwarmEvent::IncomingConnection { .. } if opts.scenario == "pnet" => {
                        pnet_observation.attempted_connections += 1;
                    }
                    SwarmEvent::NewListenAddr { address, .. } => {
                        swarm.add_external_address(address.clone());
                        if opts.scenario == "relay_reserve" {
                            if relay_addresses.len() < 16 {
                                relay_addresses.push(format!("{address}/p2p/{peer}"));
                            } else {
                                relay_trace_overflow = true;
                            }
                        }
                        if !ready {
                            write_json(&opts.ready_file, json!({
                                "implementation": "rust",
                                "role": "listener",
                                "peer_id": peer.to_string(),
                                "listen_addrs": [format!("{address}/p2p/{peer}")],
                                "transport": opts.transport.clone(),
                                "autonat_v2_active": false,
                                "relay_service_active": swarm.behaviour().relay.is_enabled(),
                                "relay_client_active": swarm.behaviour().relay_client.is_enabled(),
                                "dcutr_active": swarm.behaviour().dcutr.is_enabled(),
                                "status": "ready"
                            }))?;
                            ready = true;
                        }
                    }
                    SwarmEvent::ConnectionEstablished { peer_id, endpoint, .. }
                        if opts.scenario == "pnet" && !pnet_reported =>
                    {
                        pnet_observation.established_connections += 1;
                        write_json(
                            &opts.result_file,
                            json!({
                                "implementation": "rust",
                                "role": "listener",
                                "scenario": "pnet",
                                "status": "ok",
                                "authenticated_remote_peer_id": peer_id.to_string(),
                                "negotiated_transport": "tcp",
                                "autonat_v2_active": false,
                                "relay_service_active": swarm.behaviour().relay.is_enabled(),
                                "relay_client_active": swarm.behaviour().relay_client.is_enabled(),
                                "dcutr_active": swarm.behaviour().dcutr.is_enabled(),
                                "pnet_enabled": true,
                                "negotiated_pnet": true,
                                "pnet_fingerprint": opts.pnet_fingerprint
                            }),
                        )?;
                        swarm.behaviour_mut().kad.add_address(&peer_id, transport_addr(endpoint.get_remote_address().clone()));
                        pnet_reported = true;
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Identify(identify::Event::Received { .. }))
                        if opts.scenario == "pnet" =>
                    {
                        pnet_observation.identify_streams += 1;
                    }
                    SwarmEvent::ConnectionEstablished { peer_id, endpoint, .. }
                        if opts.scenario == "dht_hidden_find_peer" &&
                            dht_seed_peer.is_some_and(|seed_peer| seed_peer == peer_id) =>
                    {
                        swarm.behaviour_mut().kad.add_address(&peer_id, transport_addr(endpoint.get_remote_address().clone()));
                        dht_seed_query = Some(swarm.behaviour_mut().kad.get_closest_peers(PeerId::random()));
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Kad(kad::Event::OutboundQueryProgressed {
                        id,
                        result: kad::QueryResult::GetClosestPeers(Ok(_)),
                        ..
                    })) if opts.scenario == "dht_hidden_find_peer" && Some(id) == dht_seed_query => {
                        let seed_peer = dht_seed_peer.ok_or("missing DHT seed peer")?;
                        write_json(
                            &opts.result_file,
                            json!({
                                "implementation": "rust",
                                "role": "routing_listener",
                                "scenario": opts.scenario,
                                "status": "ok",
                                "seed_peer_id": seed_peer.to_string(),
                                "authenticated_seed": true,
                                "dht_queries_delta": 1,
                                "negotiated_protocol": "/ipfs/kad/1.0.0"
                            }),
                        )?;
                        seeded = true;
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Kad(kad::Event::OutboundQueryProgressed {
                        id,
                        result: kad::QueryResult::GetClosestPeers(Err(error)),
                        ..
                    })) if opts.scenario == "dht_hidden_find_peer" && Some(id) == dht_seed_query => {
                        return Err(format!("DHT seed query failed: {error:?}").into());
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Gossipsub(
                        gossipsub::Event::Message {
                            propagation_source,
                            message,
                            ..
                        },
                    )) if opts.scenario == "gossipsub_publish" => {
                            let status = if message.data == PUBSUB_PAYLOAD { "ok" } else { "mismatch" };
                            write_json(
                                &opts.result_file,
                                json!({
                                    "implementation": "rust",
                                    "scenario": "gossipsub_publish",
                                    "status": status,
                                    "topic": message.topic.to_string(),
                                    "payload": String::from_utf8_lossy(&message.data).to_string(),
                                    "propagation_source": propagation_source.to_string(),
                                    "source": message.source.map(|peer| peer.to_string()).unwrap_or_default()
                                }),
                            )?;
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Gossipsub(
                        gossipsub::Event::Message { message, .. },
                    )) if opts.scenario == "gossipsub_mixed_mesh_stress" => {
                        let payload = String::from_utf8_lossy(&message.data).to_string();
                        if !payloads.insert(payload) {
                            duplicates += 1;
                        }
                    }
                    _ => {}
                }
            }
        }
    }
}

async fn dial(opts: Options) -> Result<(), Box<dyn Error>> {
    if opts.scenario == "dht_provide_find_provider" {
        return provider::run(&opts).await;
    }
    let mut swarm = new_swarm(&opts).await?;
    let remote_peer: PeerId = opts.peer_id.parse()?;
    let remote: Multiaddr = opts.addr.parse()?;
    let dns_root = remote
        .iter()
        .any(|protocol| matches!(protocol, Protocol::Dnsaddr(_)));
    if dns_root && opts.dns_server.is_none() {
        return Err("DNSADDR fixture dial requires --dns-server".into());
    }
    if let Some(Protocol::P2p(peer)) = remote.iter().last()
        && peer != remote_peer
    {
        return Err("address peer disagrees with --peer-id".into());
    }
    let relay_transport = if opts.scenario == "relay_reserve" {
        match relay_transport_addr(remote.clone(), remote_peer) {
            Ok(address) => Some(address),
            Err(error) => {
                write_json(
                    &opts.result_file,
                    json!({
                        "implementation": "rust",
                        "role": "dialer",
                        "scenario": opts.scenario,
                        "status": "rejected",
                        "error_code": error.status_code(),
                    }),
                )?;
                return Err(error.into());
            }
        }
    } else {
        None
    };
    if opts.scenario == "gossipsub_publish" {
        let topic = gossipsub::IdentTopic::new(PUBSUB_TOPIC);
        swarm.behaviour_mut().gossipsub.subscribe(&topic)?;
    }
    let mut pnet_observation = PnetObservation::default();
    let dial_options = if dns_root {
        DialOpts::peer_id(remote_peer)
            .addresses(vec![remote.clone()])
            .build()
    } else {
        remote.clone().into()
    };
    if let Err(error) = swarm.dial(dial_options) {
        if opts.scenario == "pnet" && !opts.pnet_control.is_empty() {
            return write_json(
                &opts.result_file,
                pnet_rejection(&opts, "dialer", &remote_peer.to_string(), pnet_observation),
            );
        }
        return Err(error.into());
    }
    let started = Instant::now();
    let mut connected = false;
    let mut ping_ok = false;
    let mut identify_count = 0usize;
    let mut identify_signed_record = false;
    let mut authenticated_remote_peer_id = None;
    let mut connection_remote_addr = None;
    let mut negotiated_transport = None;
    let mut tcp_upgrade_observation = None;
    while started.elapsed() < Duration::from_secs(20) {
        let event = swarm.select_next_some().await;
        if let SwarmEvent::ConnectionEstablished { connection_id, peer_id, endpoint, .. } = &event {
            opts.upgrade_observer.established(connection_id, *peer_id, endpoint);
        }
        match event {
            SwarmEvent::ConnectionEstablished {
                peer_id, endpoint, ..
            } if peer_id == remote_peer => {
                eprintln!("rust-dial connected: {peer_id}");
                connected = true;
                if opts.scenario == "pnet" {
                    pnet_observation.established_connections += 1;
                }
                authenticated_remote_peer_id = Some(peer_id.to_string());
                connection_remote_addr = Some(endpoint.get_remote_address().to_string());
                negotiated_transport = observed_quic_transport(&endpoint);
                if dns_root && opts.transport == "tcp" {
                    tcp_upgrade_observation = Some(
                        opts.tcp_upgrade_observation
                            .lock()
                            .map_err(|_| "TCP upgrade observation lock poisoned")?
                            .clone()
                            .filter(|observed| observed.peer == peer_id)
                            .ok_or("DNSADDR connection lacks the expected peer's TCP upgrade observation")?,
                    );
                }
                swarm
                    .behaviour_mut()
                    .kad
                    .add_address(&remote_peer, transport_addr(remote.clone()));
                if opts.scenario != "ping" && opts.scenario != "identify" && opts.scenario != "pnet"
                {
                    break;
                }
            }
            SwarmEvent::NewListenAddr { address, .. } => {
                swarm.add_external_address(address);
            }
            SwarmEvent::Behaviour(BehaviourEvent::Ping(ping::Event { peer, result, .. }))
                if peer == remote_peer =>
            {
                eprintln!("rust-dial ping: {result:?}");
                ping_ok = result.is_ok();
                if opts.scenario == "ping" {
                    break;
                }
            }
            SwarmEvent::Behaviour(BehaviourEvent::Identify(identify::Event::Received {
                peer_id,
                info,
                ..
            })) if peer_id == remote_peer => {
                eprintln!("rust-dial identify: protocols={}", info.protocols.len());
                identify_count = info.protocols.len();
                identify_signed_record = info.signed_peer_record.is_some();
                if opts.scenario == "pnet" {
                    pnet_observation.identify_streams += 1;
                }
                if opts.scenario == "identify" || opts.scenario == "pnet" {
                    break;
                }
            }
            SwarmEvent::OutgoingConnectionError {
                peer_id: Some(peer_id),
                ..
            } if peer_id == remote_peer
                && opts.scenario == "pnet"
                && !opts.pnet_control.is_empty() =>
            {
                pnet_observation.attempted_connections += 1;
                return write_json(
                    &opts.result_file,
                    pnet_rejection(&opts, "dialer", &remote_peer.to_string(), pnet_observation),
                );
            }
            SwarmEvent::OutgoingConnectionError { error, .. } if dns_root => {
                return Err(format!("DNSADDR dial failed: {error}").into());
            }
            other => {
                eprintln!("rust-dial event: {other:?}");
            }
        }
    }
    if !connected {
        if opts.scenario == "pnet" && !opts.pnet_control.is_empty() {
            return write_json(
                &opts.result_file,
                pnet_rejection(&opts, "dialer", &remote_peer.to_string(), pnet_observation),
            );
        }
        return Err("connection was not established".into());
    }
    if opts.scenario == "pnet" && !opts.pnet_control.is_empty() {
        return Err(
            "pnet rejection control unexpectedly established an authenticated session".into(),
        );
    }
    if matches!(
        opts.scenario.as_str(),
        "dht_pk_put_get" | "dht_ipns_put_get"
    ) {
        wait_dht_remote_ready(&mut swarm, remote_peer).await?;
    }
    let mut separate_identify = None;
    match opts.scenario.as_str() {
        "ping" if !ping_ok => return Err("ping did not complete".into()),
        "identify" if identify_count == 0 => return Err("identify did not return protocols".into()),
        "ping" => {}
        "identify" => {
            let evidence = exchange_raw_identify(&mut swarm, remote_peer, &opts.upgrade_observer).await?;
            if evidence["status"] != "verified" {
                write_json(&opts.result_file, json!({
                    "implementation": "rust", "role": "dialer", "scenario": "identify", "status": "failed",
                    "signed_peer_record": identify_signed_record, "protocol_count": identify_count,
                    "authenticated_remote_peer_id": authenticated_remote_peer_id,
                    "raw_identify_exchange": evidence,
                }))?;
                return Err("raw Identify verification failed; captured evidence is in result-file".into());
            }
            separate_identify = Some(evidence);
        }
        "relay_reserve" => {
            let reservation = reserve_relay_address(
                &mut swarm,
                remote_peer,
                relay_transport.ok_or("relay transport address was not prepared")?,
            )
            .await?;
            write_json(
                &opts.result_file,
                json!({
                    "implementation": "rust",
                    "role": "dialer",
                    "scenario": opts.scenario,
                    "status": "ok",
                    "negotiated_transport": negotiated_transport,
                    "authenticated_remote_peer_id": authenticated_remote_peer_id,
                    "relay_reservation_accepted": reservation.accepted,
                    "relay_reservation_relay_peer_id": reservation.relay_peer_id,
                    "relay_reservation_client_peer_id": reservation.client_peer_id,
                    "relay_reservation_circuit_addr": reservation.circuit_addr,
                    "relay_reservation_renewal": reservation.renewal
                }),
            )?;
            return Ok(());
        }
        "echo" | "echo_large" => {
            let payload = if opts.scenario == "echo_large" {
                (0..192 * 1024)
                    .map(|index| (index % 251) as u8)
                    .collect::<Vec<_>>()
            } else {
                opts.payload.as_bytes().to_vec()
            };
            let bytes = open_echo_stream_direct(&mut swarm, remote_peer, &payload, &opts.upgrade_observer).await?;
            let mut result = json!({
                "implementation": "rust",
                "role": "dialer",
                "scenario": opts.scenario,
                "status": "ok",
                "protocol": "/forge/interop/relay-echo/1",
                "payload_bytes": bytes,
                "echo_ok": true
            });
            if dns_root {
                result["dns_input_address"] = json!(opts.addr);
                result["dns_resolver_configured"] = json!(opts.dns_server.is_some());
                result["expected_peer_id"] = json!(remote_peer.to_string());
                result["authenticated_remote_peer_id"] = json!(authenticated_remote_peer_id);
                result["connection_remote_addr"] = json!(connection_remote_addr);
                if let Some(observed) = tcp_upgrade_observation {
                    result["negotiated_transport"] = json!("tcp");
                    result["negotiated_security"] = json!(observed.security);
                    result["negotiated_muxer"] = json!(observed.muxer);
                    result["connection_remote_addr"] = json!(observed.remote_address.to_string());
                }
            }
            write_json(&opts.result_file, result)?;
            return Ok(());
        }
        "pnet" => {
            if identify_count == 0 {
                return Err("pnet connection did not complete authenticated Identify".into());
            }
            let bytes =
                open_echo_stream_direct(&mut swarm, remote_peer, opts.payload.as_bytes(), &opts.upgrade_observer).await?;
            let mut result = json!({
                "implementation": "rust",
                "role": "dialer",
                "scenario": "pnet",
                "status": "ok",
                "protocol": "/forge/interop/relay-echo/1",
                "payload_bytes": bytes,
                "echo_ok": true,
                "protocol_count": identify_count,
                "signed_peer_record": identify_signed_record,
                "identify_observed": true,
                "negotiated_transport": "tcp",
                "negotiated_security": "/noise",
                "negotiated_muxer": "/yamux/1.0.0",
                "authenticated_remote_peer_id": authenticated_remote_peer_id,
                "autonat_v2_active": false,
                "relay_service_active": swarm.behaviour().relay.is_enabled(),
                "relay_client_active": swarm.behaviour().relay_client.is_enabled(),
                "dcutr_active": swarm.behaviour().dcutr.is_enabled(),
                "pnet_enabled": true,
                "negotiated_pnet": true,
                "pnet_fingerprint": opts.pnet_fingerprint
            });
            if dns_root {
                result["dns_input_address"] = json!(opts.addr);
                result["dns_resolver_configured"] = json!(opts.dns_server.is_some());
                result["expected_peer_id"] = json!(remote_peer.to_string());
                result["connection_remote_addr"] = json!(connection_remote_addr);
            }
            write_json(&opts.result_file, result)?;
            return Ok(());
        }
        "dcutr" => {
            open_required_stream(&mut swarm, remote_peer, "/libp2p/dcutr").await?;
        }
        "dht_find_peer" => {
            let evidence = wait_dht_find_peer(&mut swarm, remote_peer).await?;
            write_json(
                &opts.result_file,
                json!({
                    "implementation": "rust",
                    "role": "dialer",
                    "scenario": opts.scenario,
                    "status": "ok",
                    "closest_peers": evidence.closest_peers,
                    "query_requests": evidence.requests,
                    "query_successes": evidence.successes,
                    "query_failures": evidence.failures
                }),
            )?;
            return Ok(());
        }
        "dht_hidden_find_peer" => {
            let target_peer: PeerId = opts.target_peer_id.parse()?;
            if target_peer == remote_peer {
                return Err("hidden target must differ from the known routing peer".into());
            }
            let evidence = wait_dht_hidden_peer(&mut swarm, target_peer).await?;
            write_json(
                &opts.result_file,
                json!({
                    "implementation": "rust",
                    "role": "dialer",
                    "scenario": opts.scenario,
                    "status": "ok",
                    "preexisting_target": false,
                    "found_peer": target_peer.to_string(),
                    "target_dialed": evidence.target_dialed,
                    "target_connected": evidence.target_connected,
                    "routing_updated": evidence.routing_updated,
                    "address_count": evidence.address_count,
                    "followup_exact_rpc": evidence.followup_exact_rpc,
                    "closest_peers": evidence.closest_peers,
                    "dht_queries_delta": 1,
                    "negotiated_protocol": "/ipfs/kad/1.0.0"
                }),
            )?;
            return Ok(());
        }
        "dht_pk_put_get" | "dht_ipns_put_get" => {
            let bytes = wait_dht_put_get(&mut swarm, &opts.scenario, &opts.payload).await?;
            let operation = match opts.payload.as_str() {
                "put_only" => "put_only",
                "get_only" => "get_only",
                _ => "put_get",
            };
            write_json(
                &opts.result_file,
                json!({
                    "implementation": "rust",
                    "role": "dialer",
                    "scenario": opts.scenario,
                    "status": "ok",
                    "operation": operation,
                    "remote_get": operation == "get_only",
                    "value_bytes": bytes
                }),
            )?;
            return Ok(());
        }
        "rendezvous_register_discover" => {
            let evidence = wait_rendezvous_register_discover(&mut swarm, remote_peer).await?;
            write_json(
                &opts.result_file,
                json!({
                    "implementation": "rust",
                    "role": "dialer",
                    "scenario": opts.scenario,
                    "status": "ok",
                    "negotiated_protocol": "/rendezvous/1.0.0",
                    "wire_registration_count": evidence.wire_registration_count,
                    "signed_peer_record_valid": true,
                    "matching_peer_record": true,
                    "record_sequence": evidence.record_sequence,
                    "record_address_count": evidence.record_address_count,
                    "registered_ttl_seconds": evidence.registered_ttl_seconds,
                    "discovered_ttl_seconds": evidence.discovered_ttl_seconds,
                    "cookie_bytes": evidence.cookie_bytes
                }),
            )?;
            return Ok(());
        }
        "rendezvous_lifecycle" => {
            let evidence = wait_rendezvous_lifecycle(&mut swarm, remote_peer).await?;
            write_json(
                &opts.result_file,
                json!({
                    "implementation": "rust",
                    "role": "dialer",
                    "scenario": opts.scenario,
                    "status": "ok",
                    "negotiated_protocol": "/rendezvous/1.0.0",
                    "legacy_signed_peer_record": true,
                    "initial_ttl_seconds": evidence.initial_ttl_seconds,
                    "updated_ttl_seconds": evidence.updated_ttl_seconds,
                    "renewed_ttl_seconds": evidence.renewed_ttl_seconds,
                    "initial_record_sequence": evidence.initial_record_sequence,
                    "updated_record_sequence": evidence.updated_record_sequence,
                    "renewed_record_sequence": evidence.renewed_record_sequence,
                    "pre_unregister_record_sequence": evidence.pre_unregister_record_sequence,
                    "initial_cookie_bytes": evidence.initial_cookie_bytes,
                    "delta_cookie_bytes": evidence.delta_cookie_bytes,
                    "cookie_changed": true,
                    "initial_visible_count": evidence.initial_visible_count,
                    "updated_visible_count": evidence.updated_visible_count,
                    "renewed_visible_after_original_expiry": evidence.renewed_visible_after_original_expiry,
                    "renewed_visible_count": evidence.renewed_visible_count,
                    "expired_registration_count": evidence.expired_registration_count,
                    "pre_unregister_count": evidence.pre_unregister_count,
                    "final_registration_count": evidence.final_registration_count
                }),
            )?;
            return Ok(());
        }
        "gossipsub_publish" | "gossipsub_mixed_mesh_stress" => {
            wait_gossipsub_peer_and_publish(&mut swarm, remote_peer, opts.payload.as_bytes())
                .await?;
            write_json(
                &opts.result_file,
                json!({
                    "implementation": "rust",
                    "role": "dialer",
                    "scenario": opts.scenario,
                    "status": "ok",
                    "topic": PUBSUB_TOPIC,
                    "payload": opts.payload,
                    "payload_bytes": opts.payload.len(),
                    "mesh_peer": true
                }),
            )?;
            return Ok(());
        }
        "unknown_protocol" => {
            let error = expect_unknown_stream_rejection(
                &mut swarm,
                remote_peer,
                "/forge/interop/unknown/1",
            )
            .await?;
            write_json(
                &opts.result_file,
                json!({
                    "implementation": "rust",
                    "role": "dialer",
                    "scenario": opts.scenario,
                    "status": "ok",
                    "expected_error": error
                }),
            )?;
            return Ok(());
        }
        _ => return Err(format!("unknown scenario {}", opts.scenario).into()),
    }
    write_json(
        &opts.result_file,
        json!({
            "implementation": "rust",
            "role": "dialer",
            "scenario": opts.scenario,
            "status": "ok",
            "ping_ok": ping_ok,
            "protocol_count": identify_count,
            "signed_peer_record": identify_signed_record,
            "raw_identify_exchange": separate_identify,
            "negotiated_transport": negotiated_transport,
            "authenticated_remote_peer_id": authenticated_remote_peer_id
        }),
    )
}

async fn destination(opts: Options) -> Result<(), Box<dyn Error>> {
    let mut swarm = new_swarm(&opts).await?;
    spawn_incoming_stream_echo(&mut swarm, "/forge/interop/relay-echo/1", None, &opts.tasks)?;
    let peer = *swarm.local_peer_id();
    let relay_addr: Multiaddr = opts.relay_addr.parse()?;
    let relay_listener_id = swarm.listen_on(relay_addr.clone().with(Protocol::P2pCircuit))?;
    let started = Instant::now();
    let mut relay_addrs = Vec::new();
    let mut reservation = false;
    while started.elapsed() < Duration::from_secs(30) {
        let event = swarm.select_next_some().await;
        eprintln!("rust-destination event: {:?}", event);
        match event {
            SwarmEvent::ListenerClosed { listener_id, reason: Err(error), .. }
                if listener_id == relay_listener_id =>
            {
                return Err(format!("relay destination listener closed: {error}").into());
            }
            SwarmEvent::NewListenAddr { address, .. } => {
                swarm.add_external_address(address.clone());
                if address.to_string().contains("p2p-circuit") {
                    relay_addrs.push(address.to_string());
                    reservation = true;
                }
            }
            SwarmEvent::ExternalAddrConfirmed { address } => {
                if address.to_string().contains("p2p-circuit") {
                    relay_addrs.push(address.to_string());
                    reservation = true;
                }
            }
            SwarmEvent::Behaviour(BehaviourEvent::RelayClient(
                relay::client::Event::ReservationReqAccepted { .. },
            )) => {
                reservation = true;
                if relay_addrs.is_empty() {
                    relay_addrs.push(
                        relay_addr
                            .clone()
                            .with(Protocol::P2pCircuit)
                            .with(Protocol::P2p(peer))
                            .to_string(),
                    );
                }
                if !relay_addrs.is_empty() {
                    break;
                }
            }
            SwarmEvent::Behaviour(BehaviourEvent::Identify(identify::Event::Received {
                info,
                ..
            })) => {
                swarm.add_external_address(info.observed_addr);
            }
            _ => {}
        }
    }
    if !reservation || relay_addrs.is_empty() {
        return Err("relay reservation was not established".into());
    }
    write_json(
        &opts.ready_file,
        json!({
            "implementation": "rust",
            "role": "destination",
            "peer_id": peer.to_string(),
            "relay_addrs": relay_addrs,
            "relay_peer_id": opts.relay_peer_id,
            "native_relay_transport": true,
            "status": "ready"
        }),
    )?;
    loop {
        tokio::select! {
            _ = tokio::time::sleep(Duration::from_millis(100)) => {
                if opts.stop_file.exists() {
                    return Ok(());
                }
            }
            event = swarm.select_next_some() => {
                match event {
                    SwarmEvent::NewListenAddr { address, .. } => swarm.add_external_address(address),
                    SwarmEvent::ListenerClosed { listener_id, reason: Err(error), .. }
                        if listener_id == relay_listener_id =>
                    {
                        return Err(format!("relay destination listener closed: {error}").into());
                    }
                    _ => {}
                }
            }
        }
    }
}

async fn open_echo_stream(
    swarm: &mut libp2p::Swarm<Behaviour>,
    peer: PeerId,
    expect_direct_upgrade: bool,
) -> Result<bool, Box<dyn Error>> {
    let mut control = swarm.behaviour().stream.new_control();
    let mut open =
        Box::pin(control.open_stream(peer, StreamProtocol::new("/forge/interop/relay-echo/1")));
    let deadline = tokio::time::sleep(Duration::from_secs(30));
    tokio::pin!(deadline);
    let mut direct_upgrade = false;
    loop {
        tokio::select! {
            result = &mut open => {
                let mut stream = result?;
                write_frame(&mut stream, b"relay-echo").await?;
                let echoed = read_frame(&mut stream).await?;
                stream.close().await?;
                if echoed != b"relay-echo" {
                    return Err("relay echo mismatch".into());
                }
                if expect_direct_upgrade && !direct_upgrade {
                    let settle = tokio::time::sleep(Duration::from_secs(5));
                    tokio::pin!(settle);
                    loop {
                        tokio::select! {
                            _ = &mut settle => return Ok(direct_upgrade),
                                event = swarm.select_next_some() => {
                                    if let SwarmEvent::ConnectionEstablished { peer_id, endpoint, .. } = event {
                                        if peer_id == peer && !format!("{endpoint:?}").contains("P2pCircuit") {
                                            return Ok(true);
                                        }
                                    }
                                }
                        }
                    }
                }
                return Ok(direct_upgrade);
            }
            _ = &mut deadline => {
                return Err("timed out opening relay echo stream".into());
            }
            event = swarm.select_next_some() => {
                match event {
                    SwarmEvent::ConnectionEstablished { peer_id, endpoint, .. } if peer_id == peer => {
                        if !format!("{endpoint:?}").contains("P2pCircuit") {
                            direct_upgrade = true;
                        }
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Identify(identify::Event::Received { info, .. })) => {
                        swarm.add_external_address(info.observed_addr);
                    }
                    _ => {}
                }
            }
        }
    }
}

async fn dial_and_wait(
    swarm: &mut libp2p::Swarm<Behaviour>,
    peer: PeerId,
    address: Multiaddr,
) -> Result<(), Box<dyn Error>> {
    swarm.dial(address)?;
    let deadline = tokio::time::sleep(Duration::from_secs(20));
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            _ = &mut deadline => return Err("timed out connecting relay".into()),
            event = swarm.select_next_some() => {
                match event {
                    SwarmEvent::ConnectionEstablished { peer_id, .. } if peer_id == peer => return Ok(()),
                    SwarmEvent::Behaviour(BehaviourEvent::Identify(identify::Event::Received { info, .. })) => {
                        swarm.add_external_address(info.observed_addr);
                    }
                    _ => {}
                }
            }
        }
    }
}

async fn dial_relay(opts: Options) -> Result<(), Box<dyn Error>> {
    let mut swarm = new_swarm(&opts).await?;
    let target_peer: PeerId = opts.peer_id.parse()?;
    let relay_peer: PeerId = opts.relay_peer_id.parse()?;
    let relay_addr: Multiaddr = opts.relay_addr.parse()?;
    dial_and_wait(&mut swarm, relay_peer, relay_addr.clone()).await?;
    let target_addr = relay_addr
        .with(Protocol::P2pCircuit)
        .with(Protocol::P2p(target_peer));
    swarm.dial(target_addr.clone())?;
    let direct_upgrade = open_echo_stream(
        &mut swarm,
        target_peer,
        opts.scenario == "dcutr_relay_topology",
    )
    .await?;
    if opts.scenario == "dcutr_relay_topology" && !direct_upgrade {
        return Err("DCUtR did not produce a direct connection".into());
    }
    write_json(
        &opts.result_file,
        json!({
            "implementation": "rust",
            "role": "relay_dialer",
            "scenario": opts.scenario,
            "status": "ok",
            "relay_peer": relay_peer.to_string(),
            "target_peer": target_peer.to_string(),
            "relayed_addr": target_addr.to_string(),
            "relay_echo": true,
            "direct_upgrade": direct_upgrade
        }),
    )
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn Error>> {
    let opts = parse_args()?;
    if autonat_fixture::is_scenario(&opts.scenario) {
        return autonat_fixture::run(opts).await;
    }
    let capture = matches!(opts.transport.as_str(), "tcp" | "tcp-tls")
        && matches!(opts.command.as_str(), "listen" | "dial")
        && matches!(opts.scenario.as_str(), "identify" | "echo" | "echo_large");
    let observer = opts.upgrade_observer.clone();
    let tasks = opts.tasks.clone();
    let result_file = opts.result_file.clone();
    let role = if opts.command == "listen" { "listener" } else { "dialer" };
    let scenario = opts.scenario.clone();
    let outcome = match opts.command.as_str() {
        "listen" => listen(opts).await,
        "destination" => destination(opts).await,
        "dial" => dial(opts).await,
        "dial-relay" => dial_relay(opts).await,
        _ => Err(format!("unknown command {}", opts.command).into()),
    };
    // Commands own their Swarms; drain the public-executor and echo tasks only after drop.
    let lifecycle = tasks.close_and_join().await;
    let mut outcome = lifecycle.combine(outcome);
    let observation = capture.then(|| {
        let mut evidence = observer.finalized(lifecycle.snapshot()["fixture_owned_tasks_joined"] == true);
        if outcome.is_err() { evidence["complete"] = json!(false); }
        if role == "dialer" && outcome.is_ok() && evidence["complete"] != true {
            outcome = Err("native TCP application observation is unbound, ambiguous or incomplete".into());
        }
        evidence
    });
    if !result_file.as_os_str().is_empty() {
        if let Err(error) = lifecycle.write_result(
            &result_file, role, &scenario, &outcome, observation,
        ) {
            if outcome.is_ok() { return Err(error.into()); }
            eprintln!("fixture result finalization failed: {error}");
        }
    }
    outcome
}

#[cfg(test)]
mod tests {
    use super::{RelayAddressError, RelayReservationProgress, relay_transport_addr,
                decode_hex, raw_identify_fields, raw_identify_evidence, read_raw_identify_frame, write_frame};
    use libp2p::{Multiaddr, core::{PeerRecord, SignedEnvelope}, identity, multiaddr::Protocol};
    use quick_protobuf::Writer;
    use sha2::{Digest, Sha256};
    use std::net::Ipv4Addr;

    fn identify_payload(key: &[u8], envelope: &[u8]) -> Vec<u8> {
        let mut bytes = Vec::new();
        let mut writer = Writer::new(&mut bytes);
        writer.write_with_tag(10, |writer| writer.write_bytes(key)).unwrap();
        writer.write_with_tag(66, |writer| writer.write_bytes(envelope)).unwrap();
        bytes
    }

    #[test]
    fn raw_identify_verifies_legacy_and_standard_without_behaviour_substitution() {
        let key = identity::Keypair::generate_ed25519();
        for standard in [false, true] {
            let addresses = vec!["/ip4/127.0.0.1/tcp/4001".parse().unwrap()];
            let record = if standard { PeerRecord::new_interop(&key, addresses) }
                         else { PeerRecord::new(&key, addresses) }.unwrap();
            let envelope = record.into_signed_envelope().into_protobuf_encoding();
            let bytes = identify_payload(&key.public().encode_protobuf(), &envelope);
            let evidence = raw_identify_evidence(&bytes, key.public().to_peer_id());
            assert_eq!(evidence["status"], "verified");
            assert_eq!(evidence["basis"], "fixture_separate_authenticated_identify_exchange");
            assert_eq!(evidence["envelope_format"], if standard { "standard" } else { "legacy" });
            assert_eq!(evidence["signed_peer_record_verified"], true);
            assert_eq!(evidence["record_peer_id"], key.public().to_peer_id().to_string());
            assert_eq!(decode_hex(evidence["raw_protobuf_hex"].as_str().unwrap()).unwrap(), bytes);
            assert_eq!(evidence["raw_protobuf_sha256"], format!("{:x}", Sha256::digest(&bytes)));
            assert_eq!(decode_hex(evidence["signed_envelope_hex"].as_str().unwrap()).unwrap(), envelope);
            assert_eq!(evidence["signed_envelope_sha256"], format!("{:x}", Sha256::digest(&envelope)));
        }
    }

    fn assert_raw_identify_failed(bytes: &[u8], peer: libp2p::PeerId) -> serde_json::Value {
        let evidence = raw_identify_evidence(bytes, peer);
        assert_eq!(evidence["status"], "failed");
        assert_eq!(evidence["signed_peer_record_verified"], false);
        assert!(!evidence["error"].as_str().unwrap().is_empty());
        assert_eq!(decode_hex(evidence["raw_protobuf_hex"].as_str().unwrap()).unwrap(), bytes);
        assert_eq!(evidence["raw_protobuf_sha256"], format!("{:x}", Sha256::digest(bytes)));
        evidence
    }

    #[test]
    fn raw_identify_rejects_wrong_peer_key_signature_domain_and_record_signer() {
        let key = identity::Keypair::generate_ed25519();
        let other = identity::Keypair::generate_ed25519();
        let peer = key.public().to_peer_id();
        let record = PeerRecord::new_interop(&key, vec![]).unwrap().into_signed_envelope();
        let encoded = record.clone().into_protobuf_encoding();
        assert_raw_identify_failed(&identify_payload(&key.public().encode_protobuf(), &encoded), other.public().to_peer_id());
        assert_raw_identify_failed(&identify_payload(&other.public().encode_protobuf(), &encoded), peer);
        let mut corrupt = encoded;
        *corrupt.last_mut().unwrap() ^= 1;
        let failed = assert_raw_identify_failed(&identify_payload(&key.public().encode_protobuf(), &corrupt), peer);
        assert_eq!(failed["legacy_validation"]["status"], "rejected");
        assert_eq!(failed["interop_validation"]["status"], "rejected");
        assert!(failed["legacy_validation"]["error"].as_str().unwrap().contains("UnexpectedPayloadType"));
        assert!(failed["interop_validation"]["error"].as_str().unwrap().contains("InvalidSignature"));
        let (payload, _) = record.payload_and_signing_key("libp2p-peer-record".into(), &[3, 1]).unwrap();
        for (signer, domain, kind) in [(&other, "libp2p-peer-record", vec![3, 1]),
                                      (&key, "unrelated-domain", vec![3, 1]),
                                      (&key, "libp2p-peer-record", vec![3, 2])] {
            let envelope = SignedEnvelope::new(signer, domain.into(), kind, payload.to_vec()).unwrap();
            let bytes = identify_payload(&signer.public().encode_protobuf(), &envelope.into_protobuf_encoding());
            assert_raw_identify_failed(&bytes, signer.public().to_peer_id());
        }
    }

    #[test]
    fn raw_identify_malformed_failure_retains_capture_before_validation() {
        let peer = test_peer();
        let failed = assert_raw_identify_failed(&[10], peer);
        assert_eq!(failed["legacy_validation"]["status"], "not_attempted");
        assert_eq!(failed["interop_validation"]["status"], "not_attempted");
        let bytes = identify_payload(b"invalid key", b"invalid envelope");
        let failed = assert_raw_identify_failed(&bytes, peer);
        assert_eq!(decode_hex(failed["signed_envelope_hex"].as_str().unwrap()).unwrap(), b"invalid envelope");
    }

    #[test]
    fn raw_identify_parser_rejects_duplicate_missing_malformed_and_truncated_fields() {
        let valid = identify_payload(b"key", b"record");
        assert_eq!(raw_identify_fields(&valid).unwrap(), (b"key".as_slice(), b"record".as_slice()));
        for suffix in [&[10, 0][..], &[66, 0], &[0], &[8, 1], &[64, 1], &[18, 2, 1], &[0x80]] {
            let mut bytes = valid.clone();
            bytes.extend_from_slice(suffix);
            assert!(raw_identify_fields(&bytes).is_err());
        }
        for bytes in [vec![], vec![10, 0], vec![66, 0], vec![0; 4097]] {
            assert!(raw_identify_fields(&bytes).is_err());
        }
        // Repeated protocols are legal; only the two security fields are singular.
        let mut bytes = valid;
        bytes.extend_from_slice(&[26, 1, b'a', 26, 1, b'b']);
        assert!(raw_identify_fields(&bytes).is_ok());
    }

    #[tokio::test]
    async fn raw_identify_frame_enforces_cap_before_body_read() {
        let mut oversized = futures::io::Cursor::new(vec![0x81, 0x20]); // 4097, no body.
        let error = read_raw_identify_frame(&mut oversized).await.unwrap_err();
        assert!(error.to_string().contains("4096-byte limit"));
        for prefix in [vec![0], vec![0x81, 0], vec![0x80, 0x80], vec![1]] {
            assert!(read_raw_identify_frame(&mut futures::io::Cursor::new(prefix)).await.is_err());
        }
        let payload = vec![42; 4096];
        let mut frame = futures::io::Cursor::new(Vec::new());
        write_frame(&mut frame, &payload).await.unwrap();
        frame.set_position(0);
        assert_eq!(read_raw_identify_frame(&mut frame).await.unwrap(), payload);
    }

    fn test_peer() -> libp2p::PeerId {
        identity::Keypair::generate_ed25519().public().to_peer_id()
    }

    fn test_transport_addr() -> Multiaddr {
        Multiaddr::empty()
            .with(Protocol::Ip4(Ipv4Addr::LOCALHOST))
            .with(Protocol::Udp(4001))
            .with(Protocol::QuicV1)
    }

    #[test]
    fn relay_transport_addr_strips_matching_terminal_peer() {
        let peer = test_peer();
        let transport = test_transport_addr();
        let endpoint = transport.clone().with(Protocol::P2p(peer));

        assert_eq!(relay_transport_addr(endpoint, peer), Ok(transport));
    }

    #[test]
    fn relay_transport_addr_keeps_peerless_endpoint() {
        let transport = test_transport_addr();

        assert_eq!(
            relay_transport_addr(transport.clone(), test_peer()),
            Ok(transport)
        );
    }

    #[test]
    fn relay_transport_addr_rejects_conflicting_terminal_peer() {
        let endpoint = test_transport_addr().with(Protocol::P2p(test_peer()));

        assert_eq!(
            relay_transport_addr(endpoint, test_peer()),
            Err(RelayAddressError::TerminalPeerMismatch),
        );
    }

    #[test]
    fn relay_reservation_progress_waits_for_acceptance_after_address() {
        let mut progress = RelayReservationProgress::default();
        progress.record_circuit_addr("/p2p/relay/p2p-circuit/p2p/client".to_string());

        assert_eq!(progress.complete(), None);
        assert_eq!(
            progress.circuit_addr.as_deref(),
            Some("/p2p/relay/p2p-circuit/p2p/client")
        );

        progress.record_acceptance(false);
        assert_eq!(
            progress.complete(),
            Some((false, "/p2p/relay/p2p-circuit/p2p/client".to_string(),))
        );
        assert_eq!(progress.complete(), None);
    }

    #[test]
    fn relay_reservation_progress_waits_for_address_after_acceptance() {
        let mut progress = RelayReservationProgress::default();
        progress.record_acceptance(false);

        assert_eq!(progress.complete(), None);
        assert_eq!(progress.accepted_renewal, Some(false));

        progress.record_circuit_addr("/p2p/relay/p2p-circuit/p2p/client".to_string());
        assert_eq!(
            progress.complete(),
            Some((false, "/p2p/relay/p2p-circuit/p2p/client".to_string(),))
        );
        assert_eq!(progress.complete(), None);
    }
}
