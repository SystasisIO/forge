package main

// Controlled delegates test instrumentation only, not live handshakes or signed Identify proof.
import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net"
	"strings"
	"testing"

	"github.com/libp2p/go-libp2p/core/event"
	"github.com/libp2p/go-libp2p/core/host"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/core/protocol"
	"github.com/libp2p/go-libp2p/core/record"
	"github.com/libp2p/go-libp2p/core/sec"
	"github.com/libp2p/go-libp2p/core/transport"
	ma "github.com/multiformats/go-multiaddr"
	manet "github.com/multiformats/go-multiaddr/net"
)

func upgradeWire(tokens ...string) []byte {
	var wire []byte
	for _, token := range tokens {
		wire = binary.AppendUvarint(wire, uint64(len(token)+1))
		wire = append(wire, token...)
		wire = append(wire, '\n')
	}
	return wire
}

type fakeRawConn struct {
	manet.Conn
	input    []byte
	written  []byte
	writeErr error
	closeErr error
	closed   bool
}

func (c *fakeRawConn) Read(p []byte) (int, error) {
	if len(c.input) == 0 {
		return 0, io.EOF
	}
	n := copy(p, c.input)
	c.input = c.input[n:]
	return n, nil
}
func (c *fakeRawConn) Write(p []byte) (int, error) {
	c.written = append(c.written, p...)
	return len(p), c.writeErr
}
func (c *fakeRawConn) Close() error                  { c.closed = true; return c.closeErr }
func (c *fakeRawConn) LocalMultiaddr() ma.Multiaddr  { return ma.StringCast("/ip4/127.0.0.1/tcp/4100") }
func (c *fakeRawConn) RemoteMultiaddr() ma.Multiaddr { return ma.StringCast("/ip4/127.0.0.1/tcp/4200") }
func (c *fakeRawConn) LocalAddr() net.Addr {
	return &net.TCPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 4100}
}
func (c *fakeRawConn) RemoteAddr() net.Addr {
	return &net.TCPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 4200}
}

type fakeSecureConn struct {
	sec.SecureConn
	inner  net.Conn
	state  network.ConnectionState
	remote peer.ID
}

func (c *fakeSecureConn) Read(p []byte) (int, error)  { return c.inner.Read(p) }
func (c *fakeSecureConn) Write(p []byte) (int, error) { return c.inner.Write(p) }
func (c *fakeSecureConn) LocalPeer() peer.ID          { return peer.ID("local") }
func (c *fakeSecureConn) RemotePeer() peer.ID {
	if c.remote != "" {
		return c.remote
	}
	return peer.ID("remote")
}
func (c *fakeSecureConn) ConnState() network.ConnectionState { return c.state }

type fakeSecurity struct {
	selected          protocol.ID
	state             network.ConnectionState
	remote            peer.ID
	err               error
	inbound, outbound int
}

func (s *fakeSecurity) ID() protocol.ID { return s.selected }
func (s *fakeSecurity) SecureInbound(_ context.Context, c net.Conn, _ peer.ID) (sec.SecureConn, error) {
	s.inbound++
	if s.err != nil {
		return nil, s.err
	}
	return &fakeSecureConn{inner: c, state: s.state, remote: s.remote}, nil
}
func (s *fakeSecurity) SecureOutbound(_ context.Context, c net.Conn, _ peer.ID) (sec.SecureConn, error) {
	s.outbound++
	if s.err != nil {
		return nil, s.err
	}
	return &fakeSecureConn{inner: c, state: s.state, remote: s.remote}, nil
}

type fakeMuxedStream struct {
	network.MuxedStream
	io *fakeRawConn
}

func (s *fakeMuxedStream) Read(p []byte) (int, error)                   { return s.io.Read(p) }
func (s *fakeMuxedStream) Write(p []byte) (int, error)                  { return s.io.Write(p) }
func (s *fakeMuxedStream) Close() error                                 { return s.io.Close() }
func (s *fakeMuxedStream) CloseWrite() error                            { return nil }
func (s *fakeMuxedStream) Reset() error                                 { return s.io.Close() }
func (s *fakeMuxedStream) ResetWithError(network.StreamErrorCode) error { return s.io.Close() }

type fakeMuxedConn struct {
	network.MuxedConn
	stream      network.MuxedStream
	delegatedAs bool
}

func (c *fakeMuxedConn) OpenStream(context.Context) (network.MuxedStream, error) {
	return c.stream, nil
}
func (c *fakeMuxedConn) AcceptStream() (network.MuxedStream, error) { return c.stream, nil }
func (c *fakeMuxedConn) As(any) bool                                { c.delegatedAs = true; return false }

type fakeMultiplexer struct {
	result network.MuxedConn
	err    error
	calls  int
}

func (m *fakeMultiplexer) NewConn(net.Conn, bool, network.PeerScope) (network.MuxedConn, error) {
	m.calls++
	return m.result, m.err
}

func negotiation(t *testing.T, conn io.ReadWriter, raw *fakeRawConn, selected string) {
	t.Helper()
	wire := upgradeWire("/multistream/1.0.0", selected)
	raw.input = append([]byte{}, wire...)
	if _, err := conn.Write(wire); err != nil {
		t.Fatal(err)
	}
	buffer := make([]byte, len(wire))
	if _, err := io.ReadFull(conn, buffer); err != nil {
		t.Fatal(err)
	}
}

func securityFixture(t *testing.T, selected string, early bool) (*upgradeObserver, *observedRawConn, sec.SecureConn) {
	return securityFixtureDirection(t, selected, early, network.DirOutbound)
}

func securityFixtureDirection(t *testing.T, selected string, early bool, direction network.Direction) (*upgradeObserver, *observedRawConn, sec.SecureConn) {
	return securityFixtureOnObserver(t, &upgradeObserver{}, selected, early, direction, "")
}

func securityFixtureOnObserver(t *testing.T, o *upgradeObserver, selected string, early bool, direction network.Direction, remote peer.ID) (*upgradeObserver, *observedRawConn, sec.SecureConn) {
	t.Helper()
	raw := &fakeRawConn{}
	c := &observedRawConn{Conn: raw, trace: o.connection(raw, direction)}
	negotiation(t, c, raw, selected)
	delegate := &fakeSecurity{selected: protocol.ID(selected), remote: remote}
	if early {
		delegate.state.StreamMultiplexer = "/yamux/1.0.0"
		delegate.state.UsedEarlyMuxerNegotiation = true
	}
	security := &observedSecurity{SecureTransport: delegate}
	var s sec.SecureConn
	var err error
	if direction == network.DirOutbound {
		s, err = security.SecureOutbound(context.Background(), c, peer.ID("remote"))
	} else {
		s, err = security.SecureInbound(context.Background(), c, peer.ID("remote"))
	}
	if err != nil || delegate.outbound+delegate.inbound != 1 {
		t.Fatalf("delegation: %v", err)
	}
	if !early {
		negotiation(t, s, raw, "/yamux/1.0.0")
	}
	return o, c, s
}

func TestUpgradeFramesFragmentationAndNoPayloadCapture(t *testing.T) {
	wire := upgradeWire("/multistream/1.0.0", "/noise")
	for chunk := 1; chunk <= len(wire); chunk++ {
		o := &upgradeObserver{}
		trace := o.connection(&fakeRawConn{}, network.DirInbound)
		withPayload := append(append([]byte{}, wire...), []byte("NEVER_CAPTURE_BODY")...)
		for _, read := range []bool{true, false} {
			for start := 0; start < len(withPayload); start += chunk {
				end := min(start+chunk, len(withPayload))
				trace.security.feed(read, withPayload[start:end])
			}
		}
		if trace.security.selected != "/noise" || trace.security.failed {
			t.Fatalf("chunk %d", chunk)
		}
		frames := 0
		for _, e := range trace.evidence.Events {
			if e.Kind == "multistream_frame" {
				frames++
				data, err := hex.DecodeString(e.FrameHex)
				if err != nil || bytes.Contains(data, []byte("NEVER_CAPTURE")) {
					t.Fatal("payload captured")
				}
			}
		}
		if frames != 4 {
			t.Fatalf("frames=%d", frames)
		}
	}
}

func TestUpgradeFragmentedTLSRejectionThenNoiseSelection(t *testing.T) {
	for _, direction := range []network.Direction{network.DirInbound, network.DirOutbound} {
		for chunk := 1; chunk <= 32; chunk++ {
			o := &upgradeObserver{}
			trace := o.connection(&fakeRawConn{}, direction)
			proposerRead := direction == network.DirInbound
			feed := func(read bool, wire []byte) {
				for offset := 0; offset < len(wire); offset += chunk {
					trace.security.feed(read, wire[offset:min(offset+chunk, len(wire))])
				}
			}
			feed(proposerRead, upgradeWire("/multistream/1.0.0", "/tls/1.0.0"))
			feed(!proposerRead, upgradeWire("/multistream/1.0.0", "na"))
			if trace.security.selected != "" || trace.security.failed {
				t.Fatal("rejection was treated as terminal selection/failure")
			}
			feed(proposerRead, upgradeWire("/noise"))
			feed(!proposerRead, append(upgradeWire("/noise"), []byte("OPAQUE_SECURITY_HANDSHAKE")...))
			if trace.security.selected != "/noise" || trace.security.failed {
				t.Fatalf("direction=%v chunk=%d", direction, chunk)
			}
			frames, rejected := 0, 0
			for _, e := range trace.evidence.Events {
				if e.Kind == "multistream_frame" {
					frames++
					data, _ := hex.DecodeString(e.FrameHex)
					if bytes.Contains(data, []byte("OPAQUE")) {
						t.Fatal("security body captured")
					}
				}
				if e.Kind == "protocol_rejected" && e.Protocol == "/tls/1.0.0" {
					rejected++
				}
			}
			if frames != 6 || rejected != 1 {
				t.Fatalf("lost fallback frames: %d/%d", frames, rejected)
			}
		}
	}
}

func TestUpgradeFallbackWrongAckHeaderBoundsAndUnmatchedTail(t *testing.T) {
	for _, mode := range []string{"wrong-ack", "unexpected-header", "proposal-bound", "unmatched-tail", "missing-ack"} {
		t.Run(mode, func(t *testing.T) {
			o := &upgradeObserver{}
			trace := o.connection(&fakeRawConn{}, network.DirInbound)
			s := &trace.security
			s.feed(true, upgradeWire("/multistream/1.0.0", "/tls/1.0.0"))
			if mode == "unmatched-tail" {
				s.feed(true, upgradeWire("/not-an-authorized-next-proposal"))
			}
			s.feed(false, upgradeWire("/multistream/1.0.0", "na"))
			switch mode {
			case "wrong-ack":
				s.feed(true, upgradeWire("/noise"))
				s.feed(false, upgradeWire("/tls/1.0.0"))
			case "unexpected-header":
				s.feed(true, upgradeWire("/multistream/1.0.0", "/noise"))
			case "proposal-bound":
				for i := 0; i < upgradeProposalLimit; i++ {
					s.feed(true, upgradeWire("/tls/1.0.0"))
					s.feed(false, upgradeWire("na"))
				}
			case "missing-ack":
				s.feed(true, upgradeWire("/noise"))
			}
			if s.selected != "" {
				t.Fatal("unconfirmed fallback selected")
			}
			if mode != "missing-ack" && !s.failed {
				t.Fatal("invalid fallback accepted")
			}
		})
	}
}

func TestUpgradeFallbackAppCoalescedTailIsDigestOnly(t *testing.T) {
	o := &upgradeObserver{}
	trace := o.connection(&fakeRawConn{}, network.DirInbound)
	s := &selectionTrace{connection: trace, phase: "application_multistream", stream: 1, direction: network.DirInbound.String()}
	s.feed(true, upgradeWire("/multistream/1.0.0", "/unsupported/1"))
	s.feed(false, upgradeWire("/multistream/1.0.0", "na"))
	s.feed(true, upgradeWire("/ipfs/id/1.0.0"))
	body := upgradeWire("PRIVATE_BODY_NOT_RETAINED")
	s.feed(false, append(upgradeWire("/ipfs/id/1.0.0"), body...))
	if s.failed || s.selected != "/ipfs/id/1.0.0" || !s.writeBody.complete() || s.writeBody.total != uint64(len(body)) {
		t.Fatal("coalesced response lost or parsed as negotiation")
	}
	for _, e := range trace.evidence.Events {
		data, _ := hex.DecodeString(e.FrameHex)
		if bytes.Contains(data, []byte("PRIVATE_BODY")) {
			t.Fatal("application body captured")
		}
	}
}

func TestUpgradeMalformedOrMismatchedFramesFailClosed(t *testing.T) {
	for _, wire := range [][]byte{
		{0}, {0x81, 0}, binary.AppendUvarint(nil, upgradeFrameLimit+1),
		bytes.Repeat([]byte{0xff}, 11), upgradeWire("/wrong/1.0.0", "/noise"),
		upgradeWire("/multistream/1.0.0", "na"),
	} {
		o := &upgradeObserver{}
		trace := o.connection(&fakeRawConn{}, network.DirInbound)
		trace.security.feed(true, wire)
		if !trace.security.failed || !trace.evidence.Failed {
			t.Fatalf("accepted malformed %x", wire)
		}
	}
	o := &upgradeObserver{}
	trace := o.connection(&fakeRawConn{}, network.DirInbound)
	trace.security.feed(true, upgradeWire("/multistream/1.0.0", "/noise"))
	trace.security.feed(false, upgradeWire("/multistream/1.0.0", "/tls/1.0.0"))
	if !trace.security.failed {
		t.Fatal("accepted different proposal and acknowledgement")
	}
}

func TestUpgradeDelegateOrderEarlyMuxerAndFinalShape(t *testing.T) {
	for _, selected := range []string{"/noise", "/tls/1.0.0"} {
		for _, early := range []bool{false, true} {
			o, raw, secure := securityFixture(t, selected, early)
			streamIO := &fakeRawConn{}
			delegate := &fakeMultiplexer{result: &fakeMuxedConn{stream: &fakeMuxedStream{io: streamIO}}}
			muxed, err := (&observedMultiplexer{Multiplexer: delegate, id: "/yamux/1.0.0"}).NewConn(secure, false, nil)
			if err != nil || delegate.calls != 1 {
				t.Fatal("muxer not delegated")
			}
			ctx, binding := bindUpgradeStream(context.Background())
			stream, err := muxed.OpenStream(ctx)
			if err != nil {
				t.Fatal(err)
			}
			native := &fakeNetworkStream{connection: &fakeNetworkConn{muxed: muxed.(*observedMuxedConn),
				id: "connection", remote: peer.ID("remote")}, streamID: "explicit", selected: echoProtocol}
			binding.attach(native)
			negotiation(t, stream, streamIO, string(echoProtocol))
			body := upgradeWire("echo-body")
			streamIO.input = append([]byte{}, body...)
			stream.Write(body)
			io.ReadFull(stream, make([]byte, len(body)))
			binding.complete(native)
			stream.Close()
			raw.Close()
			proof, err := o.finish(echoProtocol, peer.ID("remote").String(), network.DirOutbound, binding.target())
			if err != nil || !proof.Complete {
				t.Fatalf("proof=%+v, err=%v", proof, err)
			}
			if _, err := o.finish(echoProtocol, peer.ID("remote").String(), network.DirOutbound); err == nil {
				t.Fatal("outbound proof accepted without the explicit stream target")
			}
			positions := map[string]int{}
			muxerFrames := 0
			for i, e := range proof.Connections[0].Events {
				positions[e.Kind] = i
				if e.Phase == "muxer_multistream" && e.Kind == "multistream_frame" {
					muxerFrames++
				}
				if e.Sequence != uint64(i+1) {
					t.Fatal("nonmonotonic sequence")
				}
			}
			if !(positions["security_enter"] < positions["security_complete"] &&
				positions["security_complete"] < positions["muxer_enter"] &&
				positions["muxer_enter"] < positions["muxer_complete"] &&
				positions["muxer_complete"] < positions["stream_open"]) {
				t.Fatal("wrong causal order")
			}
			if (early && muxerFrames != 0) || (!early && muxerFrames != 4) {
				t.Fatal("invented or missing muxer wire frames")
			}
			encoded, _ := json.Marshal(proof)
			if !strings.Contains(string(encoded), `"source":"go-libp2p.public-upgrade-hooks.v1"`) ||
				!strings.Contains(string(encoded), `"authenticated_remote_peer_id"`) ||
				proof.Connections[0].EarlyMuxer != early {
				t.Fatal("source/identity/early-muxer shape lost")
			}
			if _, err := o.finish(echoProtocol, "another-peer", network.DirOutbound); err == nil {
				t.Fatal("wrong peer credited")
			}
			if _, err := o.finish(echoProtocol, "", network.DirInbound); err == nil {
				t.Fatal("wrong direction credited")
			}
		}
	}
}

func TestUpgradeFailurePreservesDelegateErrorAndNeverInventsSuccess(t *testing.T) {
	o := &upgradeObserver{}
	base := &fakeRawConn{}
	raw := &observedRawConn{Conn: base, trace: o.connection(base, network.DirInbound)}
	negotiation(t, raw, base, "/noise")
	want := errors.New("typed delegate failure")
	security := &fakeSecurity{selected: "/noise", err: want}
	_, err := (&observedSecurity{SecureTransport: security}).SecureInbound(context.Background(), raw, "")
	if err != want || security.inbound != 1 || !raw.trace.evidence.Failed {
		t.Fatal("security failure changed")
	}
	for _, e := range raw.trace.evidence.Events {
		if e.Kind == "security_complete" {
			t.Fatal("failed handshake claimed success")
		}
	}
	o, raw, secure := securityFixture(t, "/noise", false)
	delegate := &fakeMultiplexer{err: want}
	_, err = (&observedMultiplexer{Multiplexer: delegate, id: "/yamux/1.0.0"}).NewConn(secure, false, nil)
	if err != want || delegate.calls != 1 {
		t.Fatal("muxer failure changed")
	}
	raw.Close()
	if _, err := o.finish(echoProtocol, "", network.DirOutbound); err == nil {
		t.Fatal("failed muxer accepted")
	}
}

func TestUpgradeActualSelectedDelegateMustMatchWire(t *testing.T) {
	o := &upgradeObserver{}
	base := &fakeRawConn{}
	raw := &observedRawConn{Conn: base, trace: o.connection(base, network.DirOutbound)}
	negotiation(t, raw, base, "/noise")
	delegate := &fakeSecurity{selected: "/tls/1.0.0"}
	_, err := (&observedSecurity{SecureTransport: delegate}).SecureOutbound(context.Background(), raw, "")
	if err != nil || delegate.outbound != 1 {
		t.Fatal("observer altered delegated I/O")
	}
	if !raw.trace.evidence.Failed {
		t.Fatal("wire/delegate mismatch silently relabelled")
	}
}

type fakeGatedListener struct {
	transport.GatedMaListener
	conn  manet.Conn
	scope network.ConnManagementScope
}

func (l *fakeGatedListener) Accept() (manet.Conn, network.ConnManagementScope, error) {
	return l.conn, l.scope, nil
}

type fakeUpgrader struct {
	transport.Upgrader
	seen      manet.Conn
	scope     network.ConnManagementScope
	dialCalls int
}

func (u *fakeUpgrader) Upgrade(_ context.Context, _ transport.Transport, c manet.Conn, _ network.Direction,
	_ peer.ID, scope network.ConnManagementScope) (transport.CapableConn, error) {
	u.dialCalls++
	u.seen, u.scope = c, scope
	return nil, nil
}
func (u *fakeUpgrader) UpgradeGatedMaListener(_ transport.Transport, l transport.GatedMaListener) transport.Listener {
	u.seen, u.scope, _ = l.Accept()
	return nil
}

func TestUpgradeOutboundAndInternallyDelegatedInboundAreObserved(t *testing.T) {
	o := &upgradeObserver{}
	delegate := &fakeUpgrader{}
	u := &observedUpgrader{Upgrader: delegate, observer: o}
	scope := &network.NullScope{}
	u.Upgrade(context.Background(), nil, &fakeRawConn{}, network.DirOutbound, "", scope)
	if _, ok := delegate.seen.(*observedRawConn); !ok || delegate.scope != scope {
		t.Fatal("outbound scope/raw lost")
	}
	u.UpgradeGatedMaListener(nil, &fakeGatedListener{conn: &fakeRawConn{}, scope: scope})
	if _, ok := delegate.seen.(*observedRawConn); !ok || delegate.scope != scope {
		t.Fatal("inbound bypassed observer")
	}
	if delegate.dialCalls != 1 || len(o.connections) != 2 {
		t.Fatal("inbound must not depend on external Upgrade callback")
	}
}

func TestUpgradeOverflowPreservesIOAndBoundsEvidence(t *testing.T) {
	o := &upgradeObserver{}
	base := &fakeRawConn{input: []byte("still readable"), writeErr: io.ErrClosedPipe}
	trace := o.connection(base, network.DirOutbound)
	raw := &observedRawConn{Conn: base, trace: trace}
	for i := 0; i < upgradeEventLimit+5; i++ {
		trace.eventLocked(upgradeEvent{Kind: "synthetic_capacity_event"})
	}
	buffer := make([]byte, 32)
	n, err := raw.Read(buffer)
	if err != nil || string(buffer[:n]) != "still readable" {
		t.Fatal("overflow changed read")
	}
	n, err = raw.Write([]byte("still writable"))
	if n != 14 || err != io.ErrClosedPipe || string(base.written) != "still writable" {
		t.Fatal("overflow changed write")
	}
	if len(trace.evidence.Events) != upgradeEventLimit || !o.overflow {
		t.Fatal("unbounded event storage")
	}
	for i := 0; i < upgradeConnectionLimit+5; i++ {
		o.connection(&fakeRawConn{}, network.DirInbound)
	}
	if len(o.connections) != upgradeConnectionLimit {
		t.Fatal("unbounded connections")
	}
	muxed := &observedMuxedConn{trace: trace}
	stream := &fakeMuxedStream{io: &fakeRawConn{}}
	for i := 0; i <= upgradeStreamLimit; i++ {
		muxed.stream(stream, network.DirOutbound)
	}
	if trace.streams != upgradeStreamLimit {
		t.Fatal("unbounded streams")
	}
	proof, err := o.finish(echoProtocol, "", network.DirOutbound)
	if err == nil || proof.Complete || !proof.Overflow {
		t.Fatal("overflow accepted as proof")
	}
}

type fakeNetworkConn struct {
	network.Conn
	muxed  *observedMuxedConn
	id     string
	remote peer.ID
}

func (c *fakeNetworkConn) As(target any) bool {
	return c.muxed != nil && c.muxed.As(target)
}
func (c *fakeNetworkConn) ID() string          { return c.id }
func (c *fakeNetworkConn) LocalPeer() peer.ID  { return peer.ID("local") }
func (c *fakeNetworkConn) RemotePeer() peer.ID { return c.remote }

type fakeNetworkStream struct {
	network.Stream
	connection network.Conn
	streamID   string
	selected   protocol.ID
	data       *fakeRawConn
}

func (s *fakeNetworkStream) Conn() network.Conn    { return s.connection }
func (s *fakeNetworkStream) ID() string            { return s.streamID }
func (s *fakeNetworkStream) Protocol() protocol.ID { return s.selected }
func (s *fakeNetworkStream) Stat() network.Stats {
	return network.Stats{Direction: network.DirOutbound}
}
func (s *fakeNetworkStream) Read(p []byte) (int, error) { return s.data.Read(p) }
func (s *fakeNetworkStream) Close() error               { return s.data.Close() }
func (s *fakeNetworkStream) Reset() error               { return s.data.Close() }

func TestUpgradeApplicationBindsExactConnectionViaPublicAs(t *testing.T) {
	_, raw, secure := securityFixture(t, "/noise", true)
	streamIO := &fakeRawConn{}
	delegate := &fakeMuxedConn{stream: &fakeMuxedStream{io: streamIO}}
	m, err := (&observedMultiplexer{Multiplexer: &fakeMultiplexer{result: delegate}, id: "/yamux/1.0.0"}).NewConn(secure, false, nil)
	if err != nil {
		t.Fatal(err)
	}
	muxed := m.(*observedMuxedConn)
	conn := &fakeNetworkConn{muxed: muxed, id: "actual-connection", remote: peer.ID("remote")}
	s := &fakeNetworkStream{connection: conn, streamID: "actual-stream", selected: echoProtocol}
	ctx, binding := bindUpgradeStream(context.Background())
	wireStream, _ := muxed.OpenStream(ctx)
	binding.attach(s)
	negotiation(t, wireStream, streamIO, string(echoProtocol))
	last := raw.trace.evidence.Events[len(raw.trace.evidence.Events)-1]
	if last.Kind != "protocol_selected" || last.Stream != binding.selection.stream {
		t.Fatal("exact wire stream lost")
	}
	bound := false
	for _, e := range raw.trace.evidence.Events {
		if e.Kind == "application_stream_binding" && e.ConnectionID == conn.ID() && e.NetworkStreamID == s.ID() && e.Stream == binding.selection.stream {
			bound = true
		}
	}
	if !bound {
		t.Fatal("context/network-stream/wire-stream link missing")
	}
	other := &fakeNetworkStream{connection: conn, streamID: "another-stream", selected: echoProtocol}
	otherContext, otherBinding := bindUpgradeStream(context.Background())
	otherWireStream, _ := muxed.OpenStream(otherContext)
	otherBinding.attach(other)
	negotiation(t, otherWireStream, streamIO, string(echoProtocol))
	binding.complete(other)
	if !raw.trace.evidence.Failed || binding.selection.applicationDone {
		t.Fatal("same connection/protocol different stream accepted")
	}
	var unrelated string
	if muxed.As(&unrelated) || !delegate.delegatedAs {
		t.Fatal("As fallback changed")
	}
	if upgradeProofProtocol("pnet") != "" {
		t.Fatal("private network included")
	}
}

func TestUpgradeOneShotBindingRejectsRepeatedOpenWithoutChangingIO(t *testing.T) {
	_, raw, secure := securityFixture(t, "/noise", true)
	delegate := &fakeMultiplexer{result: &fakeMuxedConn{stream: &fakeMuxedStream{io: &fakeRawConn{}}}}
	muxed, _ := (&observedMultiplexer{Multiplexer: delegate, id: "/yamux/1.0.0"}).NewConn(secure, false, nil)
	ctx, binding := bindUpgradeStream(context.Background())
	first, err := muxed.OpenStream(ctx)
	second, err2 := muxed.OpenStream(ctx)
	if err != nil || err2 != nil || first == nil || second == nil {
		t.Fatal("binding altered forwarded operations")
	}
	if !binding.invalid || !binding.selection.ioFailed || len(raw.trace.streamStates) != 2 {
		t.Fatal("repeated binding accepted")
	}
}

func TestUpgradeInboundIdentifyCompletionRequiresFramedResponseAndClose(t *testing.T) {
	for _, mode := range []string{"selected-only", "no-close", "reset", "write-error", "truncated", "oversized", "complete", "close-write"} {
		t.Run(mode, func(t *testing.T) {
			o, raw, secure := securityFixtureDirection(t, "/tls/1.0.0", true, network.DirInbound)
			streamIO := &fakeRawConn{}
			delegate := &fakeMultiplexer{result: &fakeMuxedConn{stream: &fakeMuxedStream{io: streamIO}}}
			muxed, _ := (&observedMultiplexer{Multiplexer: delegate, id: "/yamux/1.0.0"}).NewConn(secure, true, nil)
			stream, _ := muxed.AcceptStream()
			wire := upgradeWire("/multistream/1.0.0", "/ipfs/id/1.0.0")
			streamIO.input = append([]byte{}, wire...)
			io.ReadFull(stream, make([]byte, len(wire)))
			body := upgradeWire("bounded-response")
			if mode == "selected-only" {
				body = nil
			}
			if mode == "truncated" {
				body = body[:len(body)-1]
			}
			if mode == "oversized" {
				body = binary.AppendUvarint(nil, 4097)
			}
			if mode == "write-error" {
				streamIO.writeErr = io.ErrClosedPipe
			}
			stream.Write(append(wire, body...)) // Selection and body can share the same delegated Write.
			if mode == "reset" {
				stream.Reset()
			} else if mode == "close-write" {
				stream.CloseWrite()
			} else if mode != "no-close" {
				stream.Close()
			}
			raw.Close()
			proof, err := o.finish("/ipfs/id/1.0.0", "", network.DirInbound)
			want := mode == "complete" || mode == "close-write"
			if (err == nil) != want {
				t.Fatalf("mode=%s complete=%v err=%v", mode, proof.Complete, err)
			}
			if want {
				s := proof.Connections[0].Streams[0]
				if !s.ResponseWriteComplete || s.ApplicationIOComplete || s.WriteFrames != 1 || s.NetworkStreamID != "" || len(s.WriteSHA256) != 64 {
					t.Fatal("inbound completion invented network identity or lost bounded frame evidence")
				}
				encoded, _ := json.Marshal(proof)
				if bytes.Contains(encoded, []byte("bounded-response")) {
					t.Fatal("body retained")
				}
			}
		})
	}
}

func TestUpgradeBodyDigestFragmentationAndMismatch(t *testing.T) {
	wire := append(upgradeWire("one"), upgradeWire("two")...)
	var reference framedBodyDigest
	reference.feed(wire, 4096)
	var fragmented framedBodyDigest
	for _, b := range wire {
		fragmented.feed([]byte{b}, 4096)
	}
	if !fragmented.complete() || fragmented.frames != 2 || fragmented.sum() != reference.sum() {
		t.Fatal("fragmented framing/digest mismatch")
	}
	fragmented.feed([]byte{0}, 4096)
	if fragmented.complete() {
		t.Fatal("invalid trailing frame accepted")
	}
}

func completedUpgradeResponse(t *testing.T, o *upgradeObserver, remote peer.ID, direction network.Direction, closeErr error) upgradeTarget {
	t.Helper()
	_, raw, secure := securityFixtureOnObserver(t, o, "/noise", true, direction, remote)
	streamIO := &fakeRawConn{}
	delegate := &fakeMultiplexer{result: &fakeMuxedConn{stream: &fakeMuxedStream{io: streamIO}}}
	muxed, err := (&observedMultiplexer{Multiplexer: delegate, id: "/yamux/1.0.0"}).NewConn(secure, direction == network.DirInbound, nil)
	if err != nil {
		t.Fatal(err)
	}
	stream, err := muxed.AcceptStream()
	if err != nil {
		t.Fatal(err)
	}
	negotiation(t, stream, streamIO, "/ipfs/id/1.0.0")
	if _, err := stream.Write(upgradeWire("actual-framed-response")); err != nil {
		t.Fatal(err)
	}
	if err := stream.Close(); err != nil {
		t.Fatal(err)
	}
	raw.Conn.(*fakeRawConn).closeErr = closeErr
	raw.Close()
	return upgradeTarget{connection: raw.trace.evidence.ID, stream: stream.(*observedMuxedStream).selection.stream}
}

func TestUpgradeFinishRetainsFailedBackgroundWithoutRejectingTarget(t *testing.T) {
	o := &upgradeObserver{}
	target := completedUpgradeResponse(t, o, peer.ID("remote"), network.DirInbound, nil)
	base := &fakeRawConn{}
	raw := &observedRawConn{Conn: base, trace: o.connection(base, network.DirInbound)}
	negotiation(t, raw, base, "/tls/1.0.0")
	delegate := &fakeSecurity{selected: "/tls/1.0.0", err: io.EOF}
	_, err := (&observedSecurity{SecureTransport: delegate}).SecureInbound(context.Background(), raw, "")
	if err != io.EOF {
		t.Fatal("background error changed")
	}
	raw.Close()
	for _, targets := range [][]upgradeTarget{nil, {target}} {
		proof, err := o.finish("/ipfs/id/1.0.0", peer.ID("remote").String(), network.DirInbound, targets...)
		if err != nil || !proof.Complete || proof.TargetConnection != target.connection ||
			len(proof.TargetStreams) != 1 || proof.TargetStreams[0] != target.stream {
			t.Fatalf("target rejected: %+v %v", proof, err)
		}
		if len(proof.Connections) != 2 || !proof.Connections[1].Failed {
			t.Fatal("background trace removed or sanitized")
		}
		found := false
		for _, e := range proof.Connections[1].Events {
			if e.Kind == "failure" && e.Phase == "security" && e.Error == io.EOF.Error() {
				found = true
			}
		}
		if !found {
			t.Fatal("background failure evidence lost")
		}
	}
	o.mu.Lock()
	o.overflow = true
	o.mu.Unlock()
	if proof, err := o.finish("/ipfs/id/1.0.0", "", network.DirInbound, target); err == nil || proof.Complete {
		t.Fatal("global overflow ceased to block proof")
	}
}

func TestUpgradeFinishFailedTargetCannotBorrowBackgroundSuccess(t *testing.T) {
	for _, mode := range []string{"wrong-remote", "wrong-direction", "same-peer-explicit"} {
		t.Run(mode, func(t *testing.T) {
			o := &upgradeObserver{}
			target := completedUpgradeResponse(t, o, peer.ID("remote"), network.DirInbound, io.ErrClosedPipe)
			remote, direction := peer.ID("remote"), network.DirInbound
			if mode == "wrong-remote" {
				remote = peer.ID("other")
			}
			if mode == "wrong-direction" {
				direction = network.DirOutbound
			}
			completedUpgradeResponse(t, o, remote, direction, nil)
			proof, err := o.finish("/ipfs/id/1.0.0", peer.ID("remote").String(), network.DirInbound, target)
			if err == nil || proof.Complete || len(proof.Connections) != 2 || !proof.Connections[0].Failed {
				t.Fatal("failed explicit target borrowed another connection")
			}
			if mode != "same-peer-explicit" {
				if _, err := o.finish("/ipfs/id/1.0.0", peer.ID("remote").String(), network.DirInbound); err == nil {
					t.Fatal("remote/direction filter borrowed background success")
				}
			}
		})
	}
}

func TestUpgradeFinishMissingAndAmbiguousTargetFailClosed(t *testing.T) {
	o := &upgradeObserver{}
	first := completedUpgradeResponse(t, o, peer.ID("remote"), network.DirInbound, nil)
	for _, target := range []upgradeTarget{{}, {connection: first.connection + 1, stream: first.stream},
		{connection: first.connection, stream: first.stream + 1}} {
		if proof, err := o.finish("/ipfs/id/1.0.0", "", network.DirInbound, target); err == nil || proof.Complete {
			t.Fatalf("missing target accepted: %+v", target)
		}
	}
	completedUpgradeResponse(t, o, peer.ID("remote"), network.DirInbound, nil)
	if proof, err := o.finish("/ipfs/id/1.0.0", "", network.DirInbound); err == nil || proof.Complete ||
		proof.TargetConnection != 0 || len(proof.TargetStreams) != 0 {
		t.Fatal("ambiguous connections silently selected")
	}
	if proof, err := o.finish("/ipfs/id/1.0.0", "", network.DirInbound, first); err != nil || proof.TargetConnection != first.connection {
		t.Fatal("explicit target lost among other successful connections")
	}
}

type fakeExchangeHost struct {
	host.Host
	stream network.Stream
}

func (h *fakeExchangeHost) NewStream(context.Context, peer.ID, ...protocol.ID) (network.Stream, error) {
	return h.stream, nil
}

func TestUpgradeIdentifyReadBoundAndSeparateEventConnection(t *testing.T) {
	conn := &fakeNetworkConn{id: "explicit", remote: peer.ID("remote")}
	for _, size := range []int{0, 4096, 4097} {
		s := &fakeNetworkStream{connection: conn, selected: "/ipfs/id/1.0.0", streamID: "explicit-stream",
			data: &fakeRawConn{input: bytes.Repeat([]byte{'x'}, size)}}
		exchange, err := openRequiredProtocol(context.Background(), &fakeExchangeHost{stream: s}, conn.remote, s.selected)
		if (err == nil) != (size == 4096) {
			t.Fatalf("bound %d: %v", size, err)
		}
		if size == 4096 && exchange.bytes != size {
			t.Fatal("wrong actual payload size")
		}
	}
	exchange := protocolExchange{connection: conn, streamID: "explicit-stream"}
	identified := event.EvtPeerIdentificationCompleted{Peer: conn.remote, Conn: conn}
	if !identifyOnExchangeConnection(identified, exchange, conn.remote) {
		t.Fatal("same connection rejected")
	}
	identified.Conn = &fakeNetworkConn{id: "different", remote: conn.remote}
	if identifyOnExchangeConnection(identified, exchange, conn.remote) {
		t.Fatal("same peer/different connection conflated")
	}
	identified.Conn = nil
	if identifyOnExchangeConnection(identified, exchange, conn.remote) {
		t.Fatal("missing event connection accepted")
	}
}

func TestUpgradeAutomaticIdentifyRequiresSameConnectionAndLiveSubscription(t *testing.T) {
	conn := &fakeNetworkConn{id: "echo-connection", remote: peer.ID("remote")}
	exchange := protocolExchange{connection: conn, streamID: "echo-stream"}
	identified := event.EvtPeerIdentificationCompleted{
		Peer: conn.remote, Conn: conn, SignedPeerRecord: &record.Envelope{},
	}
	for _, name := range []string{"matching", "other-connection", "unsigned", "closed", "canceled"} {
		t.Run(name, func(t *testing.T) {
			events := make(chan interface{}, 2)
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			result := make(map[string]any)
			switch name {
			case "matching":
				other := identified
				other.Conn = &fakeNetworkConn{id: "another", remote: conn.remote}
				events <- other
				events <- identified
			case "other-connection":
				other := identified
				other.Conn = &fakeNetworkConn{id: "another", remote: conn.remote}
				events <- other
			case "unsigned":
				unsigned := identified
				unsigned.SignedPeerRecord = nil
				events <- unsigned
			case "canceled":
				cancel()
			}
			if name != "canceled" {
				close(events)
			}
			err := recordAutomaticIdentify(ctx, result, events, exchange, conn.remote)
			if name == "matching" {
				if err != nil || result["identify_event_connection_id"] != conn.id || result["signed_peer_record"] != true {
					t.Fatalf("missing matching Identify evidence: %v %v", result, err)
				}
			} else if err == nil || len(result) != 0 {
				t.Fatalf("invalid Identify evidence accepted: %v %v", result, err)
			}
		})
	}
}
