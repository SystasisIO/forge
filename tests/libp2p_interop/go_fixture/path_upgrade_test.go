package main

import (
	"bytes"
	"context"
	"crypto/rand"
	"errors"
	"io"
	"net"
	"strings"
	"testing"
	"time"

	libp2p "github.com/libp2p/go-libp2p"
	"github.com/libp2p/go-libp2p/core/crypto"
	"github.com/libp2p/go-libp2p/core/host"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/core/protocol"
	"github.com/libp2p/go-libp2p/core/sec"
	"github.com/libp2p/go-libp2p/p2p/muxer/yamux"
	"github.com/libp2p/go-libp2p/p2p/security/noise"
	libp2ptls "github.com/libp2p/go-libp2p/p2p/security/tls"
	ma "github.com/multiformats/go-multiaddr"
)

// Callback doubles test observation and delegation, not native crypto or live
// path acceptance. Their peer IDs are derived from real in-memory public keys.
func pathTestIdentity(t *testing.T) (crypto.PrivKey, crypto.PubKey, peer.ID) {
	t.Helper()
	priv, pub, err := crypto.GenerateEd25519Key(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	id, err := peer.IDFromPublicKey(pub)
	if err != nil {
		t.Fatal(err)
	}
	return priv, pub, id
}

type pathTestSocket struct {
	written   []byte
	closed    int
	deadlines [3]time.Time
	err       error
}

func (c *pathTestSocket) Read(p []byte) (int, error) { return copy(p, "native read"), io.EOF }
func (c *pathTestSocket) Write(p []byte) (int, error) {
	c.written = bytes.Clone(p)
	return len(p), c.err
}
func (c *pathTestSocket) Close() error         { c.closed++; return c.err }
func (c *pathTestSocket) LocalAddr() net.Addr  { return &net.TCPAddr{Port: 1234} }
func (c *pathTestSocket) RemoteAddr() net.Addr { return &net.TCPAddr{Port: 5678} }
func (c *pathTestSocket) SetDeadline(v time.Time) error {
	c.deadlines[0] = v
	return c.err
}
func (c *pathTestSocket) SetReadDeadline(v time.Time) error {
	c.deadlines[1] = v
	return c.err
}
func (c *pathTestSocket) SetWriteDeadline(v time.Time) error {
	c.deadlines[2] = v
	return c.err
}

type pathTestSecureConn struct {
	net.Conn
	local, remote peer.ID
	key           crypto.PubKey
	state         network.ConnectionState
}

func (c *pathTestSecureConn) LocalPeer() peer.ID                 { return c.local }
func (c *pathTestSecureConn) RemotePeer() peer.ID                { return c.remote }
func (c *pathTestSecureConn) RemotePublicKey() crypto.PubKey     { return c.key }
func (c *pathTestSecureConn) ConnState() network.ConnectionState { return c.state }

func newPathTestSecure(t *testing.T) *pathTestSecureConn {
	t.Helper()
	_, _, local := pathTestIdentity(t)
	_, key, remote := pathTestIdentity(t)
	return &pathTestSecureConn{Conn: &pathTestSocket{}, local: local, remote: remote, key: key,
		state: network.ConnectionState{StreamMultiplexer: yamux.ID, UsedEarlyMuxerNegotiation: true}}
}

type pathTestSecurityTransport struct {
	id       protocol.ID
	output   sec.SecureConn
	err      error
	ctx      context.Context
	conn     net.Conn
	expected peer.ID
	inbound  bool
	calls    int
	idReads  int
}

func (t *pathTestSecurityTransport) ID() protocol.ID { t.idReads++; return t.id }
func (t *pathTestSecurityTransport) SecureInbound(ctx context.Context, conn net.Conn, expected peer.ID) (sec.SecureConn, error) {
	t.ctx, t.conn, t.expected, t.inbound = ctx, conn, expected, true
	t.calls++
	return t.output, t.err
}
func (t *pathTestSecurityTransport) SecureOutbound(ctx context.Context, conn net.Conn, expected peer.ID) (sec.SecureConn, error) {
	t.ctx, t.conn, t.expected, t.inbound = ctx, conn, expected, false
	t.calls++
	return t.output, t.err
}

func TestPathSecurityCallbacksPreserveNativeFailure(t *testing.T) {
	for _, inbound := range []bool{false, true} {
		for _, partial := range []bool{false, true} {
			native := &pathTestSecurityTransport{id: noise.ID, err: errors.New("native authentication failed")}
			if partial {
				native.output = newPathTestSecure(t)
			}
			observer := &pathSecurityTransport{SecureTransport: native}
			ctx := context.Background()
			socket := &pathTestSocket{}
			var output sec.SecureConn
			var err error
			if inbound {
				output, err = observer.SecureInbound(ctx, socket, "expected")
			} else {
				output, err = observer.SecureOutbound(ctx, socket, "expected")
			}
			if output != native.output || err != native.err || native.calls != 1 || native.idReads != 0 ||
				native.ctx != ctx || native.conn != socket || native.expected != "expected" || native.inbound != inbound {
				t.Fatal("failed native callback was changed or yielded an observation")
			}
			if _, ok := output.(*pathSecureConn); ok {
				t.Fatal("failed authentication yielded a scoped output")
			}
		}
	}
}

func TestPathSecurityCallbacksObserveActualNativeOutput(t *testing.T) {
	for _, id := range []protocol.ID{noise.ID, libp2ptls.ID} {
		for _, inbound := range []bool{false, true} {
			conn := newPathTestSecure(t)
			native := &pathTestSecurityTransport{id: id, output: conn}
			observer := &pathSecurityTransport{SecureTransport: native}
			ctx, socket := context.Background(), &pathTestSocket{}
			var output sec.SecureConn
			var err error
			expected := conn.remote
			if inbound {
				expected = "" // Native inbound may authenticate an initially unknown peer.
				output, err = observer.SecureInbound(ctx, socket, expected)
			} else {
				output, err = observer.SecureOutbound(ctx, socket, expected)
			}
			observed, ok := output.(*pathSecureConn)
			if err != nil || !ok || observed.SecureConn != conn || observed.security != id ||
				observed.local != conn.local || observed.remote != conn.remote || observed.key != conn.key ||
				observed.ConnState() != conn.state || native.ctx != ctx || native.conn != socket ||
				native.expected != expected || native.inbound != inbound || native.calls != 1 || native.idReads != 1 {
				t.Fatalf("native output was not retained exactly: %v", err)
			}
		}
	}
}

func TestPathSecurityCallbacksRejectUnboundOutputs(t *testing.T) {
	for _, name := range []string{"missing-output", "missing-key", "key-peer", "wrong-requested-peer", "missing-outbound-peer", "missing-local", "unsupported-ID", "conflicting-ID"} {
		t.Run(name, func(t *testing.T) {
			conn := newPathTestSecure(t)
			expected := conn.remote
			native := &pathTestSecurityTransport{id: noise.ID, output: conn}
			switch name {
			case "missing-output":
				native.output = nil
			case "missing-key":
				conn.key = nil
			case "key-peer":
				_, conn.key, _ = pathTestIdentity(t)
			case "wrong-requested-peer":
				_, _, expected = pathTestIdentity(t)
			case "missing-outbound-peer":
				expected = ""
			case "missing-local":
				conn.local = ""
			case "unsupported-ID":
				native.id = "/configured-not-native"
			case "conflicting-ID":
				conn.state.Security = libp2ptls.ID
			}
			observer := &pathSecurityTransport{SecureTransport: native}
			if output, err := observer.SecureOutbound(context.Background(), &pathTestSocket{}, expected); err == nil || output != nil {
				t.Fatal("unbound native output yielded an observation")
			}
			if native.output != nil && conn.Conn.(*pathTestSocket).closed != 1 {
				t.Fatal("rejected native output was leaked")
			}
		})
	}
	conn := newPathTestSecure(t)
	observer := &pathSecurityTransport{SecureTransport: &pathTestSecurityTransport{id: noise.ID, output: conn}}
	if output, err := observer.SecureInbound(context.Background(), &pathTestSocket{}, conn.local); err == nil || output != nil {
		t.Fatal("inbound callback accepted a wrong expected peer")
	}
}

func TestPathSecureConnDelegatesSocketOperations(t *testing.T) {
	conn := newPathTestSecure(t)
	socket := conn.Conn.(*pathTestSocket)
	socket.err = errors.New("native socket error")
	observer := &pathSecurityTransport{SecureTransport: &pathTestSecurityTransport{id: noise.ID, output: conn}}
	output, err := observer.SecureOutbound(context.Background(), &pathTestSocket{}, conn.remote)
	if err != nil {
		t.Fatal(err)
	}
	buf := make([]byte, 32)
	if n, err := output.Read(buf); err != io.EOF || string(buf[:n]) != "native read" {
		t.Fatal("Read not delegated")
	}
	if n, err := output.Write([]byte("native write")); n != 12 || err != socket.err || string(socket.written) != "native write" {
		t.Fatal("Write not delegated")
	}
	deadline := time.Unix(42, 0)
	if output.SetDeadline(deadline) != socket.err || output.SetReadDeadline(deadline) != socket.err ||
		output.SetWriteDeadline(deadline) != socket.err || socket.deadlines != [3]time.Time{deadline, deadline, deadline} {
		t.Fatal("deadlines not delegated")
	}
	if output.LocalAddr().String() != socket.LocalAddr().String() || output.RemoteAddr().String() != socket.RemoteAddr().String() ||
		output.Close() != socket.err || socket.closed != 1 {
		t.Fatal("addresses/Close not delegated")
	}
}

type pathTestMuxedConn struct {
	stream network.MuxedStream
	err    error
	ctx    context.Context
	code   network.ConnErrorCode
	calls  []string
}

func (c *pathTestMuxedConn) Close() error { c.calls = append(c.calls, "close"); return c.err }
func (c *pathTestMuxedConn) CloseWithError(code network.ConnErrorCode) error {
	c.code = code
	c.calls = append(c.calls, "close-code")
	return c.err
}
func (c *pathTestMuxedConn) IsClosed() bool { c.calls = append(c.calls, "closed"); return true }
func (c *pathTestMuxedConn) OpenStream(ctx context.Context) (network.MuxedStream, error) {
	c.ctx = ctx
	c.calls = append(c.calls, "open")
	return c.stream, c.err
}
func (c *pathTestMuxedConn) AcceptStream() (network.MuxedStream, error) {
	c.calls = append(c.calls, "accept")
	return c.stream, c.err
}
func (c *pathTestMuxedConn) As(target any) bool {
	c.calls = append(c.calls, "as")
	if p, ok := target.(**pathTestMuxedConn); ok {
		*p = c
		return true
	}
	return false
}

type pathTestMuxer struct {
	output network.MuxedConn
	err    error
	conn   net.Conn
	server bool
	scope  network.PeerScope
	calls  int
}

func (m *pathTestMuxer) NewConn(conn net.Conn, server bool, scope network.PeerScope) (network.MuxedConn, error) {
	m.conn, m.server, m.scope = conn, server, scope
	m.calls++
	return m.output, m.err
}

type pathTestPeerScope struct{ network.PeerScope }
type pathTestMuxedStream struct{ network.MuxedStream }
type pathTestAsLayer struct{ network.MuxedConn }

func newPathTestObserved(t *testing.T, id protocol.ID) (*pathSecureConn, *pathTestSecureConn) {
	t.Helper()
	conn := newPathTestSecure(t)
	transport := &pathSecurityTransport{SecureTransport: &pathTestSecurityTransport{id: id, output: conn}}
	output, err := transport.SecureOutbound(context.Background(), &pathTestSocket{}, conn.remote)
	if err != nil {
		t.Fatal(err)
	}
	return output.(*pathSecureConn), conn
}

func TestPathYamuxCallbackRetainsNativeScopedOutput(t *testing.T) {
	for _, server := range []bool{false, true} {
		for _, early := range []bool{false, true} {
			secure, conn := newPathTestObserved(t, noise.ID)
			if !early {
				conn.state = network.ConnectionState{} // Native multistream fallback still selects this callback.
			}
			native := &pathTestMuxedConn{}
			delegate := &pathTestMuxer{output: native}
			scope := &pathTestPeerScope{}
			output, err := (&pathYamuxTransport{Multiplexer: delegate}).NewConn(secure, server, scope)
			var owner *pathInnerMuxedConn
			if err != nil || output == nil || !output.As(&owner) || owner == nil ||
				owner.MuxedConn != native || owner.secure != secure || owner.muxer != yamux.ID ||
				delegate.conn != secure || delegate.server != server || delegate.scope != scope || delegate.calls != 1 {
				t.Fatalf("native muxer output was not retained: %v", err)
			}
		}
	}
}

func TestPathYamuxCallbackFailureDoesNotCreateOwner(t *testing.T) {
	for _, partial := range []bool{false, true} {
		secure, _ := newPathTestObserved(t, noise.ID)
		delegate := &pathTestMuxer{err: errors.New("native Yamux failed")}
		if partial {
			delegate.output = &pathTestMuxedConn{}
		}
		output, err := (&pathYamuxTransport{Multiplexer: delegate}).NewConn(secure, false, nil)
		if err != delegate.err || output != delegate.output || delegate.calls != 1 {
			t.Fatal("native muxer failure was changed")
		}
		var owner *pathInnerMuxedConn
		if output != nil && output.As(&owner) {
			t.Fatal("failed muxer callback yielded an output owner")
		}
	}
}

func TestPathYamuxCallbackRejectsMissingOrChangedOutputs(t *testing.T) {
	for _, name := range []string{"unscoped", "changed-peer", "changed-key", "wrong-early-muxer", "missing-output"} {
		t.Run(name, func(t *testing.T) {
			secure, conn := newPathTestObserved(t, noise.ID)
			var input net.Conn = secure
			delegate := &pathTestMuxer{output: &pathTestMuxedConn{}}
			switch name {
			case "unscoped":
				input = conn
			case "changed-peer":
				_, _, conn.remote = pathTestIdentity(t)
			case "changed-key":
				_, conn.key, _ = pathTestIdentity(t)
			case "wrong-early-muxer":
				conn.state.StreamMultiplexer = "/other-muxer"
			case "missing-output":
				delegate.output = nil
			}
			if output, err := (&pathYamuxTransport{Multiplexer: delegate}).NewConn(input, false, nil); err == nil || output != nil {
				t.Fatal("missing/changed output yielded an owner")
			}
		})
	}
}

func TestPathInnerMuxedConnDelegatesNativeOperationsAndAs(t *testing.T) {
	secure, _ := newPathTestObserved(t, noise.ID)
	native := &pathTestMuxedConn{stream: &pathTestMuxedStream{}, err: errors.New("native stream result")}
	output, err := (&pathYamuxTransport{Multiplexer: &pathTestMuxer{output: native}}).NewConn(secure, false, nil)
	if err != nil {
		t.Fatal(err)
	}
	ctx := context.Background()
	if stream, err := output.OpenStream(ctx); stream != native.stream || err != native.err || native.ctx != ctx {
		t.Fatal("OpenStream not delegated")
	}
	if stream, err := output.AcceptStream(); stream != native.stream || err != native.err {
		t.Fatal("AcceptStream not delegated")
	}
	if !output.IsClosed() || output.Close() != native.err || output.CloseWithError(network.ConnGated) != native.err || native.code != network.ConnGated {
		t.Fatal("connection lifecycle not delegated")
	}
	var owner *pathInnerMuxedConn
	if !output.As(&owner) || owner != output || output.As((**pathInnerMuxedConn)(nil)) {
		t.Fatal("incorrect scoped As result")
	}
	var actual *pathTestMuxedConn
	if !output.As(&actual) || actual != native || output.As(new(int)) ||
		strings.Join(native.calls, ",") != "open,accept,closed,close,close-code,as,as" {
		t.Fatal("native As delegation/lifecycle was changed")
	}
}

type pathTestNetworkConn struct {
	network.Conn
	native network.ConnSecurity
	muxed  network.MuxedConn
	state  network.ConnectionState
}

func (c *pathTestNetworkConn) LocalPeer() peer.ID                 { return c.native.LocalPeer() }
func (c *pathTestNetworkConn) RemotePeer() peer.ID                { return c.native.RemotePeer() }
func (c *pathTestNetworkConn) RemotePublicKey() crypto.PubKey     { return c.native.RemotePublicKey() }
func (c *pathTestNetworkConn) ConnState() network.ConnectionState { return c.state }
func (c *pathTestNetworkConn) ID() string                         { return "native-connection" }
func (c *pathTestNetworkConn) Stat() network.ConnStats {
	return network.ConnStats{Stats: network.Stats{Direction: network.DirOutbound}}
}
func (c *pathTestNetworkConn) LocalMultiaddr() ma.Multiaddr {
	return ma.StringCast("/ip4/127.0.0.1/udp/5000/quic-v1")
}
func (c *pathTestNetworkConn) RemoteMultiaddr() ma.Multiaddr {
	return ma.StringCast("/ip4/127.0.0.1/udp/4000/quic-v1/p2p/" + c.LocalPeer().String() + "/p2p-circuit/p2p/" + c.RemotePeer().String())
}
func (c *pathTestNetworkConn) As(target any) bool {
	return c.muxed != nil && c.muxed.As(target)
}

func newPathTestCircuit(t *testing.T, id protocol.ID) (*pathTestNetworkConn, *pathInnerMuxedConn) {
	t.Helper()
	secure, native := newPathTestObserved(t, id)
	output, err := (&pathYamuxTransport{Multiplexer: &pathTestMuxer{output: &pathTestMuxedConn{}}}).NewConn(secure, false, nil)
	if err != nil {
		t.Fatal(err)
	}
	owner := output.(*pathInnerMuxedConn)
	// Three transparent As layers mirror the donor transport/circuit/swarm chain.
	wrapped := &pathTestAsLayer{&pathTestAsLayer{&pathTestAsLayer{owner}}}
	return &pathTestNetworkConn{native: native, muxed: wrapped,
		state: network.ConnectionState{Transport: "p2p-circuit"}}, owner
}

func TestPathInnerUpgradeBindsActualOwnerDespiteErasedCircuitState(t *testing.T) {
	for _, id := range []protocol.ID{noise.ID, libp2ptls.ID} {
		c, owner := newPathTestCircuit(t, id)
		security, muxer, err := pathInnerUpgrade(c, c.LocalPeer(), c.RemotePeer())
		if err != nil || security != owner.secure.security || security != id || muxer != yamux.ID ||
			c.state.Security != "" || c.state.StreamMultiplexer != "" {
			t.Fatalf("actual owner was not used: %v", err)
		}
	}
}

func TestPathInnerUpgradeRejectsUnboundOwners(t *testing.T) {
	for _, name := range []string{"no-As-with-configured-IDs", "missing-native-muxer", "missing-native-security", "wrong-muxer", "wrong-expected-peer", "wrong-local-peer", "foreign-outer-peer", "foreign-outer-key", "changed-inner-key", "conflicting-security", "conflicting-muxer"} {
		t.Run(name, func(t *testing.T) {
			c, owner := newPathTestCircuit(t, noise.ID)
			local, remote := c.LocalPeer(), c.RemotePeer()
			switch name {
			case "no-As-with-configured-IDs":
				c.muxed = nil
				c.state.Security, c.state.StreamMultiplexer = noise.ID, yamux.ID
			case "missing-native-muxer":
				owner.MuxedConn = nil
			case "missing-native-security":
				owner.secure = nil
			case "wrong-muxer":
				owner.muxer = "/other-muxer"
			case "wrong-expected-peer":
				_, _, remote = pathTestIdentity(t)
			case "wrong-local-peer":
				_, _, local = pathTestIdentity(t)
			case "foreign-outer-peer":
				c.native = newPathTestSecure(t)
			case "foreign-outer-key":
				foreign := newPathTestSecure(t)
				foreign.local, foreign.remote = local, remote
				c.native = foreign
			case "changed-inner-key":
				_, c.native.(*pathTestSecureConn).key, _ = pathTestIdentity(t)
			case "conflicting-security":
				c.state.Security = libp2ptls.ID
			case "conflicting-muxer":
				c.state.StreamMultiplexer = "/other-muxer"
			}
			if security, muxer, err := pathInnerUpgrade(c, local, remote); err == nil || security != "" || muxer != "" {
				t.Fatal("unbound native output yielded negotiated protocol claims")
			}
		})
	}
}

type pathTestNetwork struct {
	network.Network
	conn network.Conn
}

func (n *pathTestNetwork) ConnsToPeer(peer.ID) []network.Conn { return []network.Conn{n.conn} }

type pathTestHost struct {
	host.Host
	network network.Network
}

func (h *pathTestHost) Network() network.Network { return h.network }

func TestPathObserverRetainsDiagnosticAndDoesNotSkipFailedOwner(t *testing.T) {
	c, _ := newPathTestCircuit(t, noise.ID)
	wrapped := c.muxed
	c.muxed = nil
	h := &pathTestHost{network: &pathTestNetwork{conn: c}}
	o, err := newPathObserver("0123456789abcdef0123456789abcdef", c.LocalPeer())
	if err != nil {
		t.Fatal(err)
	}
	o.expected = c.RemotePeer()
	for i := 0; i < 2; i++ {
		err := o.observeConnections(h, c.LocalPeer().String())
		if err == nil || !strings.Contains(err.Error(), `security="" muxer="" transport="p2p-circuit"`) ||
			len(o.events) != 0 || o.connections[c.ID()] {
			t.Fatal("missing native owner was hidden, fabricated or incorrectly marked observed")
		}
	}
	c.muxed = wrapped
	if err := o.observeConnections(h, c.LocalPeer().String()); err != nil {
		t.Fatal(err)
	}
	if len(o.events) != 1 || o.events[0]["security"] != noise.ID || o.events[0]["muxer"] != yamux.ID ||
		o.events[0]["authentication_basis"] != "native_relay_inner_upgrade" || !o.connections[c.ID()] {
		t.Fatal("successful native owner was not recorded")
	}
	if err := o.observeConnections(h, c.LocalPeer().String()); err != nil || len(o.events) != 1 {
		t.Fatal("native owner recorded twice")
	}
}

func TestPathSecurityConstructorsUseNativeDelegates(t *testing.T) {
	key, _, _ := pathTestIdentity(t)
	noiseDelegate, err := newPathNoise(noise.ID, key, nil)
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := noiseDelegate.SecureTransport.(*noise.Transport); !ok || noiseDelegate.ID() != noise.ID {
		t.Fatal("non-native Noise delegate")
	}
	tlsDelegate, err := newPathTLS(libp2ptls.ID, key, nil)
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := tlsDelegate.SecureTransport.(*libp2ptls.Transport); !ok || tlsDelegate.ID() != libp2ptls.ID {
		t.Fatal("non-native TLS delegate")
	}
}

func TestPathUpgradeOptionsKeepNativeDefaultsAndDelegates(t *testing.T) {
	var observed, defaults libp2p.Config
	if err := observed.Apply(pathUpgradeOptions()...); err != nil {
		t.Fatal(err)
	}
	if err := defaults.Apply(libp2p.DefaultSecurity, libp2p.DefaultMuxers); err != nil {
		t.Fatal(err)
	}
	if len(observed.SecurityTransports) != len(defaults.SecurityTransports) || len(observed.Muxers) != 1 || len(defaults.Muxers) != 1 {
		t.Fatal("native upgrade options changed")
	}
	for i, selected := range observed.SecurityTransports {
		if selected.ID != defaults.SecurityTransports[i].ID {
			t.Fatal("native security preference order changed")
		}
	}
	muxer, ok := observed.Muxers[0].Muxer.(*pathYamuxTransport)
	if !ok || muxer.Multiplexer != yamux.DefaultTransport || observed.Muxers[0].ID != defaults.Muxers[0].ID {
		t.Fatal("selected delegate is not the native default Yamux transport")
	}
	if observed.Transports != nil || observed.PeerKey != nil || observed.PSK != nil {
		t.Fatal("observer options replaced transport, identity or private-network defaults")
	}
}
