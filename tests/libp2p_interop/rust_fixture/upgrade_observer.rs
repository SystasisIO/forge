//! Passive native TCP evidence; application-side correlation lives in application_observer.
use std::{
    io,
    pin::Pin,
    sync::{Arc, Mutex},
    task::{Context, Poll},
};

use futures::{AsyncRead, AsyncWrite};
use libp2p::{
    PeerId, Transport,
    core::{
        ConnectedPoint, Negotiated,
        muxing::{StreamMuxer, StreamMuxerBox, StreamMuxerEvent},
        transport::Boxed,
        upgrade::{InboundConnectionUpgrade, OutboundConnectionUpgrade},
    },
    identity, noise, tcp, tls, yamux,
};
use multistream_select::{Version, dialer_select_proto, listener_select_proto};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};

const CONNECTION_LIMIT: usize = 16;
const STREAM_LIMIT: usize = 32;
const EVENT_LIMIT: usize = 256;
const FRAME_LIMIT: usize = 256;
const PROPOSAL_LIMIT: usize = 8;
const BODY_LIMIT: usize = 256 * 1024;
const HEADER: &str = "/multistream/1.0.0\n";

#[derive(Clone, Debug, Default)]
pub(crate) struct Observer(Arc<Mutex<State>>, super::application_observer::Observer);

#[derive(Debug, Default)]
struct State {
    connections: Vec<Arc<Mutex<Connection>>>,
    swarm_events: Vec<Value>,
    overflow: bool,
}

#[derive(Debug)]
struct Connection {
    id: usize,
    endpoint: Value,
    local_address: String,
    remote_address: String,
    local_peer: String,
    remote_peer: String,
    phases: Vec<Selection>,
    events: Vec<Value>,
    overflow: bool,
    muxer_drop_observed: bool,
    muxer_close_returned: bool,
}

#[derive(Clone, Debug, Default)]
struct Trace(Option<Arc<Mutex<Connection>>>);

impl Trace {
    fn update(&self, update: impl FnOnce(&mut Connection)) {
        if let Some(trace) = &self.0 {
            update(&mut trace.lock().unwrap_or_else(|error| error.into_inner()));
        }
    }

    fn event(&self, phase: usize, kind: &str, detail: Value) {
        self.update(|connection| connection.event(phase, kind, detail));
    }

    fn feed(&self, phase: usize, direction: usize, bytes: &[u8]) {
        self.update(|connection| {
            if connection.overflow {
                return;
            }
            let Some(selection) = connection.phases.get_mut(phase) else {
                return;
            };
            // One call may contain arbitrarily much application data, but never retains it.
            for byte in bytes {
                if let Some(event) = selection.byte(direction, *byte) {
                    if connection.events.len() == EVENT_LIMIT {
                        connection.overflow = true;
                        break;
                    }
                    let mut event = event;
                    event["sequence"] = json!(connection.events.len() + 1);
                    event["phase"] = json!(phase_name(phase));
                    event["stream_trace_id"] = json!(stream_id(phase));
                    connection.events.push(event);
                }
            }
        });
    }

    fn io_error(&self, phase: usize, operation: &str, error: &io::Error) {
        self.update(|connection| {
            if let Some(selection) = connection.phases.get_mut(phase) {
                selection.io_failed = true;
            }
            connection.event(phase, operation, json!({"error": bounded_error(error)}));
        });
    }

    fn substream(&self, outbound: bool) -> usize {
        let mut index = usize::MAX;
        self.update(|connection| {
            if connection.phases.len() == STREAM_LIMIT + 2 {
                connection.overflow = true;
                return;
            }
            index = connection.phases.len();
            connection.phases.push(Selection::new(outbound, true));
            connection.event(
                index,
                "substream_opened",
                json!({"direction": direction(outbound)}),
            );
        });
        index
    }
}

fn bounded_error(error: impl std::fmt::Display) -> String {
    error.to_string().chars().take(256).collect()
}

fn direction(outbound: bool) -> &'static str {
    if outbound { "outbound" } else { "inbound" }
}

fn phase_name(phase: usize) -> &'static str {
    match phase {
        0 => "security",
        1 => "muxer",
        _ => "application",
    }
}

fn stream_id(phase: usize) -> Option<usize> {
    (phase >= 2).then(|| phase - 1)
}

impl Connection {
    fn event(&mut self, phase: usize, kind: &str, detail: Value) {
        if self.events.len() == EVENT_LIMIT {
            self.overflow = true;
            return;
        }
        self.events.push(json!({
            "sequence": self.events.len() + 1, "phase": phase_name(phase),
            "stream_trace_id": stream_id(phase), "kind": kind, "detail": detail,
        }));
    }

    fn snapshot(&self) -> Value {
        json!({
            "connection_trace_id": self.id, "endpoint": self.endpoint,
            "direction": self.endpoint["direction"],
            "local_address": self.local_address, "remote_address": self.remote_address,
            "authenticated_local_peer_id": self.local_peer,
            "authenticated_remote_peer_id": self.remote_peer,
            "selected_security": self.phases[0].delegate_protocol,
            "selected_muxer": self.phases[1].delegate_protocol,
            "security_complete": self.phases[0].upgrade_complete(),
            "muxer_complete": self.phases[1].upgrade_complete(),
            "security_delegate_completed": self.phases[0].delegate_complete,
            "muxer_delegate_completed": self.phases[1].delegate_complete,
            "early_muxer_negotiation": false,
            "muxer_drop_observed": self.muxer_drop_observed,
            "muxer_close_returned": self.muxer_close_returned,
            "overflow": self.overflow, "events": self.events,
            "negotiations": self.phases.iter().take(2).map(Selection::snapshot).collect::<Vec<_>>(),
            "streams": self.phases.iter().enumerate().skip(2).map(|(index, phase)| {
                let mut value = phase.snapshot();
                value["stream_trace_id"] = json!(stream_id(index));
                value
            }).collect::<Vec<_>>(),
        })
    }
}

impl Observer {
    pub(crate) fn application(&self, peer: PeerId, protocol: &'static str) -> super::application_observer::Attempt {
        self.1.begin(peer, protocol, &self.snapshot())
    }

    pub(crate) fn finalized(&self, joined: bool) -> Value {
        super::application_observer::finalize(self.snapshot(), joined)
    }

    fn begin(&self, point: &ConnectedPoint, local: String, remote: String, peer: PeerId) -> Trace {
        let mut state = self.0.lock().unwrap_or_else(|error| error.into_inner());
        if state.connections.len() == CONNECTION_LIMIT {
            state.overflow = true;
            return Trace::default();
        }
        let outbound = upgrade_outbound(point);
        let trace = Arc::new(Mutex::new(Connection {
            id: state.connections.len() + 1,
            endpoint: endpoint(point),
            local_address: local,
            remote_address: remote,
            local_peer: peer.to_string(),
            remote_peer: String::new(),
            phases: vec![
                Selection::new(outbound, false),
                Selection::new(outbound, false),
            ],
            events: Vec::new(),
            overflow: false,
            muxer_drop_observed: false,
            muxer_close_returned: false,
        }));
        state.connections.push(trace.clone());
        Trace(Some(trace))
    }

    // Swarm IDs are a separate event source. No guessed ID-to-raw-stream association.
    pub(crate) fn established(
        &self,
        id: impl std::fmt::Display,
        peer: PeerId,
        point: &ConnectedPoint,
    ) {
        let mut state = self.0.lock().unwrap_or_else(|error| error.into_inner());
        if state.swarm_events.len() == CONNECTION_LIMIT {
            state.overflow = true;
        } else {
            state.swarm_events.push(json!({"kind": "connection_established",
                "swarm_connection_id": id.to_string(), "authenticated_remote_peer_id": peer.to_string(),
                "endpoint": endpoint(point)}));
        }
    }

    pub(crate) fn snapshot(&self) -> Value {
        let state = self.0.lock().unwrap_or_else(|error| error.into_inner());
        let connections = state
            .connections
            .iter()
            .map(|connection| {
                connection
                    .lock()
                    .unwrap_or_else(|error| error.into_inner())
                    .snapshot()
            })
            .collect::<Vec<_>>();
        let overflow = state.overflow || connections.iter().any(|value| value["overflow"] == true);
        json!({
            "source": "rust-libp2p.public-connection-upgrades.v1",
            "complete": false, "overflow": overflow,
            "exact_application_binding_supported": false,
            "swarm_connection_binding_supported": false,
            "proof_limitation": "raw substreams and Swarm events are distinct observations, not an exact application exchange binding",
            "connections": connections, "swarm_events": state.swarm_events,
            "applications": self.1.snapshot(),
        })
    }
}

fn endpoint(point: &ConnectedPoint) -> Value {
    match point {
        ConnectedPoint::Dialer {
            address,
            role_override,
            ..
        } => json!({
            "direction": "outbound", "remote_address": address.to_string(),
            "upgrade_role": direction(role_override.is_dialer()),
        }),
        ConnectedPoint::Listener {
            local_addr,
            send_back_addr,
        } => json!({
            "direction": "inbound", "local_address": local_addr.to_string(),
            "remote_address": send_back_addr.to_string(), "upgrade_role": "inbound",
        }),
    }
}

fn upgrade_outbound(point: &ConnectedPoint) -> bool {
    matches!(point, ConnectedPoint::Dialer { role_override, .. } if role_override.is_dialer())
}

#[derive(Debug, Default)]
struct Decoder {
    frame: Vec<u8>,
    length: usize,
    prefix: usize,
    prefix_complete: bool,
    header: bool,
    paused: bool,
}

#[derive(Debug, Default)]
pub(crate) struct Body {
    digest: Sha256,
    bytes: usize,
    frames: usize,
    remaining: usize,
    prefix_value: usize,
    prefix_bytes: usize,
    invalid: bool,
}

impl Body {
    pub(crate) fn byte(&mut self, byte: u8, limit: usize) {
        if self.invalid {
            return;
        }
        if self.bytes == limit {
            self.invalid = true;
            return;
        }
        self.bytes += 1;
        self.digest.update([byte]);
        if self.remaining != 0 {
            self.remaining -= 1;
            if self.remaining == 0 {
                self.frames += 1;
            }
            return;
        }
        if self.prefix_bytes == 3 {
            self.invalid = true;
            return;
        }
        self.prefix_value |= ((byte & 127) as usize) << (7 * self.prefix_bytes);
        self.prefix_bytes += 1;
        if byte & 128 != 0 {
            return;
        }
        if self.prefix_value == 0
            || self.prefix_value > limit
            || (self.prefix_bytes > 1 && byte == 0)
        {
            self.invalid = true;
            return;
        }
        self.remaining = self.prefix_value;
        self.prefix_value = 0;
        self.prefix_bytes = 0;
    }

    fn complete(&self) -> bool {
        !self.invalid && self.frames > 0 && self.remaining == 0 && self.prefix_bytes == 0
    }

    pub(crate) fn snapshot(&self) -> Value {
        json!({"framed_bytes": self.bytes, "frames": self.frames,
            "framed_sha256": format!("{:x}", self.digest.clone().finalize()),
            "complete_frames": self.complete(), "invalid_or_over_limit": self.invalid})
    }
}

#[derive(Debug)]
struct Selection {
    outbound: bool,
    application: bool,
    decoders: [Decoder; 2],
    bodies: [Body; 2],
    proposal: Option<String>,
    reply: Option<String>,
    proposals: usize,
    selected: Option<String>,
    delegate_protocol: Option<String>,
    delegate_complete: bool,
    error: Option<&'static str>,
    ambiguous_tail: bool,
    io_failed: bool,
    write_closed: bool,
    read_eof: bool,
    drop_observed: bool,
}

impl Selection {
    fn new(outbound: bool, application: bool) -> Self {
        Self {
            outbound,
            application,
            decoders: Default::default(),
            bodies: Default::default(),
            proposal: None,
            reply: None,
            proposals: 0,
            selected: None,
            delegate_protocol: None,
            delegate_complete: false,
            error: None,
            ambiguous_tail: false,
            io_failed: false,
            write_closed: false,
            read_eof: false,
            drop_observed: false,
        }
    }

    fn byte(&mut self, direction: usize, byte: u8) -> Option<Value> {
        if self.error.is_some() {
            return None;
        }
        if self.selected.is_some() || self.decoders[direction].paused {
            // After a proposal only framing/hash is allowed, never raw payload storage.
            if self.selected.is_none() {
                self.ambiguous_tail = true;
            }
            if self.application {
                let protocol = self
                    .selected
                    .as_ref()
                    .or(self.proposal.as_ref())
                    .or(self.reply.as_ref());
                let limit = match protocol.map(String::as_str) {
                    Some("/ipfs/id/1.0.0") => 4098, // 4096-byte payload plus its actual varint prefix.
                    Some("/forge/interop/relay-echo/1") => BODY_LIMIT,
                    _ => return None,
                };
                self.bodies[direction].byte(byte, limit);
            }
            return None;
        }
        let decoder = &mut self.decoders[direction];
        if !decoder.prefix_complete {
            if decoder.prefix == 2 {
                self.error = Some("negotiation frame prefix exceeds bound");
                return None;
            }
            decoder.frame.push(byte);
            decoder.length |= ((byte & 127) as usize) << (7 * decoder.prefix);
            decoder.prefix += 1;
            if byte & 128 != 0 {
                return None;
            }
            if decoder.length == 0
                || decoder.length > FRAME_LIMIT
                || (decoder.prefix > 1 && byte == 0)
            {
                self.error = Some("invalid negotiation frame length");
                return None;
            }
            decoder.prefix_complete = true;
            return None;
        }
        decoder.frame.push(byte);
        if decoder.frame.len() < decoder.prefix + decoder.length {
            return None;
        }
        let text = match std::str::from_utf8(&decoder.frame[decoder.prefix..]) {
            Ok(text) => text.to_owned(),
            Err(_) => {
                self.error = Some("non UTF-8 negotiation frame");
                return None;
            }
        };
        let valid_token = text.ends_with('\n')
            && text.len() >= 2
            && text[..text.len() - 1]
                .bytes()
                .all(|byte| (33..=126).contains(&byte))
            && (text.starts_with('/') || text == "na\n");
        if !valid_token || (!decoder.header && text != HEADER) {
            self.error = Some("invalid multistream token or header");
            decoder.frame.clear();
            return None;
        }
        let frame_hex = decoder
            .frame
            .iter()
            .map(|byte| format!("{byte:02x}"))
            .collect::<String>();
        decoder.frame.clear();
        decoder.length = 0;
        decoder.prefix = 0;
        decoder.prefix_complete = false;
        let mut event = json!({"kind": "negotiation_frame", "direction": if direction == 0 { "read" } else { "write" }, "frame_hex": frame_hex});
        if !decoder.header {
            if text != HEADER {
                self.error = Some("missing multistream header");
            } else {
                decoder.header = true;
            }
            return Some(event);
        }
        if text == HEADER || !text.ends_with('\n') || text[..text.len() - 1].contains('\n') {
            self.error = Some("unexpected negotiation header or token");
            return Some(event);
        }
        let proposer = if self.outbound { 1 } else { 0 };
        decoder.paused = true;
        if direction == proposer {
            self.proposals += 1;
            if self.proposals > PROPOSAL_LIMIT || !text.starts_with('/') {
                self.error = Some("invalid or excessive proposal");
                return Some(event);
            }
            self.proposal = Some(text.trim_end_matches('\n').to_owned());
        } else {
            if text != "na\n" && !text.starts_with('/') {
                self.error = Some("invalid acknowledgement");
                return Some(event);
            }
            self.reply = Some(text.trim_end_matches('\n').to_owned());
        }
        if let (Some(proposal), Some(reply)) = (&self.proposal, &self.reply) {
            if reply == "na" {
                event["selection_outcome"] = json!("rejected");
                if self.ambiguous_tail {
                    self.error = Some("unmatched bytes after rejected proposal");
                }
                self.proposal = None;
                self.reply = None;
                self.decoders
                    .iter_mut()
                    .for_each(|decoder| decoder.paused = false);
            } else if proposal != reply {
                self.error = Some("acknowledgement disagrees with proposal");
            } else {
                self.selected = Some(proposal.clone());
                event["selection_outcome"] = json!("selected");
                event["protocol"] = json!(proposal);
            }
        }
        Some(event)
    }

    fn upgrade_complete(&self) -> bool {
        self.delegate_complete
            && self.selected.is_some()
            && self.selected == self.delegate_protocol
            && self.error.is_none()
            && !self.io_failed
    }

    fn snapshot(&self) -> Value {
        json!({"direction": direction(self.outbound), "protocol": self.selected,
            "proposed_protocol": self.proposal,
            "parser_error": self.error, "io_failed": self.io_failed,
            "drop_observed": self.drop_observed,
            "read_eof": self.read_eof, "write_close_returned": self.write_closed,
            "response_write_complete": self.application && self.selected.is_some() && self.error.is_none()
                && !self.io_failed && self.write_closed && self.bodies[1].complete(),
            "read": self.bodies[0].snapshot(), "write": self.bodies[1].snapshot()})
    }
}

struct ObservedIo<T> {
    inner: T,
    trace: Trace,
    phase: usize,
}

impl<T> Drop for ObservedIo<T> {
    fn drop(&mut self) {
        self.trace.update(|connection| {
            if let Some(phase) = connection.phases.get_mut(self.phase) {
                phase.drop_observed = true;
            }
        });
    }
}

impl<T: AsyncRead + Unpin> AsyncRead for ObservedIo<T> {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut [u8],
    ) -> Poll<io::Result<usize>> {
        let this = self.get_mut();
        let result = Pin::new(&mut this.inner).poll_read(cx, buf);
        match &result {
            Poll::Ready(Ok(count)) if *count != 0 => this.trace.feed(this.phase, 0, &buf[..*count]),
            Poll::Ready(Ok(_)) if !buf.is_empty() => this.trace.update(|connection| {
                if let Some(phase) = connection.phases.get_mut(this.phase) {
                    phase.read_eof = true;
                }
            }),
            Poll::Ready(Err(error)) => this.trace.io_error(this.phase, "read_error", error),
            _ => {}
        }
        result
    }

    fn poll_read_vectored(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        bufs: &mut [io::IoSliceMut<'_>],
    ) -> Poll<io::Result<usize>> {
        let this = self.get_mut();
        let result = Pin::new(&mut this.inner).poll_read_vectored(cx, bufs);
        match &result {
            Poll::Ready(Ok(count)) => {
                let mut remaining = *count;
                for buf in bufs.iter() {
                    let size = remaining.min(buf.len());
                    this.trace.feed(this.phase, 0, &buf[..size]);
                    remaining -= size;
                }
                if *count == 0 && bufs.iter().any(|buf| !buf.is_empty()) {
                    this.trace.update(|connection| {
                        if let Some(phase) = connection.phases.get_mut(this.phase) {
                            phase.read_eof = true;
                        }
                    });
                }
            }
            Poll::Ready(Err(error)) => this.trace.io_error(this.phase, "read_error", error),
            _ => {}
        }
        result
    }
}

impl<T: AsyncWrite + Unpin> AsyncWrite for ObservedIo<T> {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<io::Result<usize>> {
        let this = self.get_mut();
        let result = Pin::new(&mut this.inner).poll_write(cx, buf);
        match &result {
            Poll::Ready(Ok(count)) => this.trace.feed(this.phase, 1, &buf[..*count]),
            Poll::Ready(Err(error)) => this.trace.io_error(this.phase, "write_error", error),
            _ => {}
        }
        result
    }

    fn poll_write_vectored(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        bufs: &[io::IoSlice<'_>],
    ) -> Poll<io::Result<usize>> {
        let this = self.get_mut();
        let result = Pin::new(&mut this.inner).poll_write_vectored(cx, bufs);
        match &result {
            Poll::Ready(Ok(count)) => {
                let mut remaining = *count;
                for buf in bufs {
                    let size = remaining.min(buf.len());
                    this.trace.feed(this.phase, 1, &buf[..size]);
                    remaining -= size;
                }
            }
            Poll::Ready(Err(error)) => this.trace.io_error(this.phase, "write_error", error),
            _ => {}
        }
        result
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        let this = self.get_mut();
        let result = Pin::new(&mut this.inner).poll_flush(cx);
        if let Poll::Ready(Err(error)) = &result {
            this.trace.io_error(this.phase, "flush_error", error);
        }
        result
    }

    fn poll_close(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        let this = self.get_mut();
        let result = Pin::new(&mut this.inner).poll_close(cx);
        match &result {
            Poll::Ready(Ok(())) => this.trace.update(|connection| {
                if let Some(phase) = connection.phases.get_mut(this.phase) {
                    phase.write_closed = true;
                }
            }),
            Poll::Ready(Err(error)) => this.trace.io_error(this.phase, "close_error", error),
            _ => {}
        }
        result
    }
}

struct UpgradeGuard {
    trace: Trace,
    phase: usize,
    finished: bool,
}
impl Drop for UpgradeGuard {
    fn drop(&mut self) {
        if !self.finished {
            self.trace.event(
                self.phase,
                "upgrade_future_dropped_before_completion",
                json!({}),
            );
        }
    }
}

// Mirrors core/src/upgrade/apply.rs using public connection (not substream) traits.
async fn apply<C, U, R, E>(
    socket: C,
    upgrade: U,
    outbound: bool,
    trace: Trace,
    phase: usize,
) -> io::Result<R>
where
    C: AsyncRead + AsyncWrite + Unpin,
    U: InboundConnectionUpgrade<Negotiated<ObservedIo<C>>, Output = R, Error = E>
        + OutboundConnectionUpgrade<Negotiated<ObservedIo<C>>, Output = R, Error = E>,
    E: std::error::Error + Send + Sync + 'static,
{
    let mut guard = UpgradeGuard {
        trace: trace.clone(),
        phase,
        finished: false,
    };
    let socket = ObservedIo {
        inner: socket,
        trace: trace.clone(),
        phase,
    };
    let result = async {
        let (info, socket) = if outbound {
            dialer_select_proto(socket, upgrade.protocol_info(), Version::V1Lazy)
                .await
                .map_err(io::Error::other)?
        } else {
            listener_select_proto(socket, upgrade.protocol_info())
                .await
                .map_err(io::Error::other)?
        };
        let protocol = info.as_ref().to_owned();
        trace.event(phase, "delegate_started", json!({"protocol": protocol}));
        let output = if outbound {
            upgrade.upgrade_outbound(socket, info).await
        } else {
            upgrade.upgrade_inbound(socket, info).await
        }
        .map_err(io::Error::other)?;
        trace.update(|connection| {
            connection.phases[phase].delegate_protocol = Some(protocol.clone());
            connection.phases[phase].delegate_complete = true;
            connection.event(phase, "delegate_completed", json!({"protocol": protocol}));
        });
        Ok(output)
    }
    .await;
    if let Err(error) = &result {
        trace.io_error(phase, "upgrade_error", error);
    }
    guard.finished = true;
    result
}

struct ObservedMuxer<M> {
    inner: Pin<Box<M>>,
    trace: Trace,
}

impl<M> Drop for ObservedMuxer<M> {
    fn drop(&mut self) {
        self.trace
            .update(|connection| connection.muxer_drop_observed = true);
    }
}

impl<M: StreamMuxer> StreamMuxer for ObservedMuxer<M>
where
    M::Substream: Unpin,
{
    type Substream = ObservedIo<M::Substream>;
    type Error = M::Error;

    fn poll_inbound(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<Result<Self::Substream, Self::Error>> {
        let this = self.get_mut();
        let result = this.inner.as_mut().poll_inbound(cx);
        this.record_error("muxer_inbound_error", &result);
        result.map_ok(|inner| ObservedIo {
            inner,
            trace: this.trace.clone(),
            phase: this.trace.substream(false),
        })
    }

    fn poll_outbound(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<Result<Self::Substream, Self::Error>> {
        let this = self.get_mut();
        let result = this.inner.as_mut().poll_outbound(cx);
        this.record_error("muxer_outbound_error", &result);
        result.map_ok(|inner| ObservedIo {
            inner,
            trace: this.trace.clone(),
            phase: this.trace.substream(true),
        })
    }

    fn poll_close(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<(), Self::Error>> {
        let this = self.get_mut();
        let result = this.inner.as_mut().poll_close(cx);
        this.record_error("muxer_close_error", &result);
        if let Poll::Ready(Ok(())) = result {
            this.trace
                .update(|connection| connection.muxer_close_returned = true);
        }
        result
    }

    fn poll(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<Result<StreamMuxerEvent, Self::Error>> {
        let this = self.get_mut();
        let result = this.inner.as_mut().poll(cx);
        this.record_error("muxer_poll_error", &result);
        if let Poll::Ready(Ok(StreamMuxerEvent::AddressChange(address))) = &result {
            this.trace.event(
                1,
                "muxer_address_change",
                json!({"address": address.to_string()}),
            );
        }
        result
    }
}

impl<M: StreamMuxer> ObservedMuxer<M> {
    fn record_error<T>(&self, kind: &str, result: &Poll<Result<T, M::Error>>) {
        if let Poll::Ready(Err(error)) = result {
            self.trace.update(|connection| {
                connection.phases[1].io_failed = true;
                connection.event(1, kind, json!({"error": bounded_error(error)}));
            });
        }
    }
}

pub(crate) fn native_transport(
    key: &identity::Keypair,
    use_tls: bool,
    observer: Observer,
) -> Result<Boxed<(PeerId, StreamMuxerBox)>, Box<dyn std::error::Error + Send + Sync>> {
    let local_peer = key.public().to_peer_id();
    // Both branches use exactly the pinned TCP builder's security/muxer configurations.
    macro_rules! transport {
        ($security:expr) => {{
            let security = $security;
            tcp::tokio::Transport::new(tcp::Config::default().nodelay(true))
                .and_then(move |socket, point| {
                    let trace = observer.begin(&point,
                        socket.0.local_addr().map(socket_address).unwrap_or_default(),
                        socket.0.peer_addr().map(socket_address).unwrap_or_default(), local_peer);
                    let security = security.clone();
                    async move {
                        let outbound = upgrade_outbound(&point);
                        let (peer, secure) = apply(socket, security, outbound, trace.clone(), 0).await?;
                        trace.update(|connection| {
                            connection.remote_peer = peer.to_string();
                            connection.event(0, "authenticated_peer", json!({"peer_id": peer.to_string()}));
                        });
                        let muxer = apply(secure, yamux::Config::default(), outbound, trace.clone(), 1).await?;
                        Ok::<_, io::Error>((peer, StreamMuxerBox::new(ObservedMuxer { inner: Box::pin(muxer), trace })))
                    }
                }).boxed()
        }};
    }
    Ok(if use_tls {
        transport!(tls::Config::new(key).map_err(io::Error::other)?)
    } else {
        transport!(noise::Config::new(key).map_err(io::Error::other)?)
    })
}

fn socket_address(address: std::net::SocketAddr) -> String {
    let ip = match address.ip() {
        std::net::IpAddr::V4(ip) => libp2p::multiaddr::Protocol::Ip4(ip),
        std::net::IpAddr::V6(ip) => libp2p::multiaddr::Protocol::Ip6(ip),
    };
    libp2p::Multiaddr::empty()
        .with(ip)
        .with(libp2p::multiaddr::Protocol::Tcp(address.port()))
        .to_string()
}

#[cfg(test)]
mod tests {
    use super::*;
    use futures::{AsyncReadExt, AsyncWriteExt, FutureExt, future, task::noop_waker};
    use libp2p::core::{Endpoint, upgrade::UpgradeInfo};

    fn frame(text: &[u8]) -> Vec<u8> {
        let mut length = text.len();
        let mut bytes = Vec::new();
        while length >= 128 {
            bytes.push((length as u8 & 127) | 128);
            length >>= 7;
        }
        bytes.push(length as u8);
        bytes.extend_from_slice(text);
        bytes
    }

    fn fixture_trace() -> (Observer, Trace) {
        let observer = Observer::default();
        let point = ConnectedPoint::Listener {
            local_addr: "/ip4/127.0.0.1/tcp/4001".parse().unwrap(),
            send_back_addr: "/ip4/127.0.0.1/tcp/4002".parse().unwrap(),
        };
        let trace = observer.begin(&point, "local".into(), "remote".into(), PeerId::random());
        (observer, trace)
    }

    fn feed(trace: &Trace, phase: usize, direction: usize, bytes: &[u8], chunk: usize) {
        for bytes in bytes.chunks(chunk) {
            trace.feed(phase, direction, bytes);
        }
    }

    #[test]
    fn upgrade_parser_fragmented_rejection_then_selected_no_body_capture() {
        for outbound in [false, true] {
            for chunk in 1..=32 {
                let (observer, trace) = fixture_trace();
                trace.update(|connection| connection.phases[0] = Selection::new(outbound, false));
                let proposer = usize::from(outbound);
                for direction in [proposer, 1 - proposer] {
                    feed(&trace, 0, direction, &frame(HEADER.as_bytes()), chunk);
                }
                feed(&trace, 0, proposer, &frame(b"/tls/1.0.0\n"), chunk);
                feed(&trace, 0, 1 - proposer, &frame(b"na\n"), chunk);
                feed(&trace, 0, proposer, &frame(b"/noise\n"), chunk);
                let mut ack = frame(b"/noise\n");
                ack.extend_from_slice(b"opaque security bytes, never evidence");
                feed(&trace, 0, 1 - proposer, &ack, chunk);
                let snapshot = observer.snapshot();
                let connection = &snapshot["connections"][0];
                assert_eq!(connection["negotiations"][0]["protocol"], "/noise");
                assert!(connection["negotiations"][0]["parser_error"].is_null());
                assert_eq!(connection["events"].as_array().unwrap().len(), 6);
                assert_eq!(connection["events"][3]["selection_outcome"], "rejected");
                assert!(!snapshot.to_string().contains("opaque"));
                assert_eq!(snapshot["complete"], false);
            }
        }
    }

    #[test]
    fn upgrade_parser_wrong_ack_bounds_headers_and_unmatched_fallback() {
        for case in ["ack", "header", "length", "proposals", "tail"] {
            let (_, trace) = fixture_trace();
            feed(&trace, 0, 0, &frame(HEADER.as_bytes()), 1);
            feed(&trace, 0, 1, &frame(HEADER.as_bytes()), 1);
            match case {
                "ack" => {
                    feed(&trace, 0, 0, &frame(b"/noise\n"), 1);
                    feed(&trace, 0, 1, &frame(b"/tls/1.0.0\n"), 1);
                }
                "header" => feed(&trace, 0, 0, &frame(HEADER.as_bytes()), 1),
                "length" => feed(&trace, 0, 0, &[0x81, 0x02], 1),
                "proposals" => {
                    for _ in 0..=PROPOSAL_LIMIT {
                        feed(&trace, 0, 0, &frame(b"/noise\n"), 1);
                        feed(&trace, 0, 1, &frame(b"na\n"), 1);
                    }
                }
                "tail" => {
                    let mut proposal = frame(b"/noise\n");
                    proposal.extend_from_slice(b"not another proposal");
                    feed(&trace, 0, 0, &proposal, 1);
                    feed(&trace, 0, 1, &frame(b"na\n"), 1);
                }
                _ => unreachable!(),
            }
            trace.update(|connection| {
                assert!(connection.phases[0].error.is_some(), "{case}");
                assert!(connection.phases[0].selected.is_none(), "{case}");
                assert!(connection.phases[0].decoders[0].frame.len() <= FRAME_LIMIT + 2);
            });
        }
        let mut selection = Selection::new(false, false);
        for byte in frame(HEADER.as_bytes())
            .into_iter()
            .chain(frame(b"/noise\n"))
        {
            selection.byte(0, byte);
        }
        assert!(
            selection.selected.is_none(),
            "proposal alone is not selection"
        );
    }

    #[test]
    fn upgrade_application_framing_hash_and_local_close_not_remote_receipt() {
        for chunk in 1..=32 {
            let (observer, trace) = fixture_trace();
            let phase = trace.substream(false);
            let protocol = b"/ipfs/id/1.0.0\n";
            for direction in 0..2 {
                feed(&trace, phase, direction, &frame(HEADER.as_bytes()), chunk);
            }
            feed(&trace, phase, 0, &frame(protocol), chunk);
            let body = frame(b"opaque Identify body");
            let mut response = frame(protocol);
            response.extend_from_slice(&body);
            feed(&trace, phase, 1, &response, chunk);
            trace.update(|connection| {
                assert!(
                    !connection.phases[phase].snapshot()["response_write_complete"]
                        .as_bool()
                        .unwrap()
                );
                connection.phases[phase].write_closed = true;
            });
            let snapshot = observer.snapshot();
            let stream = &snapshot["connections"][0]["streams"][0];
            assert_eq!(stream["response_write_complete"], true);
            assert_eq!(stream["write"]["framed_bytes"], body.len());
            assert_eq!(
                stream["write"]["framed_sha256"],
                format!("{:x}", Sha256::digest(&body))
            );
            assert!(!snapshot.to_string().contains("opaque Identify body"));
            assert!(stream.get("network_stream_id").is_none());
            assert!(stream.get("remote_received").is_none());
            assert_eq!(snapshot["exact_application_binding_supported"], false);
            trace.io_error(
                phase,
                "close_error",
                &io::Error::other("delegated close failed"),
            );
            assert_eq!(
                observer.snapshot()["connections"][0]["streams"][0]["response_write_complete"],
                false
            );
        }
        let mut body = Body::default();
        for byte in frame(&[1; 4096]) {
            body.byte(byte, 4096);
        }
        assert!(body.invalid);
        assert!(body.bytes <= 4096);
    }

    #[derive(Default)]
    struct IoState {
        writes: Vec<u8>,
        closes: usize,
        flushes: usize,
        polls: usize,
        vectored_polls: usize,
    }

    struct ScriptIo {
        input: futures::io::Cursor<Vec<u8>>,
        state: Arc<Mutex<IoState>>,
        trace: Trace,
        pending_once: bool,
        fail: bool,
    }

    impl ScriptIo {
        fn new(input: Vec<u8>, trace: Trace) -> Self {
            Self {
                input: futures::io::Cursor::new(input),
                state: Default::default(),
                trace,
                pending_once: false,
                fail: false,
            }
        }

        fn check_unlocked(&self) {
            assert!(
                self.trace.0.as_ref().unwrap().try_lock().is_ok(),
                "observer locked across delegate I/O"
            );
        }
    }

    impl AsyncRead for ScriptIo {
        fn poll_read(
            self: Pin<&mut Self>,
            cx: &mut Context<'_>,
            buf: &mut [u8],
        ) -> Poll<io::Result<usize>> {
            let this = self.get_mut();
            this.check_unlocked();
            Pin::new(&mut this.input).poll_read(cx, buf)
        }
    }

    impl AsyncWrite for ScriptIo {
        fn poll_write(
            self: Pin<&mut Self>,
            cx: &mut Context<'_>,
            buf: &[u8],
        ) -> Poll<io::Result<usize>> {
            let this = self.get_mut();
            this.check_unlocked();
            this.state.lock().unwrap().polls += 1;
            if this.pending_once {
                this.pending_once = false;
                cx.waker().wake_by_ref();
                return Poll::Pending;
            }
            if this.fail {
                return Poll::Ready(Err(io::Error::from(io::ErrorKind::BrokenPipe)));
            }
            let size = buf.len().min(3);
            this.state
                .lock()
                .unwrap()
                .writes
                .extend_from_slice(&buf[..size]);
            Poll::Ready(Ok(size))
        }
        fn poll_write_vectored(
            self: Pin<&mut Self>,
            cx: &mut Context<'_>,
            bufs: &[io::IoSlice<'_>],
        ) -> Poll<io::Result<usize>> {
            let this = self.get_mut();
            this.check_unlocked();
            this.state.lock().unwrap().vectored_polls += 1;
            if this.pending_once {
                this.pending_once = false;
                cx.waker().wake_by_ref();
                return Poll::Pending;
            }
            if this.fail {
                return Poll::Ready(Err(io::Error::from(io::ErrorKind::BrokenPipe)));
            }
            let mut written = 0;
            for buf in bufs {
                let size = buf.len().min(3 - written);
                this.state
                    .lock()
                    .unwrap()
                    .writes
                    .extend_from_slice(&buf[..size]);
                written += size;
                if written == 3 {
                    break;
                }
            }
            Poll::Ready(Ok(written))
        }
        fn poll_flush(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
            self.check_unlocked();
            self.state.lock().unwrap().flushes += 1;
            Poll::Ready(Ok(()))
        }
        fn poll_close(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
            self.check_unlocked();
            self.state.lock().unwrap().closes += 1;
            Poll::Ready(Ok(()))
        }
    }

    #[test]
    fn upgrade_passive_io_pending_partial_error_and_overflow() {
        let (observer, trace) = fixture_trace();
        trace.update(|connection| connection.overflow = true);
        let mut inner = ScriptIo::new(b"read bytes".to_vec(), trace.clone());
        inner.pending_once = true;
        let state = inner.state.clone();
        let mut io = ObservedIo {
            inner,
            trace,
            phase: 0,
        };
        futures::executor::block_on(async {
            io.write_all(b"unaltered forwarded bytes").await.unwrap();
            io.flush().await.unwrap();
            let mut read = Vec::new();
            io.read_to_end(&mut read).await.unwrap();
            assert_eq!(read, b"read bytes");
            io.close().await.unwrap();
            io.inner.fail = true;
            assert_eq!(
                io.write(b"x").await.unwrap_err().kind(),
                io::ErrorKind::BrokenPipe
            );
        });
        let state = state.lock().unwrap();
        assert_eq!(state.writes, b"unaltered forwarded bytes");
        assert_eq!((state.flushes, state.closes), (1, 1));
        assert_eq!(observer.snapshot()["overflow"], true);
        assert_eq!(observer.snapshot()["complete"], false);
    }

    #[test]
    fn upgrade_active_trace_pending_partial_and_vectored_write_accounting() {
        let (observer, trace) = fixture_trace();
        let phase = trace.substream(false);
        let header = frame(HEADER.as_bytes());
        let protocol = frame(b"/ipfs/id/1.0.0\n");
        let input = [header.as_slice(), protocol.as_slice()].concat();
        let inner = ScriptIo::new(input.clone(), trace.clone());
        let state = inner.state.clone();
        let mut stream = ObservedIo {
            inner,
            trace,
            phase,
        };
        let waker = noop_waker();
        let mut context = Context::from_waker(&waker);

        // All negotiation observations enter through the real wrapper I/O methods.
        let mut read = Vec::new();
        futures::executor::block_on(stream.read_to_end(&mut read)).unwrap();
        assert_eq!(read, input);
        stream.inner.pending_once = true;
        let before = observer.snapshot();
        assert!(
            Pin::new(&mut stream)
                .poll_write(&mut context, &header)
                .is_pending()
        );
        assert_eq!(
            observer.snapshot(),
            before,
            "Pending must not advance the active parser"
        );
        assert!(state.lock().unwrap().writes.is_empty());

        assert!(matches!(
            Pin::new(&mut stream).poll_write(&mut context, &header),
            Poll::Ready(Ok(3))
        ));
        assert_eq!(state.lock().unwrap().writes, header[..3]);
        stream.trace.update(|connection| {
            assert_eq!(connection.phases[phase].decoders[1].frame, header[..3]);
            assert!(connection.phases[phase].selected.is_none());
        });
        futures::executor::block_on(async {
            stream.write_all(&header[3..]).await.unwrap();
            stream.write_all(&protocol).await.unwrap();
        });
        let selected = observer.snapshot();
        assert_eq!(
            selected["connections"][0]["streams"][0]["protocol"],
            "/ipfs/id/1.0.0"
        );
        assert!(selected["connections"][0]["streams"][0]["parser_error"].is_null());
        assert_eq!(
            selected["connections"][0]["streams"][0]["write"]["framed_bytes"],
            0
        );

        let body = frame(b"abcde");
        let bufs = [
            io::IoSlice::new(&[]),
            io::IoSlice::new(&body[..1]),
            io::IoSlice::new(&body[1..4]),
            io::IoSlice::new(&body[4..]),
            io::IoSlice::new(b"never written"),
        ];
        stream.inner.pending_once = true;
        let before_bytes = state.lock().unwrap().writes.clone();
        assert!(
            Pin::new(&mut stream)
                .poll_write_vectored(&mut context, &bufs)
                .is_pending()
        );
        assert_eq!(
            observer.snapshot(),
            selected,
            "vectored Pending must not count or hash offered bytes"
        );
        assert_eq!(state.lock().unwrap().writes, before_bytes);
        assert!(matches!(
            Pin::new(&mut stream).poll_write_vectored(&mut context, &bufs),
            Poll::Ready(Ok(3))
        ));
        assert_eq!(
            state.lock().unwrap().vectored_polls,
            2,
            "must delegate vectored I/O, not flatten it into scalar writes"
        );
        let partial = observer.snapshot();
        let write = &partial["connections"][0]["streams"][0]["write"];
        assert_eq!(write["framed_bytes"], 3);
        assert_eq!(write["frames"], 0);
        assert_eq!(
            write["framed_sha256"],
            format!("{:x}", Sha256::digest(&body[..3]))
        );
        assert_eq!(
            state.lock().unwrap().writes,
            [before_bytes.as_slice(), &body[..3]].concat()
        );

        futures::executor::block_on(async {
            stream.write_all(&body[3..]).await.unwrap();
            stream.close().await.unwrap();
        });
        let completed = observer.snapshot();
        let response = &completed["connections"][0]["streams"][0];
        assert_eq!(response["write"]["framed_bytes"], body.len());
        assert_eq!(response["write"]["frames"], 1);
        assert_eq!(
            response["write"]["framed_sha256"],
            format!("{:x}", Sha256::digest(&body))
        );
        assert_eq!(response["response_write_complete"], true);
        assert!(response["parser_error"].is_null());
        assert_eq!(completed["overflow"], false);
        assert_eq!(
            state.lock().unwrap().writes,
            [header, protocol, body].concat()
        );
    }

    #[derive(Clone, Copy)]
    enum Mode {
        Success,
        Error,
        Pending,
        LocalConstruction,
    }

    struct Delegate {
        mode: Mode,
        calls: Arc<Mutex<Vec<bool>>>,
    }
    impl UpgradeInfo for Delegate {
        type Info = &'static str;
        type InfoIter = std::iter::Once<Self::Info>;
        fn protocol_info(&self) -> Self::InfoIter {
            std::iter::once("/noise")
        }
    }

    impl Delegate {
        fn run<C: AsyncRead + AsyncWrite + Unpin + Send + 'static>(
            self,
            mut socket: C,
            outbound: bool,
        ) -> future::BoxFuture<'static, io::Result<()>> {
            async move {
                self.calls.lock().unwrap().push(outbound);
                match self.mode {
                    Mode::Success => {
                        // Forces V1Lazy confirmation before asserting actual delegate completion.
                        let mut byte = [0];
                        socket.read_exact(&mut byte).await?;
                        assert_eq!(byte, [42]);
                        socket.write_all(b"opaque handshake").await?;
                        socket.close().await?;
                        Ok(())
                    }
                    Mode::Error => Err(io::Error::from(io::ErrorKind::PermissionDenied)),
                    Mode::Pending => future::pending().await,
                    Mode::LocalConstruction => Ok(()),
                }
            }
            .boxed()
        }
    }

    impl<C: AsyncRead + AsyncWrite + Unpin + Send + 'static> InboundConnectionUpgrade<C> for Delegate {
        type Output = ();
        type Error = io::Error;
        type Future = future::BoxFuture<'static, io::Result<()>>;
        fn upgrade_inbound(self, socket: C, _: Self::Info) -> Self::Future {
            self.run(socket, false)
        }
    }
    impl<C: AsyncRead + AsyncWrite + Unpin + Send + 'static> OutboundConnectionUpgrade<C> for Delegate {
        type Output = ();
        type Error = io::Error;
        type Future = future::BoxFuture<'static, io::Result<()>>;
        fn upgrade_outbound(self, socket: C, _: Self::Info) -> Self::Future {
            self.run(socket, true)
        }
    }

    #[test]
    fn upgrade_real_select_delegates_connection_traits_and_preserves_errors() {
        for outbound in [false, true] {
            for mode in [Mode::Success, Mode::Error] {
                let (observer, trace) = fixture_trace();
                trace.update(|connection| connection.phases[0] = Selection::new(outbound, false));
                let mut input = frame(HEADER.as_bytes());
                input.extend(frame(b"/noise\n"));
                input.push(42);
                let socket = ScriptIo::new(input, trace.clone());
                let calls = Arc::new(Mutex::new(Vec::new()));
                let result = futures::executor::block_on(apply(
                    socket,
                    Delegate {
                        mode,
                        calls: calls.clone(),
                    },
                    outbound,
                    trace,
                    0,
                ));
                assert_eq!(*calls.lock().unwrap(), vec![outbound]);
                let snapshot = observer.snapshot();
                match mode {
                    Mode::Success => {
                        result.unwrap();
                        assert_eq!(snapshot["connections"][0]["security_complete"], true);
                        assert_eq!(snapshot["connections"][0]["selected_security"], "/noise");
                        assert_eq!(
                            snapshot["connections"][0]["negotiations"][0]["protocol"],
                            "/noise"
                        );
                    }
                    Mode::Error => {
                        let error = result.unwrap_err();
                        let cause = error
                            .get_ref()
                            .and_then(|source| source.downcast_ref::<io::Error>());
                        assert_eq!(
                            cause.unwrap_or(&error).kind(),
                            io::ErrorKind::PermissionDenied
                        );
                        assert_eq!(snapshot["connections"][0]["security_complete"], false);
                        assert!(snapshot.to_string().contains("upgrade_error"));
                    }
                    _ => unreachable!(),
                }
                assert!(!snapshot.to_string().contains("opaque handshake"));
            }
        }
    }

    #[test]
    fn upgrade_cancel_drops_owned_future_without_completion_or_tasks() {
        let (observer, trace) = fixture_trace();
        trace.update(|connection| connection.phases[0] = Selection::new(true, false));
        let calls = Arc::new(Mutex::new(Vec::new()));
        let input = frame(HEADER.as_bytes())
            .into_iter()
            .chain(frame(b"/noise\n"))
            .collect();
        let socket = ScriptIo::new(input, trace.clone());
        assert!(
            apply(
                socket,
                Delegate {
                    mode: Mode::Pending,
                    calls: calls.clone()
                },
                true,
                trace,
                0
            )
            .now_or_never()
            .is_none()
        );
        assert_eq!(*calls.lock().unwrap(), vec![true]);
        let snapshot = observer.snapshot();
        assert_eq!(snapshot["connections"][0]["security_complete"], false);
        assert!(
            snapshot
                .to_string()
                .contains("upgrade_future_dropped_before_completion")
        );
    }

    #[test]
    fn upgrade_lazy_local_construction_does_not_claim_remote_selection() {
        let (observer, trace) = fixture_trace();
        trace.update(|connection| connection.phases[1] = Selection::new(true, false));
        let socket = ScriptIo::new(Vec::new(), trace.clone());
        let calls = Arc::new(Mutex::new(Vec::new()));
        futures::executor::block_on(apply(
            socket,
            Delegate {
                mode: Mode::LocalConstruction,
                calls: calls.clone(),
            },
            true,
            trace,
            1,
        ))
        .unwrap();
        assert_eq!(*calls.lock().unwrap(), vec![true]);
        let snapshot = observer.snapshot();
        assert_eq!(snapshot["connections"][0]["muxer_delegate_completed"], true);
        assert_eq!(snapshot["connections"][0]["muxer_complete"], false);
        assert!(snapshot["connections"][0]["negotiations"][1]["protocol"].is_null());
    }

    #[test]
    fn upgrade_role_override_and_evidence_bounds_are_explicit() {
        let mut point = ConnectedPoint::Dialer {
            address: "/ip4/127.0.0.1/tcp/1".parse().unwrap(),
            role_override: Endpoint::Listener,
            port_use: libp2p::core::transport::PortUse::Reuse,
        };
        assert!(!upgrade_outbound(&point));
        assert_eq!(endpoint(&point)["direction"], "outbound");
        if let ConnectedPoint::Dialer { role_override, .. } = &mut point {
            *role_override = Endpoint::Dialer;
        }
        assert!(upgrade_outbound(&point));
        let observer = Observer::default();
        for _ in 0..=CONNECTION_LIMIT {
            observer.begin(&point, String::new(), String::new(), PeerId::random());
        }
        assert_eq!(
            observer.snapshot()["connections"].as_array().unwrap().len(),
            CONNECTION_LIMIT
        );
        assert_eq!(observer.snapshot()["overflow"], true);
        let (observer, trace) = fixture_trace();
        for _ in 0..=STREAM_LIMIT {
            trace.substream(false);
        }
        for _ in 0..=EVENT_LIMIT {
            trace.event(0, "bounded_test_event", json!({}));
        }
        let snapshot = observer.snapshot();
        assert_eq!(
            snapshot["connections"][0]["streams"]
                .as_array()
                .unwrap()
                .len(),
            STREAM_LIMIT
        );
        assert_eq!(
            snapshot["connections"][0]["events"]
                .as_array()
                .unwrap()
                .len(),
            EVENT_LIMIT
        );
        assert_eq!(snapshot["overflow"], true);
    }

    struct FakeMuxer {
        calls: Arc<Mutex<Vec<&'static str>>>,
        trace: Trace,
    }
    impl StreamMuxer for FakeMuxer {
        type Substream = ScriptIo;
        type Error = io::Error;
        fn poll_inbound(
            self: Pin<&mut Self>,
            _: &mut Context<'_>,
        ) -> Poll<Result<ScriptIo, io::Error>> {
            assert!(self.trace.0.as_ref().unwrap().try_lock().is_ok());
            self.calls.lock().unwrap().push("inbound");
            Poll::Pending
        }
        fn poll_outbound(
            self: Pin<&mut Self>,
            _: &mut Context<'_>,
        ) -> Poll<Result<ScriptIo, io::Error>> {
            assert!(self.trace.0.as_ref().unwrap().try_lock().is_ok());
            self.calls.lock().unwrap().push("outbound");
            Poll::Ready(Ok(ScriptIo::new(Vec::new(), self.trace.clone())))
        }
        fn poll_close(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<Result<(), io::Error>> {
            assert!(self.trace.0.as_ref().unwrap().try_lock().is_ok());
            self.calls.lock().unwrap().push("close");
            Poll::Ready(Ok(()))
        }
        fn poll(
            self: Pin<&mut Self>,
            _: &mut Context<'_>,
        ) -> Poll<Result<StreamMuxerEvent, io::Error>> {
            assert!(self.trace.0.as_ref().unwrap().try_lock().is_ok());
            self.calls.lock().unwrap().push("poll");
            Poll::Ready(Err(io::Error::from(io::ErrorKind::ConnectionReset)))
        }
    }

    #[test]
    fn upgrade_muxer_forwards_all_four_polls_and_drop_is_not_close() {
        let (observer, trace) = fixture_trace();
        let calls = Arc::new(Mutex::new(Vec::new()));
        let inner = Box::pin(FakeMuxer {
            calls: calls.clone(),
            trace: trace.clone(),
        });
        let mut muxer = ObservedMuxer {
            inner,
            trace: trace.clone(),
        };
        let waker = noop_waker();
        let mut context = Context::from_waker(&waker);
        assert!(Pin::new(&mut muxer).poll_inbound(&mut context).is_pending());
        assert!(matches!(
            Pin::new(&mut muxer).poll_outbound(&mut context),
            Poll::Ready(Ok(_))
        ));
        assert!(
            matches!(Pin::new(&mut muxer).poll(&mut context), Poll::Ready(Err(error)) if error.kind() == io::ErrorKind::ConnectionReset)
        );
        assert!(matches!(
            Pin::new(&mut muxer).poll_close(&mut context),
            Poll::Ready(Ok(()))
        ));
        assert_eq!(
            *calls.lock().unwrap(),
            vec!["inbound", "outbound", "poll", "close"]
        );
        drop(muxer);
        assert_eq!(
            observer.snapshot()["connections"][0]["muxer_close_returned"],
            true
        );
        let (observer, trace) = fixture_trace();
        drop(ObservedMuxer {
            inner: Box::pin(FakeMuxer {
                calls,
                trace: trace.clone(),
            }),
            trace,
        });
        assert_eq!(
            observer.snapshot()["connections"][0]["muxer_close_returned"],
            false
        );
        assert_eq!(
            observer.snapshot()["connections"][0]["muxer_drop_observed"],
            true
        );
    }
}
