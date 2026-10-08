//! Original-donor public-boundary probe, not PubSub acceptance evidence.
//! Setup follows rust-libp2p 22fb4c784fc55ad8b15d05fdc9f98d663107d4cb
//! transports/quic/tests/{smoke,stream_compliance}.rs. No observer is installed.

use std::{error::Error, io, panic::AssertUnwindSafe, pin::Pin, time::Duration};

use futures::{
    AsyncReadExt, AsyncWriteExt, FutureExt,
    future::{Either, join, poll_fn, select},
};
use libp2p::{
    Multiaddr, PeerId, Transport,
    core::{
        Endpoint,
        muxing::StreamMuxerExt,
        transport::{DialOpts, ListenerId, PortUse, TransportEvent},
    },
    identity, quic,
};
use serde_json::json;
use tokio::time::timeout;

const PROBE_TIMEOUT: Duration = Duration::from_secs(8);
const CLEANUP_TIMEOUT: Duration = Duration::from_secs(3);

#[derive(Clone, Copy, Debug)]
enum Order {
    LocalFirst,
    RemoteFirst,
    BothCloseBeforeObservation,
}

struct Peer {
    role: &'static str,
    id: PeerId,
    remote: Option<PeerId>,
    transport: quic::tokio::Transport,
    listener: ListenerId,
    listening: bool,
    connection: Option<quic::Connection>,
    stream: Option<quic::Stream>,
    close_returned: bool,
}

impl Peer {
    fn new(role: &'static str) -> Self {
        let key = identity::Keypair::generate_ed25519();
        let id = key.public().to_peer_id();
        let mut config = quic::Config::new(&key);
        config.handshake_timeout = Duration::from_secs(2);
        Self {
            role,
            id,
            remote: None,
            transport: quic::tokio::Transport::new(config),
            listener: ListenerId::next(),
            listening: false,
            connection: None,
            stream: None,
            close_returned: false,
        }
    }

    async fn listen(&mut self) -> Multiaddr {
        self.transport
            .listen_on(
                self.listener,
                "/ip4/127.0.0.1/udp/0/quic-v1".parse().unwrap(),
            )
            .expect("native listen_on");
        self.listening = true;
        match poll_fn(|cx| Pin::new(&mut self.transport).poll(cx)).await {
            TransportEvent::NewAddress {
                listener_id,
                listen_addr,
            } => {
                assert_eq!(listener_id, self.listener);
                listen_addr
            }
            event => panic!("unexpected listen event: {event:?}"),
        }
    }

    async fn close_connection(&mut self, order: Order) -> Result<(), quic::Error> {
        let connection = self.connection.as_mut().expect("authenticated connection");
        let result = poll_fn(|cx| {
            assert!(connection.poll_unpin(cx).is_pending());
            connection.poll_close_unpin(cx)
        })
        .await;
        self.close_returned = true;
        eprintln!(
            "{}",
            json!({
                "probe": "original_quic_shutdown", "schedule": format!("{order:?}"),
                "operation": "muxer_close", "role": self.role,
                "local_peer": self.id.to_string(), "authenticated_remote": self.remote.map(|p| p.to_string()),
                "ok": result.is_ok(), "error": result.as_ref().err().map(error_text),
                "public_quinn_cause": result.as_ref().err().and_then(|e| public_cause(e)).map(|e| format!("{e:?}")),
            })
        );
        result
    }

    async fn observe_terminal(&mut self, order: Order, locally_closed: Option<bool>) {
        // Both handles are retained from this one authenticated native upgrade.
        // A stream cause is reported separately; it never reclassifies the muxer error.
        self.read_terminal(order, locally_closed).await;
        let connection = self.connection.as_mut().unwrap();
        let inbound = poll_fn(|cx| {
            assert!(connection.poll_unpin(cx).is_pending());
            connection.poll_inbound_unpin(cx)
        })
        .await;
        eprintln!(
            "{}",
            json!({
                "probe": "original_quic_shutdown", "schedule": format!("{order:?}"),
                "operation": "muxer_inbound_after_read", "role": self.role,
                "local_peer": self.id.to_string(), "authenticated_remote": self.remote.map(|p| p.to_string()),
                "ok": inbound.is_ok(), "error": inbound.as_ref().err().map(error_text),
                "connection_variant": matches!(&inbound, Err(quic::Error::Connection(_))),
                "public_quinn_cause": inbound.as_ref().err().and_then(|e| public_cause(e)).map(|e| format!("{e:?}")),
            })
        );
        assert!(matches!(&inbound, Err(quic::Error::Connection(_))));
        assert!(
            public_cause(inbound.as_ref().err().unwrap()).is_none(),
            "original muxer wrapper must not borrow this or another stream's typed cause"
        );
    }

    async fn read_terminal(&mut self, order: Order, locally_closed: Option<bool>) {
        let mut byte = [0];
        let read = self.stream.as_mut().unwrap().read(&mut byte).await;
        let direct_read = read
            .as_ref()
            .err()
            .and_then(io::Error::get_ref)
            .and_then(|e| e.downcast_ref::<quinn::ReadError>());
        let cause = match direct_read {
            Some(quinn::ReadError::ConnectionLost(cause)) => Some(cause),
            _ => None,
        };
        eprintln!(
            "{}",
            json!({
                "probe": "original_quic_shutdown", "schedule": format!("{order:?}"),
                "operation": "stream_read", "role": self.role, "local_peer": self.id.to_string(),
                "authenticated_remote": self.remote.map(|p| p.to_string()),
                "ownership": "stream_returned_by_this_retained_native_connection",
                "own_close_returned": self.close_returned,
                "n": read.as_ref().ok(), "error": read.as_ref().err().map(error_text),
                "kind": read.as_ref().err().map(|e| format!("{:?}", e.kind())),
                "raw_os_error": read.as_ref().err().and_then(io::Error::raw_os_error),
                "direct_quinn_read_error": direct_read.map(|e| format!("{e:?}")),
                "typed_connection_cause": cause.map(|e| format!("{e:?}")),
            })
        );
        let cause = cause.expect("original stream must expose direct ReadError::ConnectionLost");
        match locally_closed {
            Some(true) => assert_eq!(cause, &quinn::ConnectionError::LocallyClosed),
            Some(false) => assert!(matches!(cause,
                quinn::ConnectionError::ApplicationClosed(close) if close.error_code.into_inner() == 0
            )),
            None => assert!(
                matches!(cause, quinn::ConnectionError::LocallyClosed)
                    || matches!(cause, quinn::ConnectionError::ApplicationClosed(close) if close.error_code.into_inner() == 0)
            ),
        }
        let read_error = read.as_ref().unwrap_err();
        assert_eq!(read_error.kind(), io::ErrorKind::NotConnected);
        assert_eq!(read_error.raw_os_error(), None);
        assert_eq!(public_cause(read_error), Some(cause));
    }

    async fn cleanup(&mut self, order: Order) {
        if self.connection.is_some() && !self.close_returned {
            check_close(self.close_connection(order).await);
        }
        if let Some(stream) = self.stream.as_mut() {
            let result = stream.close().await;
            eprintln!(
                "{}",
                json!({
                    "probe": "original_quic_shutdown", "schedule": format!("{order:?}"),
                    "operation": "stream_close", "role": self.role,
                    "ok": result.is_ok(), "error": result.as_ref().err().map(error_text),
                })
            );
        }
        drop(self.stream.take());
        drop(self.connection.take());
        if self.listening {
            assert!(self.transport.remove_listener(self.listener));
            match poll_fn(|cx| Pin::new(&mut self.transport).poll(cx)).await {
                TransportEvent::ListenerClosed {
                    listener_id,
                    reason,
                } => {
                    assert_eq!(listener_id, self.listener);
                    reason.expect("native listener closure");
                }
                event => panic!("unexpected listener closure: {event:?}"),
            }
            self.listening = false;
        }
    }
}

fn error_text<E: Error>(error: &E) -> String {
    error.to_string().chars().take(512).collect()
}

fn public_cause<'a>(mut error: &'a (dyn Error + 'static)) -> Option<&'a quinn::ConnectionError> {
    for _ in 0..8 {
        if let Some(cause) = error.downcast_ref::<quinn::ConnectionError>() {
            return Some(cause);
        }
        error = match error
            .downcast_ref::<io::Error>()
            .and_then(io::Error::get_ref)
        {
            Some(inner) => inner,
            None => error.source()?,
        };
    }
    panic!("public native error chain exceeds probe bound");
}

fn check_close(result: Result<(), quic::Error>) {
    // Remote/simultaneous poll_close is an observation, not an Ok-only premise.
    if let Err(error) = result {
        assert!(matches!(&error, quic::Error::Connection(_)), "{error:?}");
        assert!(public_cause(&error).is_none());
    }
}

async fn connect_and_exchange(local: &mut Peer, remote: &mut Peer) {
    let (_, remote_address) = join(local.listen(), remote.listen()).await;
    let dial = local
        .transport
        .dial(
            remote_address,
            DialOpts {
                role: Endpoint::Dialer,
                port_use: PortUse::Reuse,
            },
        )
        .expect("native dial");
    let ((), ()) = join(
        async {
            let output =
                match select(dial, poll_fn(|cx| Pin::new(&mut local.transport).poll(cx))).await {
                    Either::Left((output, _)) => output.expect("native outbound upgrade"),
                    Either::Right((event, _)) => panic!("unexpected dial event: {event:?}"),
                };
            local.remote = Some(output.0);
            local.connection = Some(output.1);
        },
        async {
            let event = poll_fn(|cx| Pin::new(&mut remote.transport).poll(cx)).await;
            let (upgrade, _) = event.into_incoming().expect("native inbound upgrade");
            let (peer, connection) = upgrade.await.expect("authenticated native inbound");
            remote.remote = Some(peer);
            remote.connection = Some(connection);
        },
    )
    .await;
    assert_ne!(local.id, remote.id);
    assert_eq!(local.remote, Some(remote.id));
    assert_eq!(remote.remote, Some(local.id));

    let connection = local.connection.as_mut().unwrap();
    local.stream = Some(
        poll_fn(|cx| {
            assert!(connection.poll_unpin(cx).is_pending());
            connection.poll_outbound_unpin(cx)
        })
        .await
        .expect("native bidirectional stream"),
    );
    // As in the donor helper, a real write makes the stream visible to accept_bi.
    local
        .stream
        .as_mut()
        .unwrap()
        .write_all(b"probe-request")
        .await
        .unwrap();
    let connection = remote.connection.as_mut().unwrap();
    remote.stream = Some(
        poll_fn(|cx| {
            assert!(connection.poll_unpin(cx).is_pending());
            connection.poll_inbound_unpin(cx)
        })
        .await
        .expect("native inbound stream"),
    );
    let mut request = [0; 13];
    remote
        .stream
        .as_mut()
        .unwrap()
        .read_exact(&mut request)
        .await
        .unwrap();
    assert_eq!(&request, b"probe-request");
    remote
        .stream
        .as_mut()
        .unwrap()
        .write_all(b"probe-response")
        .await
        .unwrap();
    let mut response = [0; 14];
    local
        .stream
        .as_mut()
        .unwrap()
        .read_exact(&mut response)
        .await
        .unwrap();
    assert_eq!(&response, b"probe-response");
}

async fn probe(order: Order) {
    let mut local = Peer::new("dialer");
    let mut remote = Peer::new("listener");
    // All probe futures are scoped and joined; no test-owned task is detached.
    let outcome = AssertUnwindSafe(timeout(PROBE_TIMEOUT, async {
        connect_and_exchange(&mut local, &mut remote).await;
        match order {
            Order::LocalFirst => {
                local
                    .close_connection(order)
                    .await
                    .expect("healthy local close");
                join(
                    local.observe_terminal(order, Some(true)),
                    remote.observe_terminal(order, Some(false)),
                )
                .await;
                check_close(remote.close_connection(order).await);
                remote.read_terminal(order, Some(true)).await;
            }
            Order::RemoteFirst => {
                remote
                    .close_connection(order)
                    .await
                    .expect("healthy remote close");
                join(
                    local.observe_terminal(order, Some(false)),
                    remote.observe_terminal(order, Some(true)),
                )
                .await;
                check_close(local.close_connection(order).await);
                local.read_terminal(order, Some(true)).await;
            }
            Order::BothCloseBeforeObservation => {
                let (left, right) = join(
                    local.close_connection(order),
                    remote.close_connection(order),
                )
                .await;
                check_close(left);
                check_close(right);
                join(
                    local.observe_terminal(order, None),
                    remote.observe_terminal(order, None),
                )
                .await;
            }
        }
    }))
    .catch_unwind()
    .await;

    // Cleanup also runs after a probe assertion or timeout. Each peer has its own
    // bounded cleanup so one failure cannot cancel the other peer's cleanup.
    let (left_cleanup, right_cleanup) = join(
        AssertUnwindSafe(timeout(CLEANUP_TIMEOUT, local.cleanup(order))).catch_unwind(),
        AssertUnwindSafe(timeout(CLEANUP_TIMEOUT, remote.cleanup(order))).catch_unwind(),
    )
    .await;
    let owners_released = local.stream.is_none()
        && local.connection.is_none()
        && !local.listening
        && remote.stream.is_none()
        && remote.connection.is_none()
        && !remote.listening;
    // GenTransport exposes no Quinn Endpoint::wait_idle or internal task handles.
    // This joins test-owned futures and drops both transports, not Quinn internals.
    drop(local);
    drop(remote);
    let cleanup_ok = matches!(&left_cleanup, Ok(Ok(())))
        && matches!(&right_cleanup, Ok(Ok(())))
        && owners_released;
    eprintln!(
        "{}",
        json!({
            "probe": "original_quic_shutdown", "schedule": format!("{order:?}"),
            "operation": "test_owner_join", "ok": cleanup_ok,
            "streams_and_connections_released": owners_released,
            "test_tasks_spawned": 0, "transports_dropped": 2,
            "quinn_internal_tasks_joined": false,
        })
    );
    assert!(cleanup_ok, "bounded native/test-owner cleanup failed");
    match outcome {
        Ok(Ok(())) => {}
        Ok(Err(error)) => panic!("native shutdown probe timed out: {error}"),
        Err(panic) => std::panic::resume_unwind(panic),
    }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn original_quic_local_first_shutdown() {
    probe(Order::LocalFirst).await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn original_quic_remote_first_shutdown() {
    probe(Order::RemoteFirst).await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn original_quic_both_close_before_observation() {
    probe(Order::BothCloseBeforeObservation).await;
}
