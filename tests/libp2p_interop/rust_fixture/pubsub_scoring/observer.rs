//! Passive successful native I/O, multistream selection and exact RPC receipts.
use super::{
    CONNECTION_LIMIT, EVENT_BYTE_LIMIT, EVENT_LIMIT, FRAME_LIMIT, STREAM_LIMIT, TRACE_BYTE_LIMIT,
    invalid,
};
use futures::{AsyncRead, AsyncWrite};
use libp2p::swarm::ConnectionId;
use libp2p::{
    PeerId,
    core::{
        ConnectedPoint,
        muxing::{StreamMuxer, StreamMuxerBox, StreamMuxerEvent, SubstreamBox},
    },
};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{
    error::Error as StdError,
    io,
    pin::Pin,
    sync::{Arc, Mutex},
    task::{Context, Poll},
    time::Instant,
};

const WIRE_SOURCE: &str = "rust.libp2p.passive-upgraded-stream-io";
pub(super) const QUIC_CAUSE_OBSERVER_ENABLED: bool = cfg!(feature = "quic-cause-observer");

// Do not infer normal closure from io::ErrorKind or a diagnostic string.
fn connection_cause(error: &quinn::ConnectionError) -> (&'static str, bool) {
    match error {
        quinn::ConnectionError::ApplicationClosed(close) if close.error_code == 0u32.into() => {
            ("quinn_application_closed_0", true)
        }
        quinn::ConnectionError::LocallyClosed => ("quinn_locally_closed", true),
        quinn::ConnectionError::ApplicationClosed(_) => ("quinn_application_closed_nonzero", false),
        quinn::ConnectionError::ConnectionClosed(_) => ("quinn_connection_closed", false),
        quinn::ConnectionError::Reset => ("quinn_reset", false),
        quinn::ConnectionError::TimedOut => ("quinn_timed_out", false),
        quinn::ConnectionError::TransportError(_) => ("quinn_transport_error", false),
        _ => ("quinn_other_connection_error", false),
    }
}

fn native_cause(error: &io::Error) -> (&'static str, bool) {
    let mut current: &(dyn StdError + 'static) = error;
    for _ in 0..8 {
        if let Some(error) = current.downcast_ref::<quinn::ConnectionError>() {
            return connection_cause(error);
        }
        if let Some(quinn::ReadError::ConnectionLost(error)) =
            current.downcast_ref::<quinn::ReadError>()
        {
            return connection_cause(error);
        }
        if let Some(quinn::WriteError::ConnectionLost(error)) =
            current.downcast_ref::<quinn::WriteError>()
        {
            return connection_cause(error);
        }
        if let Some(libp2p::quic::Error::Connection(connection)) =
            current.downcast_ref::<libp2p::quic::Error>()
        {
            #[cfg(feature = "quic-cause-observer")]
            {
                // Only the isolated, provenance-checked donor copy has this read-only accessor.
                return connection_cause(connection.inner());
            }
            #[cfg(not(feature = "quic-cause-observer"))]
            let _ = connection;
            #[cfg(not(feature = "quic-cause-observer"))]
            {
                // The pinned donor's transparent private wrapper erases the public Quinn cause.
                return ("quic_connection_cause_unavailable", false);
            }
        }
        let next = if let Some(error) = current.downcast_ref::<io::Error>() {
            error
                .get_ref()
                .map(|inner| inner as &(dyn StdError + 'static))
        } else {
            current.source()
        };
        let Some(next) = next else {
            return ("unclassified", false);
        };
        current = next;
    }
    ("source_chain_limit", false)
}

#[derive(Clone, Copy, Debug)]
pub(super) enum NativeStack {
    Quic,
    NoiseYamux,
    PnetNoiseYamux,
}

impl NativeStack {
    fn facts(self) -> Value {
        match self {
            Self::Quic => {
                json!({"transport": "quic", "security": "/tls/1.0.0", "muxer": "quic", "authentication_basis": "native_QUIC_authenticated_transport_output"})
            }
            Self::NoiseYamux => {
                json!({"transport": "tcp", "security": "/noise", "muxer": "/yamux/1.0.0", "authentication_basis": "native_Noise_authenticated_Yamux_upgrade_output", "muxer_error_boundary": crate::upgrade_observer::yamux_error::BOUNDARY})
            }
            Self::PnetNoiseYamux => {
                json!({"transport": "tcp", "security": "/noise", "muxer": "/yamux/1.0.0", "authentication_basis": "native_PNET_then_Noise_authenticated_Yamux_upgrade_output", "muxer_error_boundary": crate::upgrade_observer::yamux_error::BOUNDARY})
            }
        }
    }
}

pub(super) struct Connection {
    pub(super) peer: PeerId,
    pub(super) point: ConnectedPoint,
    pub(super) stack: NativeStack,
    pub(super) swarm_id: Option<String>,
    pub(super) native_id: Option<ConnectionId>,
    pub(super) streams: usize,
    pub(super) dropped: bool,
    pub(super) closed: bool,
    pub(super) terminal: bool,
}

pub(super) struct Capture {
    pub(super) started: Instant,
    pub(super) events: Vec<Value>,
    pub(super) connections: Vec<Connection>,
    pub(super) live_muxers: usize,
    pub(super) live_streams: usize,
    pub(super) streams_created: usize,
    pub(super) trace_bytes: usize,
    pub(super) fingerprint: Option<String>,
    pub(super) overflow: bool,
    pub(super) error: Option<String>,
    pub(super) prepared: bool,
}

#[derive(Clone)]
pub(super) struct Evidence(Arc<Mutex<Capture>>);

impl Default for Evidence {
    fn default() -> Self {
        Self(Arc::new(Mutex::new(Capture {
            started: Instant::now(),
            events: Vec::new(),
            connections: Vec::new(),
            live_muxers: 0,
            live_streams: 0,
            streams_created: 0,
            trace_bytes: 0,
            fingerprint: None,
            overflow: false,
            error: None,
            prepared: false,
        })))
    }
}

impl Capture {
    pub(super) fn fail(&mut self, message: impl std::fmt::Display) {
        if self.error.is_none() {
            self.error = Some(message.to_string().chars().take(512).collect());
        }
    }

    fn native_error(
        &mut self,
        connection: Option<usize>,
        stream: Option<usize>,
        operation: &str,
        error: &io::Error,
    ) {
        let yamux_state = crate::upgrade_observer::yamux_error::terminal_state(error);
        let prepare_ack = self
            .events
            .iter()
            .find(|event| event["kind"] == "shutdown_prepared")
            .and_then(|event| event["sequence"].as_u64());
        let terminal = self.prepared
            && self.error.is_none()
            && !self.overflow
            && prepare_ack.is_some()
            && yamux_state.is_some()
            && stream.is_none()
            && matches!(
                operation,
                "muxer_inbound" | "muxer_outbound" | "muxer_poll" | "muxer_close"
            )
            && connection.is_some_and(|id| {
                self.connections.get(id).is_some_and(|owner| {
                    matches!(
                        owner.stack,
                        NativeStack::NoiseYamux | NativeStack::PnetNoiseYamux
                    ) && owner.native_id.is_some()
                        && owner.swarm_id.is_some()
                        && !owner.dropped
                })
            });
        if terminal {
            self.owned(
                connection.unwrap(),
                None,
                "native_terminal_state",
                json!({
                    "operation": operation, "io_kind": format!("{:?}", error.kind()),
                    "raw_os_error": error.raw_os_error(), "typed_cause": yamux_state.unwrap(),
                    "prepared": true, "prepare_ack_sequence": prepare_ack.unwrap(),
                    "error_boundary": crate::upgrade_observer::yamux_error::BOUNDARY,
                    "closure_reason": "unknown",
                    "message": error.to_string().chars().take(512).collect::<String>()
                }),
            );
            return;
        }
        let (cause, normal) = native_cause(error);
        let expected = self.prepared
            && normal
            && connection.is_some_and(|id| {
                self.connections
                    .get(id)
                    .is_some_and(|owner| matches!(owner.stack, NativeStack::Quic))
            });
        let detail = json!({"operation": operation, "io_kind": format!("{:?}", error.kind()),
            "raw_os_error": error.raw_os_error(), "typed_cause": cause, "prepared": self.prepared,
            "message": error.to_string().chars().take(512).collect::<String>()});
        if !expected {
            self.fail(error);
        }
        if let Some(connection) = connection {
            self.owned(
                connection,
                stream,
                if expected {
                    "expected_native_close"
                } else {
                    "native_io_error"
                },
                detail,
            );
        } else {
            self.fail("native I/O error without authenticated connection owner");
        }
    }

    pub(super) fn emit(&mut self, kind: &str, source: &str, mut detail: Value) {
        if self.events.len() == EVENT_LIMIT {
            self.overflow = true;
            self.fail("native event log overflow");
            return;
        }
        detail["sequence"] = json!(self.events.len() + 1);
        detail["mono_ns"] =
            json!(self.started.elapsed().as_nanos().clamp(1, u64::MAX as u128) as u64);
        detail["kind"] = json!(kind);
        detail["source"] = json!(source);
        let bytes = match serde_json::to_vec(&detail) {
            Ok(bytes) => bytes.len(),
            Err(e) => {
                self.fail(e);
                return;
            }
        };
        if bytes > EVENT_BYTE_LIMIT
            || self.trace_bytes + bytes + self.events.len() + 3 > TRACE_BYTE_LIMIT
        {
            self.overflow = true;
            self.fail("native event/trace byte limit exceeded");
            return;
        }
        self.trace_bytes += bytes;
        self.events.push(detail);
    }

    fn owned(&mut self, connection: usize, stream: Option<usize>, kind: &str, mut detail: Value) {
        let Some(owner) = self.connections.get(connection) else {
            return;
        };
        detail["connection_trace_id"] = json!(connection + 1);
        detail["stream_trace_id"] = json!(stream);
        detail["remote_peer_id"] = json!(owner.peer.to_string());
        detail["swarm_connection_id"] = json!(owner.swarm_id);
        detail["peer_id"] = json!(owner.peer.to_string());
        detail["connection_id"] = json!(owner.swarm_id);
        detail["stream_id"] = json!(stream.map(|stream| format!("{}:{stream}", connection + 1)));
        detail["endpoint"] = crate::upgrade_observer::endpoint(&owner.point);
        detail["native_stack"] = owner.stack.facts();
        self.emit(kind, WIRE_SOURCE, detail);
    }
}

impl Evidence {
    pub(super) fn lock(&self) -> std::sync::MutexGuard<'_, Capture> {
        self.0.lock().unwrap_or_else(|e| e.into_inner())
    }

    pub(super) fn emit(&self, kind: &str, source: &str, detail: Value) {
        self.lock().emit(kind, source, detail);
    }

    pub(super) fn admit(&self) -> io::Result<()> {
        let capture = self.lock();
        if capture.prepared {
            return Err(invalid(
                "fixture command admission closed after prepare_shutdown",
            ));
        }
        if capture.overflow || capture.error.is_some() {
            return Err(invalid("fixture has a sticky native capture failure"));
        }
        Ok(())
    }

    pub(super) fn prepare(
        &self,
        sequence: u64,
        actor: &str,
        token: &str,
        local: PeerId,
        pending: usize,
    ) -> io::Result<()> {
        let mut capture = self.lock();
        if capture.prepared || capture.overflow || capture.error.is_some() || pending != 0 {
            return Err(invalid(
                "prepare_shutdown requires open admission, no pending commands and no native errors",
            ));
        }
        capture.prepared = true;
        capture.emit(
            "shutdown_prepared",
            "rust.fixture.prepare_shutdown",
            json!({"command_sequence": sequence,
            "actor": actor, "case_token": token, "local_peer_id": local.to_string(),
            "admission_closed": true, "pending_commands": 0}),
        );
        if capture.error.is_some() {
            return Err(invalid("prepare_shutdown acknowledgement capture failed"));
        }
        Ok(())
    }

    pub(super) fn connection(
        &self,
        peer: PeerId,
        point: ConnectedPoint,
        stack: NativeStack,
    ) -> Option<usize> {
        let mut capture = self.lock();
        if capture.connections.len() == CONNECTION_LIMIT {
            capture.overflow = true;
            capture.fail("native connection limit exceeded");
            return None;
        }
        let id = capture.connections.len();
        capture.connections.push(Connection {
            peer,
            point,
            stack,
            swarm_id: None,
            native_id: None,
            streams: 0,
            dropped: false,
            closed: false,
            terminal: false,
        });
        capture.live_muxers += 1;
        let mut facts = stack.facts();
        facts["authenticated"] = json!(true);
        facts["remote_address"] = json!(
            capture.connections[id]
                .point
                .get_remote_address()
                .to_string()
        );
        if matches!(stack, NativeStack::PnetNoiseYamux) {
            facts["pnet_verified"] = json!(true);
            facts["pnet_fingerprint"] = json!(capture.fingerprint);
        }
        facts["owner_basis"] = json!("exact_native_transport_output_muxer");
        capture.owned(id, None, "connection", facts);
        Some(id)
    }

    pub(super) fn bind(
        &self,
        peer: PeerId,
        point: &ConnectedPoint,
        native_id: ConnectionId,
    ) -> io::Result<()> {
        let swarm_id = native_id.to_string();
        let mut capture = self.lock();
        let matches: Vec<_> = capture
            .connections
            .iter()
            .enumerate()
            .filter(|(_, owner)| {
                owner.peer == peer
                    && owner.swarm_id.is_none()
                    && !owner.dropped
                    && crate::upgrade_observer::endpoint(&owner.point)
                        == crate::upgrade_observer::endpoint(point)
            })
            .map(|(id, _)| id)
            .collect();
        let [id] = matches.as_slice() else {
            return Err(invalid(
                "missing/ambiguous native transport owner for Swarm connection",
            ));
        };
        capture.connections[*id].swarm_id = Some(swarm_id.clone());
        capture.connections[*id].native_id = Some(native_id);
        // Bind prior native observations only to this unique successful output.
        for event in &mut capture.events {
            if event["connection_trace_id"] == (*id + 1) {
                event["swarm_connection_id"] = json!(swarm_id);
                event["connection_id"] = json!(swarm_id);
            }
        }
        let sizes = capture
            .events
            .iter()
            .map(|event| serde_json::to_vec(event).map(|v| v.len()))
            .collect::<Result<Vec<_>, _>>()?;
        capture.trace_bytes = sizes.iter().sum();
        if sizes.iter().any(|size| *size > EVENT_BYTE_LIMIT)
            || capture.trace_bytes + capture.events.len() + 2 > TRACE_BYTE_LIMIT
        {
            capture.overflow = true;
            capture.fail("bound native event/trace byte limit exceeded");
        }
        capture.owned(
            *id,
            None,
            "connection_established",
            json!({"owner_basis": "unique_authenticated_output_peer_and_endpoint"}),
        );
        Ok(())
    }

    pub(super) fn terminal(
        &self,
        peer: PeerId,
        native_id: ConnectionId,
        cause: Option<String>,
    ) -> io::Result<()> {
        let mut capture = self.lock();
        let owners = capture
            .connections
            .iter()
            .enumerate()
            .filter(|(_, c)| c.peer == peer && c.native_id == Some(native_id))
            .map(|(id, _)| id)
            .collect::<Vec<_>>();
        let [id] = owners.as_slice() else {
            return Err(invalid(
                "native closed connection has no unique authenticated owner",
            ));
        };
        capture.connections[*id].terminal = true;
        capture.emit(
            "connection_closed",
            "rust.libp2p.Swarm.ConnectionClosed",
            json!({"peer_id": peer.to_string(), "connection_id": native_id.to_string(),
            "cause": cause, "native_terminal_observed": true}),
        );
        Ok(())
    }
}

#[derive(Default)]
struct Frames {
    prefix: Vec<u8>,
    body: Vec<u8>,
    framed: Vec<u8>,
    length: Option<usize>,
}

struct NativeFrame {
    body: Vec<u8>,
    framed: Vec<u8>,
}

impl Frames {
    fn byte(&mut self, byte: u8, negotiation: bool) -> io::Result<Option<NativeFrame>> {
        self.framed.push(byte);
        if self.length.is_none() {
            self.prefix.push(byte);
            // The pinned multistream codec allows two prefix bytes; RPCs may need three.
            let prefix_limit = if negotiation { 2 } else { 3 };
            if self.prefix.len() > prefix_limit
                || (self.prefix.len() == prefix_limit && byte & 128 != 0)
            {
                return Err(invalid("native frame prefix over limit"));
            }
            if byte & 128 != 0 {
                return Ok(None);
            }
            let mut cursor = 0;
            let length = varint(&self.prefix, &mut cursor)? as usize;
            let length_limit = if negotiation {
                FRAME_LIMIT - 1
            } else {
                FRAME_LIMIT
            };
            if length == 0 || length > length_limit {
                return Err(invalid("native frame size over limit"));
            }
            self.length = Some(length);
            self.prefix.clear();
            return Ok(None);
        }
        self.body.push(byte);
        if Some(self.body.len()) == self.length {
            self.length = None;
            return Ok(Some(NativeFrame {
                body: std::mem::take(&mut self.body),
                framed: std::mem::take(&mut self.framed),
            }));
        }
        Ok(None)
    }

    fn partial(&self) -> bool {
        !self.prefix.is_empty() || self.length.is_some()
    }
}

fn varint(bytes: &[u8], cursor: &mut usize) -> io::Result<u64> {
    let mut value = 0u64;
    for shift in (0..70).step_by(7) {
        let byte = *bytes
            .get(*cursor)
            .ok_or_else(|| invalid("truncated native varint"))?;
        *cursor += 1;
        if shift == 63 && byte > 1 {
            return Err(invalid("native varint overflow"));
        }
        value |= ((byte & 127) as u64) << shift;
        if byte & 128 == 0 {
            if shift != 0 && byte == 0 {
                return Err(invalid("noncanonical native varint"));
            }
            return Ok(value);
        }
    }
    Err(invalid("native varint overflow"))
}

enum Field<'a> {
    Bytes(u64, &'a [u8]),
    Number(u64, u64),
    Fixed,
}

fn fields(bytes: &[u8]) -> io::Result<Vec<Field<'_>>> {
    let mut cursor = 0;
    let mut fields = Vec::new();
    while cursor < bytes.len() {
        let key = varint(bytes, &mut cursor)?;
        if key >> 3 == 0 {
            return Err(invalid("zero native protobuf field"));
        }
        fields.push(match key & 7 {
            0 => Field::Number(key >> 3, varint(bytes, &mut cursor)?),
            2 => {
                let length = usize::try_from(varint(bytes, &mut cursor)?)
                    .map_err(|_| invalid("native field length overflow"))?;
                let end = cursor
                    .checked_add(length)
                    .filter(|end| *end <= bytes.len())
                    .ok_or_else(|| invalid("truncated native protobuf field"))?;
                let field = Field::Bytes(key >> 3, &bytes[cursor..end]);
                cursor = end;
                field
            }
            1 | 5 => {
                cursor = cursor
                    .checked_add(if key & 7 == 1 { 8 } else { 4 })
                    .filter(|end| *end <= bytes.len())
                    .ok_or_else(|| invalid("truncated native fixed field"))?;
                Field::Fixed
            }
            _ => return Err(invalid("unsupported native protobuf wire type")),
        });
    }
    Ok(fields)
}

fn utf8(bytes: &[u8]) -> io::Result<&str> {
    std::str::from_utf8(bytes).map_err(|_| invalid("non-UTF8 native topic/protocol"))
}
pub(super) fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

// Decode receipts only. This is not a router, encoder, validator or score model.
fn rpc_receipts(bytes: &[u8]) -> io::Result<Vec<(&'static str, Value)>> {
    let mut receipts = Vec::new();
    for field in fields(bytes)? {
        match field {
            Field::Bytes(2, message) => {
                let mut detail = json!({});
                for field in fields(message)? {
                    match field {
                        Field::Bytes(1, author) => {
                            detail["author"] = json!(
                                PeerId::from_bytes(author)
                                    .map_err(|_| invalid("invalid native message author"))?
                                    .to_string()
                            );
                        }
                        Field::Bytes(2, payload) => {
                            detail["payload_sha256"] =
                                json!(format!("{:x}", Sha256::digest(payload)));
                            detail["payload_bytes"] = json!(payload.len());
                        }
                        Field::Bytes(3, sequence) => {
                            detail["author_sequence_hex"] = json!(hex(sequence));
                        }
                        Field::Bytes(4, topic) => {
                            detail["topic"] = json!(utf8(topic)?);
                        }
                        _ => {}
                    }
                }
                receipts.push(("wire_message", detail));
            }
            Field::Bytes(3, control) => {
                for field in fields(control)? {
                    let Field::Bytes(number, body) = field else {
                        continue;
                    };
                    if !matches!(number, 3 | 4) {
                        continue;
                    }
                    let mut detail = json!({"wire_fields_hex": hex(body)});
                    let mut px = Vec::new();
                    for field in fields(body)? {
                        match field {
                            Field::Bytes(1, topic) => {
                                detail["topic"] = json!(utf8(topic)?);
                            }
                            Field::Bytes(2, peer) if number == 4 => {
                                px.push(json!({"wire_hex": hex(peer)}));
                            }
                            Field::Number(3, backoff) if number == 4 => {
                                detail["backoff"] = json!(backoff);
                            }
                            _ => {}
                        }
                    }
                    // Absence is preserved: never synthesize a zero backoff or empty PX field.
                    if !px.is_empty() {
                        detail["px"] = json!(px);
                    }
                    receipts.push((if number == 3 { "graft" } else { "prune" }, detail));
                }
            }
            _ => {}
        }
    }
    Ok(receipts)
}

struct WireStream {
    evidence: Evidence,
    connection: Option<usize>,
    stream: usize,
    frames: [Frames; 2],
    headers: [bool; 2],
    proposal_direction: usize,
    proposal: Option<String>,
    negotiation_frames: usize,
    listing: bool,
    selected: Option<String>,
    excluded: bool,
    other_rejected: bool,
    pending: Vec<(usize, NativeFrame)>,
    pending_bytes: usize,
    failed: bool,
}

impl WireStream {
    fn new(evidence: Evidence, connection: Option<usize>, outbound: bool) -> Self {
        let mut capture = evidence.lock();
        let mut stream = 0;
        let connection = connection.filter(|id| {
            if capture.streams_created == STREAM_LIMIT {
                capture.overflow = true;
                capture.fail("native stream limit exceeded");
                return false;
            }
            capture.streams_created += 1;
            capture.connections[*id].streams += 1;
            stream = capture.connections[*id].streams;
            capture.live_streams += 1;
            true
        });
        if let Some(id) = connection {
            capture.owned(
                id,
                Some(stream),
                "stream_opened",
                json!({"direction": if outbound { "outbound" } else { "inbound" }}),
            );
        }
        drop(capture);
        Self {
            evidence,
            connection,
            stream,
            frames: Default::default(),
            headers: [false; 2],
            proposal_direction: usize::from(outbound),
            proposal: None,
            negotiation_frames: 0,
            listing: false,
            selected: None,
            excluded: false,
            other_rejected: false,
            pending: Vec::new(),
            pending_bytes: 0,
            failed: false,
        }
    }

    pub(super) fn fail(&mut self, error: impl std::fmt::Display) {
        self.failed = true;
        let mut capture = self.evidence.lock();
        capture.fail(error);
    }

    fn feed(&mut self, direction: usize, bytes: &[u8]) {
        if self.failed || self.connection.is_none() {
            return;
        }
        for byte in bytes {
            if self.excluded
                || (direction == self.proposal_direction
                    && self.proposal.as_deref().is_some_and(|p| !meshsub(p)))
            {
                // V1Lazy may write another protocol's opaque body before its ACK. Never parse it as RPC.
                break;
            }
            let negotiation = self.selected.is_none()
                && (direction != self.proposal_direction || self.proposal.is_none());
            match self.frames[direction].byte(*byte, negotiation) {
                Ok(Some(frame)) => {
                    if let Err(e) = self.frame(direction, frame) {
                        self.fail(e);
                        break;
                    }
                }
                Ok(None) => {}
                Err(e) => {
                    // Rejection permits another proposal, not a framing-limit bypass.
                    self.fail(e);
                    break;
                }
            }
        }
    }

    fn frame(&mut self, direction: usize, frame: NativeFrame) -> io::Result<()> {
        if self.selected.is_some() {
            return self.rpc(direction, &frame);
        }
        if !self.headers[direction]
            || direction != self.proposal_direction
            || self.proposal.is_none()
        {
            // Two headers and at most 32 proposal/response attempts, independently of lazy RPC bytes.
            self.negotiation_frames += 1;
            if self.negotiation_frames > 66 {
                return Err(invalid("native negotiation attempt limit exceeded"));
            }
        }
        if !self.headers[direction] {
            if frame.body != b"/multistream/1.0.0\n" {
                return Err(invalid(format!(
                    "missing native multistream header: {}",
                    negotiation_token(&frame.body)
                )));
            }
            self.headers[direction] = true;
            return Ok(());
        }
        if direction == self.proposal_direction {
            if self.proposal.is_none() {
                if self.listing {
                    return Err(invalid(format!(
                        "native proposal before protocol list response: {}",
                        negotiation_token(&frame.body)
                    )));
                }
                if frame.body == b"ls\n" {
                    self.listing = true;
                    return Ok(());
                }
                match protocol_token(&frame.body) {
                    Ok(protocol) => {
                        self.proposal = Some(protocol.to_owned());
                        self.other_rejected = false;
                    }
                    Err(_) if self.other_rejected => self.exclude_body(),
                    Err(error) => return Err(error),
                }
                return Ok(());
            }
            self.pending_bytes += frame.framed.len();
            if self.pending_bytes > FRAME_LIMIT || self.pending.len() == 32 {
                return Err(invalid("unacknowledged native body capture over limit"));
            }
            self.pending.push((direction, frame));
            return Ok(());
        }
        if self.listing {
            protocol_list(&frame.body)
                .map_err(|error| invalid(format!("{error}: {}", negotiation_token(&frame.body))))?;
            self.listing = false;
            return Ok(());
        }
        let proposal = self.proposal.as_deref().ok_or_else(|| {
            invalid(format!(
                "native acknowledgement without a protocol proposal: {}",
                negotiation_token(&frame.body)
            ))
        })?;
        if frame.body == b"na\n" {
            // Donor listener_select resumes RecvMessage after NotAvailable, on the SAME substream.
            self.other_rejected = !meshsub(proposal);
            self.proposal = None;
            self.pending = Vec::new();
            self.pending_bytes = 0;
            self.frames[self.proposal_direction] = Frames::default();
            return Ok(());
        }
        let response = protocol_token(&frame.body)?;
        if proposal != response {
            return Err(invalid(format!(
                "native multistream acknowledgement mismatch: proposal={}, response={}",
                negotiation_token(proposal.as_bytes()),
                negotiation_token(&frame.body)
            )));
        }
        if !meshsub(proposal) {
            self.exclude_body();
            return Ok(());
        }
        self.selected = self.proposal.take();
        self.evidence.lock().owned(self.connection.unwrap(), Some(self.stream), "protocol", json!({"protocol": self.selected,
            "owner_basis": "exact_muxer_substream", "selection_basis": "matching_native_multistream_proposal_and_ack"}));
        for (direction, frame) in std::mem::take(&mut self.pending) {
            self.rpc(direction, &frame)?;
        }
        self.pending_bytes = 0;
        Ok(())
    }

    fn exclude_body(&mut self) {
        // Another protocol's ACK, or its rejected V1Lazy body, supplies no PubSub selection.
        self.excluded = true;
        self.other_rejected = false;
        self.listing = false;
        self.proposal = None;
        self.frames = Default::default();
        self.pending = Vec::new();
        self.pending_bytes = 0;
    }

    fn rpc(&self, direction: usize, frame: &NativeFrame) -> io::Result<()> {
        let receipts = rpc_receipts(&frame.body)?;
        let mut capture = self.evidence.lock();
        let side = if direction == 0 { "read" } else { "write" };
        let mut receipt = json!({"framed_hex": hex(&frame.framed)});
        receipt[side] = json!({"framed_bytes": frame.framed.len(), "framed_sha256": format!("{:x}", Sha256::digest(&frame.framed)),
            "frames": 1, "complete_frames": true, "invalid_or_over_limit": false});
        capture.owned(self.connection.unwrap(), Some(self.stream), "rpc", json!({"protocol": self.selected,
            "direction": side, "receipt": receipt,
            "io_basis": if direction == 0 { "successful_native_read" } else { "successful_native_write_not_remote_ack" }}));
        for (kind, mut detail) in receipts {
            detail["protocol"] = json!(self.selected);
            detail["direction"] = json!(if direction == 0 { "receive" } else { "send" });
            detail["rpc_sha256"] = json!(format!("{:x}", Sha256::digest(&frame.body)));
            detail["io_basis"] = json!(if direction == 0 {
                "successful_native_read"
            } else {
                "successful_native_write_not_remote_ack"
            });
            capture.owned(self.connection.unwrap(), Some(self.stream), kind, detail);
        }
        Ok(())
    }
}

fn meshsub(protocol: &str) -> bool {
    matches!(protocol, "/meshsub/1.0.0" | "/meshsub/1.1.0")
}

fn protocol_token(bytes: &[u8]) -> io::Result<&str> {
    let rejected = || {
        invalid(format!(
            "invalid native protocol proposal/acknowledgement: {}",
            negotiation_token(bytes)
        ))
    };
    let token = std::str::from_utf8(bytes).map_err(|_| rejected())?;
    let protocol = token
        .strip_suffix('\n')
        .filter(|p| p.starts_with('/') && !p.contains('\n'))
        .ok_or_else(rejected)?;
    Ok(protocol)
}

fn negotiation_token(bytes: &[u8]) -> String {
    let shown = &bytes[..bytes.len().min(48)];
    let token = match std::str::from_utf8(shown) {
        Ok(token) => format!("{token:?}"),
        Err(_) => format!("hex:{}", hex(shown)),
    };
    if shown.len() == bytes.len() {
        token
    } else {
        format!("{token}... ({} bytes)", bytes.len())
    }
}

fn protocol_list(bytes: &[u8]) -> io::Result<()> {
    let mut cursor = 0;
    for count in 0..=1000 {
        if bytes.get(cursor..) == Some(b"\n".as_slice()) {
            return Ok(());
        }
        if count == 1000 {
            break;
        }
        let length = usize::try_from(varint(bytes, &mut cursor)?)
            .map_err(|_| invalid("native protocol list length overflow"))?;
        let end = cursor
            .checked_add(length)
            .filter(|end| *end <= bytes.len())
            .ok_or_else(|| invalid("truncated native protocol list"))?;
        protocol_token(&bytes[cursor..end])?;
        cursor = end;
    }
    Err(invalid("native protocol list over limit"))
}

impl Drop for WireStream {
    fn drop(&mut self) {
        let mut capture = self.evidence.lock();
        if let Some(id) = self.connection {
            capture.live_streams -= 1;
            let other_proposal = self.proposal.as_deref().is_some_and(|p| !meshsub(p));
            let partial = (!self.excluded
                && !self.other_rejected
                && !other_proposal
                && self.frames.iter().any(Frames::partial))
                || !self.pending.is_empty()
                || self.proposal.as_deref().is_some_and(meshsub);
            if partial {
                let protocol = self.proposal.as_deref().or(self.selected.as_deref());
                capture.fail(format!(
                    "native stream dropped with incomplete wire receipt: protocol={}",
                    protocol
                        .map(|p| negotiation_token(p.as_bytes()))
                        .unwrap_or_else(|| "unselected".to_owned())
                ));
            }
            capture.owned(
                id,
                Some(self.stream),
                "stream_dropped",
                json!({"partial_frame": partial}),
            );
        }
    }
}

struct ObservedIo<T> {
    inner: T,
    wire: WireStream,
}

impl<T: AsyncRead + Unpin> AsyncRead for ObservedIo<T> {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        bytes: &mut [u8],
    ) -> Poll<io::Result<usize>> {
        let this = self.get_mut();
        let result = Pin::new(&mut this.inner).poll_read(cx, bytes);
        match &result {
            Poll::Ready(Ok(count)) => this.wire.feed(0, &bytes[..*count]),
            Poll::Ready(Err(e)) => this.wire.evidence.lock().native_error(
                this.wire.connection,
                Some(this.wire.stream),
                "stream_read",
                e,
            ),
            _ => {}
        }
        result
    }
}

impl<T: AsyncWrite + Unpin> AsyncWrite for ObservedIo<T> {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        bytes: &[u8],
    ) -> Poll<io::Result<usize>> {
        let this = self.get_mut();
        let result = Pin::new(&mut this.inner).poll_write(cx, bytes);
        match &result {
            Poll::Ready(Ok(count)) => this.wire.feed(1, &bytes[..*count]),
            Poll::Ready(Err(e)) => this.wire.evidence.lock().native_error(
                this.wire.connection,
                Some(this.wire.stream),
                "stream_write",
                e,
            ),
            _ => {}
        }
        result
    }
    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        let this = self.get_mut();
        let result = Pin::new(&mut this.inner).poll_flush(cx);
        if let Poll::Ready(Err(e)) = &result {
            this.wire.evidence.lock().native_error(
                this.wire.connection,
                Some(this.wire.stream),
                "stream_flush",
                e,
            );
        }
        result
    }
    fn poll_close(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        let this = self.get_mut();
        let result = Pin::new(&mut this.inner).poll_close(cx);
        if let Poll::Ready(Err(e)) = &result {
            this.wire.evidence.lock().native_error(
                this.wire.connection,
                Some(this.wire.stream),
                "stream_close",
                e,
            );
        }
        result
    }
}

struct ObservedMuxer {
    inner: StreamMuxerBox,
    evidence: Evidence,
    connection: Option<usize>,
}

impl ObservedMuxer {
    fn io(&self, inner: SubstreamBox, outbound: bool) -> ObservedIo<SubstreamBox> {
        ObservedIo {
            inner,
            wire: WireStream::new(self.evidence.clone(), self.connection, outbound),
        }
    }
    fn error<T>(&self, operation: &str, result: &Poll<io::Result<T>>) {
        if let Poll::Ready(Err(e)) = result {
            self.evidence
                .lock()
                .native_error(self.connection, None, operation, e);
        }
    }
}

impl StreamMuxer for ObservedMuxer {
    type Substream = ObservedIo<SubstreamBox>;
    type Error = io::Error;
    fn poll_inbound(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<io::Result<Self::Substream>> {
        let this = self.get_mut();
        let result = Pin::new(&mut this.inner).poll_inbound(cx);
        this.error("muxer_inbound", &result);
        result.map_ok(|inner| this.io(inner, false))
    }
    fn poll_outbound(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<io::Result<Self::Substream>> {
        let this = self.get_mut();
        let result = Pin::new(&mut this.inner).poll_outbound(cx);
        this.error("muxer_outbound", &result);
        result.map_ok(|inner| this.io(inner, true))
    }
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<StreamMuxerEvent>> {
        let this = self.get_mut();
        let result = Pin::new(&mut this.inner).poll(cx);
        this.error("muxer_poll", &result);
        result
    }
    fn poll_close(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        let this = self.get_mut();
        let result = Pin::new(&mut this.inner).poll_close(cx);
        this.error("muxer_close", &result);
        if let Poll::Ready(Ok(())) = &result {
            if let Some(id) = this.connection {
                let mut capture = this.evidence.lock();
                if !capture.connections[id].closed {
                    capture.connections[id].closed = true;
                    capture.owned(
                        id,
                        None,
                        "native_close",
                        json!({"basis": "native_muxer_poll_close_returned"}),
                    );
                }
            }
        }
        result
    }
}

impl Drop for ObservedMuxer {
    fn drop(&mut self) {
        if let Some(id) = self.connection {
            let mut capture = self.evidence.lock();
            capture.live_muxers -= 1;
            capture.connections[id].dropped = true;
            capture.owned(
                id,
                None,
                "host_muxer_dropped",
                json!({"drop_is_not_remote_close_ack": true}),
            );
        }
    }
}

pub(super) fn wrap(
    peer: PeerId,
    muxer: StreamMuxerBox,
    point: ConnectedPoint,
    stack: NativeStack,
    evidence: Evidence,
) -> (PeerId, StreamMuxerBox) {
    let connection = evidence.connection(peer, point, stack);
    (
        peer,
        StreamMuxerBox::new(ObservedMuxer {
            inner: muxer,
            evidence,
            connection,
        }),
    )
}

#[cfg(test)]
#[path = "observer_tests.rs"]
mod tests;
