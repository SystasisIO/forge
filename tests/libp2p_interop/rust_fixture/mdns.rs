//! Native mDNS only supplies candidates. Receipts bind echo to an authenticated
//! connection, never to the requested transport or a runner-supplied peer.
use std::{collections::BTreeMap, error::Error, net::IpAddr, time::Duration};

use futures::{AsyncWriteExt, FutureExt, StreamExt, future::LocalBoxFuture};
use libp2p::{
    Multiaddr, PeerId, StreamProtocol,
    core::ConnectedPoint,
    mdns,
    multiaddr::Protocol,
    swarm::{ConnectionId, SwarmEvent, dial_opts::DialOpts},
};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};

use super::{BehaviourEvent, Options, application_observer};

type Result<T> = std::result::Result<T, Box<dyn Error>>;
const SCHEMA: &str = "forge.mdns.interop.v1";
const SERVICE: &str = "_p2p._udp.local";
const TIMEOUT: Duration = Duration::from_secs(45);

pub(super) fn listen_address(opts: &Options) -> Result<Multiaddr> {
    if !matches!(opts.command.as_str(), "listen" | "dial") {
        return Err("mDNS requires listen or dial".into());
    }
    if [
        &opts.peer_id,
        &opts.addr,
        &opts.relay_addr,
        &opts.relay_peer_id,
        &opts.seed_peer_id,
        &opts.seed_addr,
        &opts.target_peer_id,
        &opts.probe_addr,
        &opts.internet_egress,
        &opts.pnet_fingerprint,
        &opts.pnet_control,
        &opts.pnet_correlation,
    ]
    .iter()
    .any(|value| !value.is_empty())
        || !opts.seed_file.as_os_str().is_empty()
        || opts.dns_server.is_some()
        || !opts.pnet_key_file.as_os_str().is_empty()
    {
        return Err("mDNS forbids remote coordinates, seeds and private Rust configuration".into());
    }
    if opts.payload.is_empty()
        || opts.payload.len() > 4096
        || opts.ready_file.as_os_str().is_empty()
        || opts.result_file.as_os_str().is_empty()
        || opts.stop_file.as_os_str().is_empty()
    {
        return Err("mDNS requires payload 1..4096 bytes and ready/result/stop files".into());
    }
    let ip: IpAddr = opts.bind_ip.parse()?;
    let link_local = match ip {
        IpAddr::V4(v) => v.is_link_local(),
        IpAddr::V6(v) => v.is_unicast_link_local(),
    };
    if ip.is_unspecified() || ip.is_loopback() || ip.is_multicast() || link_local {
        return Err("mDNS requires a usable local bind IP (not loopback or scoped)".into());
    }
    let address = Multiaddr::empty().with(match ip {
        IpAddr::V4(v) => Protocol::Ip4(v),
        IpAddr::V6(v) => Protocol::Ip6(v),
    });
    match opts.transport.as_str() {
        "tcp" | "tcp-tls" => Ok(address.with(Protocol::Tcp(0))),
        "quic" => Ok(address.with(Protocol::Udp(0)).with(Protocol::QuicV1)),
        _ => Err("Rust mDNS fixture supports public TCP Noise/TLS and QUIC only".into()),
    }
}

#[derive(Default)]
struct Discovery {
    peers: BTreeMap<PeerId, Vec<Multiaddr>>,
    bytes: usize,
    events: usize,
}

impl Discovery {
    fn observe(&mut self, event: mdns::Event) -> Result<()> {
        self.events += 1;
        if self.events > 4096 {
            return Err("mDNS event bound exceeded".into());
        }
        match event {
            mdns::Event::Discovered(values) => {
                for (peer, address) in values {
                    if !self.peers.contains_key(&peer) && self.peers.len() == 64 {
                        return Err("mDNS peer bound exceeded".into());
                    }
                    let addresses = self.peers.entry(peer).or_default();
                    if addresses.contains(&address) {
                        continue;
                    }
                    if addresses.len() == 16
                        || address.len() > 4096
                        || self.bytes + address.len() > 256 * 1024
                    {
                        return Err("mDNS address bound exceeded".into());
                    }
                    self.bytes += address.len();
                    addresses.push(address);
                }
            }
            mdns::Event::Expired(values) => {
                for (peer, address) in values {
                    if let Some(addresses) = self.peers.get_mut(&peer) {
                        addresses.retain(|old| {
                            if *old == address {
                                self.bytes -= old.len();
                                false
                            } else {
                                true
                            }
                        });
                        if addresses.is_empty() {
                            self.peers.remove(&peer);
                        }
                    }
                }
            }
        }
        Ok(())
    }
}

struct Connection {
    id: ConnectionId,
    peer: PeerId,
    endpoint: ConnectedPoint,
}

fn connection_receipt(
    connection: &Connection,
    local: PeerId,
    raw: &Value,
    outbound: bool,
) -> Result<Value> {
    let (direction, remote, mut local_address) = match &connection.endpoint {
        ConnectedPoint::Dialer { address, .. } => ("outbound", address, Value::Null),
        ConnectedPoint::Listener {
            local_addr,
            send_back_addr,
        } => ("inbound", send_back_addr, json!(local_addr.to_string())),
    };
    if (direction == "outbound") != outbound {
        return Err("unexpected actual connection direction".into());
    }
    if remote.iter().any(|p| matches!(p, Protocol::P2pCircuit)) {
        return Err("mDNS echo must be direct".into());
    }
    let quic = remote.iter().any(|p| matches!(p, Protocol::QuicV1));
    let tcp = remote.iter().any(|p| matches!(p, Protocol::Tcp(_)));
    let (transport, security, muxer, remote_address) = if quic && !tcp {
        // ConnectedPoint does not expose the dialer's local UDP socket address.
        // Do not substitute the configured listener/bind address for it.
        ("/quic-v1", Value::Null, Value::Null, remote.to_string())
    } else if tcp && !quic {
        let traces = raw["connections"]
            .as_array()
            .ok_or("missing TCP transport observations")?;
        let matches: Vec<_> = traces
            .iter()
            .filter(|c| c["authenticated_remote_peer_id"] == connection.peer.to_string())
            .collect();
        let [trace] = matches.as_slice() else {
            return Err("ambiguous TCP transport observations".into());
        };
        let events = raw["swarm_events"]
            .as_array()
            .ok_or("missing swarm observations")?;
        let matches: Vec<_> = events
            .iter()
            .filter(|e| e["swarm_connection_id"] == connection.id.to_string())
            .collect();
        let [event] = matches.as_slice() else {
            return Err("missing unique swarm connection".into());
        };
        let resolved = application_observer::bound_remote_endpoint(
            trace,
            event,
            &connection.peer.to_string(),
        )?;
        if trace["security_complete"] != true
            || trace["muxer_complete"] != true
            || trace["overflow"] != false
            || trace["direction"] != direction
            || trace["authenticated_local_peer_id"] != local.to_string()
        {
            return Err("incomplete TCP upgrade evidence".into());
        }
        local_address = trace["local_address"].clone();
        (
            "tcp",
            trace["selected_security"].clone(),
            trace["selected_muxer"].clone(),
            resolved,
        )
    } else {
        return Err("unknown actual connection transport".into());
    };
    Ok(
        json!({"id":connection.id.to_string(), "local_peer_id":local.to_string(),
        "remote_peer_id":connection.peer.to_string(), "direction":direction,
        "local_address":local_address, "remote_address":remote_address,
        "transport":transport, "security":security, "muxer":muxer,
        "binding":"single_lifetime_swarm_connection_and_stream_peer"}),
    )
}

async fn execute(opts: &Options) -> Result<Value> {
    let bind: IpAddr = opts.bind_ip.parse()?;
    let mut swarm = super::new_swarm(opts).await?;
    let local = *swarm.local_peer_id();
    swarm.behaviour_mut().mdns = Some(mdns::tokio::Behaviour::new(
        mdns::Config {
            ttl: Duration::from_secs(120),
            query_interval: Duration::from_secs(2),
            enable_ipv6: bind.is_ipv6(),
        },
        local,
    )?)
    .into();
    let outbound = opts.command == "dial";
    let role = if outbound { "dialer" } else { "listener" };
    let mut control = swarm.behaviour().stream.new_control();
    let mut incoming = if outbound {
        None
    } else {
        Some(control.accept(StreamProtocol::new(application_observer::ECHO))?)
    };
    let mut discovered = Discovery::default();
    let mut connection: Option<Connection> = None;
    let mut dialed = None;
    let mut exchange: Option<LocalBoxFuture<'static, Result<(PeerId, usize)>>> = None;
    let mut ready = false;
    let mut receipt = None;
    let deadline = tokio::time::sleep(TIMEOUT);
    tokio::pin!(deadline);
    let mut stop_check = tokio::time::interval(Duration::from_millis(100));
    loop {
        if ready && outbound && dialed.is_none() {
            if let Some((peer, addresses)) = discovered
                .peers
                .iter()
                .find(|(p, a)| **p != local && !a.is_empty())
            {
                let peer = *peer;
                swarm.dial(DialOpts::peer_id(peer).addresses(addresses.clone()).build())?;
                dialed = Some(peer);
            }
        }
        if exchange.is_none() && receipt.is_none() {
            if let Some(c) = &connection {
                if discovered.peers.contains_key(&c.peer) {
                    let expected = c.peer;
                    let payload = opts.payload.as_bytes().to_vec();
                    if outbound {
                        let mut control = control.clone();
                        let observer = opts.upgrade_observer.clone();
                        exchange = Some(
                            async move {
                                let attempt =
                                    observer.application(expected, application_observer::ECHO);
                                let stream = control
                                    .open_stream(
                                        expected,
                                        StreamProtocol::new(application_observer::ECHO),
                                    )
                                    .await?;
                                let mut stream = attempt.wrap(stream);
                                super::write_frame(&mut stream, &payload).await?;
                                let echoed = super::read_frame(&mut stream).await?;
                                if echoed != payload {
                                    return Err("echo challenge mismatch".into());
                                }
                                stream.close().await?;
                                stream.complete();
                                Ok((expected, echoed.len()))
                            }
                            .boxed_local(),
                        );
                    } else {
                        let mut incoming = incoming
                            .take()
                            .ok_or("listener already consumed echo admission")?;
                        exchange = Some(
                            async move {
                                let (peer, mut stream) =
                                    incoming.next().await.ok_or("echo admission closed")?;
                                if peer != expected {
                                    return Err(
                                        "echo peer differs from authenticated connection".into()
                                    );
                                }
                                let received = super::read_frame(&mut stream).await?;
                                if received != payload {
                                    return Err("echo challenge mismatch".into());
                                }
                                super::write_frame(&mut stream, &received).await?;
                                stream.close().await?;
                                Ok((peer, received.len()))
                            }
                            .boxed_local(),
                        );
                    }
                }
            }
        }
        tokio::select! {
            biased;
            _ = &mut deadline => return Err("mDNS fixture deadline expired".into()),
            _ = stop_check.tick() => {
                match std::fs::metadata(&opts.stop_file) {
                    Ok(_) => return receipt.ok_or_else(|| "stopped before mDNS echo evidence".into()),
                    Err(e) if e.kind() == std::io::ErrorKind::NotFound => {},
                    Err(e) => return Err(e.into()),
                }
            }
            event = swarm.select_next_some() => match event {
                SwarmEvent::NewListenAddr { .. } if !ready => {
                    super::write_json(&opts.ready_file, json!({"schema":SCHEMA,"status":"ready","role":role,
                        "local_peer_id":local.to_string(),"service_name":SERVICE}))?;
                    ready = true;
                },
                SwarmEvent::Behaviour(BehaviourEvent::Mdns(event)) => discovered.observe(event)?,
                SwarmEvent::ConnectionEstablished { connection_id, peer_id, endpoint, .. } => {
                    if connection.is_some() || (outbound && dialed != Some(peer_id)) {
                        return Err("multiple, reconnected or unsolicited authenticated connections".into());
                    }
                    opts.upgrade_observer.established(connection_id, peer_id, &endpoint);
                    connection = Some(Connection { id:connection_id, peer:peer_id, endpoint });
                },
                SwarmEvent::ConnectionClosed { .. } if receipt.is_none() => return Err("connection closed before echo receipt".into()),
                SwarmEvent::OutgoingConnectionError { error, .. } => return Err(error.into()),
                SwarmEvent::ListenerError { error, .. } => return Err(error.into()),
                _ => {},
            },
            outcome = async { exchange.as_mut().expect("guarded exchange").await }, if exchange.is_some() => {
                let (peer, bytes) = outcome?;
                exchange = None;
                let c = connection.as_ref().ok_or("echo without authenticated connection")?;
                if c.peer != peer { return Err("echo moved to another peer".into()); }
                let addresses = discovered.peers.get(&peer).ok_or("discovery expired before echo completed")?;
                let proof = connection_receipt(c, local, &opts.upgrade_observer.snapshot(), outbound)?;
                let result = json!({"schema":SCHEMA,"implementation":"rust","role":role,"status":"ok",
                    "local_peer_id":local.to_string(),"service_name":SERVICE,
                    "discovery":{"source":"mdns","peer_id":peer.to_string(),
                        "addresses":addresses.iter().map(ToString::to_string).collect::<Vec<_>>()},
                    "connection":proof,"echo":{"protocol":application_observer::ECHO,"bytes":bytes,
                        "sha256":format!("{:x}",Sha256::digest(opts.payload.as_bytes())),
                        "connection_id":c.id.to_string(),"remote_peer_id":peer.to_string()}});
                if outbound { return Ok(result); }
                super::write_json(&opts.result_file, result.clone())?;
                receipt = Some(result);
            },
        }
    }
}

pub(super) async fn run(opts: Options) -> Result<()> {
    listen_address(&opts)?;
    let role = if opts.command == "dial" {
        "dialer"
    } else {
        "listener"
    };
    let outcome = execute(&opts).await;
    // execute owns the Swarm and all local echo futures. Drop them before
    // joining the existing fixture executor; no detached mDNS echo task.
    let report = opts.tasks.close_and_join().await;
    let outcome = match outcome {
        Ok(value) => {
            super::write_json(&opts.result_file, value)?;
            Ok(())
        }
        Err(error) => {
            super::write_json(
                &opts.result_file,
                json!({"schema":SCHEMA,"implementation":"rust","role":role,
                "service_name":SERVICE,"status":"error","error":error.to_string()}),
            )?;
            Err(error)
        }
    };
    let outcome = report.combine(outcome);
    report.write_result(
        &opts.result_file,
        role,
        "mdns",
        &outcome,
        Some(opts.upgrade_observer.finalized(true)),
    )?;
    outcome
}

#[cfg(test)]
mod tests {
    use super::*;

    fn options() -> Options {
        Options {
            command: "dial".into(),
            scenario: "mdns".into(),
            bind_ip: "192.0.2.1".into(),
            transport: "tcp".into(),
            payload: "challenge".into(),
            ready_file: "ready".into(),
            result_file: "result".into(),
            stop_file: "stop".into(),
            ..Default::default()
        }
    }

    #[test]
    fn hidden_peer_rejects_remote_inputs_and_private_rust() {
        assert!(listen_address(&options()).is_ok());
        let mut opts = options();
        opts.peer_id = PeerId::random().to_string();
        assert!(listen_address(&opts).is_err());
        let mut opts = options();
        opts.addr = "/ip4/192.0.2.2/tcp/4001".into();
        assert!(listen_address(&opts).is_err());
        let mut opts = options();
        opts.transport = "tcp-pnet".into();
        assert!(listen_address(&opts).is_err());
        for ip in ["127.0.0.1", "0.0.0.0", "224.0.0.251", "::1", "fe80::1%7"] {
            let mut opts = options();
            opts.bind_ip = ip.into();
            assert!(listen_address(&opts).is_err());
        }
    }

    #[test]
    fn discovery_deduplicates_expires_and_enforces_bounds() {
        let peer = PeerId::random();
        let address: Multiaddr = "/ip4/192.0.2.2/tcp/4001".parse().unwrap();
        let mut state = Discovery::default();
        state
            .observe(mdns::Event::Discovered(vec![(peer, address.clone()); 2]))
            .unwrap();
        assert_eq!(state.peers[&peer].len(), 1);
        state
            .observe(mdns::Event::Expired(vec![(peer, address)]))
            .unwrap();
        assert!(state.peers.is_empty());
        assert_eq!(state.bytes, 0);
        for port in 1..=16 {
            state
                .observe(mdns::Event::Discovered(vec![(
                    peer,
                    format!("/ip4/192.0.2.2/tcp/{port}").parse().unwrap(),
                )]))
                .unwrap();
        }
        assert!(
            state
                .observe(mdns::Event::Discovered(vec![(
                    peer,
                    "/ip4/192.0.2.2/tcp/17".parse().unwrap()
                )]))
                .is_err()
        );
    }

    #[test]
    fn receipt_uses_established_endpoint_not_requested_transport() {
        let connection = Connection {
            id: ConnectionId::new_unchecked(1),
            peer: PeerId::random(),
            endpoint: ConnectedPoint::Listener {
                local_addr: "/ip4/192.0.2.1/udp/4001/quic-v1".parse().unwrap(),
                send_back_addr: "/ip4/192.0.2.2/udp/5001/quic-v1".parse().unwrap(),
            },
        };
        let receipt = connection_receipt(&connection, PeerId::random(), &json!({}), false).unwrap();
        assert_eq!(receipt["transport"], "/quic-v1");
        assert_eq!(receipt["direction"], "inbound");
        assert!(connection_receipt(&connection, PeerId::random(), &json!({}), true).is_err());
        let missing_tcp = Connection {
            endpoint: ConnectedPoint::Listener {
                local_addr: "/ip4/192.0.2.1/tcp/4001".parse().unwrap(),
                send_back_addr: "/ip4/192.0.2.2/tcp/5001".parse().unwrap(),
            },
            ..connection
        };
        assert!(connection_receipt(&missing_tcp, PeerId::random(), &json!({}), false).is_err());
    }
}
