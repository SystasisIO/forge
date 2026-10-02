//! Bounded observations around the existing native Swarm, never a relay implementation.
use super::*;
use serde_json::Value;

const HOP: &str = "/libp2p/circuit/relay/0.2.0/hop";
const IDENTIFY: &str = "/ipfs/id/1.0.0";
const PUSH: &str = "/ipfs/id/push/1.0.0";

fn unix_ms() -> Result<u128, Box<dyn Error>> {
    Ok(SystemTime::now().duration_since(UNIX_EPOCH)?.as_millis())
}

fn write_atomic(path: &Path, value: Value) -> Result<(), Box<dyn Error>> {
    let temporary = path.with_extension("autorelay.tmp");
    write_json(&temporary, value)?;
    fs::rename(temporary, path)?;
    Ok(())
}

fn append(events: &mut Vec<Value>, mut value: Value) -> Result<(), Box<dyn Error>> {
    if events.len() >= 128 { return Err("AutoRelay event limit exceeded".into()); }
    value["unix_ms"] = json!(unix_ms()?);
    events.push(value);
    Ok(())
}

pub(crate) fn connection(connection_id: impl std::fmt::Display, peer: PeerId,
                         point: &libp2p::core::ConnectedPoint, opts: &Options) -> Value {
    opts.upgrade_observer.established(connection_id.to_string(), peer, point);
    // Raw upgrade traces retain their own IDs; no guessed Swarm-to-stream binding.
    json!({"connection_id": connection_id.to_string(), "peer_id": peer.to_string(),
        "remote_addr": point.get_remote_address().to_string(),
        "negotiated_transport": observed_quic_transport(point),
        "upgrade_observation": opts.upgrade_observer.snapshot()})
}

fn identify_addresses(raw: &[u8], peer: PeerId) -> Result<Vec<String>, Box<dyn Error>> {
    let mut reader = BytesReader::from_bytes(raw);
    let mut key = None;
    let mut addresses = Vec::new();
    while !reader.is_eof() {
        match reader.next_tag(raw)? {
            10 => {
                if key.is_some() { return Err("duplicate Identify public key".into()); }
                key = Some(identity::PublicKey::try_decode_protobuf(reader.read_bytes(raw)?)?);
            }
            18 => {
                if addresses.len() >= 16 { return Err("Identify address limit".into()); }
                addresses.push(Multiaddr::try_from(reader.read_bytes(raw)?.to_vec())?.to_string());
            }
            tag => reader.read_unknown(raw, tag)?,
        }
    }
    if key.ok_or("missing Identify public key")?.to_peer_id() != peer {
        return Err("Identify key differs from authenticated peer".into());
    }
    Ok(addresses)
}

fn captured_payload(hex: &str) -> Result<Vec<u8>, Box<dyn Error>> {
    if hex.len() > 8196 || hex.len() % 2 != 0 { return Err("native capture bound".into()); }
    let bytes = (0..hex.len()).step_by(2)
        .map(|i| u8::from_str_radix(&hex[i..i+2], 16)).collect::<Result<Vec<_>, _>>()?;
    let mut prefix = 0usize;
    let mut length = 0usize;
    loop {
        let byte = *bytes.get(prefix).ok_or("truncated native frame prefix")?;
        if prefix >= 2 { return Err("oversized native frame prefix".into()); }
        length |= ((byte & 127) as usize) << (prefix * 7);
        prefix += 1;
        if byte & 128 == 0 {
            if prefix > 1 && byte == 0 { return Err("noncanonical native frame prefix".into()); }
            break;
        }
    }
    if length == 0 || length > 4096 || prefix + length != bytes.len() {
        return Err("invalid native frame length".into());
    }
    Ok(bytes[prefix..].to_vec())
}

fn native_reservation_receipt(opts: &Options, relay: PeerId, peer: PeerId) -> Result<Value, Box<dyn Error>> {
    let observation = opts.upgrade_observer.snapshot();
    if observation["overflow"] != false { return Err("reservation trace overflow".into()); }
    let mut matches = Vec::new();
    for connection in observation["connections"].as_array().ok_or("missing native connections")? {
        if connection["authenticated_remote_peer_id"] != relay.to_string() { continue; }
        for stream in connection["streams"].as_array().ok_or("missing native streams")? {
            if stream["protocol"] == HOP && stream["direction"] == "outbound"
                && stream["parser_error"].is_null() && stream["io_failed"] == false
                && stream["read"]["complete_frames"] == true && stream["read"]["frames"] == 1 {
                matches.push((connection, stream));
            }
        }
    }
    if matches.len() != 1 { return Err("native reservation response absent/ambiguous".into()); }
    let (connection, stream) = matches[0];
    let framed_hex = stream["hop_read_framed_hex"].as_str().ok_or("missing HOP bytes")?;
    let raw = captured_payload(framed_hex)?;
    let mut reader = BytesReader::from_bytes(&raw);
    let (mut kind, mut status, mut reservation) = (None, None, None);
    while !reader.is_eof() {
        match reader.next_tag(&raw)? {
            8 if kind.is_none() => kind = Some(reader.read_uint32(&raw)?),
            40 if status.is_none() => status = Some(reader.read_uint32(&raw)?),
            26 if reservation.is_none() => reservation = Some(reader.read_bytes(&raw)?),
            8 | 40 | 26 => return Err("duplicate HOP response field".into()),
            tag => reader.read_unknown(&raw, tag)?,
        }
    }
    if kind != Some(2) || status != Some(100) { return Err("native HOP response not successful STATUS".into()); }
    let raw = reservation.ok_or("missing native reservation")?;
    let mut reader = BytesReader::from_bytes(raw);
    let (mut expiry, mut voucher) = (None, None);
    let mut addresses = Vec::new();
    while !reader.is_eof() {
        match reader.next_tag(raw)? {
            8 if expiry.is_none() => expiry = Some(reader.read_uint64(raw)?),
            18 => addresses.push(Multiaddr::try_from(reader.read_bytes(raw)?.to_vec())?.to_string()),
            26 if voucher.is_none() => voucher = Some(reader.read_bytes(raw)?),
            8 | 26 => return Err("duplicate reservation field".into()),
            tag => reader.read_unknown(raw, tag)?,
        }
    }
    let expiry = expiry.ok_or("missing native reservation expiry")?;
    let now = SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs();
    if expiry <= now || expiry > now + 9 || addresses.is_empty() || addresses.len() > 16 {
        return Err("native reservation expiry/address bound".into());
    }
    let mut value = json!({"basis": "passive_native_client_HOP_STATUS_response", "protocol": HOP,
        "status": 100, "expires_unix_ms": expiry * 1000, "addresses": addresses,
        "connection_trace_id": connection["connection_trace_id"], "stream_trace_id": stream["stream_trace_id"],
        "framed_hex": framed_hex, "read": stream["read"], "voucher": voucher.is_some(), "unix_ms": unix_ms()?});
    if let Some(voucher) = voucher {
        let envelope = SignedEnvelope::from_protobuf_encoding(voucher)?;
        let (raw, key) = envelope.payload_and_signing_key("libp2p-relay-rsvp".to_owned(), &[3, 2])?;
        if key.to_peer_id() != relay { return Err("voucher signer differs from authenticated relay".into()); }
        let mut reader = BytesReader::from_bytes(raw);
        let (mut signed_relay, mut signed_peer, mut signed_expiry) = (None, None, None);
        while !reader.is_eof() {
            match reader.next_tag(raw)? {
                10 if signed_relay.is_none() => signed_relay = Some(PeerId::from_bytes(reader.read_bytes(raw)?)?),
                18 if signed_peer.is_none() => signed_peer = Some(PeerId::from_bytes(reader.read_bytes(raw)?)?),
                24 if signed_expiry.is_none() => signed_expiry = Some(reader.read_uint64(raw)?),
                10 | 18 | 24 => return Err("duplicate voucher field".into()),
                tag => reader.read_unknown(raw, tag)?,
            }
        }
        if signed_relay != Some(relay) || signed_peer != Some(peer) || signed_expiry != Some(expiry) {
            return Err("signed voucher reservation identity/expiry mismatch".into());
        }
        value["voucher_validated"] = json!(true);
        value["voucher_validation_basis"] = json!("pinned_SignedEnvelope_signature_domain_payload_signer_peer_expiry");
        value["voucher_relay"] = json!(relay.to_string());
        value["voucher_peer"] = json!(peer.to_string());
        value["voucher_expiration"] = json!(expiry);
    }
    Ok(value)
}

async fn observe(swarm: &mut libp2p::Swarm<Behaviour>, peer: PeerId, revision: &str,
                 control: &Value, opts: &Options, events: &mut Vec<Value>,
                 seen: &mut std::collections::BTreeSet<(u64, u64)>) -> Result<Value, Box<dyn Error>> {
    let mut streams = swarm.behaviour().stream.new_control();
    let exchange = async {
        let mut stream = streams.open_stream(peer, StreamProtocol::new(IDENTIFY)).await?;
        let raw = read_raw_identify_frame(&mut stream).await?;
        drop(stream);
        Ok::<_, Box<dyn Error>>(identify_addresses(&raw, peer)?)
    };
    tokio::pin!(exchange);
    let deadline = tokio::time::sleep(Duration::from_secs(5));
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            result = &mut exchange => {
                let mut row = control.clone();
                row["kind"] = json!("identify");
                row["protocol"] = json!(IDENTIFY);
                row["basis"] = json!("independent_authenticated_identify_stream");
                row["addresses"] = json!(result?);
                row["revision"] = json!(revision);
                return Ok(row);
            }
            _ = &mut deadline => return Err("AutoRelay independent Identify deadline".into()),
            event = swarm.select_next_some() => {
                match event {
                    SwarmEvent::ConnectionClosed { peer_id, .. } if peer_id == peer => {
                        return Err("AutoRelay observer lost authenticated target connection".into());
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Identify(identify::Event::Received { peer_id, connection_id, info }))
                        if peer_id == peer => {
                        if let Some(row) = push_receipt(peer_id, connection_id, info, control, opts, seen)? {
                            append(events, row)?;
                        }
                    }
                    _ => {}
                }
            }
        }
    }
}

fn push_receipt(peer: PeerId, connection_id: impl std::fmt::Display, info: identify::Info,
                control: &Value, opts: &Options,
                seen: &mut std::collections::BTreeSet<(u64, u64)>) -> Result<Option<Value>, Box<dyn Error>> {
    if info.public_key.to_peer_id() != peer { return Err("native Identify key mismatch".into()); }
    let observation = opts.upgrade_observer.snapshot();
    if observation["overflow"] != false { return Err("native Push trace overflow".into()); }
    let mut fresh = Vec::new();
    let addresses = info.listen_addrs.iter().map(ToString::to_string).collect::<Vec<_>>();
    for connection in observation["connections"].as_array().ok_or("missing native traces")? {
        if connection["authenticated_remote_peer_id"] != peer.to_string() { continue; }
        for stream in connection["streams"].as_array().ok_or("missing native streams")? {
            if stream["protocol"] == PUSH && stream["direction"] == "inbound"
                && stream["parser_error"].is_null() && stream["io_failed"] == false
                && stream["read"]["complete_frames"] == true {
                let id = (connection["connection_trace_id"].as_u64().ok_or("missing trace ID")?,
                          stream["stream_trace_id"].as_u64().ok_or("missing stream ID")?);
                if !seen.contains(&id) {
                    let wire = stream["push_read_framed_hex"].as_str().ok_or("missing Push bytes")?;
                    let payload = captured_payload(wire)?;
                    if stream["read"]["frames"] != 1 { return Err("invalid Push frame".into()); }
                    // A concurrent ordinary Identify response must not consume this receipt.
                    if identify_addresses(&payload, peer)? != addresses { continue; }
                    fresh.push(json!({"connection_trace_id": id.0, "stream_trace_id": id.1,
                        "protocol": PUSH, "direction": "inbound", "read": stream["read"],
                        "framed_hex": wire}));
                    seen.insert(id);
                }
            }
        }
    }
    if fresh.len() > 1 { return Err("ambiguous native Identify Push receipt".into()); }
    if fresh.is_empty() { return Ok(None); }
    let mut row = control.clone();
    if row["connection_id"] != connection_id.to_string() { return Err("Push connection mismatch".into()); }
    row["kind"] = json!("identify_push");
    row["protocol"] = json!(PUSH);
    row["basis"] = json!("native_Identify_Received_with_unique_inbound_push_wire_receipt");
    row["wire_receipts"] = json!(fresh);
    row["addresses"] = json!(addresses);
    Ok(Some(row))
}

pub(crate) async fn run(opts: Options) -> Result<(), Box<dyn Error>> {
    let service = opts.command == "autorelay-relay";
    if opts.scenario != "autorelay" || (service && !(3..=30).contains(&opts.relay_ttl_seconds)) {
        return Err("AutoRelay service requires explicit native TTL/scenario".into());
    }
    let mut swarm = new_swarm(&opts).await?;
    let peer = *swarm.local_peer_id();
    let target = if service { None } else { Some(opts.peer_id.parse::<PeerId>()?) };
    if let Some(target) = target {
        swarm.dial(DialOpts::peer_id(target).addresses(vec![opts.addr.parse()?]).build())?;
    }
    let mut events = Vec::new();
    let mut ready = false;
    let mut revision = String::new();
    let mut control = None;
    let mut seen_pushes = std::collections::BTreeSet::new();
    let deadline = tokio::time::sleep(Duration::from_secs(55));
    tokio::pin!(deadline);
    let mut tick = tokio::time::interval(Duration::from_millis(100));
    let result = loop {
        tokio::select! {
            _ = &mut deadline => break Err::<(), Box<dyn Error>>("AutoRelay fixture deadline".into()),
            _ = tick.tick() => {
                if opts.stop_file.exists() { break Ok(()); }
                if let (Some(target), Some(control)) = (target, &control)
                    && let Ok(request) = fs::read_to_string(&opts.probe_file)
                    && request != revision {
                    if request.len() > 64 { break Err("probe revision limit".into()); }
                    let row = observe(&mut swarm, target, &request, control, &opts, &mut events, &mut seen_pushes).await?;
                    append(&mut events, row)?;
                    revision = request;
                }
                write_atomic(&opts.result_file, json!({"schema_version": 1, "implementation": "rust", "scenario": "autorelay",
                    "role": if service { "service" } else { "observer" }, "peer_id": peer.to_string(),
                    "transport": opts.transport, "complete": false, "overflow": false,
                    "native_ttl_seconds": opts.relay_ttl_seconds, "events": events}))?;
            }
            event = swarm.select_next_some() => {
                match event {
                    SwarmEvent::NewListenAddr { address, .. } if !ready => {
                        swarm.add_external_address(address.clone());
                        write_atomic(&opts.ready_file, json!({"implementation": "rust", "peer_id": peer.to_string(),
                            "listen_addrs": [format!("{address}/p2p/{peer}")], "status": "ready"}))?;
                        ready = true;
                    }
                    SwarmEvent::ConnectionEstablished { peer_id, connection_id, endpoint, .. } => {
                        let mut row = connection(connection_id, peer_id, &endpoint, &opts);
                        row["kind"] = json!("connection");
                        if Some(peer_id) == target { control = Some(row.clone()); }
                        append(&mut events, row)?;
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Identify(identify::Event::Received { peer_id, connection_id, info }))
                        if Some(peer_id) == target => {
                        if let Some(row) = push_receipt(peer_id, connection_id, info,
                                control.as_ref().ok_or("Push lacks authenticated control")?, &opts, &mut seen_pushes)? {
                            append(&mut events, row)?;
                        }
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Relay(relay::Event::ReservationReqAccepted { src_peer_id, renewed })) => {
                        append(&mut events, json!({"kind": "reservation_accepted", "peer_id": src_peer_id.to_string(),
                            "renewed": renewed, "protocol": HOP, "basis": "native_relay_ReservationReqAccepted"}))?;
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Relay(relay::Event::CircuitReqAccepted { src_peer_id, dst_peer_id })) => {
                        append(&mut events, json!({"kind": "circuit_accepted", "peer_id": src_peer_id.to_string(),
                            "target_peer_id": dst_peer_id.to_string(), "protocol": HOP}))?;
                    }
                    _ => {}
                }
            }
        }
    };
    drop(swarm);
    write_atomic(&opts.result_file, json!({"schema_version": 1, "implementation": "rust", "scenario": "autorelay",
        "role": if service { "service" } else { "observer" }, "peer_id": peer.to_string(), "transport": opts.transport,
        "complete": result.is_ok(), "overflow": false, "native_ttl_seconds": opts.relay_ttl_seconds,
        "events": events, "error": result.as_ref().err().map(|e| e.to_string())}))?;
    result
}

pub(crate) async fn destination(mut swarm: libp2p::Swarm<Behaviour>, opts: Options) -> Result<(), Box<dyn Error>> {
    let relay: PeerId = opts.relay_peer_id.parse()?;
    let remote = relay_transport_addr(opts.relay_addr.parse()?, relay).map_err(|_| "relay peer mismatch")?;
    let reservation = reserve_relay_address(&mut swarm, relay, remote, &opts).await?;
    let receipt = native_reservation_receipt(&opts, relay, *swarm.local_peer_id())?;
    write_atomic(&opts.ready_file, json!({"implementation": "rust", "role": "destination", "status": "ready",
        "peer_id": swarm.local_peer_id().to_string(), "relay_addrs": [reservation.circuit_addr],
        "relay_peer_id": relay.to_string(), "protocol": HOP, "reservation_accepted": reservation.accepted,
        "reservation_basis": "native_relay_client_ReservationReqAccepted_and_NewListenAddr",
        "relay_connection": reservation.control_connection.ok_or("missing authenticated reservation control")?,
        "native_reservation_receipt": receipt}))?;
    let deadline = tokio::time::sleep(Duration::from_secs(45));
    tokio::pin!(deadline);
    let mut tick = tokio::time::interval(Duration::from_millis(100));
    loop {
        tokio::select! {
            _ = tick.tick() => if opts.stop_file.exists() { return Ok(()); },
            _ = &mut deadline => return Err("AutoRelay donor destination deadline".into()),
            _ = swarm.select_next_some() => {}
        }
    }
}
