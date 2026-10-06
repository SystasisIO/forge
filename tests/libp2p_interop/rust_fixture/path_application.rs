//! Outbound-only application streams on an exact existing native connection.
//! This uses Swarm's public NotifyHandler::One rather than changing DCUtR or
//! repeatedly sampling the generic stream control's random connection choice.
use std::{
    collections::{HashMap, VecDeque},
    convert::Infallible,
    io,
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    },
    task::{Context, Poll, Waker},
    time::{Duration, Instant},
};

use futures::channel::oneshot;
use libp2p::{
    Multiaddr, PeerId, Stream, StreamProtocol,
    core::{
        Endpoint,
        transport::PortUse,
        upgrade::{DeniedUpgrade, ReadyUpgrade},
    },
    swarm::{
        ConnectionDenied, ConnectionHandler, ConnectionHandlerEvent, ConnectionId, FromSwarm,
        NetworkBehaviour, NotifyHandler, StreamUpgradeError, SubstreamProtocol, THandler,
        THandlerInEvent, THandlerOutEvent, ToSwarm,
        handler::{ConnectionEvent, DialUpgradeError, FullyNegotiatedOutbound},
    },
};

const MAX_OPENS: usize = 8;
const OPEN_TIMEOUT: Duration = Duration::from_secs(3);
pub(crate) type Opened = oneshot::Receiver<io::Result<Stream>>;

#[derive(Debug)]
struct Permit(Arc<AtomicUsize>);

impl Drop for Permit {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Ordering::SeqCst);
    }
}

#[derive(Debug)]
pub(crate) struct OpenRequest {
    sender: oneshot::Sender<io::Result<Stream>>,
    deadline: Instant,
    _permit: Permit,
}

impl OpenRequest {
    fn fail(self, kind: io::ErrorKind, message: &'static str) {
        let _ = self.sender.send(Err(io::Error::new(kind, message)));
    }
}

#[derive(Debug)]
pub(crate) struct Behaviour {
    protocol: StreamProtocol,
    connected: HashMap<ConnectionId, PeerId>,
    queued: VecDeque<(PeerId, ConnectionId, OpenRequest)>,
    opens: Arc<AtomicUsize>,
    waker: Option<Waker>,
}

impl Behaviour {
    pub(crate) fn new(protocol: StreamProtocol) -> Self {
        Self {
            protocol,
            connected: HashMap::new(),
            queued: VecDeque::new(),
            opens: Arc::new(AtomicUsize::new(0)),
            waker: None,
        }
    }

    pub(crate) fn open(&mut self, peer: PeerId, connection: ConnectionId) -> io::Result<Opened> {
        if self.connected.get(&connection) != Some(&peer) {
            return Err(io::Error::new(
                io::ErrorKind::NotConnected,
                "exact application owner is not live",
            ));
        }
        self.opens
            .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |count| {
                (count < MAX_OPENS).then_some(count + 1)
            })
            .map_err(|_| {
                io::Error::new(
                    io::ErrorKind::WouldBlock,
                    "application open capacity exhausted",
                )
            })?;
        let permit = Permit(self.opens.clone());
        let (sender, receiver) = oneshot::channel();
        self.queued.push_back((
            peer,
            connection,
            OpenRequest {
                sender,
                deadline: Instant::now() + OPEN_TIMEOUT,
                _permit: permit,
            },
        ));
        if let Some(waker) = &self.waker {
            waker.wake_by_ref();
        }
        Ok(receiver)
    }
}

impl NetworkBehaviour for Behaviour {
    type ConnectionHandler = Handler;
    type ToSwarm = ();

    fn handle_established_inbound_connection(
        &mut self,
        _: ConnectionId,
        _: PeerId,
        _: &Multiaddr,
        _: &Multiaddr,
    ) -> Result<THandler<Self>, ConnectionDenied> {
        Ok(Handler::new(self.protocol.clone()))
    }

    fn handle_established_outbound_connection(
        &mut self,
        _: ConnectionId,
        _: PeerId,
        _: &Multiaddr,
        _: Endpoint,
        _: PortUse,
    ) -> Result<THandler<Self>, ConnectionDenied> {
        Ok(Handler::new(self.protocol.clone()))
    }

    fn on_swarm_event(&mut self, event: FromSwarm) {
        match event {
            FromSwarm::ConnectionEstablished(event) => {
                self.connected.insert(event.connection_id, event.peer_id);
            }
            FromSwarm::ConnectionClosed(event) => {
                self.connected.remove(&event.connection_id);
                let mut retained = VecDeque::new();
                while let Some((peer, connection, request)) = self.queued.pop_front() {
                    if connection == event.connection_id {
                        request.fail(
                            io::ErrorKind::NotConnected,
                            "exact application owner closed",
                        );
                    } else {
                        retained.push_back((peer, connection, request));
                    }
                }
                self.queued = retained;
            }
            _ => {}
        }
    }

    fn on_connection_handler_event(
        &mut self,
        _: PeerId,
        _: ConnectionId,
        event: THandlerOutEvent<Self>,
    ) {
        match event {}
    }

    fn poll(
        &mut self,
        cx: &mut Context<'_>,
    ) -> Poll<ToSwarm<Self::ToSwarm, THandlerInEvent<Self>>> {
        self.waker = Some(cx.waker().clone());
        while let Some((peer, connection, request)) = self.queued.pop_front() {
            if request.sender.is_canceled() {
                continue;
            }
            if Instant::now() >= request.deadline {
                request.fail(
                    io::ErrorKind::TimedOut,
                    "application open deadline expired before dispatch",
                );
                continue;
            }
            if self.connected.get(&connection) != Some(&peer) {
                request.fail(
                    io::ErrorKind::NotConnected,
                    "exact application owner is not live",
                );
                continue;
            }
            // This component never returns ToSwarm::Dial or a DCUtR command.
            return Poll::Ready(ToSwarm::NotifyHandler {
                peer_id: peer,
                handler: NotifyHandler::One(connection),
                event: request,
            });
        }
        Poll::Pending
    }
}

#[derive(Debug)]
pub(crate) struct Handler {
    protocol: StreamProtocol,
    queued: VecDeque<OpenRequest>,
    pending: Option<OpenRequest>,
}

impl Handler {
    fn new(protocol: StreamProtocol) -> Self {
        Self {
            protocol,
            queued: VecDeque::new(),
            pending: None,
        }
    }
}

impl ConnectionHandler for Handler {
    type FromBehaviour = OpenRequest;
    type ToBehaviour = Infallible;
    type InboundProtocol = DeniedUpgrade;
    type OutboundProtocol = ReadyUpgrade<StreamProtocol>;
    type InboundOpenInfo = ();
    type OutboundOpenInfo = ();

    fn listen_protocol(&self) -> SubstreamProtocol<Self::InboundProtocol> {
        SubstreamProtocol::new(DeniedUpgrade, ())
    }

    fn connection_keep_alive(&self) -> bool {
        self.pending.is_some() || !self.queued.is_empty()
    }

    fn on_behaviour_event(&mut self, request: Self::FromBehaviour) {
        self.queued.push_back(request);
    }

    fn poll(
        &mut self,
        _: &mut Context<'_>,
    ) -> Poll<ConnectionHandlerEvent<Self::OutboundProtocol, (), Infallible>> {
        if self.pending.is_some() {
            return Poll::Pending;
        }
        while let Some(request) = self.queued.pop_front() {
            if request.sender.is_canceled() {
                continue;
            }
            let Some(remaining) = request.deadline.checked_duration_since(Instant::now()) else {
                request.fail(
                    io::ErrorKind::TimedOut,
                    "application open deadline expired in handler queue",
                );
                continue;
            };
            self.pending = Some(request);
            return Poll::Ready(ConnectionHandlerEvent::OutboundSubstreamRequest {
                protocol: SubstreamProtocol::new(ReadyUpgrade::new(self.protocol.clone()), ())
                    .with_timeout(remaining),
            });
        }
        Poll::Pending
    }

    fn on_connection_event(
        &mut self,
        event: ConnectionEvent<Self::InboundProtocol, Self::OutboundProtocol>,
    ) {
        match event {
            ConnectionEvent::FullyNegotiatedOutbound(FullyNegotiatedOutbound {
                protocol, ..
            }) => {
                if let Some(request) = self.pending.take() {
                    let _ = request.sender.send(Ok(protocol));
                }
            }
            ConnectionEvent::DialUpgradeError(DialUpgradeError { error, .. }) => {
                if let Some(request) = self.pending.take() {
                    let error = match error {
                        StreamUpgradeError::Timeout => io::Error::from(io::ErrorKind::TimedOut),
                        StreamUpgradeError::NegotiationFailed => {
                            io::Error::from(io::ErrorKind::Unsupported)
                        }
                        StreamUpgradeError::Io(error) => error,
                        StreamUpgradeError::Apply(error) => match error {},
                    };
                    let _ = request.sender.send(Err(error));
                }
            }
            _ => {}
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use libp2p::{
        core::ConnectedPoint,
        swarm::behaviour::{ConnectionClosed, ConnectionEstablished},
    };

    fn point() -> ConnectedPoint {
        ConnectedPoint::Listener {
            local_addr: "/memory/1".parse().unwrap(),
            send_back_addr: "/memory/2".parse().unwrap(),
        }
    }

    fn established(
        behaviour: &mut Behaviour,
        peer: PeerId,
        id: ConnectionId,
        point: &ConnectedPoint,
    ) {
        behaviour.on_swarm_event(FromSwarm::ConnectionEstablished(ConnectionEstablished {
            peer_id: peer,
            connection_id: id,
            endpoint: point,
            failed_addresses: &[],
            other_established: 0,
        }));
    }

    #[test]
    fn exact_existing_peer_owner_only_and_never_dials() {
        let mut behaviour = Behaviour::new(StreamProtocol::new(super::super::ECHO_PROTOCOL));
        let peer = PeerId::random();
        let id = ConnectionId::new_unchecked(1);
        assert_eq!(
            behaviour.open(peer, id).unwrap_err().kind(),
            io::ErrorKind::NotConnected
        );
        let endpoint = point();
        established(&mut behaviour, peer, id, &endpoint);
        assert_eq!(
            behaviour.open(PeerId::random(), id).unwrap_err().kind(),
            io::ErrorKind::NotConnected
        );
        let _receiver = behaviour.open(peer, id).unwrap();
        let mut cx = Context::from_waker(futures::task::noop_waker_ref());
        match behaviour.poll(&mut cx) {
            Poll::Ready(ToSwarm::NotifyHandler {
                peer_id,
                handler: NotifyHandler::One(actual),
                ..
            }) => {
                assert_eq!(peer_id, peer);
                assert_eq!(actual, id);
            }
            other => panic!("not a scoped native handler notification: {other:?}"),
        }
        assert!(behaviour.poll(&mut cx).is_pending());
    }

    #[tokio::test]
    async fn owner_loss_returns_error_and_capacity_without_replacement() {
        let mut behaviour = Behaviour::new(StreamProtocol::new(super::super::ECHO_PROTOCOL));
        let peer = PeerId::random();
        let id = ConnectionId::new_unchecked(1);
        let endpoint = point();
        established(&mut behaviour, peer, id, &endpoint);
        let receiver = behaviour.open(peer, id).unwrap();
        behaviour.on_swarm_event(FromSwarm::ConnectionClosed(ConnectionClosed {
            peer_id: peer,
            connection_id: id,
            endpoint: &endpoint,
            cause: None,
            remaining_established: 0,
        }));
        assert_eq!(
            receiver.await.unwrap().unwrap_err().kind(),
            io::ErrorKind::NotConnected
        );
        assert_eq!(behaviour.opens.load(Ordering::SeqCst), 0);
        let mut cx = Context::from_waker(futures::task::noop_waker_ref());
        assert!(behaviour.poll(&mut cx).is_pending());
    }

    #[test]
    fn bounded_open_queue_releases_abandoned_operations() {
        let mut behaviour = Behaviour::new(StreamProtocol::new(super::super::ECHO_PROTOCOL));
        let peer = PeerId::random();
        let id = ConnectionId::new_unchecked(1);
        let endpoint = point();
        established(&mut behaviour, peer, id, &endpoint);
        let receivers: Vec<_> = (0..MAX_OPENS)
            .map(|_| behaviour.open(peer, id).unwrap())
            .collect();
        assert_eq!(
            behaviour.open(peer, id).unwrap_err().kind(),
            io::ErrorKind::WouldBlock
        );
        drop(receivers);
        let mut cx = Context::from_waker(futures::task::noop_waker_ref());
        assert!(behaviour.poll(&mut cx).is_pending());
        assert_eq!(behaviour.opens.load(Ordering::SeqCst), 0);
        assert!(behaviour.open(peer, id).is_ok());
    }

    #[tokio::test]
    async fn handler_owner_drop_and_native_negotiation_failure_return_capacity() {
        let mut behaviour = Behaviour::new(StreamProtocol::new(super::super::ECHO_PROTOCOL));
        let peer = PeerId::random();
        let id = ConnectionId::new_unchecked(1);
        let endpoint = point();
        established(&mut behaviour, peer, id, &endpoint);
        let mut cx = Context::from_waker(futures::task::noop_waker_ref());
        for failure in [true, false] {
            let receiver = behaviour.open(peer, id).unwrap();
            let Poll::Ready(ToSwarm::NotifyHandler { event, .. }) = behaviour.poll(&mut cx) else {
                panic!()
            };
            let mut handler = Handler::new(StreamProtocol::new(super::super::ECHO_PROTOCOL));
            handler.on_behaviour_event(event);
            assert!(matches!(
                handler.poll(&mut cx),
                Poll::Ready(ConnectionHandlerEvent::OutboundSubstreamRequest { .. })
            ));
            if failure {
                handler.on_connection_event(ConnectionEvent::DialUpgradeError(DialUpgradeError {
                    error: StreamUpgradeError::Timeout,
                    info: (),
                }));
                assert_eq!(
                    receiver.await.unwrap().unwrap_err().kind(),
                    io::ErrorKind::TimedOut
                );
            } else {
                drop(handler);
                assert!(receiver.await.is_err());
            }
            assert_eq!(behaviour.opens.load(Ordering::SeqCst), 0);
        }
    }

    #[tokio::test]
    async fn expired_admission_and_handler_queues_do_not_start_native_work() {
        let mut behaviour = Behaviour::new(StreamProtocol::new(super::super::ECHO_PROTOCOL));
        let peer = PeerId::random();
        let id = ConnectionId::new_unchecked(1);
        let endpoint = point();
        established(&mut behaviour, peer, id, &endpoint);
        let mut cx = Context::from_waker(futures::task::noop_waker_ref());
        let receiver = behaviour.open(peer, id).unwrap();
        behaviour.queued.front_mut().unwrap().2.deadline = Instant::now() - OPEN_TIMEOUT;
        assert!(behaviour.poll(&mut cx).is_pending());
        assert_eq!(
            receiver.await.unwrap().unwrap_err().kind(),
            io::ErrorKind::TimedOut
        );
        assert_eq!(behaviour.opens.load(Ordering::SeqCst), 0);

        let receiver = behaviour.open(peer, id).unwrap();
        let Poll::Ready(ToSwarm::NotifyHandler { mut event, .. }) = behaviour.poll(&mut cx) else {
            panic!()
        };
        event.deadline = Instant::now() - OPEN_TIMEOUT;
        let mut handler = Handler::new(StreamProtocol::new(super::super::ECHO_PROTOCOL));
        handler.on_behaviour_event(event);
        assert!(handler.poll(&mut cx).is_pending());
        assert_eq!(
            receiver.await.unwrap().unwrap_err().kind(),
            io::ErrorKind::TimedOut
        );
        assert_eq!(behaviour.opens.load(Ordering::SeqCst), 0);
    }

    #[test]
    fn caller_cancel_retains_pending_owner_until_its_native_terminal_callback() {
        let mut behaviour = Behaviour::new(StreamProtocol::new(super::super::ECHO_PROTOCOL));
        let peer = PeerId::random();
        let id = ConnectionId::new_unchecked(1);
        let endpoint = point();
        established(&mut behaviour, peer, id, &endpoint);
        let mut cx = Context::from_waker(futures::task::noop_waker_ref());
        let receiver = behaviour.open(peer, id).unwrap();
        let Poll::Ready(ToSwarm::NotifyHandler { event, .. }) = behaviour.poll(&mut cx) else {
            panic!()
        };
        let mut handler = Handler::new(StreamProtocol::new(super::super::ECHO_PROTOCOL));
        handler.on_behaviour_event(event);
        assert!(matches!(
            handler.poll(&mut cx),
            Poll::Ready(ConnectionHandlerEvent::OutboundSubstreamRequest { .. })
        ));
        drop(receiver);
        assert_eq!(behaviour.opens.load(Ordering::SeqCst), 1);

        let _next = behaviour.open(peer, id).unwrap();
        let Poll::Ready(ToSwarm::NotifyHandler { event, .. }) = behaviour.poll(&mut cx) else {
            panic!()
        };
        handler.on_behaviour_event(event);
        assert!(handler.poll(&mut cx).is_pending());
        assert_eq!(behaviour.opens.load(Ordering::SeqCst), 2);
        handler.on_connection_event(ConnectionEvent::DialUpgradeError(DialUpgradeError {
            error: StreamUpgradeError::Timeout,
            info: (),
        }));
        assert_eq!(behaviour.opens.load(Ordering::SeqCst), 1);
        assert!(matches!(
            handler.poll(&mut cx),
            Poll::Ready(ConnectionHandlerEvent::OutboundSubstreamRequest { .. })
        ));
        drop(handler);
        assert_eq!(behaviour.opens.load(Ordering::SeqCst), 0);
    }
}
