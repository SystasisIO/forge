package main

import (
	"bytes"
	"io"
	"testing"

	"github.com/libp2p/go-libp2p/core/host"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/core/protocol"
	ma "github.com/multiformats/go-multiaddr"
)

type privateEchoHost struct {
	host.Host
	handler network.StreamHandler
}

func (h *privateEchoHost) SetStreamHandler(_ protocol.ID, handler network.StreamHandler) {
	h.handler = handler
}

type privateEchoStream struct {
	network.Stream
	reader        *bytes.Reader
	written       bytes.Buffer
	writeErr      error
	closed, reset bool
}

func (s *privateEchoStream) Read(data []byte) (int, error) { return s.reader.Read(data) }
func (s *privateEchoStream) Write(data []byte) (int, error) {
	if s.writeErr != nil {
		return 0, s.writeErr
	}
	return s.written.Write(data)
}
func (s *privateEchoStream) Close() error { s.closed = true; return nil }
func (s *privateEchoStream) Reset() error { s.reset = true; return nil }

func TestPrivateDialerHandlesActualReverseEchoBeforeClose(t *testing.T) {
	packet := publicFramed([]byte("private-receipt"))
	for _, failure := range []string{"", "truncated", "write"} {
		h, state := &privateEchoHost{}, &pnetConnectionState{}
		installEchoHandler(h, state)
		request := packet
		if failure == "truncated" {
			request = packet[:2]
		}
		s := &privateEchoStream{reader: bytes.NewReader(request)}
		if failure == "write" {
			s.writeErr = io.ErrClosedPipe
		}
		h.handler(s)
		if !s.closed || state.applicationStreams.Load() != 1 {
			t.Fatal("reverse handler did not own/close the stream")
		}
		if failure == "" && (s.reset || !bytes.Equal(s.written.Bytes(), packet)) {
			t.Fatal("actual reverse frame was not echoed")
		}
		if failure != "" && !s.reset {
			t.Fatal("failed reverse exchange was not reset")
		}
	}
}

// Synthetic adapter values are not a live handshake proof.
type privateReceiptConn struct {
	network.Conn
	state network.ConnectionState
}

func (c *privateReceiptConn) ConnState() network.ConnectionState { return c.state }
func (*privateReceiptConn) ID() string                           { return "actual-connection" }
func (*privateReceiptConn) LocalPeer() peer.ID                   { return peer.ID("local") }
func (*privateReceiptConn) RemotePeer() peer.ID                  { return peer.ID("remote") }
func (*privateReceiptConn) LocalMultiaddr() ma.Multiaddr {
	return ma.StringCast("/ip4/127.0.0.1/tcp/4100")
}
func (*privateReceiptConn) RemoteMultiaddr() ma.Multiaddr {
	return ma.StringCast("/ip4/127.0.0.1/tcp/4200")
}

func TestPrivateReceiptUsesActualConnState(t *testing.T) {
	c := &privateReceiptConn{state: network.ConnectionState{Security: "/tls/1.0.0", StreamMultiplexer: "/yamux/1.0.0", Transport: "tcp", UsedEarlyMuxerNegotiation: true}}
	receipt := endpointConnectionReceipt(c)
	if receipt["security"] != "/tls/1.0.0" || receipt["muxer"] != "/yamux/1.0.0" ||
		receipt["connection_id"] != "actual-connection" || receipt["early_muxer_negotiation"] != true {
		t.Fatal("native ConnState was not retained")
	}
	c.state.Security, c.state.StreamMultiplexer, c.state.UsedEarlyMuxerNegotiation = "", "", false
	receipt = endpointConnectionReceipt(c)
	if receipt["security"] != "" || receipt["muxer"] != "" || receipt["early_muxer_negotiation"] != false {
		t.Fatal("configured defaults substituted for actual state")
	}
}

func TestPrivatePublicFrameBounds(t *testing.T) {
	good := publicFramed([]byte("private-profile-exchange"))
	got, err := readPrivateFrame(bytes.NewReader(good))
	if err != nil || !bytes.Equal(got, good) {
		t.Fatal("canonical public frame was rejected")
	}
	for _, bad := range [][]byte{{0}, {0x81, 0, 'x'}, {3, 'x'}, {0xff, 0xff, 0xff, 1}, {0x81, 0x40}} {
		if _, err := readPrivateFrame(bytes.NewReader(bad)); err == nil {
			t.Fatal("invalid bounded frame was accepted")
		}
	}
	if _, err := readPrivateFrame(io.LimitReader(bytes.NewReader(good), 1)); err == nil {
		t.Fatal("truncated frame was accepted")
	}
}

func TestPrivateProtocolCompletionRequiresActualBodiesAndBinding(t *testing.T) {
	for _, id := range []string{"/ipfs/ping/1.0.0", "/ipfs/kad/1.0.0"} {
		o := &upgradeObserver{}
		connection := o.connection(&fakeRawConn{}, network.DirOutbound)
		connection.privateProfile = true
		s := &selectionTrace{connection: connection, stream: 1, selected: id, direction: network.DirOutbound.String(),
			applicationDone: true, boundID: "actual-stream", writeClosed: true}
		packet := publicFramed([]byte{8, 4})
		if id == "/ipfs/ping/1.0.0" {
			packet = bytes.Repeat([]byte{'p'}, 32)
		}
		s.bodyLocked(true, packet, id)
		s.bodyLocked(false, packet, id)
		if !s.completedLocked() {
			t.Fatal("private protocol I/O completion was lost")
		}
		s.applicationDone = false
		if s.completedLocked() {
			t.Fatal("unbound protocol I/O was accepted")
		}
		s.applicationDone, s.writeBody.failed = true, true
		if s.completedLocked() {
			t.Fatal("failed framing was accepted")
		}
	}
}

func TestPathSharedEntrypointRejectsUnknownOrDuplicateFlags(t *testing.T) {
	for _, argv := range [][]string{{"--unknown", "value"}, {"--scenario"}, {"--scenario", "dcutr", "--scenario", "dcutr"}} {
		if _, err := parsePathArgs(argv); err == nil {
			t.Fatal("invalid shared path argv accepted")
		}
	}
}
