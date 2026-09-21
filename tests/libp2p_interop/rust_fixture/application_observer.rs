//! Application-side framed I/O, independently correlated with passive mux observations.
use std::{
    io,
    pin::Pin,
    sync::{Arc, Mutex},
    task::{Context, Poll},
};

use futures::{AsyncRead, AsyncWrite};
use libp2p::{Multiaddr, PeerId, multiaddr::Protocol};
use serde_json::{Value, json};

use super::upgrade_observer::Body;

const LIMIT: usize = 32;
const ERROR_LIMIT: usize = 8;
pub(crate) const IDENTIFY: &str = "/ipfs/id/1.0.0";
pub(crate) const ECHO: &str = "/forge/interop/relay-echo/1";

#[derive(Clone, Debug, Default)]
pub(crate) struct Observer(Arc<Mutex<State>>);

#[derive(Debug, Default)]
struct State {
    attempts: Vec<Arc<Mutex<Record>>>,
    overflow: bool,
}

#[derive(Debug)]
struct Record {
    id: usize,
    peer: String,
    protocol: &'static str,
    preexisting: Vec<Value>,
    bodies: [Body; 2],
    opened: bool,
    completed: bool,
    ended: bool,
    dropped: bool,
    close_returned: bool,
    read_eof: bool,
    errors: Vec<&'static str>,
    overflow: bool,
}

impl Record {
    fn snapshot(&self) -> Value {
        json!({"application_trace_id": self.id, "authenticated_remote_peer_id": self.peer,
            "protocol": self.protocol, "direction": "outbound", "preexisting_raw_streams": self.preexisting,
            "opened": self.opened, "application_io_complete": self.completed,
            "attempt_ended": self.ended, "stream_drop_returned": self.dropped,
            "write_close_returned": self.close_returned, "read_eof": self.read_eof,
            "remote_receipt_claimed": false, "errors": self.errors, "overflow": self.overflow,
            "read": self.bodies[0].snapshot(), "write": self.bodies[1].snapshot()})
    }
}

impl Observer {
    pub(crate) fn begin(&self, peer: PeerId, protocol: &'static str, raw: &Value) -> Attempt {
        let mut state = self.0.lock().unwrap_or_else(|e| e.into_inner());
        if state.attempts.len() == LIMIT {
            state.overflow = true;
            return Attempt(None);
        }
        // Causal exclusion only: streams already opened cannot be returned by this new open.
        // Every later stream, including failures and concurrent automatic Identify, remains eligible.
        let preexisting = raw["connections"]
            .as_array()
            .into_iter()
            .flatten()
            .map(|c| {
                json!({"connection_trace_id": c["connection_trace_id"],
                "stream_count": c["streams"].as_array().map_or(0, Vec::len)})
            })
            .collect();
        let record = Arc::new(Mutex::new(Record {
            id: state.attempts.len() + 1,
            peer: peer.to_string(),
            protocol,
            preexisting,
            bodies: Default::default(),
            opened: false,
            completed: false,
            ended: false,
            dropped: false,
            close_returned: false,
            read_eof: false,
            errors: Vec::new(),
            overflow: false,
        }));
        state.attempts.push(record.clone());
        Attempt(Some(record))
    }

    pub(crate) fn snapshot(&self) -> Value {
        let state = self.0.lock().unwrap_or_else(|e| e.into_inner());
        json!({"source": "actual_swarm_stream_framed_io", "overflow": state.overflow,
            "attempts": state.attempts.iter().map(|r| r.lock().unwrap_or_else(|e| e.into_inner()).snapshot()).collect::<Vec<_>>()})
    }
}

pub(crate) struct Attempt(Option<Arc<Mutex<Record>>>);

impl Attempt {
    fn update(&self, f: impl FnOnce(&mut Record)) {
        if let Some(record) = &self.0 {
            f(&mut record.lock().unwrap_or_else(|e| e.into_inner()));
        }
    }

    pub(crate) fn wrap<T>(self, inner: T) -> ObservedStream<T> {
        self.update(|r| r.opened = true);
        ObservedStream {
            inner: Some(inner),
            attempt: self,
        }
    }

    fn feed(&self, direction: usize, bytes: &[u8]) {
        self.update(|r| {
            let limit = if r.protocol == IDENTIFY {
                4098
            } else {
                256 * 1024
            };
            for &byte in bytes {
                r.bodies[direction].byte(byte, limit);
            }
        });
    }

    fn error(&self, operation: &'static str) {
        self.update(|r| {
            if r.errors.len() == ERROR_LIMIT {
                r.overflow = true;
            } else {
                r.errors.push(operation);
            }
        });
    }
}

impl Drop for Attempt {
    fn drop(&mut self) {
        self.update(|r| r.ended = true);
    }
}

pub(crate) struct ObservedStream<T> {
    inner: Option<T>,
    attempt: Attempt,
}

impl<T> ObservedStream<T> {
    pub(crate) fn complete(&self) {
        self.attempt.update(|r| r.completed = true);
    }
}

impl<T> Drop for ObservedStream<T> {
    fn drop(&mut self) {
        // Record only after the actual application stream destructor has returned.
        drop(self.inner.take());
        self.attempt.update(|r| r.dropped = true);
    }
}

impl<T: AsyncRead + Unpin> AsyncRead for ObservedStream<T> {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut [u8],
    ) -> Poll<io::Result<usize>> {
        let this = self.get_mut();
        let result = Pin::new(this.inner.as_mut().expect("live stream")).poll_read(cx, buf);
        match &result {
            Poll::Ready(Ok(n)) => {
                this.attempt.feed(0, &buf[..*n]);
                if *n == 0 && !buf.is_empty() {
                    this.attempt.update(|r| r.read_eof = true);
                }
            }
            Poll::Ready(Err(_)) => this.attempt.error("read"),
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
        let result =
            Pin::new(this.inner.as_mut().expect("live stream")).poll_read_vectored(cx, bufs);
        match &result {
            Poll::Ready(Ok(n)) => {
                let mut remaining = *n;
                for buf in bufs.iter() {
                    let n = remaining.min(buf.len());
                    this.attempt.feed(0, &buf[..n]);
                    remaining -= n;
                }
                if *n == 0 && bufs.iter().any(|b| !b.is_empty()) {
                    this.attempt.update(|r| r.read_eof = true);
                }
            }
            Poll::Ready(Err(_)) => this.attempt.error("read_vectored"),
            _ => {}
        }
        result
    }
}

impl<T: AsyncWrite + Unpin> AsyncWrite for ObservedStream<T> {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<io::Result<usize>> {
        let this = self.get_mut();
        let result = Pin::new(this.inner.as_mut().expect("live stream")).poll_write(cx, buf);
        match &result {
            Poll::Ready(Ok(n)) => {
                this.attempt.feed(1, &buf[..*n]);
                if *n == 0 && !buf.is_empty() {
                    this.attempt.error("write_zero");
                }
            }
            Poll::Ready(Err(_)) => this.attempt.error("write"),
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
        let result =
            Pin::new(this.inner.as_mut().expect("live stream")).poll_write_vectored(cx, bufs);
        match &result {
            Poll::Ready(Ok(n)) => {
                let mut remaining = *n;
                for buf in bufs {
                    let n = remaining.min(buf.len());
                    this.attempt.feed(1, &buf[..n]);
                    remaining -= n;
                }
                if *n == 0 && bufs.iter().any(|b| !b.is_empty()) {
                    this.attempt.error("write_zero");
                }
            }
            Poll::Ready(Err(_)) => this.attempt.error("write_vectored"),
            _ => {}
        }
        result
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        let this = self.get_mut();
        let result = Pin::new(this.inner.as_mut().expect("live stream")).poll_flush(cx);
        if let Poll::Ready(Err(_)) = result {
            this.attempt.error("flush");
        }
        result
    }

    fn poll_close(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        let this = self.get_mut();
        let result = Pin::new(this.inner.as_mut().expect("live stream")).poll_close(cx);
        match &result {
            Poll::Ready(Ok(())) => this.attempt.update(|r| r.close_returned = true),
            Poll::Ready(Err(_)) => this.attempt.error("close"),
            _ => {}
        }
        result
    }
}

fn tcp_address(value: &Value, peer: &str) -> Option<String> {
    let mut addr: Multiaddr = value.as_str()?.parse().ok()?;
    if let Some(Protocol::P2p(id)) = addr.iter().last() {
        if id.to_string() != peer {
            return None;
        }
        addr.pop();
    }
    let parts = addr.iter().collect::<Vec<_>>();
    if !matches!(
        parts.as_slice(),
        [Protocol::Ip4(_), Protocol::Tcp(_)] | [Protocol::Ip6(_), Protocol::Tcp(_)]
    ) {
        return None;
    }
    Some(addr.to_string())
}

pub(crate) fn bound_remote_endpoint(
    connection: &Value,
    event: &Value,
    peer: &str,
) -> Result<String, &'static str> {
    let numeric = tcp_address(&event["endpoint"]["remote_address"], peer);
    let Some(receipts) = connection.get("transport_output_receipts") else {
        return numeric.ok_or("DNS Swarm endpoint lacks a causal transport output receipt");
    };
    let receipts = receipts.as_array().ok_or("invalid transport output receipts")?;
    let [detail] = receipts.as_slice() else { return Err("ambiguous transport output receipts"); };
    let boundary = detail["after_event_sequence"].as_u64().ok_or("missing transport output boundary")?;
    let events = connection["events"].as_array().ok_or("missing transport output event history")?;
    if detail["basis"] != "donor_transport_output_identity"
        || boundary > events.len() as u64
        || !detail["dns_wrapper_enabled"].is_boolean()
        || detail["connection_trace_id"] != connection["connection_trace_id"]
        || connection["authenticated_remote_peer_id"] != peer
        || detail["authenticated_remote_peer_id"] != peer
        || detail["request_endpoint"] != event["endpoint"]
        || detail["resolved_endpoint"] != connection["endpoint"]
        || detail["local_address"] != connection["local_address"]
        || detail["remote_address"] != connection["remote_address"]
        || detail["request_endpoint"]["direction"] != detail["resolved_endpoint"]["direction"]
        || detail["request_endpoint"]["upgrade_role"] != detail["resolved_endpoint"]["upgrade_role"] {
        return Err("transport output receipt disagrees with authenticated endpoints");
    }
    let resolved = tcp_address(&detail["resolved_endpoint"]["remote_address"], peer)
        .ok_or("transport output is not a numeric TCP endpoint")?;
    if connection["remote_address"].as_str() != Some(resolved.as_str()) {
        return Err("resolved dial endpoint disagrees with actual socket");
    }
    if let Some(numeric) = numeric {
        if numeric != resolved { return Err("numeric request disagrees with resolved endpoint"); }
    } else {
        let request: Multiaddr = event["endpoint"]["remote_address"].as_str()
            .ok_or("missing DNS request address")?.parse().map_err(|_| "invalid DNS request address")?;
        let protocols = request.iter().collect::<Vec<_>>();
        let has_dns = protocols.iter().any(|p| matches!(p, Protocol::Dns(_) | Protocol::Dns4(_) | Protocol::Dns6(_) | Protocol::Dnsaddr(_)));
        let peer_matches = protocols.iter().enumerate().all(|(i, p)| match p {
            Protocol::P2p(id) => i + 1 == protocols.len() && id.to_string() == peer,
            Protocol::P2pCircuit => false,
            _ => true,
        });
        let resolved_addr: Multiaddr = resolved.parse().map_err(|_| "invalid resolved socket")?;
        let resolved_port = resolved_addr.iter().find_map(|p| match p {
            Protocol::Tcp(port) => Some(port),
            _ => None,
        });
        let port_matches = protocols.iter().all(|p| match p {
            Protocol::Tcp(port) => Some(*port) == resolved_port,
            _ => true,
        });
        if detail["dns_wrapper_enabled"] != true || detail["request_endpoint"]["direction"] != "outbound"
            || !has_dns || !peer_matches || !port_matches {
            return Err("non-numeric request lacks matching donor DNS provenance");
        }
    }
    Ok(resolved)
}

fn bind(raw: &Value, app: &Value) -> Result<Value, &'static str> {
    let peer = app["authenticated_remote_peer_id"]
        .as_str()
        .ok_or("missing application peer")?;
    let connections = raw["connections"]
        .as_array()
        .ok_or("missing raw connections")?;
    // Do not discard failed authenticated connections before resolving uniqueness.
    let candidates = connections
        .iter()
        .filter(|c| c["authenticated_remote_peer_id"] == peer)
        .collect::<Vec<_>>();
    let [connection] = candidates.as_slice() else {
        return Err("absent or ambiguous authenticated connection");
    };
    let events = raw["swarm_events"]
        .as_array()
        .ok_or("missing Swarm events")?;
    let matches = events
        .iter()
        .filter(|e| e["authenticated_remote_peer_id"] == peer)
        .collect::<Vec<_>>();
    let [event] = matches.as_slice() else {
        return Err("absent or ambiguous Swarm connection");
    };
    let remote = bound_remote_endpoint(connection, event, peer)?;
    if connection["remote_address"].as_str() != Some(remote.as_str())
        || event["endpoint"]["direction"] != connection["direction"]
        || tcp_address(&connection["local_address"], peer).is_none()
        || (event["endpoint"]["direction"] == "inbound"
            && tcp_address(&event["endpoint"]["local_address"], peer).as_deref()
                != connection["local_address"].as_str())
    {
        return Err("authenticated endpoint mismatch");
    }
    let cid = &connection["connection_trace_id"];
    let previous = app["preexisting_raw_streams"]
        .as_array()
        .ok_or("missing open boundary")?;
    let before = previous
        .iter()
        .find(|c| c["connection_trace_id"] == *cid)
        .and_then(|c| c["stream_count"].as_u64())
        .unwrap_or(0);
    let protocol = app["protocol"]
        .as_str()
        .ok_or("missing application protocol")?;
    let streams = connection["streams"]
        .as_array()
        .ok_or("missing raw streams")?;
    let matches = streams
        .iter()
        .filter(|s| {
            s["stream_trace_id"].as_u64().is_some_and(|id| id > before)
                && s["direction"] == app["direction"]
                && (s["protocol"] == protocol || s["proposed_protocol"] == protocol)
                && s["read"] == app["read"]
                && s["write"] == app["write"]
        })
        .collect::<Vec<_>>();
    let [stream] = matches.as_slice() else {
        return Err("absent or ambiguous framed I/O match");
    };
    if connection["security_complete"] != true
        || connection["muxer_complete"] != true
        || connection["authenticated_local_peer_id"]
            .as_str()
            .is_none_or(str::is_empty)
        || connection["overflow"] != false
        || connection["muxer_drop_observed"] != true
        || stream["protocol"] != protocol
        || !stream["parser_error"].is_null()
        || stream["io_failed"] != false
        || stream["drop_observed"] != true
    {
        return Err("matched raw path failed or has not terminated");
    }
    if app["opened"] != true
        || app["application_io_complete"] != true
        || app["attempt_ended"] != true
        || app["stream_drop_returned"] != true
        || app["overflow"] != false
        || app["errors"].as_array().is_none_or(|e| !e.is_empty())
        || app["read"]["complete_frames"] != true
        || app["read"]["frames"] != 1
    {
        return Err("application failed or framing incomplete");
    }
    match protocol {
        IDENTIFY if app["write"]["framed_bytes"] == 0 => {}
        ECHO if app["write"] == app["read"]
            && app["write_close_returned"] == true
            && stream["write_close_returned"] == true => {}
        _ => return Err("application protocol completion mismatch"),
    }
    Ok(
        json!({"basis": "unique_authenticated_connection_and_framed_io",
        "connection_trace_id": cid, "stream_trace_id": stream["stream_trace_id"],
        "swarm_connection_id": event["swarm_connection_id"],
        "local_address": connection["local_address"], "remote_address": connection["remote_address"],
        "authenticated_local_peer_id": connection["authenticated_local_peer_id"],
        "authenticated_remote_peer_id": peer, "remote_receipt_claimed": false}),
    )
}

pub(crate) fn finalize(mut raw: Value, joined: bool) -> Value {
    let attempts = raw["applications"]["attempts"]
        .as_array()
        .cloned()
        .unwrap_or_default();
    let overflow = raw["overflow"] != false
        || raw["applications"]["overflow"] != false
        || attempts.iter().any(|a| a["overflow"] != false);
    raw["overflow"] = json!(overflow);
    let mut complete = joined && !overflow && !attempts.is_empty();
    let mut used = Vec::new();
    let receipts = attempts
        .into_iter()
        .map(|mut app| {
            match bind(&raw, &app) {
                Ok(binding) => {
                    let key = (
                        binding["connection_trace_id"].clone(),
                        binding["stream_trace_id"].clone(),
                    );
                    if used.contains(&key) {
                        complete = false;
                        app["binding_error"] =
                            json!("raw stream matched more than one application");
                    } else {
                        used.push(key);
                    }
                    app["binding"] = binding;
                }
                Err(error) => {
                    complete = false;
                    app["binding_error"] = json!(error);
                }
            }
            app
        })
        .collect::<Vec<_>>();
    raw["applications"]["attempts"] = json!(receipts);
    raw["complete"] = json!(complete);
    raw["exact_application_binding_supported"] = json!(true);
    raw["swarm_connection_binding_supported"] = json!(true);
    raw["fixture_owned_tasks_joined"] = json!(joined);
    raw["binding_basis"] = json!("unique_authenticated_connection_and_framed_io");
    raw["proof_limitation"] =
        json!("framed I/O correlation, not a native stream ID or remote delivery acknowledgement");
    raw
}

// The future is not polled unless the independently captured Identify was verified.
pub(crate) async fn after_verified_identify<T>(
    identify: &Value,
    echo: impl std::future::Future<Output = Result<T, Box<dyn std::error::Error>>>,
) -> Result<T, Box<dyn std::error::Error>> {
    if identify["status"] != "verified" || identify["signed_peer_record_verified"] != true {
        return Err("raw Identify verification failed; echo was not started".into());
    }
    echo.await
}

pub(crate) fn require_identify_echo_pair(raw: &mut Value) -> Result<(), &'static str> {
    if let Some(object) = raw.as_object_mut() {
        object.remove("application_pair_binding");
    }
    let result = identify_echo_pair(raw);
    match result {
        Ok(pair) => {
            raw["application_pair_binding"] = pair;
            Ok(())
        }
        Err(error) => {
            raw["complete"] = json!(false);
            raw["application_pair_binding_error"] = json!(error);
            Err(error)
        }
    }
}

fn identify_echo_pair(raw: &Value) -> Result<Value, &'static str> {
    if raw["complete"] != true || raw["fixture_owned_tasks_joined"] != true {
        return Err("Identify/echo pair is not finalized and joined");
    }
    let attempts = raw["applications"]["attempts"]
        .as_array()
        .ok_or("missing application pair")?;
    let [identify, echo] = attempts.as_slice() else {
        return Err("expected exactly one Identify then one echo");
    };
    if identify["protocol"] != IDENTIFY || echo["protocol"] != ECHO {
        return Err("application pair is not Identify then echo");
    }
    let first = &identify["binding"];
    let second = &echo["binding"];
    for key in [
        "basis",
        "connection_trace_id",
        "swarm_connection_id",
        "local_address",
        "remote_address",
        "authenticated_local_peer_id",
        "authenticated_remote_peer_id",
    ] {
        if first[key].is_null() || first[key] != second[key] {
            return Err("Identify and echo used different authenticated connections");
        }
    }
    let first_stream = first["stream_trace_id"]
        .as_u64()
        .ok_or("missing Identify stream binding")?;
    let second_stream = second["stream_trace_id"]
        .as_u64()
        .ok_or("missing echo stream binding")?;
    if first_stream == second_stream {
        return Err("Identify and echo did not use distinct streams");
    }
    Ok(
        json!({"basis": first["basis"], "connection_trace_id": first["connection_trace_id"],
        "swarm_connection_id": first["swarm_connection_id"],
        "identify_stream_trace_id": first_stream, "echo_stream_trace_id": second_stream}),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use futures::{AsyncReadExt, AsyncWriteExt, task::noop_waker};

    struct Duplex {
        read: futures::io::Cursor<Vec<u8>>,
        written: Arc<Mutex<Vec<u8>>>,
        pending: bool,
        fail: bool,
    }

    impl AsyncRead for Duplex {
        fn poll_read(
            mut self: Pin<&mut Self>,
            cx: &mut Context<'_>,
            buf: &mut [u8],
        ) -> Poll<io::Result<usize>> {
            Pin::new(&mut self.read).poll_read(cx, buf)
        }
    }
    impl AsyncWrite for Duplex {
        fn poll_write(
            mut self: Pin<&mut Self>,
            cx: &mut Context<'_>,
            buf: &[u8],
        ) -> Poll<io::Result<usize>> {
            if self.pending {
                self.pending = false;
                cx.waker().wake_by_ref();
                return Poll::Pending;
            }
            if self.fail {
                return Poll::Ready(Err(io::ErrorKind::BrokenPipe.into()));
            }
            let n = buf.len().min(2);
            self.written.lock().unwrap().extend_from_slice(&buf[..n]);
            Poll::Ready(Ok(n))
        }
        fn poll_flush(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
            Poll::Ready(Ok(()))
        }
        fn poll_close(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
            if self.fail {
                Poll::Ready(Err(io::ErrorKind::BrokenPipe.into()))
            } else {
                Poll::Ready(Ok(()))
            }
        }
    }

    fn body(bytes: &[u8]) -> Value {
        let mut body = Body::default();
        for &byte in bytes {
            body.byte(byte, 4098);
        }
        body.snapshot()
    }

    // Matcher-only fixtures are deliberately independent from real transport observation tests.
    fn fixture() -> Value {
        let peer = PeerId::random();
        let local = PeerId::random();
        let read = body(&[3, 7, 8, 9]);
        let write = body(&[]);
        json!({"overflow": false, "connections": [{"connection_trace_id": 7,
            "authenticated_remote_peer_id": peer.to_string(), "authenticated_local_peer_id": local.to_string(),
            "direction": "outbound", "local_address": "/ip4/127.0.0.1/tcp/3333",
            "remote_address": "/ip4/127.0.0.1/tcp/4444", "security_complete": true, "muxer_complete": true,
            "overflow": false, "muxer_drop_observed": true,
            "streams": [{"stream_trace_id": 19, "direction": "outbound", "protocol": IDENTIFY,
                "parser_error": null, "io_failed": false, "drop_observed": true,
                "write_close_returned": false, "read": read, "write": write}]}],
            "swarm_events": [{"swarm_connection_id": "unrelated-native-id-500",
                "authenticated_remote_peer_id": peer.to_string(), "endpoint": {"direction": "outbound",
                    "remote_address": format!("/ip4/127.0.0.1/tcp/4444/p2p/{peer}")}}],
            "applications": {"overflow": false, "attempts": [{"application_trace_id": 2,
                "authenticated_remote_peer_id": peer.to_string(), "protocol": IDENTIFY, "direction": "outbound",
                "preexisting_raw_streams": [], "opened": true, "application_io_complete": true,
                "attempt_ended": true, "stream_drop_returned": true, "write_close_returned": false,
                "read_eof": false, "overflow": false, "errors": [], "read": read, "write": write}]}})
    }

    fn output_fixture(dns: bool) -> Value {
        let mut raw = fixture();
        let peer = raw["connections"][0]["authenticated_remote_peer_id"].as_str().unwrap().to_owned();
        raw["swarm_events"][0]["endpoint"]["upgrade_role"] = json!("outbound");
        let resolved = raw["swarm_events"][0]["endpoint"].clone();
        if dns {
            raw["swarm_events"][0]["endpoint"]["remote_address"] =
                json!(format!("/dnsaddr/fixture.test/p2p/{peer}"));
        }
        let request = raw["swarm_events"][0]["endpoint"].clone();
        let connection = &mut raw["connections"][0];
        connection["endpoint"] = resolved.clone();
        connection["events"] = json!([]);
        connection["transport_output_receipts"] = json!([{
            "basis": "donor_transport_output_identity", "after_event_sequence": 0,
            "connection_trace_id": connection["connection_trace_id"],
            "authenticated_remote_peer_id": peer, "dns_wrapper_enabled": dns,
            "request_endpoint": request, "resolved_endpoint": resolved,
            "local_address": connection["local_address"], "remote_address": connection["remote_address"],
        }]);
        raw
    }

    #[test]
    fn application_dns_output_binding_preserves_original_request_and_numeric_socket() {
        let raw = output_fixture(true);
        let result = finalize(raw.clone(), true);
        assert_eq!(result["complete"], true);
        let binding = &result["applications"]["attempts"][0]["binding"];
        assert_eq!(binding["remote_address"], "/ip4/127.0.0.1/tcp/4444");
        assert_eq!(binding["connection_trace_id"], 7);
        assert_eq!(binding["swarm_connection_id"], "unrelated-native-id-500");
        assert_eq!(result["connections"][0]["transport_output_receipts"],
            raw["connections"][0]["transport_output_receipts"]);
        for (port, complete) in [(4444, true), (5555, false)] {
            let mut raw = raw.clone();
            let peer = raw["connections"][0]["authenticated_remote_peer_id"].as_str().unwrap();
            let request = json!(format!("/dns4/fixture.test/tcp/{port}/p2p/{peer}"));
            raw["swarm_events"][0]["endpoint"]["remote_address"] = request.clone();
            raw["connections"][0]["transport_output_receipts"][0]["request_endpoint"]["remote_address"] = request;
            assert_eq!(finalize(raw, true)["complete"], complete, "port {port}");
        }
    }

    #[test]
    fn application_dns_output_binding_rejects_endpoint_peer_and_provenance_mismatch() {
        for case in 0..11 {
            let mut raw = output_fixture(true);
            match case {
                0 => raw["swarm_events"][0]["endpoint"]["remote_address"] = json!("/dnsaddr/other.test"),
                1 => raw["connections"][0]["remote_address"] = json!("/ip4/127.0.0.1/tcp/5555"),
                2 => raw["connections"][0]["transport_output_receipts"][0]["authenticated_remote_peer_id"] = json!(PeerId::random().to_string()),
                3 => raw["connections"][0]["transport_output_receipts"][0]["connection_trace_id"] = json!(8),
                4 => raw["connections"][0]["transport_output_receipts"][0]["dns_wrapper_enabled"] = json!(false),
                5 => raw["connections"][0]["transport_output_receipts"][0]["after_event_sequence"] = json!(1),
                6 => raw["connections"][0]["transport_output_receipts"] = json!([]),
                7 => raw["connections"][0]["transport_output_receipts"] = json!({}),
                8 => { raw["connections"][0].as_object_mut().unwrap().remove("transport_output_receipts"); }
                9 => {
                    // Even internally consistent receipt text cannot authorize the wrong peer suffix.
                    let wrong = json!(format!("/dnsaddr/fixture.test/p2p/{}", PeerId::random()));
                    raw["swarm_events"][0]["endpoint"]["remote_address"] = wrong.clone();
                    raw["connections"][0]["transport_output_receipts"][0]["request_endpoint"]["remote_address"] = wrong;
                }
                _ => {
                    raw["swarm_events"][0]["endpoint"]["upgrade_role"] = json!("inbound");
                    raw["connections"][0]["transport_output_receipts"][0]["request_endpoint"]["upgrade_role"] = json!("inbound");
                }
            }
            assert_eq!(finalize(raw, true)["complete"], false, "case {case}");
        }
    }

    #[test]
    fn application_dns_output_binding_rejects_duplicate_and_failed_alternatives() {
        for kind in ["receipt", "connection", "stream", "event"] {
            let mut raw = output_fixture(true);
            let list = match kind {
                "receipt" => &mut raw["connections"][0]["transport_output_receipts"],
                "connection" => &mut raw["connections"],
                "stream" => &mut raw["connections"][0]["streams"],
                _ => &mut raw["swarm_events"],
            };
            let mut second = list[0].clone();
            if kind == "connection" {
                second["connection_trace_id"] = json!(8);
                second["security_complete"] = json!(false);
                second["transport_output_receipts"] = json!([]);
            } else if kind == "stream" {
                second["stream_trace_id"] = json!(20);
                second["io_failed"] = json!(true);
            }
            list.as_array_mut().unwrap().push(second);
            assert_eq!(finalize(raw, true)["complete"], false, "{kind}");
        }
    }

    #[test]
    fn application_numeric_output_binding_stays_strict() {
        assert_eq!(finalize(fixture(), true)["complete"], true);
        assert_eq!(finalize(output_fixture(false), true)["complete"], true);
        for case in 0..3 {
            let mut raw = output_fixture(false);
            match case {
                0 => raw["connections"][0]["transport_output_receipts"] = json!(false),
                1 => raw["connections"][0]["transport_output_receipts"][0]["resolved_endpoint"]["remote_address"] = json!("/ip4/127.0.0.1/tcp/5555"),
                _ => {
                    let wrong = json!("/ip4/127.0.0.1/tcp/5555");
                    raw["swarm_events"][0]["endpoint"]["remote_address"] = wrong.clone();
                    raw["connections"][0]["transport_output_receipts"][0]["request_endpoint"]["remote_address"] = wrong;
                }
            }
            assert_eq!(finalize(raw, true)["complete"], false, "case {case}");
        }
    }

    #[test]
    fn application_binding_uses_framed_io_not_foreign_ids_and_requires_join() {
        let raw = fixture();
        assert_eq!(finalize(raw.clone(), false)["complete"], false);
        let result = finalize(raw, true);
        assert_eq!(result["complete"], true);
        let receipt = &result["applications"]["attempts"][0];
        assert_eq!(receipt["binding"]["connection_trace_id"], 7);
        assert_eq!(receipt["binding"]["stream_trace_id"], 19);
        assert_eq!(
            receipt["binding"]["swarm_connection_id"],
            "unrelated-native-id-500"
        );
        assert_eq!(receipt["binding"]["remote_receipt_claimed"], false);
        assert_eq!(
            receipt["write_close_returned"], false,
            "Identify drop is not a close ACK"
        );
    }

    #[test]
    fn application_binding_rejects_ambiguity_including_failed_history() {
        for duplicate in ["connection", "stream", "event", "application"] {
            let mut raw = fixture();
            let path = match duplicate {
                "connection" => &mut raw["connections"],
                "stream" => &mut raw["connections"][0]["streams"],
                "event" => &mut raw["swarm_events"],
                _ => &mut raw["applications"]["attempts"],
            };
            let mut second = path[0].clone();
            if duplicate == "connection" {
                second["connection_trace_id"] = json!(8);
                second["security_complete"] = json!(false);
            }
            if duplicate == "stream" {
                second["stream_trace_id"] = json!(20);
                second["io_failed"] = json!(true);
            }
            if duplicate == "application" {
                second["application_trace_id"] = json!(3);
            }
            path.as_array_mut().unwrap().push(second);
            assert_eq!(finalize(raw, true)["complete"], false, "{duplicate}");
        }
    }

    #[test]
    fn application_binding_rejects_wrong_protocol_hash_endpoint_and_failures() {
        for case in 0..10 {
            let mut raw = fixture();
            match case {
                0 => raw["connections"][0]["streams"][0]["protocol"] = json!(ECHO),
                1 => {
                    raw["connections"][0]["streams"][0]["read"]["framed_sha256"] =
                        json!("different")
                }
                2 => {
                    raw["swarm_events"][0]["endpoint"]["remote_address"] =
                        json!("/ip4/127.0.0.1/tcp/5555")
                }
                3 => raw["connections"][0]["streams"][0]["io_failed"] = json!(true),
                4 => raw["applications"]["attempts"][0]["application_io_complete"] = json!(false),
                5 => raw["applications"]["attempts"][0]["errors"] = json!(["read"]),
                6 => raw["connections"][0]["streams"][0]["drop_observed"] = json!(false),
                7 => raw["overflow"] = json!(true),
                8 => {
                    raw["applications"]["attempts"][0]["authenticated_remote_peer_id"] =
                        json!(PeerId::random().to_string())
                }
                _ => raw["applications"]["attempts"][0]["direction"] = json!("inbound"),
            }
            assert_eq!(finalize(raw, true)["complete"], false, "case {case}");
        }
    }

    #[test]
    fn application_binding_excludes_only_streams_opened_before_actual_attempt() {
        let mut raw = fixture();
        let mut old = raw["connections"][0]["streams"][0].clone();
        old["stream_trace_id"] = json!(1);
        raw["connections"][0]["streams"]
            .as_array_mut()
            .unwrap()
            .push(old);
        assert_eq!(finalize(raw.clone(), true)["complete"], false);
        raw["applications"]["attempts"][0]["preexisting_raw_streams"] =
            json!([{"connection_trace_id": 7, "stream_count": 1}]);
        assert_eq!(finalize(raw.clone(), true)["complete"], true);
        raw["connections"][0]["streams"][1]["stream_trace_id"] = json!(20);
        assert_eq!(finalize(raw, true)["complete"], false);
    }

    #[test]
    fn application_echo_requires_matching_framed_reply_and_actual_close() {
        let mut raw = fixture();
        let read = raw["applications"]["attempts"][0]["read"].clone();
        {
            let value = &mut raw["applications"]["attempts"][0];
            value["protocol"] = json!(ECHO);
            value["write"] = read.clone();
            value["write_close_returned"] = json!(true);
        }
        let stream = &mut raw["connections"][0]["streams"][0];
        stream["protocol"] = json!(ECHO);
        stream["write"] = read;
        stream["write_close_returned"] = json!(true);
        assert_eq!(finalize(raw.clone(), true)["complete"], true);
        raw["connections"][0]["streams"][0]["write_close_returned"] = json!(false);
        assert_eq!(finalize(raw, true)["complete"], false);
    }

    #[test]
    fn application_io_records_actual_prefix_partial_pending_error_and_drop() {
        let observer = Observer::default();
        let written = Arc::new(Mutex::new(Vec::new()));
        let mut io = observer
            .begin(PeerId::random(), ECHO, &json!({"connections": []}))
            .wrap(Duplex {
                read: futures::io::Cursor::new(vec![3, 7, 8, 9]),
                written: written.clone(),
                pending: true,
                fail: false,
            });
        let before = observer.snapshot();
        let waker = noop_waker();
        let mut cx = Context::from_waker(&waker);
        assert!(
            Pin::new(&mut io)
                .poll_write(&mut cx, &[3, 7, 8, 9])
                .is_pending()
        );
        assert_eq!(observer.snapshot(), before);
        futures::executor::block_on(async {
            io.write_all(&[3, 7, 8, 9]).await.unwrap();
            let mut read = Vec::new();
            io.read_to_end(&mut read).await.unwrap();
            assert_eq!(read, [3, 7, 8, 9]);
            io.close().await.unwrap();
        });
        io.complete();
        assert_eq!(*written.lock().unwrap(), [3, 7, 8, 9]);
        assert_eq!(
            observer.snapshot()["attempts"][0]["write"],
            body(&[3, 7, 8, 9])
        );
        io.inner.as_mut().unwrap().fail = true;
        assert!(futures::executor::block_on(io.write(&[1])).is_err());
        drop(io);
        let snapshot = observer.snapshot();
        assert_eq!(snapshot["attempts"][0]["stream_drop_returned"], true);
        assert_eq!(snapshot["attempts"][0]["attempt_ended"], true);
        assert_eq!(snapshot["attempts"][0]["errors"], json!(["write"]));
        assert_eq!(snapshot["attempts"][0]["read"], body(&[3, 7, 8, 9]));
    }

    #[test]
    fn application_pair_rejects_reconnect_duplicate_or_unjoined_bindings() {
        let mut raw = finalize(fixture(), true);
        let mut echo = raw["applications"]["attempts"][0].clone();
        echo["protocol"] = json!(ECHO);
        echo["binding"]["stream_trace_id"] = json!(20);
        raw["applications"]["attempts"]
            .as_array_mut()
            .unwrap()
            .push(echo);
        assert!(require_identify_echo_pair(&mut raw).is_ok());
        assert_eq!(
            raw["application_pair_binding"]["identify_stream_trace_id"],
            19
        );
        for key in [
            "connection_trace_id",
            "swarm_connection_id",
            "local_address",
            "remote_address",
            "authenticated_local_peer_id",
            "authenticated_remote_peer_id",
            "stream_trace_id",
        ] {
            let mut changed = raw.clone();
            changed["applications"]["attempts"][1]["binding"][key] = if key == "stream_trace_id" {
                json!(19)
            } else {
                json!("different")
            };
            assert!(require_identify_echo_pair(&mut changed).is_err(), "{key}");
            assert_eq!(changed["complete"], false);
        }
        raw["fixture_owned_tasks_joined"] = json!(false);
        assert!(require_identify_echo_pair(&mut raw).is_err());
    }

    #[test]
    fn application_failed_identify_never_polls_echo_or_changes_behaviour_flag() {
        for verified in [false, true] {
            let mut result = json!({"signed_peer_record": false, "protocol_count": 4});
            result["raw_identify_exchange"] = json!({"status": if verified { "verified" } else { "failed" },
                "signed_peer_record_verified": verified, "raw_protobuf_hex": "retained-test-capture"});
            let called = std::cell::Cell::new(false);
            let outcome = futures::executor::block_on(after_verified_identify(
                &result["raw_identify_exchange"],
                async {
                    called.set(true);
                    Ok::<_, Box<dyn std::error::Error>>(42)
                },
            ));
            assert_eq!(called.get(), verified);
            assert_eq!(outcome.is_ok(), verified);
            assert_eq!(result["signed_peer_record"], false);
            assert_eq!(
                result["raw_identify_exchange"]["raw_protobuf_hex"],
                "retained-test-capture"
            );
        }
    }

    #[test]
    fn application_attempt_cancel_and_bounds_remain_failed_history() {
        let observer = Observer::default();
        for _ in 0..=LIMIT {
            drop(observer.begin(PeerId::random(), IDENTIFY, &json!({"connections": []})));
        }
        let snapshot = observer.snapshot();
        assert_eq!(snapshot["overflow"], true);
        assert_eq!(snapshot["attempts"].as_array().unwrap().len(), LIMIT);
        assert_eq!(snapshot["attempts"][0]["attempt_ended"], true);
        assert_eq!(snapshot["attempts"][0]["application_io_complete"], false);
        assert_eq!(snapshot["attempts"][0]["stream_drop_returned"], false);
    }
}
