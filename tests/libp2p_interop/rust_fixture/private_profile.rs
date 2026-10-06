//! Private contracts use the native donor Behaviours and actual protocol streams.
//! Connection upgrades remain reusable by the independent path suite.
use super::*;
use futures::{AsyncRead, AsyncReadExt, AsyncWriteExt};
use sha2::{Digest, Sha256};

const UNKNOWN: &str = "/forge/interop/private-unknown/1";

pub(crate) fn is_transport(value: &str) -> bool {
    matches!(value, "tcp-pnet-noise" | "tcp-pnet-tls")
}

fn transport_address(value: &str, peer: PeerId) -> Result<Multiaddr, Box<dyn Error>> {
    let mut address = Multiaddr::from_str(value)?;
    if address.pop() != Some(Protocol::P2p(peer)) {
        return Err("private peer/address mismatch".into());
    }
    let parts: Vec<_> = address.iter().collect();
    if !matches!(parts.as_slice(), [Protocol::Ip4(ip), Protocol::Tcp(port)]
        if ip.is_loopback() && *port != 0) {
        return Err("private dial requires one numeric loopback TCP address and peer suffix".into());
    }
    Ok(address)
}

fn protocol(scenario: &str) -> Result<&'static str, Box<dyn Error>> {
    if scenario.starts_with("inline_muxer_") { return Ok(application_observer::ECHO); }
    Ok(match scenario {
        "tcp_yamux_private_pnet" | "multistream_select_private_pnet" |
        "noise_identity_private_pnet" | "tls_identity_private_pnet" => application_observer::ECHO,
        "ping_private_tcp_yamux_pnet" => "/ipfs/ping/1.0.0",
        "identify_private_tcp_yamux_pnet" => application_observer::IDENTIFY,
        "kademlia_amino_private_tcp_yamux_pnet" => "/ipfs/kad/1.0.0",
        "rendezvous_rust_private_tcp_yamux_pnet" => "/rendezvous/1.0.0",
        _ => return Err("unknown private contract".into()),
    })
}

fn frame(body: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    let mut size = body.len();
    while size >= 128 { out.push((size as u8 & 127) | 128); size >>= 7; }
    out.push(size as u8); out.extend_from_slice(body); out
}

async fn read_public_frame<S: AsyncRead + Unpin>(stream: &mut S) -> Result<Vec<u8>, Box<dyn Error>> {
    let mut prefix = Vec::new();
    let mut size = 0usize;
    loop {
        let mut byte = [0u8]; stream.read_exact(&mut byte).await?;
        if prefix.len() == 3 { return Err("private frame header exceeds bound".into()); }
        size |= usize::from(byte[0] & 127) << (7 * prefix.len());
        prefix.push(byte[0]);
        if byte[0] & 128 == 0 {
            if size == 0 || size > 8192 || (prefix.len() > 1 && byte[0] == 0) {
                return Err("noncanonical or oversized private frame".into());
            }
            break;
        }
    }
    let start = prefix.len(); prefix.resize(start + size, 0);
    stream.read_exact(&mut prefix[start..]).await?; Ok(prefix)
}

// Same byte receipt shape as the Stage 6 and path wire validators. Only public
// application protocol frames are captured, never protector/security traffic.
fn receipt(data: &[u8], raw: bool) -> serde_json::Value {
    json!({"framed_hex": data.iter().map(|byte| format!("{byte:02x}")).collect::<String>(),
        "raw": raw, "read": {"framed_bytes": data.len(), "framed_sha256": format!("{:x}", Sha256::digest(data)),
        "frames": 1, "complete_frames": true, "invalid_or_over_limit": false}})
}

fn bytes_field(mut data: &[u8], wanted: u64) -> Result<&[u8], Box<dyn Error>> {
    fn integer(data: &mut &[u8]) -> Result<u64, Box<dyn Error>> {
        let mut value = 0u64;
        for index in 0..10 {
            let byte = *data.first().ok_or("truncated public protobuf")?;
            *data = &data[1..];
            if index == 9 && byte > 1 { return Err("overflowed public protobuf integer".into()); }
            value |= u64::from(byte & 127) << (7 * index);
            if byte & 128 == 0 { return Ok(value); }
        }
        Err("overflowed public protobuf integer".into())
    }
    let mut found = None;
    while !data.is_empty() {
        let tag = integer(&mut data)?;
        match tag & 7 {
            0 => { integer(&mut data)?; }
            2 => {
                let length = usize::try_from(integer(&mut data)?)?;
                if length > data.len() { return Err("truncated public protobuf bytes".into()); }
                let (value, rest) = data.split_at(length); data = rest;
                if tag >> 3 == wanted {
                    if found.is_some() { return Err("ambiguous public protobuf field".into()); }
                    found = Some(value);
                }
            }
            _ => return Err("unsupported public protobuf wire type".into()),
        }
    }
    found.ok_or_else(|| "absent public protobuf field".into())
}

pub(crate) fn connection_receipt(observer: &upgrade_observer::Observer, remote: PeerId) -> Result<serde_json::Value, Box<dyn Error>> {
    let proof = observer.snapshot();
    let matches: Vec<_> = proof["connections"].as_array().ok_or("missing connections")?.iter()
        .filter(|c| c["authenticated_remote_peer_id"] == remote.to_string() && c["security_complete"] == true && c["muxer_complete"] == true).collect();
    if matches.len() != 1 { return Err("private stream lacks one completed authenticated connection".into()); }
    let c = matches[0];
    Ok(json!({"source": "rust-libp2p.completed-public-upgrades", "connection_id": c["connection_trace_id"],
        "local_peer_id": c["authenticated_local_peer_id"], "remote_peer_id": c["authenticated_remote_peer_id"],
        "local_address": c["local_address"], "remote_address": c["remote_address"], "transport": "tcp",
        "security": c["selected_security"], "muxer": c["selected_muxer"]}))
}

async fn wait_native_identify(swarm: &mut libp2p::Swarm<Behaviour>, remote: PeerId,
                              observer: &upgrade_observer::Observer) -> Result<(), Box<dyn Error>> {
    let deadline = tokio::time::sleep(Duration::from_secs(15)); tokio::pin!(deadline);
    loop { tokio::select! {
        _ = &mut deadline => return Err("private automatic Identify did not complete before the correlation barrier".into()),
        event = swarm.select_next_some() => match event {
            SwarmEvent::Behaviour(BehaviourEvent::Identify(identify::Event::Received { peer_id, info, .. })) if peer_id == remote => {
                if info.public_key.to_peer_id() != remote || !swarm.is_connected(&remote) {
                    return Err("private automatic Identify differs from the retained authenticated peer".into());
                }
                connection_receipt(observer, remote)?;
                return Ok(());
            }
            SwarmEvent::Behaviour(BehaviourEvent::Identify(identify::Event::Error { peer_id, .. })) if peer_id == remote => {
                return Err("private automatic Identify failed before the correlation barrier".into());
            }
            SwarmEvent::ConnectionClosed { peer_id, .. } if peer_id == remote => {
                return Err("private connection closed before automatic Identify completed".into());
            }
            SwarmEvent::ConnectionEstablished { peer_id, connection_id, endpoint, .. } => observer.established(connection_id, peer_id, &endpoint),
            SwarmEvent::NewListenAddr { address, .. } => swarm.add_external_address(address),
            _ => {},
        },
    }}
}

async fn reject_unknown(swarm: &mut libp2p::Swarm<Behaviour>, observer: &upgrade_observer::Observer,
                        remote: PeerId) -> Result<serde_json::Value, Box<dyn Error>> {
    if !swarm.is_connected(&remote) { return Err("private barrier requires the retained authenticated connection".into()); }
    let connection = connection_receipt(observer, remote)?;
    let connection_id = usize::try_from(connection["connection_id"].as_u64().ok_or("missing raw connection identity")?)?;
    let proof = observer.snapshot();
    let connections = proof["connections"].as_array().ok_or("missing raw connections")?;
    let c = connections.iter().find(|c| c["connection_trace_id"] == connection_id).ok_or("absent retained raw connection")?;
    let after_sequence = usize::try_from(c["events"].as_array().and_then(|events| events.last())
        .and_then(|e| e["sequence"].as_u64()).ok_or("absent retained connection events")?)?;
    let mut control = swarm.behaviour().stream.new_control();
    let open = control.open_stream(remote, StreamProtocol::new(UNKNOWN));
    tokio::pin!(open);
    let deadline = tokio::time::sleep(Duration::from_secs(10)); tokio::pin!(deadline);
    loop { tokio::select! {
        out = &mut open => match out {
            Err(raw_stream::OpenStreamError::UnsupportedProtocol(protocol)) if protocol.as_ref() == UNKNOWN => break,
            _ => return Err("private barrier did not produce the native requested-protocol rejection".into()),
        },
        _ = &mut deadline => return Err("private unknown protocol rejection timed out".into()),
        event = swarm.select_next_some() => match event {
            SwarmEvent::ConnectionClosed { peer_id, .. } if peer_id == remote => {
                return Err("private connection closed during the protocol barrier".into());
            }
            SwarmEvent::ConnectionEstablished { peer_id, connection_id, endpoint, .. } => observer.established(connection_id, peer_id, &endpoint),
            SwarmEvent::NewListenAddr { address, .. } => swarm.add_external_address(address),
            _ => {},
        },
    }}
    if !swarm.is_connected(&remote) || connection_receipt(observer, remote)? != connection {
        return Err("private barrier changed its authenticated connection".into());
    }
    let mut receipt = observer.rejection_receipt(connection_id, after_sequence, UNKNOWN)?;
    receipt["native_error"] = json!("unsupported_protocol");
    Ok(receipt)
}

async fn exchange(swarm: &mut libp2p::Swarm<Behaviour>, observer: &upgrade_observer::Observer,
                  remote: PeerId, id: &'static str, request: &[u8]) -> Result<serde_json::Value, Box<dyn Error>> {
    let mut control = swarm.behaviour().stream.new_control();
    let exchange = async {
        let mut stream = control.open_stream(remote, StreamProtocol::new(id)).await?;
        let connection = connection_receipt(observer, remote)?;
        if !request.is_empty() { stream.write_all(request).await?; stream.flush().await?; }
        let raw = id == "/ipfs/ping/1.0.0";
        let response = if raw { let mut response = vec![0; 32]; stream.read_exact(&mut response).await?; response }
            else { read_public_frame(&mut stream).await? };
        if (raw || id == application_observer::ECHO) && response != request { return Err("private response mismatch".into()); }
        stream.close().await?;
        let mut out = json!({"connection_receipt": connection, "protocol": id, "stream_closed": true,
            "response": receipt(&response, raw)});
        if id == application_observer::IDENTIFY {
            let prefix = response.iter().position(|byte| byte & 128 == 0).ok_or("missing frame prefix")? + 1;
            let verified = raw_identify_evidence(&response[prefix..], remote);
            if verified["status"] != "verified" { return Err("private Identify signature or identity validation failed".into()); }
            out["identify_verified"] = json!(true);
            out["raw_identify_exchange"] = verified;
        }
        if !request.is_empty() { out["request"] = receipt(request, raw); }
        Ok::<_, Box<dyn Error>>(out)
    };
    tokio::pin!(exchange);
    let deadline = tokio::time::sleep(Duration::from_secs(15)); tokio::pin!(deadline);
    loop {
        tokio::select! {
            out = &mut exchange => return out,
            _ = &mut deadline => return Err("private protocol exchange timed out".into()),
            event = swarm.select_next_some() => {
                if let SwarmEvent::ConnectionEstablished { peer_id, connection_id, endpoint, .. } = event {
                    observer.established(connection_id, peer_id, &endpoint);
                }
            }
        }
    }
}

pub(crate) async fn run(opts: Options) -> Result<(), Box<dyn Error>> {
    let mut swarm = new_swarm(&opts).await?;
    spawn_incoming_stream_echo(&mut swarm, application_observer::ECHO, None, &opts.tasks)?;
    let local = *swarm.local_peer_id();
    let listener = opts.command == "listen";
    if !listener && opts.command != "dial" { return Err("private fixture requires listen or dial".into()); }
    let mut result = json!({"implementation":"rust", "role": if listener { "listener" } else { "dialer" },
        "scenario":opts.scenario, "local_peer_id":local.to_string(), "status":"ok", "pnet_fingerprint":opts.pnet_fingerprint});
    let mut observed = PnetObservation::default();
    let mut remote = None;
    if !listener {
        let peer = PeerId::from_str(&opts.peer_id)?;
        // The pinned Swarm supplies the target /p2p component itself. Give it
        // the validated bare transport address, never a second peer suffix.
        let addr = transport_address(&opts.addr, peer)?;
        swarm.dial(DialOpts::peer_id(peer).addresses(vec![addr]).build())?;
        remote = Some(peer); observed.attempted_connections = 1;
    }
    let deadline = tokio::time::sleep(Duration::from_secs(25)); tokio::pin!(deadline);
    loop {
        tokio::select! {
            _ = tokio::time::sleep(Duration::from_millis(25)), if listener => {
                if opts.stop_file.exists() {
                    if !opts.pnet_control.is_empty() { result = pnet_rejection(&opts, "listener", "", observed); }
                    return write_json(&opts.result_file, result);
                }
                if opts.pnet_control.is_empty() {
                    result["upgrade_observation"] = opts.upgrade_observer.snapshot();
                    write_json(&opts.result_file, result.clone())?;
                }
            }
            _ = &mut deadline, if !listener => {
                // Timeout is not a rejection receipt: a donor failure event must
                // have been received for every successful negative control.
                return Err("private dial timed out without rejection event".into());
            }
            event = swarm.select_next_some() => match event {
                SwarmEvent::NewListenAddr { address, .. } => {
                    swarm.add_external_address(address.clone());
                    if listener { write_json(&opts.ready_file, json!({"implementation":"rust", "role":"listener", "status":"ready",
                        "peer_id":local.to_string(), "listen_addrs":[format!("{address}/p2p/{local}")]}))?; }
                }
                SwarmEvent::IncomingConnection { .. } => { observed.attempted_connections += 1; }
                SwarmEvent::OutgoingConnectionError { .. } if !listener && !opts.pnet_control.is_empty() => {
                    return write_json(&opts.result_file, pnet_rejection(&opts, "dialer", &opts.peer_id, observed));
                }
                SwarmEvent::ConnectionEstablished { peer_id, connection_id, endpoint, .. } => {
                    observed.established_connections += 1;
                    opts.upgrade_observer.established(connection_id, peer_id, &endpoint);
                    if !opts.pnet_control.is_empty() { return Err("private rejection control authenticated a peer".into()); }
                    if !listener && Some(peer_id) == remote { break; }
                }
                _ => {}
            }
        }
    }
    let remote = remote.ok_or("absent dial peer")?;
    let id = protocol(&opts.scenario)?;
    let request = match id {
        application_observer::ECHO => frame(opts.payload.as_bytes()),
        "/ipfs/ping/1.0.0" => (1..=32).collect(),
        "/ipfs/id/1.0.0" => Vec::new(),
        "/ipfs/kad/1.0.0" => {
            let key = remote.to_bytes(); let mut body = vec![8, 4, 18, key.len() as u8]; body.extend_from_slice(&key); frame(&body)
        }
        "/rendezvous/1.0.0" => {
            let proof = wait_rendezvous_register_discover(&mut swarm, remote).await?;
            result["rendezvous"] = json!({"signed_peer_record_valid":true, "record_sequence":proof.record_sequence,
                "record_address_count":proof.record_address_count, "registered_ttl_seconds":proof.registered_ttl_seconds,
                "discovered_ttl_seconds":proof.discovered_ttl_seconds, "wire_registration_count":proof.wire_registration_count,
                "cookie_bytes":proof.cookie_bytes});
            let mut discover = vec![10, 15]; discover.extend_from_slice(b"forge.discovery"); discover.extend_from_slice(&[16, 10]);
            let mut body = vec![8, 3, 42, discover.len() as u8]; body.extend(discover); frame(&body)
        }
        _ => unreachable!(),
    };
    if id == application_observer::IDENTIFY {
        // Keep production automatic Identify enabled and wait for its actual
        // native result, rather than allowing it to race the manual exchange.
        wait_native_identify(&mut swarm, remote, &opts.upgrade_observer).await?;
    }
    let barrier = if matches!(id, application_observer::IDENTIFY | "/rendezvous/1.0.0") {
        // The native Rendezvous Register/Discover above has also completed.
        // Both sides can distinguish the next stream by their own observed na,
        // without assuming that donor-native stream IDs equal raw muxer IDs.
        Some(reject_unknown(&mut swarm, &opts.upgrade_observer, remote).await?)
    } else { None };
    let application = exchange(&mut swarm, &opts.upgrade_observer, remote, id, &request).await?;
    for (key, value) in application.as_object().ok_or("invalid application receipt")? { result[key] = value.clone(); }
    if let Some(barrier) = barrier {
        if result["connection_receipt"]["connection_id"] != barrier["connection_trace_id"] {
            return Err("private application escaped its observed protocol barrier connection".into());
        }
        result["protocol_barrier"] = barrier;
    }
    if id == "/rendezvous/1.0.0" {
        let wire = decode_hex(result["response"]["framed_hex"].as_str().ok_or("absent Rendezvous frame")?)?;
        let prefix = wire.iter().position(|byte| byte & 128 == 0).ok_or("missing frame prefix")? + 1;
        let discovered = bytes_field(&wire[prefix..], 6)?;
        let registration = bytes_field(discovered, 1)?;
        let encoded = bytes_field(registration, 2)?;
        let envelope = libp2p::core::SignedEnvelope::from_protobuf_encoding(encoded)?;
        let record = libp2p::core::PeerRecord::from_signed_envelope(envelope)?;
        if record.peer_id() != local || record.seq() == 0 || record.addresses().is_empty() {
            return Err("private Rendezvous raw record differs from registered identity".into());
        }
        result["rendezvous_verified"] = json!(true);
    }
    if opts.scenario == "multistream_select_private_pnet" {
        let mut control = swarm.behaviour().stream.new_control();
        let open = control.open_stream(remote, StreamProtocol::new(UNKNOWN));
        tokio::pin!(open);
        let deadline = tokio::time::sleep(Duration::from_secs(10)); tokio::pin!(deadline);
        loop { tokio::select! {
            out = &mut open => match out {
                Err(raw_stream::OpenStreamError::UnsupportedProtocol(_)) => { result["unknown_protocol_rejected"] = json!(true); break; }
                _ => return Err("unknown protocol did not produce typed rejection".into()),
            },
            _ = &mut deadline => return Err("unknown protocol rejection timed out".into()),
            _ = swarm.select_next_some() => {},
        }}
    }
    write_json(&opts.result_file, result)?;
    loop { tokio::select! {
        _ = tokio::time::sleep(Duration::from_millis(25)) => { if opts.stop_file.exists() { break; } },
        event = swarm.select_next_some() => {
            if let SwarmEvent::ConnectionEstablished { peer_id, connection_id, endpoint, .. } = event {
                opts.upgrade_observer.established(connection_id, peer_id, &endpoint);
            }
        }
    }}
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn private_dial_has_exactly_one_peer_suffix_before_native_swarm_dial() {
        let peer = identity::Keypair::generate_ed25519().public().to_peer_id();
        let socket = "/ip4/127.0.0.1/tcp/4001";
        assert_eq!(transport_address(&format!("{socket}/p2p/{peer}"), peer).unwrap().to_string(), socket);
        let other = identity::Keypair::generate_ed25519().public().to_peer_id();
        for value in [socket.to_string(), format!("{socket}/p2p/{other}"),
            format!("{socket}/p2p/{peer}/p2p/{peer}"), format!("/ip4/127.0.0.1/tcp/0/p2p/{peer}")] {
            assert!(transport_address(&value, peer).is_err());
        }
    }

    #[tokio::test]
    async fn public_frame_is_canonical_bounded_and_complete() {
        let expected = frame(b"private-profile-exchange");
        assert_eq!(read_public_frame(&mut futures::io::Cursor::new(expected.clone())).await.unwrap(), expected);
        for bad in [vec![0], vec![0x81, 0, b'x'], vec![3, b'x'], vec![0xff, 0xff, 0xff, 1], vec![0x81, 0x40]] {
            assert!(read_public_frame(&mut futures::io::Cursor::new(bad)).await.is_err());
        }
    }

    #[test]
    fn duplicate_or_truncated_public_protobuf_field_is_not_verification() {
        assert!(bytes_field(&[10, 1, b'x', 10, 1, b'y'], 1).is_err());
        assert!(bytes_field(&[10, 4, b'x'], 1).is_err());
        assert!(bytes_field(&[13, 1], 1).is_err());
    }
}
