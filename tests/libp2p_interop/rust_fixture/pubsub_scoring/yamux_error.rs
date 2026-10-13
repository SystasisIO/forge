//! PubSub-only public Yamux error conversion, before core's generic boxing.
use libp2p::core::muxing::{StreamMuxer, StreamMuxerBox, StreamMuxerEvent};
use std::{
    error::Error,
    fmt, io,
    pin::Pin,
    task::{Context, Poll},
};

pub(crate) const BOUNDARY: &str = "libp2p_yamux_public_into_io";

#[derive(Debug)]
struct ConversionState {
    converted: io::Error,
    cause: Option<&'static str>,
}

impl fmt::Display for ConversionState {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(&self.converted, f)
    }
}

impl Error for ConversionState {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        Some(&self.converted)
    }
}

fn converted_closed(error: &io::Error) -> Option<&'static str> {
    let inner = error.get_ref()?;
    if matches!(
        inner.downcast_ref::<yamux012::ConnectionError>(),
        Some(yamux012::ConnectionError::Closed)
    ) {
        return Some("yamux012_closed");
    }
    if matches!(
        inner.downcast_ref::<yamux013::ConnectionError>(),
        Some(yamux013::ConnectionError::Closed)
    ) {
        return Some("yamux013_closed");
    }
    None
}

pub(crate) fn convert(error: libp2p::yamux::Error) -> io::Error {
    // Native Io/Decode expose a source. From removes Io's enum, so pin that
    // fact before consuming it; an Io containing Closed must never gain a mark.
    let has_source = error.source().is_some();
    let converted: io::Error = error.into();
    let cause = if has_source {
        None
    } else {
        converted_closed(&converted)
    };
    if cause.is_some() || terminal_state(&converted).is_some() {
        // A native Io containing a previous mark receives a denying boundary,
        // never that older error's classification. Ordinary Io is unchanged.
        return io::Error::new(converted.kind(), ConversionState { converted, cause });
    }
    converted
}

struct PublicErrors<M> {
    inner: Pin<Box<M>>,
}

impl<M: StreamMuxer<Error = libp2p::yamux::Error>> StreamMuxer for PublicErrors<M> {
    type Substream = M::Substream;
    type Error = io::Error;

    fn poll_inbound(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<io::Result<Self::Substream>> {
        self.get_mut()
            .inner
            .as_mut()
            .poll_inbound(cx)
            .map_err(convert)
    }

    fn poll_outbound(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<io::Result<Self::Substream>> {
        self.get_mut()
            .inner
            .as_mut()
            .poll_outbound(cx)
            .map_err(convert)
    }

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<StreamMuxerEvent>> {
        self.get_mut().inner.as_mut().poll(cx).map_err(convert)
    }

    fn poll_close(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        self.get_mut()
            .inner
            .as_mut()
            .poll_close(cx)
            .map_err(convert)
    }
}

pub(crate) fn boxed<M>(muxer: M, convert: bool) -> StreamMuxerBox
where
    M: StreamMuxer<Error = libp2p::yamux::Error> + Send + 'static,
    M::Substream: Send + 'static,
{
    if convert {
        StreamMuxerBox::new(PublicErrors {
            inner: Box::pin(muxer),
        })
    } else {
        StreamMuxerBox::new(muxer)
    }
}

pub(crate) fn terminal_state(error: &io::Error) -> Option<&'static str> {
    let mut current = error;
    // Only accept this adapter's private mark through bounded io::Error boxing.
    for _ in 0..8 {
        let inner = current.get_ref()?;
        if let Some(state) = inner.downcast_ref::<ConversionState>() {
            return state
                .cause
                .filter(|cause| converted_closed(&state.converted) == Some(*cause));
        }
        current = inner.downcast_ref::<io::Error>()?;
    }
    None
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use futures::{AsyncRead, AsyncWrite, task::noop_waker};
    use libp2p::core::upgrade::InboundConnectionUpgrade;

    #[derive(Clone, Copy)]
    pub(crate) enum Scenario {
        Closed,
        Io,
        IoContainingClosed,
        IoContainingMarkedClosed,
        Decode,
        Pending,
        Open,
    }

    pub(crate) struct Socket {
        scenario: Scenario,
        offset: usize,
    }

    impl AsyncRead for Socket {
        fn poll_read(
            mut self: Pin<&mut Self>,
            _: &mut Context<'_>,
            buf: &mut [u8],
        ) -> Poll<io::Result<usize>> {
            if matches!(self.scenario, Scenario::Decode) && self.offset < 12 {
                let mut header = [0u8; 12];
                header[0] = 1;
                let n = buf.len().min(12 - self.offset);
                buf[..n].copy_from_slice(&header[self.offset..self.offset + n]);
                self.offset += n;
                return Poll::Ready(Ok(n));
            }
            Poll::Pending
        }
    }

    impl AsyncWrite for Socket {
        fn poll_write(
            self: Pin<&mut Self>,
            _: &mut Context<'_>,
            buf: &[u8],
        ) -> Poll<io::Result<usize>> {
            match self.scenario {
                Scenario::Io => Poll::Ready(Err(io::Error::from_raw_os_error(38))),
                Scenario::IoContainingClosed => {
                    Poll::Ready(Err(io::Error::other(yamux013::ConnectionError::Closed)))
                }
                Scenario::IoContainingMarkedClosed => {
                    Poll::Ready(Err(convert(native_error(false, Scenario::Closed))))
                }
                Scenario::Pending => Poll::Pending,
                _ => Poll::Ready(Ok(buf.len())),
            }
        }
        fn poll_flush(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
            Poll::Ready(Ok(()))
        }
        fn poll_close(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
            Poll::Ready(Ok(()))
        }
    }

    pub(crate) fn native(version012: bool, scenario: Scenario) -> libp2p::yamux::Muxer<Socket> {
        let mut config = libp2p::yamux::Config::default();
        if version012 {
            config.set_max_num_streams(32);
        }
        futures::executor::block_on(config.upgrade_inbound(
            Socket {
                scenario,
                offset: 0,
            },
            "/yamux/1.0.0",
        ))
        .unwrap()
    }

    pub(crate) fn native_error(version012: bool, scenario: Scenario) -> libp2p::yamux::Error {
        let mut muxer = native(version012, scenario);
        let waker = noop_waker();
        let mut cx = Context::from_waker(&waker);
        if matches!(scenario, Scenario::Closed) {
            assert!(matches!(
                Pin::new(&mut muxer).poll_close(&mut cx),
                Poll::Ready(Ok(()))
            ));
        }
        let result = match scenario {
            Scenario::Io | Scenario::IoContainingClosed | Scenario::IoContainingMarkedClosed => {
                Pin::new(&mut muxer).poll_close(&mut cx)
            }
            _ => Pin::new(&mut muxer).poll_inbound(&mut cx).map_ok(|_| ()),
        };
        let Poll::Ready(Err(error)) = result else {
            panic!("native muxer did not produce the requested error")
        };
        error
    }

    #[test]
    fn public_conversion_exposes_actual_pinned_closed_io_and_decode() {
        for version012 in [false, true] {
            let native_closed = native_error(version012, Scenario::Closed);
            assert!(native_closed.source().is_none());
            let closed: io::Error = native_closed.into();
            assert_eq!(
                converted_closed(&closed),
                Some(if version012 {
                    "yamux012_closed"
                } else {
                    "yamux013_closed"
                })
            );
            assert_eq!(
                terminal_state(&closed),
                None,
                "bare public type lacks conversion provenance"
            );
            let marked = convert(native_error(version012, Scenario::Closed));
            assert_eq!(terminal_state(&marked), converted_closed(&closed));
            let native_io = native_error(version012, Scenario::Io);
            assert!(native_io.source().is_some());
            let native_io: io::Error = native_io.into();
            assert_eq!(native_io.raw_os_error(), Some(38));
            assert_eq!(native_io.kind(), io::Error::from_raw_os_error(38).kind());
            assert_eq!(terminal_state(&native_io), None);
            let native_decode = native_error(version012, Scenario::Decode);
            assert!(native_decode.source().is_some());
            let decode: io::Error = native_decode.into();
            let inner = decode.get_ref().unwrap();
            if version012 {
                assert!(matches!(
                    inner.downcast_ref::<yamux012::ConnectionError>(),
                    Some(yamux012::ConnectionError::Decode(_))
                ));
            } else {
                assert!(matches!(
                    inner.downcast_ref::<yamux013::ConnectionError>(),
                    Some(yamux013::ConnectionError::Decode(_))
                ));
            }
            assert_eq!(terminal_state(&decode), None);
            let opaque = io::Error::other(native_error(version012, Scenario::Closed));
            assert_eq!(terminal_state(&opaque), None);
            let native_io = native_error(version012, Scenario::IoContainingClosed);
            assert!(native_io.source().is_some());
            let inherited = convert(native_io);
            assert_eq!(
                terminal_state(&inherited),
                None,
                "native Io inherited a Closed cause"
            );
            let native_io = native_error(version012, Scenario::IoContainingMarkedClosed);
            assert!(native_io.source().is_some());
            assert_eq!(
                terminal_state(&convert(native_io)),
                None,
                "native Io borrowed a prior converter mark"
            );
        }
    }

    #[test]
    fn adapter_preserves_native_poll_states_and_generic_boxing_opt_out() {
        for version012 in [false, true] {
            let waker = noop_waker();
            let mut cx = Context::from_waker(&waker);
            let mut pending = PublicErrors {
                inner: Box::pin(native(version012, Scenario::Pending)),
            };
            assert!(Pin::new(&mut pending).poll_inbound(&mut cx).is_pending());
            assert!(Pin::new(&mut pending).poll(&mut cx).is_pending());
            assert!(Pin::new(&mut pending).poll_close(&mut cx).is_pending());
            let mut open = PublicErrors {
                inner: Box::pin(native(version012, Scenario::Open)),
            };
            assert!(matches!(
                Pin::new(&mut open).poll_outbound(&mut cx),
                Poll::Ready(Ok(_))
            ));
            assert!(matches!(
                Pin::new(&mut open).poll_close(&mut cx),
                Poll::Ready(Ok(()))
            ));
            for convert in [false, true] {
                let mut muxer = native(version012, Scenario::Closed);
                assert!(matches!(
                    Pin::new(&mut muxer).poll_close(&mut cx),
                    Poll::Ready(Ok(()))
                ));
                let mut boxed = boxed(muxer, convert);
                let Poll::Ready(Err(error)) = Pin::new(&mut boxed).poll_inbound(&mut cx) else {
                    panic!("boxing hid native Closed")
                };
                assert_eq!(terminal_state(&error).is_some(), convert);
                if !convert {
                    assert!(error.get_ref().unwrap().is::<libp2p::yamux::Error>());
                }
            }
        }
    }

    #[test]
    fn terminal_state_rejects_nested_native_io_opaque_text_and_excessive_boxing() {
        let hidden = io::Error::other(yamux013::ConnectionError::Io(io::Error::other(
            yamux013::ConnectionError::Closed,
        )));
        assert_eq!(terminal_state(&hidden), None);
        assert_eq!(
            terminal_state(&io::Error::other("connection is closed")),
            None
        );
        let mut deep = convert(native_error(false, Scenario::Closed));
        for _ in 0..8 {
            deep = io::Error::other(deep);
        }
        assert_eq!(terminal_state(&deep), None);
    }
}
