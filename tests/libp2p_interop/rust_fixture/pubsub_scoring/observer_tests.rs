use super::*;
use futures::{AsyncReadExt, AsyncWriteExt, task::noop_waker};
use libp2p::core::{Endpoint, transport::PortUse};

// Synthetic observer units exercise the live wrapper code, but establish no live donor acceptance.

fn framed(body: &[u8]) -> Vec<u8> {
    let mut length = body.len();
    let mut out = Vec::new();
    while length >= 128 {
        out.push((length as u8 & 127) | 128);
        length >>= 7;
    }
    out.push(length as u8);
    out.extend_from_slice(body);
    out
}

fn point() -> ConnectedPoint {
    ConnectedPoint::Dialer {
        address: "/ip4/127.0.0.1/tcp/41000".parse().unwrap(),
        role_override: Endpoint::Dialer,
        port_use: PortUse::Reuse,
    }
}

fn observed() -> (Evidence, usize, WireStream) {
    observed_role(true)
}

fn observed_role(outbound: bool) -> (Evidence, usize, WireStream) {
    let evidence = Evidence::default();
    let peer = PeerId::random();
    let id = evidence
        .connection(peer, point(), NativeStack::NoiseYamux)
        .unwrap();
    evidence
        .bind(peer, &point(), ConnectionId::new_unchecked(7))
        .unwrap();
    let wire = WireStream::new(evidence.clone(), Some(id), outbound);
    (evidence, id, wire)
}

fn negotiate(wire: &mut WireStream, protocol: &str) {
    for side in [wire.proposal_direction, 1 - wire.proposal_direction] {
        for byte in framed(b"/multistream/1.0.0\n")
            .into_iter()
            .chain(framed(format!("{protocol}\n").as_bytes()))
        {
            wire.feed(side, &[byte]);
        }
    }
}

fn prepare(evidence: &Evidence) {
    evidence
        .prepare(1, "victim", &"a".repeat(32), PeerId::random(), 0)
        .unwrap();
}

#[test]
fn actual_yamux_terminal_state_requires_prepared_authenticated_muxer_and_clean_capture() {
    use crate::upgrade_observer::yamux_error::{
        self,
        tests::{Scenario, native_error},
    };
    for version012 in [false, true] {
        for case in 0..15 {
            let (evidence, id, wire) = observed();
            if !matches!(case, 1 | 11 | 12) {
                prepare(&evidence);
            }
            if case == 2 {
                evidence.lock().fail("earlier actual failure");
            }
            if case == 3 {
                evidence.lock().connections[id].stack = NativeStack::Quic;
            }
            if case == 4 {
                evidence.lock().connections[id].native_id = None;
            }
            if case == 9 {
                evidence.lock().overflow = true;
            }
            if case == 10 {
                evidence.lock().connections[id].stack = NativeStack::PnetNoiseYamux;
            }
            let ack_sequence = evidence
                .lock()
                .events
                .iter()
                .find(|event| event["kind"] == "shutdown_prepared")
                .map(|event| event["sequence"].clone());
            let scenario = match case {
                7 | 11 => Scenario::Io,
                8 | 12 => Scenario::Decode,
                13 => Scenario::IoContainingClosed,
                14 => Scenario::IoContainingMarkedClosed,
                _ => Scenario::Closed,
            };
            let error = yamux_error::convert(native_error(version012, scenario));
            let expected = error.to_string();
            evidence.lock().native_error(
                if case == 5 { None } else { Some(id) },
                if case == 6 { Some(wire.stream) } else { None },
                "muxer_inbound",
                &error,
            );
            let capture = evidence.lock();
            let accepted = matches!(case, 0 | 10);
            assert_eq!(capture.error.is_none(), accepted, "case {case}");
            let terminal = capture
                .events
                .iter()
                .find(|event| event["kind"] == "native_terminal_state");
            assert_eq!(terminal.is_some(), accepted);
            if let Some(event) = terminal {
                assert_eq!(
                    event["typed_cause"],
                    yamux_error::terminal_state(&error).unwrap()
                );
                assert_eq!(event["error_boundary"], yamux_error::BOUNDARY);
                assert_eq!(event["closure_reason"], "unknown");
                assert_eq!(event["prepare_ack_sequence"], ack_sequence.clone().unwrap());
                assert_eq!(event["message"], expected);
                assert!(!capture.connections[id].closed);
                assert!(!capture.connections[id].dropped);
                assert_eq!(capture.live_muxers, 1);
            }
            if case == 2 {
                assert_eq!(capture.error.as_deref(), Some("earlier actual failure"));
            }
            drop(capture);
            if matches!(case, 1 | 11 | 12) {
                assert!(
                    evidence
                        .prepare(1, "victim", &"a".repeat(32), PeerId::random(), 0)
                        .is_err()
                );
                assert!(!evidence.lock().prepared);
            }
            drop(wire);
        }
    }
}

#[test]
fn typed_yamux_closed_never_completes_partial_rpc_or_changes_native_result() {
    use crate::upgrade_observer::yamux_error::{
        self,
        tests::{Scenario, native},
    };
    for version012 in [false, true] {
        let (evidence, id, mut wire) = observed();
        negotiate(&mut wire, "/meshsub/1.1.0");
        wire.feed(0, &[3, 1]);
        prepare(&evidence);
        let mut muxer = ObservedMuxer {
            inner: yamux_error::boxed(native(version012, Scenario::Closed), true),
            evidence: evidence.clone(),
            connection: Some(id),
        };
        let mut baseline = yamux_error::boxed(native(version012, Scenario::Closed), true);
        let waker = noop_waker();
        let mut cx = Context::from_waker(&waker);
        assert!(matches!(
            Pin::new(&mut muxer).poll_close(&mut cx),
            Poll::Ready(Ok(()))
        ));
        assert!(matches!(
            Pin::new(&mut baseline).poll_close(&mut cx),
            Poll::Ready(Ok(()))
        ));
        let Poll::Ready(Err(expected)) = Pin::new(&mut baseline).poll_inbound(&mut cx) else {
            panic!("missing native Closed")
        };
        let Poll::Ready(Err(actual)) = Pin::new(&mut muxer).poll_inbound(&mut cx) else {
            panic!("observer hid native Closed")
        };
        assert_eq!(actual.kind(), expected.kind());
        assert_eq!(actual.raw_os_error(), expected.raw_os_error());
        assert_eq!(actual.to_string(), expected.to_string());
        assert_eq!(
            yamux_error::terminal_state(&actual),
            yamux_error::terminal_state(&expected)
        );
        assert!(evidence.lock().error.is_none());
        drop(wire);
        assert!(
            evidence.lock().error.is_some(),
            "typed Closed hid partial framing"
        );
        drop(muxer);
        assert_eq!(evidence.lock().live_streams, 0);
        assert_eq!(evidence.lock().live_muxers, 0);
    }
}

#[test]
fn prepare_closes_command_admission_and_cannot_clear_pending_or_sticky_errors() {
    for failure in 0..3 {
        let evidence = Evidence::default();
        if failure == 0 {
            evidence.lock().fail("active native failure");
        }
        if failure == 1 {
            evidence.lock().overflow = true;
        }
        assert!(
            evidence
                .prepare(
                    1,
                    "victim",
                    &"a".repeat(32),
                    PeerId::random(),
                    usize::from(failure == 2)
                )
                .is_err()
        );
        assert!(!evidence.lock().prepared);
        assert!(
            !evidence
                .lock()
                .events
                .iter()
                .any(|event| event["kind"] == "shutdown_prepared")
        );
        if failure == 0 {
            assert_eq!(
                evidence.lock().error.as_deref(),
                Some("active native failure")
            );
        }
    }
    let evidence = Evidence::default();
    prepare(&evidence);
    assert!(evidence.admit().is_err());
    assert!(
        evidence
            .prepare(2, "victim", &"a".repeat(32), PeerId::random(), 0)
            .is_err()
    );
    assert_eq!(evidence.lock().events.len(), 1);
}

#[test]
fn malformed_wrong_ack_partial_and_overflow_are_fatal_after_prepare() {
    for failure in 0..4 {
        let (evidence, _, mut wire) = observed();
        prepare(&evidence);
        if failure == 0 {
            wire.feed(1, &framed(b"/multistream/1.0.0\n"));
            wire.feed(1, &framed(b"/meshsub/1.1.0\n"));
            wire.feed(0, &framed(b"/multistream/1.0.0\n"));
            wire.feed(0, &framed(b"/meshsub/1.0.0\n"));
        } else {
            negotiate(&mut wire, "/meshsub/1.1.0");
            match failure {
                1 => wire.feed(0, &framed(&[0xff])),
                2 => wire.feed(0, &[2, 0x0a]),
                _ => wire.feed(0, &[0x80; 4]),
            }
        }
        drop(wire);
        assert!(
            evidence.lock().error.is_some(),
            "failure {failure} was suppressed after prepare"
        );
    }
}

fn normal_read_error() -> io::Error {
    io::Error::new(
        io::ErrorKind::NotConnected,
        quinn::ReadError::ConnectionLost(quinn::ConnectionError::ApplicationClosed(
            quinn::ApplicationClose {
                error_code: 0u32.into(),
                reason: Vec::new().into(),
            },
        )),
    )
}

#[tokio::test]
async fn actual_quic_standard_source_is_visible_only_in_diagnostic_copy() {
    use futures::future::{Either, join, poll_fn, select};
    use libp2p::{
        Transport,
        core::transport::{DialOpts, ListenerId, TransportEvent},
        identity,
    };

    tokio::time::timeout(std::time::Duration::from_secs(5), async {
        let listener_key = identity::Keypair::generate_ed25519();
        let dialer_key = identity::Keypair::generate_ed25519();
        let mut listener =
            libp2p::quic::tokio::Transport::new(libp2p::quic::Config::new(&listener_key));
        let mut dialer =
            libp2p::quic::tokio::Transport::new(libp2p::quic::Config::new(&dialer_key));
        listener
            .listen_on(
                ListenerId::next(),
                "/ip4/127.0.0.1/udp/0/quic-v1".parse().unwrap(),
            )
            .unwrap();
        let TransportEvent::NewAddress { listen_addr, .. } =
            poll_fn(|cx| Pin::new(&mut listener).poll(cx)).await
        else {
            panic!("missing actual QUIC listen address")
        };
        let dial = dialer
            .dial(
                listen_addr.clone(),
                DialOpts {
                    role: Endpoint::Dialer,
                    port_use: PortUse::Reuse,
                },
            )
            .unwrap();
        let (incoming, outgoing) = join(
            async {
                let TransportEvent::Incoming {
                    upgrade,
                    local_addr,
                    send_back_addr,
                    ..
                } = poll_fn(|cx| Pin::new(&mut listener).poll(cx)).await
                else {
                    panic!("missing actual QUIC incoming connection")
                };
                (
                    upgrade.await.unwrap(),
                    ConnectedPoint::Listener {
                        local_addr,
                        send_back_addr,
                    },
                )
            },
            async {
                match select(dial, poll_fn(|cx| Pin::new(&mut dialer).poll(cx))).await {
                    Either::Left((result, _)) => result.unwrap(),
                    Either::Right(_) => panic!("unexpected dialer transport event"),
                }
            },
        )
        .await;
        let ((listener_peer, mut listener_connection), listener_point) = incoming;
        let (dialer_peer, mut dialer_connection) = outgoing;
        assert_eq!(listener_peer, dialer_key.public().to_peer_id());
        assert_eq!(dialer_peer, listener_key.public().to_peer_id());
        poll_fn(|cx| Pin::new(&mut listener_connection).poll_close(cx))
            .await
            .unwrap();
        let local = poll_fn(|cx| Pin::new(&mut listener_connection).poll_inbound(cx))
            .await
            .err()
            .expect("missing native LocallyClosed result");
        let remote = poll_fn(|cx| Pin::new(&mut dialer_connection).poll_inbound(cx))
            .await
            .err()
            .expect("missing native ApplicationClosed result");
        let dialer_point = ConnectedPoint::Dialer {
            address: listen_addr,
            role_override: Endpoint::Dialer,
            port_use: PortUse::Reuse,
        };
        for (peer, point, error, cause) in [
            (listener_peer, listener_point, local, "quinn_locally_closed"),
            (
                dialer_peer,
                dialer_point,
                remote,
                "quinn_application_closed_0",
            ),
        ] {
            assert!(matches!(error, libp2p::quic::Error::Connection(_)));
            let libp2p::quic::Error::Connection(wrapper) = &error else {
                unreachable!()
            };
            let wrapper_source = wrapper
                .source()
                .and_then(|source| source.downcast_ref::<quinn::ConnectionError>());
            let outer_source = error
                .source()
                .and_then(|source| source.downcast_ref::<quinn::ConnectionError>());
            if QUIC_CAUSE_OBSERVER_ENABLED {
                let source = wrapper_source.expect("copy must expose the actual standard source");
                assert!(std::ptr::eq(
                    source,
                    outer_source.expect("outer standard source")
                ));
                assert_eq!(connection_cause(source), (cause, true));
                assert_eq!(error.to_string(), source.to_string());
                assert_eq!(format!("{error:>80}"), format!("{source:>80}"));
                assert_eq!(format!("{error:.3}"), format!("{source:.3}"));
                assert_eq!(format!("{error:#}"), format!("{source:#}"));
            } else {
                assert!(wrapper_source.is_none());
                assert!(outer_source.is_none());
            }
            let error = io::Error::other(error);
            let expected = (error.kind(), error.raw_os_error(), error.to_string());
            assert_eq!(
                native_cause(&error),
                if QUIC_CAUSE_OBSERVER_ENABLED {
                    (cause, true)
                } else {
                    ("quic_connection_cause_unavailable", false)
                }
            );
            for (prepared, prior_failure) in [(false, false), (true, false), (true, true)] {
                let evidence = Evidence::default();
                let id = evidence
                    .connection(peer, point.clone(), NativeStack::Quic)
                    .unwrap();
                evidence
                    .bind(peer, &point, ConnectionId::new_unchecked(7))
                    .unwrap();
                if prepared {
                    prepare(&evidence);
                }
                if prior_failure {
                    evidence.lock().fail("earlier native failure");
                }
                evidence
                    .lock()
                    .native_error(Some(id), None, "muxer_inbound", &error);
                let capture = evidence.lock();
                let accepted = prepared && QUIC_CAUSE_OBSERVER_ENABLED;
                assert_eq!(capture.error.is_none(), accepted && !prior_failure);
                if prior_failure {
                    assert_eq!(capture.error.as_deref(), Some("earlier native failure"));
                }
                let record = capture.events.last().unwrap();
                assert_eq!(
                    record["kind"],
                    if accepted {
                        "expected_native_close"
                    } else {
                        "native_io_error"
                    }
                );
                assert_eq!(record["prepared"], prepared);
                assert_eq!(record["operation"], "muxer_inbound");
                assert_eq!(record["message"], expected.2);
                assert!(!capture.connections[id].closed);
                assert!(!capture.connections[id].dropped);
                assert_eq!(capture.live_muxers, 1);
                assert_eq!(
                    (error.kind(), error.raw_os_error(), error.to_string()),
                    expected
                );
            }
            let mut deep = error;
            for _ in 0..8 {
                deep = io::Error::other(deep);
            }
            assert_eq!(native_cause(&deep), ("source_chain_limit", false));
        }
        poll_fn(|cx| Pin::new(&mut dialer_connection).poll_close(cx))
            .await
            .unwrap();
    })
    .await
    .expect("bounded native QUIC pair timed out");
}

#[test]
fn current_typed_quic_causes_require_prepare_and_never_hide_nonzero_or_sticky_failure() {
    for prepared in [false, true] {
        for case in 0..7 {
            let (evidence, id, wire) = observed();
            evidence.lock().connections[id].stack = NativeStack::Quic;
            if prepared {
                prepare(&evidence);
            }
            let cause = match case {
                0 | 2 => quinn::ConnectionError::ApplicationClosed(quinn::ApplicationClose {
                    error_code: if case == 0 { 0u32.into() } else { 1u32.into() },
                    reason: b"connection lost; closed by peer: 0".to_vec().into(),
                }),
                1 => quinn::ConnectionError::LocallyClosed,
                3 => quinn::ConnectionError::Reset,
                4 => quinn::ConnectionError::TimedOut,
                5 => quinn::ConnectionError::TransportError(
                    quinn::TransportErrorCode::PROTOCOL_VIOLATION.into(),
                ),
                _ => quinn::ConnectionError::LocallyClosed,
            };
            if case == 6 {
                evidence.lock().fail("earlier native error");
            }
            let error = io::Error::new(
                io::ErrorKind::NotConnected,
                quinn::ReadError::ConnectionLost(cause),
            );
            let original = (error.kind(), error.raw_os_error(), error.to_string());
            evidence
                .lock()
                .native_error(Some(id), Some(wire.stream), "stream_read", &error);
            assert_eq!(evidence.lock().error.is_none(), prepared && case < 2);
            assert_eq!(
                (error.kind(), error.raw_os_error(), error.to_string()),
                original
            );
            if case == 6 {
                assert_eq!(
                    evidence.lock().error.as_deref(),
                    Some("earlier native error")
                );
            }
            if !prepared {
                assert!(
                    evidence
                        .prepare(2, "victim", &"a".repeat(32), PeerId::random(), 0)
                        .is_err()
                );
                assert!(!evidence.lock().prepared);
            }
            drop(wire);
        }
    }
}

#[test]
fn public_quinn_and_native_transparent_source_chains_are_bounded_and_typed() {
    assert_eq!(
        native_cause(&normal_read_error()),
        ("quinn_application_closed_0", true)
    );
    let nested = io::Error::other(io::Error::other(normal_read_error()));
    assert_eq!(native_cause(&nested), ("quinn_application_closed_0", true));
    let transparent = io::Error::other(libp2p::quic::Error::Io(normal_read_error()));
    assert_eq!(
        native_cause(&transparent),
        ("quinn_application_closed_0", true)
    );
    // Transparent source() skips its direct inner error: a diagnostic alone is not a typed cause.
    let opaque = io::Error::other(libp2p::quic::Error::Io(io::Error::from(
        quinn::ConnectionError::LocallyClosed,
    )));
    assert_eq!(native_cause(&opaque), ("unclassified", false));
    let write = io::Error::new(
        io::ErrorKind::NotConnected,
        quinn::WriteError::ConnectionLost(quinn::ConnectionError::LocallyClosed),
    );
    assert_eq!(native_cause(&write), ("quinn_locally_closed", true));
    let mut deep = normal_read_error();
    for _ in 0..8 {
        deep = io::Error::other(deep);
    }
    assert_eq!(native_cause(&deep), ("source_chain_limit", false));
    for error in [
        quinn::ConnectionError::Reset,
        quinn::ConnectionError::TimedOut,
        quinn::ConnectionError::TransportError(
            quinn::TransportErrorCode::PROTOCOL_VIOLATION.into(),
        ),
        quinn::ConnectionError::ApplicationClosed(quinn::ApplicationClose {
            error_code: 1u32.into(),
            reason: Vec::new().into(),
        }),
    ] {
        let standard_source = io::Error::other(libp2p::quic::Error::Io(io::Error::other(
            quinn::ReadError::ConnectionLost(error.clone()),
        )));
        assert_eq!(native_cause(&standard_source), connection_cause(&error));
        assert!(!native_cause(&standard_source).1);
        assert!(
            !native_cause(&io::Error::new(
                io::ErrorKind::NotConnected,
                quinn::ReadError::ConnectionLost(error)
            ))
            .1
        );
    }
    assert!(
        !native_cause(&io::Error::new(
            io::ErrorKind::NotConnected,
            "connection lost; closed by peer: 0"
        ))
        .1
    );
}

#[tokio::test]
async fn native_actor_prepare_binds_actual_identity_and_rejects_late_commands() {
    use super::super::{Command, Config, Control, PendingConnect, command};
    use std::collections::{BTreeMap, BTreeSet};
    let config = Config {
        version: "1.1".to_owned(),
        transport: "tcp".to_owned(),
        actor: "victim".to_owned(),
        token: "a".repeat(32),
        ready: "unit.ready".into(),
        control: "unit.control".into(),
        result: "unit.result".into(),
        stop: "unit.stop".into(),
        store: "unit.store".into(),
        key_file: None,
        fingerprint: None,
    };
    for state in 0..5 {
        let evidence = Evidence::default();
        let tasks = crate::task_owner::Owner::default();
        let mut swarm = super::super::new_swarm(
            &config,
            &evidence,
            &crate::upgrade_observer::Observer::default(),
            &tasks,
        )
        .unwrap();
        let local = *swarm.local_peer_id();
        let mut pending = BTreeMap::new();
        if state == 3 {
            evidence.lock().fail("active native failure");
        }
        if state == 4 {
            pending.insert(
                PeerId::random(),
                PendingConnect {
                    sequence: 1,
                    started: Instant::now(),
                },
            );
        }
        let input = json!({"sequence": 1, "kind": "prepare_shutdown", "actor": if state == 0 { "sink" } else { "victim" },
            "case_token": if state == 1 { "b".repeat(32) } else { config.token.clone() },
            "local_peer_id": if state == 2 { PeerId::random() } else { local }.to_string()});
        let mut line = serde_json::to_vec(&input).unwrap();
        line.push(b'\n');
        let next = Control::default().ingest(&line).unwrap().pop().unwrap();
        assert!(
            command(
                next,
                &mut swarm,
                &config,
                &evidence,
                &BTreeSet::new(),
                &mut pending
            )
            .is_err()
        );
        assert!(!evidence.lock().prepared);
        assert!(
            !evidence
                .lock()
                .events
                .iter()
                .any(|event| event["kind"] == "shutdown_prepared")
        );
    }
    let evidence = Evidence::default();
    let tasks = crate::task_owner::Owner::default();
    let mut swarm = super::super::new_swarm(
        &config,
        &evidence,
        &crate::upgrade_observer::Observer::default(),
        &tasks,
    )
    .unwrap();
    let local = *swarm.local_peer_id();
    let mut pending = BTreeMap::new();
    command(
        Command::PrepareShutdown {
            sequence: 1,
            actor: config.actor.clone(),
            token: config.token.clone(),
            local,
        },
        &mut swarm,
        &config,
        &evidence,
        &BTreeSet::new(),
        &mut pending,
    )
    .unwrap();
    assert_eq!(
        evidence.lock().events[0]["local_peer_id"],
        local.to_string()
    );
    assert!(
        command(
            Command::Publish {
                sequence: 2,
                payload: "must-not-publish".to_owned()
            },
            &mut swarm,
            &config,
            &evidence,
            &BTreeSet::new(),
            &mut pending
        )
        .is_err()
    );
    assert!(
        !evidence
            .lock()
            .events
            .iter()
            .any(|event| event["kind"] == "publish")
    );
}

struct OneErrorIo(Option<io::Error>);

impl AsyncRead for OneErrorIo {
    fn poll_read(
        self: Pin<&mut Self>,
        _: &mut Context<'_>,
        _: &mut [u8],
    ) -> Poll<io::Result<usize>> {
        Poll::Ready(Err(self.get_mut().0.take().unwrap()))
    }
}
impl AsyncWrite for OneErrorIo {
    fn poll_write(self: Pin<&mut Self>, _: &mut Context<'_>, _: &[u8]) -> Poll<io::Result<usize>> {
        Poll::Ready(Err(self.get_mut().0.take().unwrap()))
    }
    fn poll_flush(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(Err(self.get_mut().0.take().unwrap()))
    }
    fn poll_close(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(Err(self.get_mut().0.take().unwrap()))
    }
}

fn quic_wire(outbound: bool) -> (Evidence, usize, WireStream) {
    let evidence = Evidence::default();
    let peer = PeerId::random();
    let point = ConnectedPoint::Dialer {
        address: "/ip4/127.0.0.1/udp/41000/quic-v1".parse().unwrap(),
        role_override: Endpoint::Dialer,
        port_use: PortUse::Reuse,
    };
    let id = evidence
        .connection(peer, point.clone(), NativeStack::Quic)
        .unwrap();
    evidence
        .bind(peer, &point, ConnectionId::new_unchecked(7))
        .unwrap();
    let wire = WireStream::new(evidence.clone(), Some(id), outbound);
    (evidence, id, wire)
}

fn reject_other(wire: &mut WireStream, protocol: &str) {
    let proposal = wire.proposal_direction;
    for (side, body) in [
        (proposal, b"/multistream/1.0.0\n".to_vec()),
        (proposal, format!("{protocol}\n").into_bytes()),
        (1 - proposal, b"/multistream/1.0.0\n".to_vec()),
        (1 - proposal, b"na\n".to_vec()),
    ] {
        for byte in framed(&body) {
            wire.feed(side, &[byte]);
        }
    }
}

#[test]
fn non_pubsub_na_direct_reset_is_diagnostic_and_returns_the_exact_native_error() {
    for prepared in [false, true] {
        for protocol in ["/ipfs/id/1.0.0", "/ipfs/ping/1.0.0", "/ipfs/id/push/1.0.0"] {
            let (evidence, id, mut wire) = quic_wire(false);
            reject_other(&mut wire, protocol);
            if prepared {
                prepare(&evidence);
            }
            let error = io::Error::from(quinn::ReadError::Reset(0u32.into()));
            let expected = (error.kind(), error.raw_os_error(), error.to_string());
            let identity = error
                .get_ref()
                .unwrap()
                .downcast_ref::<quinn::ReadError>()
                .unwrap() as *const _;
            let mut stream = ObservedIo {
                inner: OneErrorIo(Some(error)),
                wire,
            };
            let waker = noop_waker();
            let mut cx = Context::from_waker(&waker);
            let Poll::Ready(Err(actual)) = Pin::new(&mut stream).poll_read(&mut cx, &mut [0; 8])
            else {
                panic!("native Reset was hidden");
            };
            assert_eq!(
                (actual.kind(), actual.raw_os_error(), actual.to_string()),
                expected
            );
            assert_eq!(
                actual
                    .get_ref()
                    .unwrap()
                    .downcast_ref::<quinn::ReadError>()
                    .unwrap() as *const _,
                identity
            );
            let capture = evidence.lock();
            assert!(capture.error.is_none() && !capture.overflow);
            let diagnostic = capture.events.last().unwrap();
            assert_eq!(diagnostic["kind"], "non_pubsub_negotiation_failure");
            assert_eq!(diagnostic["connection_trace_id"], id + 1);
            assert_eq!(diagnostic["stream_trace_id"], 1);
            assert_eq!(diagnostic["rejected_protocol"], protocol);
            assert_eq!(diagnostic["prepared"], prepared);
            assert_eq!(diagnostic["typed_cause"], "quinn_read_reset_0");
            assert_eq!(diagnostic["error_code"], 0);
            assert_eq!(diagnostic["message"], actual.to_string());
            let refs = diagnostic["negotiation_frame_sequences"]
                .as_array()
                .unwrap();
            assert_eq!(refs.len(), 4);
            for reference in refs {
                let frame = &capture.events[reference.as_u64().unwrap() as usize - 1];
                assert_eq!(frame["kind"], "native_multistream_frame");
                assert_eq!(frame["stream_id"], diagnostic["stream_id"]);
                assert_eq!(frame["connection_id"], diagnostic["connection_id"]);
            }
            assert!(!capture.events.iter().any(|event| matches!(
                event["kind"].as_str(),
                Some("protocol" | "rpc" | "expected_native_close" | "native_io_error")
            )));
            assert_eq!(capture.live_streams, 1);
            assert_eq!(capture.live_muxers, 1);
            drop(capture);
            drop(stream);
            assert_eq!(evidence.lock().live_streams, 0);
            assert!(evidence.lock().error.is_none());
        }
    }
}

#[test]
fn non_pubsub_reset_requires_complete_same_owner_na_without_any_pubsub_or_parser_failure() {
    for mode in 0..23 {
        let (evidence, id, mut wire) = quic_wire(mode == 18);
        if matches!(mode, 0 | 1 | 19) {
            wire.feed(0, &framed(b"/multistream/1.0.0\n"));
            wire.feed(0, &framed(b"/ipfs/id/1.0.0\n"));
            if mode != 19 {
                wire.feed(1, &framed(b"/multistream/1.0.0\n"));
            }
            if mode == 1 {
                wire.feed(1, &[3, b'n']);
            }
            if mode == 19 {
                wire.feed(1, &framed(b"na\n"));
            }
        } else if mode == 22 {
            reject_other(&mut wire, "/meshsub/1.1.0");
            wire.feed(0, &framed(b"/ipfs/id/1.0.0\n"));
            wire.feed(1, &framed(b"na\n"));
        } else {
            reject_other(
                &mut wire,
                if mode == 2 {
                    "/meshsub/1.1.0"
                } else {
                    "/ipfs/id/1.0.0"
                },
            );
        }
        match mode {
            3 => wire.feed(0, &framed(b"/ipfs/ping/1.0.0\n")),
            4 => wire.feed(0, &[3, b'/']),
            5 => wire.pending_bytes = 1,
            6 => wire.pending.push((
                0,
                NativeFrame {
                    body: vec![0],
                    framed: vec![1, 0],
                },
            )),
            7 => wire.fail("earlier parser failure"),
            8 => evidence.lock().fail("earlier sticky I/O failure"),
            9 => evidence.lock().overflow = true,
            10 => evidence.lock().connections[id].native_id = None,
            11 => evidence.lock().connections[id].stack = NativeStack::NoiseYamux,
            12 => wire.stream += 1,
            13 => wire.ignored_bytes = true,
            20 => evidence.lock().connections[id].dropped = true,
            21 => evidence.lock().connections[id].swarm_id = None,
            _ => (),
        }
        let error = match mode {
            14 => io::Error::from(quinn::ReadError::Reset(7u32.into())),
            15 => io::Error::other(io::Error::from(quinn::ReadError::Reset(0u32.into()))),
            16 => io::Error::other("opaque reset 0"),
            17 => io::Error::from_raw_os_error(38),
            _ => io::Error::from(quinn::ReadError::Reset(0u32.into())),
        };
        let expected = (error.kind(), error.raw_os_error(), error.to_string());
        let mut stream = ObservedIo {
            inner: OneErrorIo(Some(error)),
            wire,
        };
        let waker = noop_waker();
        let mut cx = Context::from_waker(&waker);
        let Poll::Ready(Err(actual)) = Pin::new(&mut stream).poll_read(&mut cx, &mut [0; 8]) else {
            panic!("native error hidden in mode {mode}");
        };
        assert_eq!(
            (actual.kind(), actual.raw_os_error(), actual.to_string()),
            expected
        );
        let capture = evidence.lock();
        assert!(capture.error.is_some(), "mode {mode}");
        assert!(
            !capture
                .events
                .iter()
                .any(|event| event["kind"] == "non_pubsub_negotiation_failure"),
            "mode {mode}"
        );
        if mode == 8 {
            assert_eq!(capture.error.as_deref(), Some("earlier sticky I/O failure"));
        }
    }
}

#[test]
fn non_pubsub_read_reset_does_not_authorize_write_errors_or_later_wire_bytes() {
    for write_error in [false, true] {
        let (evidence, _, mut wire) = quic_wire(false);
        reject_other(&mut wire, "/ipfs/id/1.0.0");
        let mut stream = ObservedIo {
            inner: OneErrorIo(Some(io::Error::from(quinn::ReadError::Reset(0u32.into())))),
            wire,
        };
        let waker = noop_waker();
        let mut cx = Context::from_waker(&waker);
        if write_error {
            assert!(matches!(
                Pin::new(&mut stream).poll_write(&mut cx, b"x"),
                Poll::Ready(Err(_))
            ));
        } else {
            assert!(matches!(
                Pin::new(&mut stream).poll_read(&mut cx, &mut [0; 8]),
                Poll::Ready(Err(_))
            ));
            assert!(evidence.lock().error.is_none());
            stream.wire.feed(0, &[1, 0]);
        }
        assert!(evidence.lock().error.is_some());
    }
}

#[test]
fn prepared_typed_close_and_real_native_io_failures_return_original_results() {
    for prepared in [false, true] {
        for operation in 0..4 {
            for normal in [false, true] {
                let (evidence, id, mut wire) = observed();
                evidence.lock().connections[id].stack = NativeStack::Quic;
                negotiate(&mut wire, "/meshsub/1.1.0");
                if prepared {
                    prepare(&evidence);
                }
                let error = if normal {
                    normal_read_error()
                } else {
                    io::Error::from_raw_os_error(38)
                };
                let expected = (error.kind(), error.raw_os_error(), error.to_string());
                let mut stream = ObservedIo {
                    inner: OneErrorIo(Some(error)),
                    wire,
                };
                let waker = noop_waker();
                let mut cx = Context::from_waker(&waker);
                let mut bytes = [0; 4];
                let result = match operation {
                    0 => Pin::new(&mut stream)
                        .poll_read(&mut cx, &mut bytes)
                        .map_ok(|_| ()),
                    1 => Pin::new(&mut stream)
                        .poll_write(&mut cx, &bytes)
                        .map_ok(|_| ()),
                    2 => Pin::new(&mut stream).poll_flush(&mut cx),
                    _ => Pin::new(&mut stream).poll_close(&mut cx),
                };
                let Poll::Ready(Err(error)) = result else {
                    panic!("native error hidden");
                };
                assert_eq!(
                    (error.kind(), error.raw_os_error(), error.to_string()),
                    expected
                );
                if normal {
                    assert!(
                        error
                            .get_ref()
                            .unwrap()
                            .downcast_ref::<quinn::ReadError>()
                            .is_some()
                    );
                }
                drop(stream);
                if !prepared {
                    assert!(
                        evidence
                            .prepare(2, "victim", &"a".repeat(32), PeerId::random(), 0)
                            .is_err()
                    );
                    assert!(!evidence.lock().prepared);
                }
                let capture = evidence.lock();
                assert_eq!(capture.error.is_none(), normal && prepared);
                let record = capture
                    .events
                    .iter()
                    .find(|event| {
                        event["kind"]
                            == if normal && prepared {
                                "expected_native_close"
                            } else {
                                "native_io_error"
                            }
                    })
                    .unwrap();
                assert_eq!(record["connection_trace_id"], id + 1);
                assert_eq!(record["stream_trace_id"], 1);
                assert_eq!(record["prepared"], prepared);
                assert_eq!(
                    record["operation"],
                    [
                        "stream_read",
                        "stream_write",
                        "stream_flush",
                        "stream_close"
                    ][operation]
                );
            }
        }
    }
}

#[test]
fn prepared_normal_close_requires_quic_owner_and_never_clears_real_failure() {
    let (evidence, id, wire) = observed();
    prepare(&evidence);
    evidence.lock().native_error(
        Some(id),
        Some(wire.stream),
        "stream_read",
        &normal_read_error(),
    );
    assert!(
        evidence.lock().error.is_some(),
        "Quinn cause was accepted on a non-QUIC owner"
    );
    let failure = evidence.lock().error.clone();
    evidence.lock().connections[id].stack = NativeStack::Quic;
    evidence.lock().native_error(
        Some(id),
        Some(wire.stream),
        "stream_read",
        &normal_read_error(),
    );
    assert_eq!(evidence.lock().error, failure);
    assert!(
        evidence
            .lock()
            .events
            .iter()
            .any(|event| event["kind"] == "expected_native_close")
    );
    drop(wire);
}

#[test]
fn native_frames_are_byte_exact_bounded_and_never_completed_by_config() {
    let (evidence, _, mut wire) = observed();
    // Exact proto2 RPC.control.PRUNE(topic="t", backoff=0).
    let body = [0x1a, 7, 0x22, 5, 0x0a, 1, b't', 0x18, 0];
    let rpc = framed(&body);
    wire.feed(1, &framed(b"/multistream/1.0.0\n"));
    wire.feed(1, &framed(b"/meshsub/1.1.0\n"));
    wire.feed(1, &rpc);
    assert!(
        !evidence
            .lock()
            .events
            .iter()
            .any(|e| e["kind"] == "rpc" || e["kind"] == "protocol")
    );
    wire.feed(0, &framed(b"/multistream/1.0.0\n"));
    wire.feed(0, &framed(b"/meshsub/1.1.0\n"));
    let capture = evidence.lock();
    let event = capture.events.iter().find(|e| e["kind"] == "rpc").unwrap();
    assert_eq!(event["receipt"]["framed_hex"], hex(&rpc));
    assert_eq!(event["receipt"]["write"]["framed_bytes"], rpc.len());
    assert_eq!(
        event["receipt"]["write"]["framed_sha256"],
        format!("{:x}", Sha256::digest(&rpc))
    );
    assert_eq!(event["receipt"]["write"]["frames"], 1);
    assert_eq!(event["receipt"]["write"]["complete_frames"], true);
    assert_eq!(event["receipt"]["write"]["invalid_or_over_limit"], false);
    assert_eq!(event["peer_id"], capture.connections[0].peer.to_string());
    assert_eq!(event["connection_id"], "7");
    assert_eq!(event["stream_id"], "1:1");
    let prune = capture
        .events
        .iter()
        .find(|e| e["kind"] == "prune")
        .unwrap();
    assert_eq!(prune["backoff"], 0);
    assert!(prune.get("px").is_none());
    assert!(capture.error.is_none());
}

#[test]
fn prune_presence_and_raw_rpc_receipts_do_not_synthesize_missing_extensions() {
    let no_extensions = [0x1a, 5, 0x22, 3, 0x0a, 1, b't'];
    let zero_backoff = [0x1a, 7, 0x22, 5, 0x0a, 1, b't', 0x18, 0];
    for (protocol, body) in [
        ("/meshsub/1.0.0", no_extensions.as_slice()),
        ("/meshsub/1.1.0", zero_backoff.as_slice()),
    ] {
        let (evidence, _, mut wire) = observed();
        negotiate(&mut wire, protocol);
        let rpc = framed(body);
        for byte in &rpc {
            wire.feed(0, &[*byte]);
        }
        let capture = evidence.lock();
        let event = capture.events.iter().find(|e| e["kind"] == "rpc").unwrap();
        assert_eq!(event["direction"], "read");
        assert_eq!(event["receipt"]["framed_hex"], hex(&rpc));
        assert!(event["receipt"].get("write").is_none());
        let prune = capture
            .events
            .iter()
            .find(|e| e["kind"] == "prune")
            .unwrap();
        assert_eq!(prune.get("backoff").is_some(), protocol.ends_with("1.1.0"));
        assert!(prune.get("px").is_none());
    }
}

#[test]
fn native_owner_ambiguity_protocol_mismatch_partial_frames_and_overflow_fail_closed() {
    let evidence = Evidence::default();
    let peer = PeerId::random();
    evidence.connection(peer, point(), NativeStack::NoiseYamux);
    evidence.connection(peer, point(), NativeStack::NoiseYamux);
    assert!(
        evidence
            .bind(peer, &point(), ConnectionId::new_unchecked(9))
            .is_err()
    );
    let (evidence, _, mut wire) = observed();
    wire.feed(1, &framed(b"/multistream/1.0.0\n"));
    wire.feed(1, &framed(b"/meshsub/1.0.0\n"));
    wire.feed(0, &framed(b"/multistream/1.0.0\n"));
    wire.feed(0, &framed(b"/meshsub/1.1.0\n"));
    assert!(evidence.lock().error.is_some());
    assert!(
        !evidence
            .lock()
            .events
            .iter()
            .any(|e| e["kind"] == "protocol")
    );
    let (evidence, _, mut wire) = observed();
    negotiate(&mut wire, "/meshsub/1.1.0");
    wire.feed(0, &[3, 0x1a]);
    drop(wire);
    assert!(
        evidence
            .lock()
            .error
            .as_ref()
            .unwrap()
            .contains("incomplete")
    );
    let evidence = Evidence::default();
    for _ in 0..=EVENT_LIMIT {
        evidence.emit("bounded", WIRE_SOURCE, json!({}));
    }
    let capture = evidence.lock();
    assert!(capture.overflow && capture.error.is_some());
    assert_eq!(capture.events.len(), EVENT_LIMIT);
    let evidence = Evidence::default();
    evidence.emit(
        "oversized",
        WIRE_SOURCE,
        json!({"blob": "x".repeat(EVENT_BYTE_LIMIT)}),
    );
    assert!(evidence.lock().overflow);
}

struct ScriptIo {
    input: Vec<u8>,
    cursor: usize,
    written: Vec<u8>,
    pending_write: bool,
    fail_write: bool,
}

impl AsyncRead for ScriptIo {
    fn poll_read(
        mut self: Pin<&mut Self>,
        _: &mut Context<'_>,
        out: &mut [u8],
    ) -> Poll<io::Result<usize>> {
        let count = (self.input.len() - self.cursor).min(out.len()).min(3);
        out[..count].copy_from_slice(&self.input[self.cursor..self.cursor + count]);
        self.cursor += count;
        Poll::Ready(Ok(count))
    }
}
impl AsyncWrite for ScriptIo {
    fn poll_write(
        mut self: Pin<&mut Self>,
        _: &mut Context<'_>,
        bytes: &[u8],
    ) -> Poll<io::Result<usize>> {
        if self.pending_write {
            self.pending_write = false;
            return Poll::Pending;
        }
        if self.fail_write {
            return Poll::Ready(Err(io::Error::from(io::ErrorKind::BrokenPipe)));
        }
        let count = bytes.len().min(2);
        self.written.extend_from_slice(&bytes[..count]);
        Poll::Ready(Ok(count))
    }
    fn poll_flush(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(Ok(()))
    }
    fn poll_close(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(Ok(()))
    }
}

#[test]
fn passive_io_forwards_pending_partial_and_failed_operations_without_invented_bytes() {
    let (evidence, id, wire) = observed();
    let handshake = [framed(b"/multistream/1.0.0\n"), framed(b"/meshsub/1.1.0\n")].concat();
    let mut io = ObservedIo {
        inner: ScriptIo {
            input: handshake.clone(),
            cursor: 0,
            written: Vec::new(),
            pending_write: true,
            fail_write: false,
        },
        wire,
    };
    let waker = noop_waker();
    let mut cx = Context::from_waker(&waker);
    assert!(
        Pin::new(&mut io)
            .poll_write(&mut cx, &handshake)
            .is_pending()
    );
    assert!(io.inner.written.is_empty());
    futures::executor::block_on(async {
        io.write_all(&handshake).await.unwrap();
        let mut read = Vec::new();
        io.read_to_end(&mut read).await.unwrap();
        assert_eq!(read, handshake);
    });
    assert_eq!(io.inner.written, handshake);
    assert_eq!(
        evidence
            .lock()
            .events
            .iter()
            .filter(|e| e["kind"] == "protocol")
            .count(),
        1
    );
    let before = evidence.lock().events.clone();
    io.inner.fail_write = true;
    let Poll::Ready(Err(error)) = Pin::new(&mut io).poll_write(&mut cx, &[2, 8, 0]) else {
        panic!("native write error was not forwarded");
    };
    let native = io::Error::from(io::ErrorKind::BrokenPipe);
    let message = native.to_string();
    assert_eq!(error.kind(), native.kind());
    assert_eq!(error.raw_os_error(), native.raw_os_error());
    assert_eq!(error.to_string(), native.to_string());
    assert_eq!(io.inner.written, handshake);
    let capture = evidence.lock();
    assert_eq!(capture.events.len(), before.len() + 1);
    assert_eq!(&capture.events[..before.len()], before.as_slice());
    let record = capture.events.last().unwrap();
    assert_eq!(record["kind"], "native_io_error");
    assert_eq!(record["source"], WIRE_SOURCE);
    assert_eq!(record["operation"], "stream_write");
    assert_eq!(record["io_kind"], "BrokenPipe");
    assert_eq!(record["raw_os_error"], Value::Null);
    assert_eq!(record["typed_cause"], "unclassified");
    assert_eq!(record["prepared"], false);
    assert_eq!(record["message"], message);
    assert_eq!(record["connection_trace_id"], id + 1);
    assert_eq!(record["connection_id"], "7");
    assert_eq!(record["swarm_connection_id"], "7");
    assert_eq!(record["stream_trace_id"], 1);
    assert_eq!(record["stream_id"], format!("{}:1", id + 1));
    assert_eq!(record["peer_id"], capture.connections[id].peer.to_string());
    assert_eq!(record["remote_peer_id"], record["peer_id"]);
    assert_eq!(capture.error.as_deref(), Some(message.as_str()));
}

fn negotiation_headers(wire: &mut WireStream) {
    for side in [wire.proposal_direction, 1 - wire.proposal_direction] {
        wire.feed(side, &framed(b"/multistream/1.0.0\n"));
    }
}

#[test]
fn fragmented_multistream_listing_and_frame_end_reset_do_not_select_pubsub() {
    let protocols = [
        framed(b"/ipfs/id/1.0.0\n"),
        framed(b"/meshsub/1.1.0\n"),
        vec![b'\n'],
    ]
    .concat();
    let full_list = [framed(b"/p\n").repeat(1000), vec![b'\n']].concat();
    for outbound in [false, true] {
        let (evidence, _, mut wire) = observed_role(outbound);
        let proposal = wire.proposal_direction;
        for side in [proposal, 1 - proposal] {
            for byte in framed(b"/multistream/1.0.0\n") {
                wire.feed(side, &[byte]);
            }
        }
        for list in [b"\n".as_slice(), protocols.as_slice(), full_list.as_slice()] {
            for byte in framed(b"ls\n") {
                wire.feed(proposal, &[byte]);
            }
            assert!(wire.listing && wire.selected.is_none());
            for chunk in framed(list).chunks(2) {
                wire.feed(1 - proposal, chunk);
            }
            assert!(!wire.listing && wire.proposal.is_none() && wire.selected.is_none());
            assert!(
                wire.frames
                    .iter()
                    .all(|f| !f.partial() && f.body.is_empty() && f.framed.is_empty())
            );
            assert!(
                !evidence
                    .lock()
                    .events
                    .iter()
                    .any(|e| e["kind"] == "protocol" || e["kind"] == "rpc")
            );
            wire.feed(proposal, &framed(b"/ipfs/id/1.0.0\n"));
            wire.feed(1 - proposal, &framed(b"na\n"));
        }
        for side in [proposal, 1 - proposal] {
            for byte in framed(b"/meshsub/1.1.0\n") {
                wire.feed(side, &[byte]);
            }
        }
        let rpc = framed(&[8, 0]);
        for side in 0..2 {
            for byte in &rpc {
                wire.feed(side, &[*byte]);
            }
        }
        assert!(
            wire.frames
                .iter()
                .all(|f| !f.partial() && f.body.is_empty() && f.framed.is_empty())
        );
        drop(wire);
        let capture = evidence.lock();
        assert!(capture.error.is_none());
        assert_eq!(capture.live_streams, 0);
        assert_eq!(
            capture
                .events
                .iter()
                .filter(|e| e["kind"] == "protocol")
                .count(),
            1
        );
        assert_eq!(
            capture.events.iter().filter(|e| e["kind"] == "rpc").count(),
            2
        );
    }
}

#[test]
fn malformed_or_unterminated_protocol_lists_never_prove_selection() {
    for outbound in [false, true] {
        for list in [
            b"na\n".to_vec(),
            framed(b"/meshsub/1.1.0\n"),
            [vec![b'\n'], framed(b"/meshsub/1.1.0\n")].concat(),
            [framed(b"/p\n").repeat(1001), vec![b'\n']].concat(),
        ] {
            let (evidence, _, mut wire) = observed_role(outbound);
            negotiation_headers(&mut wire);
            let proposal = wire.proposal_direction;
            wire.feed(proposal, &framed(b"ls\n"));
            for chunk in framed(&list).chunks(3) {
                wire.feed(1 - proposal, chunk);
            }
            drop(wire);
            let capture = evidence.lock();
            assert!(capture.error.is_some());
            assert_eq!(capture.live_streams, 0);
            assert!(
                !capture
                    .events
                    .iter()
                    .any(|e| e["kind"] == "protocol" || e["kind"] == "rpc")
            );
        }
    }
}

#[test]
fn rejected_negotiation_diagnostic_retains_the_exact_token_but_is_bounded() {
    for rejected in [
        b"/meshsub/1.0.0\n".as_slice(),
        b"na".as_slice(),
        &[0xff, 0xfe],
    ] {
        let (evidence, _, mut wire) = observed();
        negotiation_headers(&mut wire);
        wire.feed(1, &framed(b"/meshsub/1.1.0\n"));
        wire.feed(0, &framed(rejected));
        let capture = evidence.lock();
        assert!(
            capture
                .error
                .as_ref()
                .unwrap()
                .contains(&negotiation_token(rejected))
        );
        assert!(
            !capture
                .events
                .iter()
                .any(|e| e["kind"] == "protocol" || e["kind"] == "rpc")
        );
    }
    let (evidence, _, mut wire) = observed();
    negotiation_headers(&mut wire);
    let rejected = vec![0; FRAME_LIMIT - 1];
    wire.feed(1, &framed(&rejected));
    let capture = evidence.lock();
    let error = capture.error.as_ref().unwrap();
    assert!(error.contains(&negotiation_token(&rejected)));
    assert!(error.len() < 512);
    assert!(!error.contains('\n'));
}

#[test]
fn identify_ping_push_rejection_can_continue_to_meshsub_on_either_native_stream_role() {
    for outbound in [false, true] {
        for (other, protocol) in ["/ipfs/id/1.0.0", "/ipfs/ping/1.0.0", "/ipfs/id/push/1.0.0"]
            .into_iter()
            .flat_map(|other| {
                ["/meshsub/1.0.0", "/meshsub/1.1.0"].map(|protocol| (other, protocol))
            })
        {
            let (evidence, _, mut wire) = observed_role(outbound);
            negotiation_headers(&mut wire);
            let proposal = wire.proposal_direction;
            let response = 1 - proposal;
            wire.feed(proposal, &framed(format!("{other}\n").as_bytes()));
            wire.feed(response, &framed(b"na\n"));
            assert!(!wire.excluded && wire.selected.is_none() && wire.proposal.is_none());
            assert!(
                !evidence
                    .lock()
                    .events
                    .iter()
                    .any(|e| e["kind"] == "protocol")
            );
            for byte in framed(format!("{protocol}\n").as_bytes()) {
                wire.feed(proposal, &[byte]);
            }
            assert!(wire.selected.is_none());
            for byte in framed(format!("{protocol}\n").as_bytes()) {
                wire.feed(response, &[byte]);
            }
            wire.feed(0, &framed(&[0x1a, 5, 0x22, 3, 0x0a, 1, b't']));
            let capture = evidence.lock();
            let selected = capture
                .events
                .iter()
                .filter(|e| e["kind"] == "protocol")
                .collect::<Vec<_>>();
            assert_eq!(selected.len(), 1);
            assert_eq!(selected[0]["protocol"], protocol);
            assert_eq!(selected[0]["stream_id"], "1:1");
            assert_eq!(selected[0]["connection_id"], "7");
            assert_eq!(
                selected[0]["peer_id"],
                capture.connections[0].peer.to_string()
            );
            assert_eq!(
                capture.events.iter().filter(|e| e["kind"] == "rpc").count(),
                1
            );
            assert!(capture.error.is_none());
            drop(capture);
            drop(wire);
            assert_eq!(evidence.lock().live_streams, 0);
            assert!(evidence.lock().error.is_none());
        }
    }
}

#[test]
fn selected_other_protocol_excludes_opaque_body_without_losing_stream_lifetime() {
    for outbound in [false, true] {
        for protocol in [
            "/ipfs/id/1.0.0",
            "/ipfs/ping/1.0.0",
            "/ipfs/id/push/1.0.0",
            "/meshsub/1.2.0",
            "/meshsub/1.1.0/extra",
        ] {
            let (evidence, _, mut wire) = observed_role(outbound);
            negotiation_headers(&mut wire);
            let proposal = wire.proposal_direction;
            let response = 1 - proposal;
            wire.feed(proposal, &framed(format!("{protocol}\n").as_bytes()));
            // Another protocol's V1Lazy body is not even a length-delimited PubSub RPC.
            wire.feed(proposal, &vec![0xff; FRAME_LIMIT * 2]);
            assert!(wire.pending.is_empty() && wire.pending_bytes == 0);
            wire.feed(
                response,
                &[
                    framed(format!("{protocol}\n").as_bytes()),
                    vec![0xff; FRAME_LIMIT * 2],
                ]
                .concat(),
            );
            assert!(wire.excluded && wire.selected.is_none());
            for side in 0..2 {
                wire.feed(side, &[0xff, 0xff, 0xff, 0xff]);
                wire.feed(side, &framed(&[0x1a, 5, 0x22, 3, 0x0a, 1, b't']));
            }
            assert!(wire.frames.iter().all(|f| f.prefix.capacity() == 0
                && f.body.capacity() == 0
                && f.framed.capacity() == 0));
            assert_eq!(wire.pending.capacity(), 0);
            assert_eq!(wire.pending_bytes, 0);
            assert!(wire.proposal.is_none());
            let capture = evidence.lock();
            assert!(capture.error.is_none());
            assert_eq!(capture.live_streams, 1);
            assert_eq!(capture.streams_created, 1);
            assert!(
                !capture
                    .events
                    .iter()
                    .any(|e| e["kind"] == "protocol" || e["kind"] == "rpc")
            );
            drop(capture);
            drop(wire);
            let capture = evidence.lock();
            assert_eq!(capture.live_streams, 0);
            assert_eq!(capture.connections[0].streams, 1);
            assert_eq!(capture.events.last().unwrap()["kind"], "stream_dropped");
            assert_eq!(capture.events.last().unwrap()["partial_frame"], false);
            assert!(capture.error.is_none());
        }
    }
}

#[test]
fn rejected_other_lazy_body_is_not_an_incomplete_pubsub_rpc_or_a_protocol_ack() {
    for outbound in [false, true] {
        for body in [framed(&[8, 0]), vec![3, 0x1a]] {
            let (evidence, _, mut wire) = observed_role(outbound);
            negotiation_headers(&mut wire);
            let proposal = wire.proposal_direction;
            let response = 1 - proposal;
            wire.feed(proposal, &framed(b"/ipfs/id/1.0.0\n"));
            wire.feed(response, &framed(b"na\n"));
            wire.feed(proposal, &body);
            assert!(wire.selected.is_none());
            drop(wire);
            let capture = evidence.lock();
            assert!(capture.error.is_none());
            assert_eq!(capture.live_streams, 0);
            assert!(
                !capture
                    .events
                    .iter()
                    .any(|e| e["kind"] == "protocol" || e["kind"] == "rpc")
            );
            assert_eq!(capture.events.last().unwrap()["partial_frame"], false);
        }
    }
}

#[test]
fn other_proposal_with_missing_or_partial_reply_is_not_an_incomplete_pubsub_rpc() {
    for outbound in [false, true] {
        for protocol in ["/ipfs/id/1.0.0", "/ipfs/ping/1.0.0", "/ipfs/id/push/1.0.0"] {
            for reply in [Vec::new(), vec![0x80], vec![3, b'n']] {
                let (evidence, _, mut wire) = observed_role(outbound);
                negotiation_headers(&mut wire);
                let proposal = wire.proposal_direction;
                wire.feed(proposal, &framed(format!("{protocol}\n").as_bytes()));
                wire.feed(1 - proposal, &reply);
                drop(wire);
                let capture = evidence.lock();
                assert!(capture.error.is_none(), "{outbound}/{protocol}/{reply:?}");
                assert_eq!(capture.live_streams, 0);
                assert_eq!(capture.events.last().unwrap()["partial_frame"], false);
                assert!(
                    !capture
                        .events
                        .iter()
                        .any(|e| e["kind"] == "protocol" || e["kind"] == "rpc")
                );
            }
        }
    }
}

#[test]
fn other_and_pubsub_protocol_owners_are_isolated_on_the_same_native_muxer() {
    let (evidence, id, mut other) = observed();
    let mut pubsub = WireStream::new(evidence.clone(), Some(id), false);
    negotiate(&mut other, "/ipfs/id/1.0.0");
    negotiate(&mut pubsub, "/meshsub/1.0.0");
    let rpc = framed(&[0x1a, 5, 0x22, 3, 0x0a, 1, b't']);
    other.feed(0, &rpc);
    other.feed(1, &rpc);
    pubsub.feed(0, &rpc);
    let capture = evidence.lock();
    assert_eq!(capture.live_streams, 2);
    for event in capture
        .events
        .iter()
        .filter(|e| e["kind"] == "protocol" || e["kind"] == "rpc")
    {
        assert_eq!(event["stream_id"], "1:2");
        assert_eq!(event["connection_id"], "7");
        assert_eq!(event["protocol"], "/meshsub/1.0.0");
    }
    assert_eq!(
        capture.events.iter().filter(|e| e["kind"] == "rpc").count(),
        1
    );
    drop(capture);
    drop(other);
    assert_eq!(evidence.lock().live_streams, 1);
    drop(pubsub);
    assert_eq!(evidence.lock().live_streams, 0);
    assert!(evidence.lock().error.is_none());
}

#[test]
fn negative_ack_is_not_selection_and_rejected_lazy_rpc_never_acquires_the_next_owner() {
    for outbound in [false, true] {
        let (evidence, _, mut wire) = observed_role(outbound);
        negotiation_headers(&mut wire);
        let proposal = wire.proposal_direction;
        let response = 1 - proposal;
        wire.feed(proposal, &framed(b"/meshsub/1.1.0\n"));
        wire.feed(proposal, &framed(&[0x1a, 5, 0x22, 3, 0x0a, 1, b't']));
        wire.feed(response, &framed(b"na\n"));
        assert!(wire.selected.is_none() && wire.pending.is_empty() && wire.pending_bytes == 0);
        wire.feed(proposal, &framed(b"/ipfs/ping/1.0.0\n"));
        wire.feed(response, &framed(b"/ipfs/ping/1.0.0\n"));
        drop(wire);
        let capture = evidence.lock();
        assert!(
            !capture
                .events
                .iter()
                .any(|e| e["kind"] == "protocol" || e["kind"] == "rpc")
        );
        assert!(capture.error.is_none());
    }
}

#[test]
fn rejected_meshsub_attempt_cannot_contribute_rpc_to_a_later_matching_meshsub_ack() {
    for outbound in [false, true] {
        let (evidence, _, mut wire) = observed_role(outbound);
        negotiation_headers(&mut wire);
        let proposal = wire.proposal_direction;
        let response = 1 - proposal;
        wire.feed(proposal, &framed(b"/meshsub/1.1.0\n"));
        wire.feed(proposal, &framed(&[0x1a, 5, 0x22, 3, 0x0a, 1, b't']));
        wire.feed(response, &framed(b"na\n"));
        wire.feed(proposal, &framed(b"/meshsub/1.0.0\n"));
        wire.feed(response, &framed(b"/meshsub/1.0.0\n"));
        let capture = evidence.lock();
        assert_eq!(
            capture
                .events
                .iter()
                .filter(|e| e["kind"] == "protocol")
                .count(),
            1
        );
        assert!(!capture.events.iter().any(|e| e["kind"] == "rpc"));
        assert_eq!(
            capture
                .events
                .iter()
                .find(|e| e["kind"] == "protocol")
                .unwrap()["protocol"],
            "/meshsub/1.0.0"
        );
        assert!(capture.error.is_none());
    }
}

#[test]
fn wrong_or_missing_ack_and_wrong_native_role_cannot_prove_pubsub() {
    for outbound in [false, true] {
        for kind in ["wrong", "missing", "partial", "wrong_role"] {
            let (evidence, _, mut wire) = observed_role(outbound);
            negotiation_headers(&mut wire);
            let proposal = wire.proposal_direction;
            let response = 1 - proposal;
            if kind == "wrong_role" {
                wire.feed(response, &framed(b"/meshsub/1.1.0\n"));
            } else {
                wire.feed(proposal, &framed(b"/meshsub/1.1.0\n"));
                if kind == "wrong" {
                    wire.feed(response, &framed(b"/meshsub/1.0.0\n"));
                } else if kind == "partial" {
                    wire.feed(response, &[14, b'/']);
                }
            }
            drop(wire);
            let capture = evidence.lock();
            assert!(capture.error.is_some());
            if kind == "missing" {
                assert!(capture.error.as_ref().unwrap().contains("/meshsub/1.1.0"));
            }
            assert!(
                !capture
                    .events
                    .iter()
                    .any(|e| e["kind"] == "protocol" || e["kind"] == "rpc")
            );
            assert_eq!(capture.live_streams, 0);
        }
    }
}

#[test]
fn negotiation_and_unacknowledged_pubsub_buffers_are_bounded_without_fabricated_receipts() {
    let (evidence, _, mut wire) = observed();
    negotiation_headers(&mut wire);
    for _ in 0..33 {
        wire.feed(1, &framed(b"/ipfs/id/1.0.0\n"));
        wire.feed(0, &framed(b"na\n"));
    }
    assert!(
        evidence
            .lock()
            .error
            .as_ref()
            .unwrap()
            .contains("attempt limit")
    );
    assert!(
        !evidence
            .lock()
            .events
            .iter()
            .any(|e| e["kind"] == "protocol")
    );
    let (evidence, _, mut wire) = observed();
    negotiation_headers(&mut wire);
    wire.feed(1, &framed(b"/meshsub/1.1.0\n"));
    wire.feed(1, &framed(&vec![0; FRAME_LIMIT]));
    assert!(
        evidence
            .lock()
            .error
            .as_ref()
            .unwrap()
            .contains("over limit")
    );
    assert!(
        !evidence
            .lock()
            .events
            .iter()
            .any(|e| e["kind"] == "protocol" || e["kind"] == "rpc")
    );
}

#[test]
fn framing_overflow_after_other_rejection_cannot_disable_negotiation_checks() {
    // The pinned multistream codec rejects a third prefix byte, including 16384-byte proposals.
    for outbound in [false, true] {
        for bytes in [
            vec![0x80, 0x80],
            vec![0x80, 0x80, 1],
            vec![0xff; FRAME_LIMIT * 2],
        ] {
            let (evidence, _, mut wire) = observed_role(outbound);
            negotiation_headers(&mut wire);
            let proposal = wire.proposal_direction;
            wire.feed(proposal, &framed(b"/ipfs/id/1.0.0\n"));
            wire.feed(1 - proposal, &framed(b"na\n"));
            wire.feed(proposal, &bytes);
            assert!(wire.failed && !wire.excluded);
            wire.feed(proposal, &framed(b"/meshsub/1.1.0\n"));
            wire.feed(1 - proposal, &framed(b"/meshsub/1.1.0\n"));
            drop(wire);
            let capture = evidence.lock();
            assert!(
                capture
                    .error
                    .as_ref()
                    .unwrap()
                    .contains("prefix over limit")
            );
            assert_eq!(capture.live_streams, 0);
            assert!(
                !capture
                    .events
                    .iter()
                    .any(|e| e["kind"] == "protocol" || e["kind"] == "rpc")
            );
        }
    }
}

#[test]
fn pubsub_partial_rpc_and_frame_overflow_fail_closed_in_both_io_directions() {
    for outbound in [false, true] {
        for side in 0..2 {
            for bytes in [vec![3, 0x1a], vec![0x81, 0x80, 1], vec![0x80; 3]] {
                let (evidence, _, mut wire) = observed_role(outbound);
                negotiate(&mut wire, "/meshsub/1.1.0");
                wire.feed(side, &bytes);
                drop(wire);
                let capture = evidence.lock();
                assert!(capture.error.is_some(), "{outbound}/{side}/{bytes:?}");
                assert_eq!(capture.live_streams, 0);
                assert!(!capture.events.iter().any(|e| e["kind"] == "rpc"));
            }
        }
    }
}

#[test]
fn maximum_rpc_frame_can_use_three_prefix_bytes_after_a_matching_ack() {
    // A single unknown length-delimited protobuf field is a valid RPC with no decoded receipts.
    let mut body = vec![0x22, 0xfd, 0x7f];
    body.resize(FRAME_LIMIT, 0);
    let rpc = framed(&body);
    for outbound in [false, true] {
        let (evidence, _, mut wire) = observed_role(outbound);
        negotiate(&mut wire, "/meshsub/1.1.0");
        wire.feed(0, &rpc);
        drop(wire);
        let capture = evidence.lock();
        assert!(capture.error.is_none());
        let receipt = capture.events.iter().find(|e| e["kind"] == "rpc").unwrap();
        assert_eq!(receipt["receipt"]["framed_hex"], hex(&rpc));
        assert_eq!(receipt["receipt"]["read"]["framed_bytes"], FRAME_LIMIT + 3);
    }
}

#[test]
fn excluded_other_protocol_still_forwards_the_original_native_io_error() {
    let (evidence, _, mut wire) = observed();
    negotiate(&mut wire, "/ipfs/ping/1.0.0");
    let mut io = ObservedIo {
        inner: ScriptIo {
            input: Vec::new(),
            cursor: 0,
            written: Vec::new(),
            pending_write: false,
            fail_write: true,
        },
        wire,
    };
    let waker = noop_waker();
    let mut cx = Context::from_waker(&waker);
    let Poll::Ready(Err(error)) = Pin::new(&mut io).poll_write(&mut cx, &[0xff; 4]) else {
        panic!("original native error was not returned");
    };
    assert_eq!(error.kind(), io::ErrorKind::BrokenPipe);
    assert_eq!(
        error.to_string(),
        io::Error::from(io::ErrorKind::BrokenPipe).to_string()
    );
    assert_eq!(
        evidence.lock().error.as_deref(),
        Some(error.to_string().as_str())
    );
    assert!(
        !evidence
            .lock()
            .events
            .iter()
            .any(|e| e["kind"] == "protocol" || e["kind"] == "rpc")
    );
}

struct ErrorIo;

impl AsyncRead for ErrorIo {
    fn poll_read(
        self: Pin<&mut Self>,
        _: &mut Context<'_>,
        _: &mut [u8],
    ) -> Poll<io::Result<usize>> {
        Poll::Ready(Err(io::Error::from_raw_os_error(38)))
    }
}

impl AsyncWrite for ErrorIo {
    fn poll_write(self: Pin<&mut Self>, _: &mut Context<'_>, _: &[u8]) -> Poll<io::Result<usize>> {
        Poll::Ready(Err(io::Error::from_raw_os_error(38)))
    }
    fn poll_flush(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(Err(io::Error::from_raw_os_error(38)))
    }
    fn poll_close(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(Err(io::Error::from_raw_os_error(38)))
    }
}

#[test]
fn excluded_other_protocol_forwards_read_write_flush_and_close_errors_unchanged() {
    for outbound in [false, true] {
        for operation in 0..4 {
            let (evidence, _, mut wire) = observed_role(outbound);
            negotiate(&mut wire, "/ipfs/id/1.0.0");
            let mut io = ObservedIo {
                inner: ErrorIo,
                wire,
            };
            let waker = noop_waker();
            let mut cx = Context::from_waker(&waker);
            let mut bytes = [0xab; 4];
            let result = match operation {
                0 => Pin::new(&mut io)
                    .poll_read(&mut cx, &mut bytes)
                    .map_ok(|_| ()),
                1 => Pin::new(&mut io).poll_write(&mut cx, &bytes).map_ok(|_| ()),
                2 => Pin::new(&mut io).poll_flush(&mut cx),
                _ => Pin::new(&mut io).poll_close(&mut cx),
            };
            let Poll::Ready(Err(error)) = result else {
                panic!("native I/O error was hidden for operation {operation}");
            };
            let expected = io::Error::from_raw_os_error(38);
            assert_eq!(error.raw_os_error(), expected.raw_os_error());
            assert_eq!(error.kind(), expected.kind());
            assert_eq!(error.to_string(), expected.to_string());
            assert_eq!(bytes, [0xab; 4]);
            drop(io);
            let capture = evidence.lock();
            assert_eq!(
                capture.error.as_deref(),
                Some(expected.to_string().as_str())
            );
            assert_eq!(capture.live_streams, 0);
            assert!(
                !capture
                    .events
                    .iter()
                    .any(|e| e["kind"] == "protocol" || e["kind"] == "rpc")
            );
        }
    }
}

struct ErrorMuxer;

impl StreamMuxer for ErrorMuxer {
    type Substream = futures::io::Cursor<Vec<u8>>;
    type Error = io::Error;
    fn poll_inbound(
        self: Pin<&mut Self>,
        _: &mut Context<'_>,
    ) -> Poll<io::Result<Self::Substream>> {
        Poll::Ready(Err(io::Error::from_raw_os_error(38)))
    }
    fn poll_outbound(
        self: Pin<&mut Self>,
        _: &mut Context<'_>,
    ) -> Poll<io::Result<Self::Substream>> {
        Poll::Ready(Err(io::Error::from_raw_os_error(38)))
    }
    fn poll(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<StreamMuxerEvent>> {
        Poll::Ready(Err(io::Error::from_raw_os_error(38)))
    }
    fn poll_close(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(Err(io::Error::from_raw_os_error(38)))
    }
}

#[test]
fn global_muxer_errors_are_not_filtered_by_stream_protocol_selection() {
    fn poll_error<M: StreamMuxer<Error = io::Error> + Unpin>(
        muxer: &mut M,
        cx: &mut Context<'_>,
        operation: usize,
    ) -> io::Error {
        let result = match operation {
            0 => Pin::new(muxer).poll_inbound(cx).map_ok(|_| ()),
            1 => Pin::new(muxer).poll_outbound(cx).map_ok(|_| ()),
            2 => Pin::new(muxer).poll(cx).map_ok(|_| ()),
            _ => Pin::new(muxer).poll_close(cx),
        };
        let Poll::Ready(Err(error)) = result else {
            panic!("native muxer error was hidden for operation {operation}");
        };
        error
    }

    for prepared in [false, true] {
        for operation in 0..4 {
            let (evidence, id, mut wire) = observed();
            negotiate(&mut wire, "/ipfs/id/1.0.0");
            assert!(wire.excluded);
            assert!(evidence.lock().error.is_none());
            if prepared {
                prepare(&evidence);
            }
            let mut muxer = ObservedMuxer {
                inner: StreamMuxerBox::new(ErrorMuxer),
                evidence: evidence.clone(),
                connection: Some(id),
            };
            let mut native = StreamMuxerBox::new(ErrorMuxer);
            let waker = noop_waker();
            let mut cx = Context::from_waker(&waker);
            let expected = poll_error(&mut native, &mut cx, operation);
            let error = poll_error(&mut muxer, &mut cx, operation);

            // Native boxing wraps the OS error; the observer must preserve that boundary exactly.
            assert_eq!(error.kind(), expected.kind());
            assert_eq!(error.raw_os_error(), expected.raw_os_error());
            assert_eq!(error.to_string(), expected.to_string());
            let expected_inner = expected
                .get_ref()
                .and_then(|inner| inner.downcast_ref::<io::Error>())
                .expect("native boxing did not retain the injected I/O error");
            let inner = error
                .get_ref()
                .and_then(|inner| inner.downcast_ref::<io::Error>())
                .expect("observer did not retain the native inner I/O error");
            assert_eq!(expected_inner.raw_os_error(), Some(38));
            assert_eq!(inner.raw_os_error(), Some(38));
            assert_eq!(inner.kind(), expected_inner.kind());
            assert_eq!(inner.to_string(), expected_inner.to_string());
            assert_eq!(
                evidence.lock().error.as_deref(),
                Some(expected.to_string().as_str())
            );
            {
                let capture = evidence.lock();
                let record = capture
                    .events
                    .iter()
                    .find(|event| event["kind"] == "native_io_error")
                    .unwrap();
                assert_eq!(record["prepared"], prepared);
                assert_eq!(record["connection_trace_id"], id + 1);
                assert!(record["stream_trace_id"].is_null());
                assert_eq!(
                    record["operation"],
                    [
                        "muxer_inbound",
                        "muxer_outbound",
                        "muxer_poll",
                        "muxer_close"
                    ][operation]
                );
            }
            drop(wire);
            drop(muxer);
            assert_eq!(evidence.lock().live_streams, 0);
            assert_eq!(evidence.lock().live_muxers, 0);
        }
    }
}

#[test]
fn excluded_other_protocols_still_consume_the_shared_native_stream_limit() {
    let (evidence, id, mut wire) = observed();
    negotiate(&mut wire, "/ipfs/id/1.0.0");
    drop(wire);
    for _ in 1..STREAM_LIMIT {
        let mut wire = WireStream::new(evidence.clone(), Some(id), false);
        negotiate(&mut wire, "/ipfs/ping/1.0.0");
        drop(wire);
    }
    assert_eq!(evidence.lock().streams_created, STREAM_LIMIT);
    assert_eq!(evidence.lock().live_streams, 0);
    assert!(evidence.lock().error.is_none());
    let mut denied = WireStream::new(evidence.clone(), Some(id), true);
    negotiate(&mut denied, "/meshsub/1.1.0");
    let capture = evidence.lock();
    assert!(capture.overflow && capture.error.as_ref().unwrap().contains("stream limit"));
    assert_eq!(capture.streams_created, STREAM_LIMIT);
    assert_eq!(capture.live_streams, 0);
    assert!(
        !capture
            .events
            .iter()
            .any(|e| e["kind"] == "protocol" || e["kind"] == "rpc")
    );
}

struct ClosingMuxer;

impl StreamMuxer for ClosingMuxer {
    type Substream = futures::io::Cursor<Vec<u8>>;
    type Error = io::Error;
    fn poll_inbound(
        self: Pin<&mut Self>,
        _: &mut Context<'_>,
    ) -> Poll<io::Result<Self::Substream>> {
        Poll::Pending
    }
    fn poll_outbound(
        self: Pin<&mut Self>,
        _: &mut Context<'_>,
    ) -> Poll<io::Result<Self::Substream>> {
        Poll::Pending
    }
    fn poll(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<StreamMuxerEvent>> {
        Poll::Pending
    }
    fn poll_close(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(Ok(()))
    }
}

#[test]
fn native_close_and_host_drop_retain_other_and_pubsub_stream_lifetime_counts() {
    let (evidence, id, mut other) = observed();
    let mut pubsub = WireStream::new(evidence.clone(), Some(id), false);
    negotiate(&mut other, "/ipfs/id/1.0.0");
    negotiate(&mut pubsub, "/meshsub/1.1.0");
    let mut muxer = ObservedMuxer {
        inner: StreamMuxerBox::new(ClosingMuxer),
        evidence: evidence.clone(),
        connection: Some(id),
    };
    let waker = noop_waker();
    let mut cx = Context::from_waker(&waker);
    for _ in 0..2 {
        assert!(matches!(
            Pin::new(&mut muxer).poll_close(&mut cx),
            Poll::Ready(Ok(()))
        ));
    }
    let capture = evidence.lock();
    assert!(capture.connections[id].closed);
    assert_eq!(capture.live_muxers, 1);
    assert_eq!(capture.live_streams, 2);
    assert_eq!(
        capture
            .events
            .iter()
            .filter(|e| e["kind"] == "native_close")
            .count(),
        1
    );
    drop(capture);
    drop(other);
    assert_eq!(evidence.lock().live_streams, 1);
    drop(pubsub);
    drop(muxer);
    let capture = evidence.lock();
    assert!(capture.error.is_none());
    assert!(capture.connections[id].dropped);
    assert_eq!(capture.live_streams, 0);
    assert_eq!(capture.live_muxers, 0);
    assert_eq!(capture.streams_created, 2);
}
