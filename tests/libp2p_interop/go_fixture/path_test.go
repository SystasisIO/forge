package main

import (
	"bytes"
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"strconv"
	"testing"
	"time"

	"github.com/libp2p/go-libp2p/core/crypto"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/core/protocol"
	"github.com/libp2p/go-libp2p/p2p/protocol/holepunch"
	ma "github.com/multiformats/go-multiaddr"
	msmux "github.com/multiformats/go-multistream"
	quic "github.com/quic-go/quic-go"
)

// Native owner markers and I/O doubles exercise capture only, not live QUIC
// authentication or DCUtR acceptance. Identity helpers use real in-memory keys.
type pathTestNativeQUIC struct {
	network.Conn
	security  *pathTestSecureConn
	native    *quic.Conn
	local     ma.Multiaddr
	remote    ma.Multiaddr
	statReads int
}

func newPathTestNativeQUIC(t *testing.T) *pathTestNativeQUIC {
	t.Helper()
	security := newPathTestSecure(t)
	security.state = network.ConnectionState{Transport: "quic-v1"}
	return &pathTestNativeQUIC{security: security, native: new(quic.Conn),
		local:  ma.StringCast("/ip4/10.2.0.2/udp/37540/quic-v1"),
		remote: ma.StringCast("/ip4/11.0.0.2/udp/53496/quic-v1")}
}

func (c *pathTestNativeQUIC) LocalPeer() peer.ID                 { return c.security.LocalPeer() }
func (c *pathTestNativeQUIC) RemotePeer() peer.ID                { return c.security.RemotePeer() }
func (c *pathTestNativeQUIC) RemotePublicKey() crypto.PubKey     { return c.security.RemotePublicKey() }
func (c *pathTestNativeQUIC) ConnState() network.ConnectionState { return c.security.ConnState() }
func (c *pathTestNativeQUIC) LocalMultiaddr() ma.Multiaddr       { return c.local }
func (c *pathTestNativeQUIC) RemoteMultiaddr() ma.Multiaddr      { return c.remote }
func (*pathTestNativeQUIC) ID() string                           { return "circuit-owner" }
func (c *pathTestNativeQUIC) Stat() network.ConnStats {
	c.statReads++
	return network.ConnStats{Stats: network.Stats{Direction: network.DirOutbound}}
}
func (c *pathTestNativeQUIC) As(target any) bool {
	if native, ok := target.(**quic.Conn); ok && c.native != nil {
		*native = c.native
		return true
	}
	return false
}

func TestPathQUICSecurityUsesSecuredCallbackNotLogicalDirection(t *testing.T) {
	for _, direction := range []network.Direction{network.DirInbound, network.DirOutbound} {
		conn := newPathTestNativeQUIC(t)
		gater := &pathDialObserver{}
		if !gater.InterceptSecured(direction, conn.RemotePeer(), conn) {
			t.Fatal("passive observation changed native admission")
		}
		secured, err := gater.quicSecurity(conn)
		want := "server"
		if direction == network.DirOutbound {
			want = "client"
		}
		if err != nil || secured.role() != want || conn.statReads != 0 {
			t.Fatalf("role=%q logical stat reads=%d err=%v", secured.role(), conn.statReads, err)
		}
		receipt := secured.receipt()
		if receipt["source"] != "go.quic.transport.InterceptSecured" ||
			receipt["native_connection_basis"] != "network.Conn.As(**quic.Conn)" ||
			receipt["security_role"] != want || receipt["remote_peer_id"] != conn.RemotePeer().String() ||
			receipt["local_address"] != conn.local.String() || receipt["remote_address"] != conn.remote.String() ||
			secured.observedMonoNS <= 0 || secured.id == "" {
			t.Fatal("secured receipt is not bound to its authenticated native owner")
		}
	}
}

func TestPathQUICSecurityRejectsMissingForeignOrConflictingCapture(t *testing.T) {
	for _, name := range []string{"missing-callback", "foreign-native", "missing-native", "foreign-local", "foreign-remote",
		"foreign-peer", "foreign-key", "wrong-transport", "conflicting-direction", "invalid-direction", "callback-peer"} {
		t.Run(name, func(t *testing.T) {
			conn := newPathTestNativeQUIC(t)
			gater := &pathDialObserver{}
			if name != "missing-callback" && !gater.InterceptSecured(network.DirInbound, conn.RemotePeer(), conn) {
				t.Fatal("passive callback denied admission")
			}
			switch name {
			case "foreign-native":
				conn.native = new(quic.Conn) // Same peer/tuple is not native owner identity.
			case "missing-native":
				conn.native = nil
			case "foreign-local":
				conn.local = ma.StringCast("/ip4/10.2.0.2/udp/37541/quic-v1")
			case "foreign-remote":
				conn.remote = ma.StringCast("/ip4/11.0.0.2/udp/53497/quic-v1")
			case "foreign-peer":
				_, _, conn.security.remote = pathTestIdentity(t)
			case "foreign-key":
				_, conn.security.key, _ = pathTestIdentity(t)
			case "wrong-transport":
				conn.security.state.Transport = "tcp"
			case "conflicting-direction":
				if !gater.InterceptSecured(network.DirOutbound, conn.RemotePeer(), conn) {
					t.Fatal("conflicting capture changed admission")
				}
			case "invalid-direction":
				if !gater.InterceptSecured(network.DirUnknown, conn.RemotePeer(), conn) {
					t.Fatal("invalid capture changed admission")
				}
			case "callback-peer":
				_, _, another := pathTestIdentity(t)
				if !gater.InterceptSecured(network.DirInbound, another, conn) {
					t.Fatal("invalid identity capture changed admission")
				}
			}
			if _, err := gater.quicSecurity(conn); err == nil || conn.statReads != 0 {
				t.Fatal("missing/foreign capture acquired an invented QUIC security role")
			}
		})
	}
}

func TestPathQUICSecurityRepeatCallbackKeepsSameOwnerReceipt(t *testing.T) {
	conn := newPathTestNativeQUIC(t)
	gater := &pathDialObserver{}
	if !gater.InterceptSecured(network.DirInbound, conn.RemotePeer(), conn) {
		t.Fatal("passive callback denied admission")
	}
	first, err := gater.quicSecurity(conn)
	if err != nil {
		t.Fatal(err)
	}
	if !gater.InterceptSecured(network.DirInbound, conn.RemotePeer(), conn) {
		t.Fatal("repeated callback denied admission")
	}
	second, err := gater.quicSecurity(conn)
	if err != nil || len(gater.secured) != 1 || first.id != second.id || first.observedMonoNS != second.observedMonoNS {
		t.Fatal("repeated callback replaced the actual native observation")
	}
	if (pathQUICSecured{}).role() != "" {
		t.Fatal("unknown callback direction invented a security role")
	}
}

func TestPathQUICSecurityCaptureIsBounded(t *testing.T) {
	conn := newPathTestNativeQUIC(t)
	gater := &pathDialObserver{}
	for i := 0; i <= pathEventLimit; i++ {
		conn.native = new(quic.Conn)
		if !gater.InterceptSecured(network.DirInbound, conn.RemotePeer(), conn) {
			t.Fatal("capture bound changed admission")
		}
	}
	if len(gater.secured) != pathEventLimit || gater.captureErr == nil {
		t.Fatal("unbounded/undetected QUIC secured capture overflow")
	}
	if _, err := gater.quicSecurity(conn); err == nil {
		t.Fatal("overflow acquired an authenticated role receipt")
	}
}

type pathTestDCUTRStream struct {
	network.Stream
	conn       network.Conn
	readData   []byte
	readErr    error
	writeErr   error
	writeBytes int
	written    []byte
}

func (s *pathTestDCUTRStream) ID() string         { return "retry-stream" }
func (s *pathTestDCUTRStream) Conn() network.Conn { return s.conn }
func (s *pathTestDCUTRStream) Read(p []byte) (int, error) {
	n := copy(p, s.readData)
	s.readData = s.readData[n:]
	return n, s.readErr
}
func (s *pathTestDCUTRStream) Write(p []byte) (int, error) {
	s.written = bytes.Clone(p)
	n := len(p)
	if s.writeBytes >= 0 {
		n = s.writeBytes
	}
	return n, s.writeErr
}

func pathTestConnectFrame(t *testing.T) []byte {
	t.Helper()
	// A complete native CONNECT frame captured in the root14 donor fixture.
	frame, err := hex.DecodeString("0f0864120b040b000003910292a4cd03")
	if err != nil {
		t.Fatal(err)
	}
	return frame
}

func TestPathTerminalReadPreservesNativeBytesErrorAndFrameOrder(t *testing.T) {
	wire := pathTestConnectFrame(t)
	reset := &network.StreamError{ErrorCode: network.StreamProtocolViolation, Remote: true}
	for _, test := range []struct {
		name, kind         string
		data               []byte
		err                error
		completed, pending int
	}{
		{name: "complete-with-reset", kind: "reset", data: wire, err: reset, completed: 1},
		{name: "partial-with-EOF", kind: "eof", data: wire[:1], err: io.EOF, pending: 1},
		{name: "canceled", kind: "canceled", err: context.Canceled},
		{name: "deadline", kind: "deadline", err: context.DeadlineExceeded},
		{name: "other-error", kind: "io_error", err: errors.New("native I/O failure")},
	} {
		t.Run(test.name, func(t *testing.T) {
			conn := newPathTestNativeQUIC(t)
			o, _ := newPathObserver("0123456789abcdef0123456789abcdef", conn.LocalPeer())
			native := &pathTestDCUTRStream{conn: conn, readData: test.data, readErr: test.err}
			stream := &pathObservedStream{Stream: native, observer: o}
			buffer := make([]byte, len(wire))
			n, err := stream.Read(buffer)
			if n != len(test.data) || !bytes.Equal(buffer[:n], test.data) || err != test.err || len(o.events) != test.completed+1 {
				t.Fatalf("native I/O changed: n=%d err=%v events=%v", n, err, o.events)
			}
			terminal := o.events[len(o.events)-1]
			if terminal["kind"] != "dcutr_stream_terminal" || terminal["direction"] != "read" ||
				terminal["connection_id"] != conn.ID() || terminal["stream_id"] != native.ID() ||
				terminal["remote_peer_id"] != conn.RemotePeer().String() || terminal["protocol"] != string(holepunch.Protocol) ||
				terminal["error_kind"] != test.kind || terminal["error"] != test.err.Error() ||
				terminal["io_bytes"] != json.Number(strconv.Itoa(n)) ||
				terminal["completed_frame_count"] != json.Number(strconv.Itoa(test.completed)) ||
				terminal["pending_frame_bytes"] != json.Number(strconv.Itoa(test.pending)) ||
				terminal["invalid_or_over_limit"] != false {
				t.Fatal("terminal receipt lost its actual stream/I/O binding", terminal)
			}
			if test.completed > 0 && o.events[0]["kind"] != "dcutr_frame" {
				t.Fatal("terminal receipt preceded completed bytes from the same native read")
			}
			if _, err := stream.Read(make([]byte, len(wire))); err != test.err || len(o.events) != test.completed+1 {
				t.Fatal("repeated native error duplicated a terminal receipt or changed I/O")
			}
		})
	}
}

func TestPathTerminalRetryDoesNotFabricateResponseOrSYNC(t *testing.T) {
	conn := newPathTestNativeQUIC(t)
	o, _ := newPathObserver("0123456789abcdef0123456789abcdef", conn.LocalPeer())
	native := &pathTestDCUTRStream{conn: conn, readErr: network.ErrReset, writeBytes: -1}
	stream := &pathObservedStream{Stream: native, observer: o}
	wire := pathTestConnectFrame(t)
	if n, err := stream.Write(wire); n != len(wire) || err != nil || !bytes.Equal(native.written, wire) {
		t.Fatal("native CONNECT write changed")
	}
	if n, err := stream.Read(make([]byte, 64)); n != 0 || err != network.ErrReset {
		t.Fatal("native retry reset changed")
	}
	if len(o.events) != 2 || o.events[0]["kind"] != "dcutr_frame" || o.events[0]["direction"] != "write" ||
		o.events[0]["message_type"] != json.Number("100") || o.events[1]["kind"] != "dcutr_stream_terminal" ||
		o.events[1]["completed_frame_count"] != json.Number("0") ||
		o.events[0]["stream_id"] != o.events[1]["stream_id"] {
		t.Fatal("proper-prefix retry was completed synthetically or lost its reset receipt", o.events)
	}
}

func TestPathTerminalWritePreservesShortWriteAndError(t *testing.T) {
	conn := newPathTestNativeQUIC(t)
	o, _ := newPathObserver("0123456789abcdef0123456789abcdef", conn.LocalPeer())
	native := &pathTestDCUTRStream{conn: conn, writeBytes: 2, writeErr: network.ErrReset}
	stream := &pathObservedStream{Stream: native, observer: o}
	wire := pathTestConnectFrame(t)
	if n, err := stream.Write(wire); n != 2 || err != network.ErrReset || !bytes.Equal(native.written, wire) {
		t.Fatal("native short write/error changed")
	}
	if len(o.events) != 1 || o.events[0]["kind"] != "dcutr_stream_terminal" || o.events[0]["direction"] != "write" ||
		o.events[0]["io_bytes"] != json.Number("2") || o.events[0]["pending_frame_bytes"] != json.Number("2") ||
		o.events[0]["completed_frame_count"] != json.Number("0") {
		t.Fatal("partial native write became a complete frame", o.events)
	}
}

func TestPathSuccessfulIODoesNotInventTerminalReceipt(t *testing.T) {
	conn := newPathTestNativeQUIC(t)
	o, _ := newPathObserver("0123456789abcdef0123456789abcdef", conn.LocalPeer())
	wire := pathTestConnectFrame(t)
	native := &pathTestDCUTRStream{conn: conn, readData: wire, writeBytes: -1}
	stream := &pathObservedStream{Stream: native, observer: o}
	if n, err := stream.Read(make([]byte, len(wire))); n != len(wire) || err != nil {
		t.Fatal("successful native read changed")
	}
	if n, err := stream.Write(wire); n != len(wire) || err != nil || len(o.events) != 2 {
		t.Fatal("successful native write changed")
	}
	for _, event := range o.events {
		if event["kind"] != "dcutr_frame" {
			t.Fatal("successful native I/O fabricated a terminal receipt")
		}
	}
}

func TestPathHolePunchReadyUsesRegisteredProtocol(t *testing.T) {
	mux := msmux.NewMultistreamMuxer[protocol.ID]()
	mux.AddHandler(pathEchoProtocol, func(protocol.ID, io.ReadWriteCloser) error { return nil })
	mux.AddHandler(holepunch.Protocol, func(protocol.ID, io.ReadWriteCloser) error { return nil })
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	reads := 0
	registered, err := waitPathHolePunchReady(ctx, func() []protocol.ID {
		reads++
		return mux.Protocols()
	})
	if err != nil || reads != 1 || len(registered) != 2 {
		t.Fatalf("registered=%v reads=%d err=%v", registered, reads, err)
	}
	found := false
	for _, id := range registered {
		found = found || id == holepunch.Protocol
	}
	if !found {
		t.Fatal("readiness snapshot lacks actual registered DCUtR protocol")
	}
}

func TestPathHolePunchReadyWaitsForRegistration(t *testing.T) {
	mux := msmux.NewMultistreamMuxer[protocol.ID]()
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	reads := 0
	registered, err := waitPathHolePunchReady(ctx, func() []protocol.ID {
		reads++
		before := mux.Protocols()
		if reads == 1 {
			mux.AddHandler(holepunch.Protocol, func(protocol.ID, io.ReadWriteCloser) error { return nil })
		}
		return before
	})
	if err != nil || reads != 2 || len(registered) != 1 || registered[0] != holepunch.Protocol {
		t.Fatalf("registered=%v reads=%d err=%v", registered, reads, err)
	}
}

func TestPathHolePunchReadyEmptyOrUnrelatedProtocolsTimeOut(t *testing.T) {
	for _, test := range []struct {
		name      string
		protocols []protocol.ID
	}{
		{name: "nil"},
		{name: "empty", protocols: []protocol.ID{}},
		{name: "unrelated", protocols: []protocol.ID{pathEchoProtocol, protocol.ID(string(holepunch.Protocol) + "/other")}},
	} {
		t.Run(test.name, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 25*time.Millisecond)
			defer cancel()
			registered, err := waitPathHolePunchReady(ctx, func() []protocol.ID {
				return test.protocols
			})
			if !errors.Is(err, context.DeadlineExceeded) || registered != nil {
				t.Fatalf("registered=%v err=%v", registered, err)
			}
		})
	}
}

func TestPathHolePunchReadyCanceledWhileWaiting(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	reads := 0
	registered, err := waitPathHolePunchReady(ctx, func() []protocol.ID {
		reads++
		cancel()
		return nil
	})
	if !errors.Is(err, context.Canceled) || registered != nil || reads != 1 {
		t.Fatalf("registered=%v reads=%d err=%v", registered, reads, err)
	}
}

func TestPathHolePunchReadyExpiredContextCannotReportReady(t *testing.T) {
	ctx, cancel := context.WithDeadline(context.Background(), time.Now().Add(-time.Second))
	defer cancel()
	read := false
	registered, err := waitPathHolePunchReady(ctx, func() []protocol.ID {
		read = true
		return []protocol.ID{holepunch.Protocol}
	})
	if !errors.Is(err, context.DeadlineExceeded) || registered != nil || read {
		t.Fatalf("registered=%v read=%v err=%v", registered, read, err)
	}
}

func TestPathFramesAreBoundedAndComplete(t *testing.T) {
	var wire bytes.Buffer
	if err := pathWriteFrame(&wire, []byte("challenge")); err != nil {
		t.Fatal(err)
	}
	got, err := pathReadFrame(&wire)
	if err != nil || string(got) != "challenge" {
		t.Fatalf("%q %v", got, err)
	}
	for _, bad := range [][]byte{{0, 0, 0, 0}, {0, 0, 0, 129}, {0, 0, 0, 2, 1}} {
		if _, err := pathReadFrame(bytes.NewReader(bad)); err == nil {
			t.Fatal("accepted incomplete/over-limit frame")
		}
	}
}

type pathShortWriter struct{}

func (pathShortWriter) Write([]byte) (int, error) { return 0, nil }
func TestPathZeroWriteRejected(t *testing.T) {
	if err := pathWriteFrame(pathShortWriter{}, []byte("x")); err != io.ErrShortWrite {
		t.Fatal(err)
	}
}

func TestPathBarrierDoesNotObserveCircuitDeliveryFromNativeSuccess(t *testing.T) {
	o, err := newPathObserver("0123456789abcdef0123456789abcdef", peer.ID("local"))
	if err != nil {
		t.Fatal(err)
	}
	o.expected = peer.ID("remote")
	o.Trace(&holepunch.Event{Peer: o.local, Remote: o.expected, Type: holepunch.EndHolePunchEvtT,
		Evt: &holepunch.EndHolePunchEvt{Success: true}})
	if o.released || o.relayEcho {
		t.Fatal("synthetic circuit delivery")
	}
}

func TestPathTraceCannotFabricateNormalizedSuccess(t *testing.T) {
	o, _ := newPathObserver("0123456789abcdef0123456789abcdef", peer.ID("local"))
	o.Trace(&holepunch.Event{Peer: o.local, Remote: peer.ID("remote"), Type: holepunch.DirectDialEvtT,
		Evt: &holepunch.DirectDialEvt{Success: true}})
	data, _ := o.result(false, false, nil)
	var result map[string]any
	if err := json.Unmarshal(data, &result); err != nil {
		t.Fatal(err)
	}
	if result["finalized"] != false || result["joined"] != false {
		t.Fatal("early join")
	}
	if o.events[0]["kind"] != "ordinary_direct_success" {
		t.Fatal("ordinary success hidden")
	}
}

func TestPathObserverOverflow(t *testing.T) {
	o, _ := newPathObserver("0123456789abcdef0123456789abcdef", peer.ID("local"))
	for i := 0; i <= pathEventLimit; i++ {
		o.record("capture", "go.native", nil)
	}
	if !o.overflow || len(o.events) != pathEventLimit {
		t.Fatal("unbounded capture")
	}
}
