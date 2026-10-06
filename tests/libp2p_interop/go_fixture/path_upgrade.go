package main

import (
	"context"
	"fmt"
	"net"

	libp2p "github.com/libp2p/go-libp2p"
	"github.com/libp2p/go-libp2p/core/crypto"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/core/protocol"
	"github.com/libp2p/go-libp2p/core/sec"
	"github.com/libp2p/go-libp2p/p2p/muxer/yamux"
	"github.com/libp2p/go-libp2p/p2p/net/upgrader"
	"github.com/libp2p/go-libp2p/p2p/security/noise"
	libp2ptls "github.com/libp2p/go-libp2p/p2p/security/tls"
)

// The ordinary donor upgrader owns all negotiation and crypto. These options
// retain its successful outputs; configuring a delegate creates no receipt.
func pathUpgradeOptions() []libp2p.Option {
	return []libp2p.Option{
		libp2p.Security(libp2ptls.ID, newPathTLS),
		libp2p.Security(noise.ID, newPathNoise),
		libp2p.Muxer(yamux.ID, &pathYamuxTransport{Multiplexer: yamux.DefaultTransport}),
	}
}

func newPathNoise(id protocol.ID, key crypto.PrivKey, muxers []upgrader.StreamMuxer) (*pathSecurityTransport, error) {
	native, err := noise.New(id, key, muxers)
	if err != nil {
		return nil, err
	}
	return &pathSecurityTransport{SecureTransport: native}, nil
}

func newPathTLS(id protocol.ID, key crypto.PrivKey, muxers []upgrader.StreamMuxer) (*pathSecurityTransport, error) {
	native, err := libp2ptls.New(id, key, muxers)
	if err != nil {
		return nil, err
	}
	return &pathSecurityTransport{SecureTransport: native}, nil
}

type pathSecurityTransport struct {
	sec.SecureTransport
}

type pathSecureConn struct {
	sec.SecureConn
	security protocol.ID
	local    peer.ID
	remote   peer.ID
	key      crypto.PubKey
}

func (t *pathSecurityTransport) SecureInbound(ctx context.Context, conn net.Conn, expected peer.ID) (sec.SecureConn, error) {
	native, err := t.SecureTransport.SecureInbound(ctx, conn, expected)
	if err != nil {
		return native, err
	}
	return t.observe(native, expected, false)
}

func (t *pathSecurityTransport) SecureOutbound(ctx context.Context, conn net.Conn, expected peer.ID) (sec.SecureConn, error) {
	native, err := t.SecureTransport.SecureOutbound(ctx, conn, expected)
	if err != nil {
		return native, err
	}
	return t.observe(native, expected, true)
}

func (t *pathSecurityTransport) observe(native sec.SecureConn, expected peer.ID, outbound bool) (sec.SecureConn, error) {
	if native == nil {
		return nil, fmt.Errorf("native security callback returned no connection")
	}
	observed := &pathSecureConn{SecureConn: native, security: t.SecureTransport.ID(),
		local: native.LocalPeer(), remote: native.RemotePeer(), key: native.RemotePublicKey()}
	var err error
	if outbound && expected == "" {
		err = fmt.Errorf("native outbound security lacks an expected peer")
	} else if expected != "" && observed.remote != expected {
		err = fmt.Errorf("native security callback authenticated the wrong peer")
	} else {
		err = observed.validate()
	}
	if err != nil {
		_ = native.Close()
		return nil, err
	}
	return observed, nil
}

func (c *pathSecureConn) validate() error {
	if c == nil || c.SecureConn == nil || c.local == "" || c.remote == "" || c.key == nil {
		return fmt.Errorf("missing native authenticated security output")
	}
	if c.security != noise.ID && c.security != libp2ptls.ID {
		return fmt.Errorf("unsupported native security callback ID")
	}
	authenticated, err := peer.IDFromPublicKey(c.key)
	if err != nil || authenticated != c.remote {
		return fmt.Errorf("native security output key/peer mismatch")
	}
	key := c.SecureConn.RemotePublicKey()
	if c.SecureConn.LocalPeer() != c.local || c.SecureConn.RemotePeer() != c.remote || key == nil || !c.key.Equals(key) {
		return fmt.Errorf("native security output changed after callback")
	}
	if id := c.SecureConn.ConnState().Security; id != "" && id != c.security {
		return fmt.Errorf("native security callback/output ID mismatch")
	}
	return nil
}

type pathYamuxTransport struct {
	network.Multiplexer
}

type pathInnerMuxedConn struct {
	network.MuxedConn
	secure *pathSecureConn
	muxer  protocol.ID
}

func (t *pathYamuxTransport) NewConn(conn net.Conn, server bool, scope network.PeerScope) (network.MuxedConn, error) {
	secure, ok := conn.(*pathSecureConn)
	if !ok {
		return nil, fmt.Errorf("native Yamux callback lacks a scoped security output")
	}
	if err := secure.validate(); err != nil {
		return nil, err
	}
	if selected := secure.ConnState().StreamMultiplexer; selected != "" && selected != yamux.ID {
		return nil, fmt.Errorf("native early muxer output does not select Yamux")
	}
	native, err := t.Multiplexer.NewConn(conn, server, scope)
	if err != nil {
		return native, err
	}
	if native == nil {
		return nil, fmt.Errorf("native Yamux callback returned no connection")
	}
	// This ID records the selected Yamux delegate's completed NewConn callback,
	// not an advertised/configured muxer or the circuit's erased ConnState.
	return &pathInnerMuxedConn{MuxedConn: native, secure: secure, muxer: yamux.ID}, nil
}

func (c *pathInnerMuxedConn) As(target any) bool {
	if c == nil {
		return false
	}
	if owner, ok := target.(**pathInnerMuxedConn); ok {
		if owner == nil {
			return false
		}
		*owner = c
		return true
	}
	return c.MuxedConn.As(target)
}

// transportConn.As, circuit capableConn and swarm.Conn.As all delegate to this
// exact muxer output. A missing owner must not fall back to configured IDs.
func pathInnerUpgrade(conn network.Conn, local, expected peer.ID) (protocol.ID, protocol.ID, error) {
	var owner *pathInnerMuxedConn
	if !conn.As(&owner) || owner == nil || owner.MuxedConn == nil || owner.muxer != yamux.ID {
		return "", "", fmt.Errorf("missing native inner Yamux output owner")
	}
	if err := owner.secure.validate(); err != nil {
		return "", "", err
	}
	secure := owner.secure
	if local == "" || expected == "" || secure.local != local || secure.remote != expected ||
		conn.LocalPeer() != local || conn.RemotePeer() != expected {
		return "", "", fmt.Errorf("native inner/outer authenticated peer mismatch")
	}
	key := conn.RemotePublicKey()
	if key == nil || !secure.key.Equals(key) {
		return "", "", fmt.Errorf("native inner/outer authenticated key mismatch")
	}
	state := conn.ConnState()
	if (state.Security != "" && state.Security != secure.security) ||
		(state.StreamMultiplexer != "" && state.StreamMultiplexer != owner.muxer) {
		return "", "", fmt.Errorf("native inner/outer negotiated protocol mismatch")
	}
	return secure.security, owner.muxer, nil
}

var _ sec.SecureTransport = (*pathSecurityTransport)(nil)
var _ sec.SecureConn = (*pathSecureConn)(nil)
var _ network.Multiplexer = (*pathYamuxTransport)(nil)
var _ network.MuxedConn = (*pathInnerMuxedConn)(nil)
