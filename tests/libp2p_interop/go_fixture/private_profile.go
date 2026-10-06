package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"time"

	libp2p "github.com/libp2p/go-libp2p"
	kad "github.com/libp2p/go-libp2p-kad-dht"
	kadpb "github.com/libp2p/go-libp2p-kad-dht/pb"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/core/protocol"
	"github.com/libp2p/go-libp2p/core/record"
	"github.com/libp2p/go-libp2p/core/transport"
	"github.com/libp2p/go-libp2p/p2p/muxer/yamux"
	idpb "github.com/libp2p/go-libp2p/p2p/protocol/identify/pb"
	"github.com/libp2p/go-libp2p/p2p/security/noise"
	sectls "github.com/libp2p/go-libp2p/p2p/security/tls"
	"github.com/libp2p/go-libp2p/p2p/transport/tcp"
	"github.com/libp2p/go-libp2p/p2p/transport/tcpreuse"
	ma "github.com/multiformats/go-multiaddr"
	manet "github.com/multiformats/go-multiaddr/net"
	"google.golang.org/protobuf/proto"
)

func isPrivateProfileTransport(value string) bool {
	return value == "tcp-pnet-noise" || value == "tcp-pnet-tls"
}

// Reusable by path fixtures. These values come from the live authenticated Conn,
// never from command-line security or muxer preferences.
func endpointConnectionReceipt(c network.Conn) map[string]any {
	state := c.ConnState()
	return map[string]any{"source": "go-libp2p.network.Conn.ConnState",
		"connection_id": c.ID(), "local_peer_id": c.LocalPeer().String(),
		"remote_peer_id": c.RemotePeer().String(), "local_address": c.LocalMultiaddr().String(),
		"remote_address": c.RemoteMultiaddr().String(), "security": string(state.Security),
		"muxer": string(state.StreamMultiplexer), "transport": canonicalNegotiatedTransport(state.Transport),
		"early_muxer_negotiation": state.UsedEarlyMuxerNegotiation}
}

// Observe only *after* the pinned upgrader has applied PNET, authenticated the
// peer and constructed the muxer. A pre-PNET socket observer would parse PSK
// ciphertext as multistream frames and could expose protector bytes.
type privateUpgrader struct {
	transport.Upgrader
	observer *upgradeObserver
}
type privateCapableConn struct {
	transport.CapableConn
	observed *observedMuxedConn
}

func (c *privateCapableConn) OpenStream(ctx context.Context) (network.MuxedStream, error) {
	s, err := c.CapableConn.OpenStream(ctx)
	if err != nil {
		return nil, err
	}
	wrapped := c.observed.stream(s, network.DirOutbound)
	if binding, ok := ctx.Value(upgradeStreamBindingKey{}).(*upgradeStreamBinding); ok {
		if observed, ok := wrapped.(*observedMuxedStream); ok {
			binding.claim(observed.selection)
		}
	}
	return wrapped, nil
}
func (c *privateCapableConn) As(target any) bool {
	if out, ok := target.(**observedMuxedConn); ok {
		*out = c.observed
		return true
	}
	return c.CapableConn.As(target)
}
func (c *privateCapableConn) AcceptStream() (network.MuxedStream, error) {
	s, err := c.CapableConn.AcceptStream()
	if err != nil {
		return nil, err
	}
	return c.observed.stream(s, network.DirInbound), nil
}
func (c *privateCapableConn) Close() error {
	err := c.CapableConn.Close()
	c.observed.trace.owner.mu.Lock()
	c.observed.trace.evidence.Closed = err == nil
	c.observed.trace.owner.mu.Unlock()
	return err
}
func (u *privateUpgrader) Upgrade(ctx context.Context, tr transport.Transport, raw manet.Conn,
	direction network.Direction, p peer.ID, scope network.ConnManagementScope) (transport.CapableConn, error) {
	c, err := u.Upgrader.Upgrade(ctx, tr, raw, direction, p, scope)
	if err != nil {
		return nil, err
	}
	t := u.observer.connection(raw, direction)
	if t == nil {
		return c, nil
	}
	u.observer.mu.Lock()
	state := c.ConnState()
	t.privateProfile, t.stage = true, 4
	t.evidence.LocalPeer, t.evidence.RemotePeer = c.LocalPeer().String(), c.RemotePeer().String()
	t.evidence.Security, t.evidence.Muxer = string(state.Security), string(state.StreamMultiplexer)
	t.evidence.EarlyMuxer = state.UsedEarlyMuxerNegotiation
	t.eventLocked(upgradeEvent{Kind: "capable_conn_returned", Protocol: string(state.Security)})
	u.observer.mu.Unlock()
	return &privateCapableConn{CapableConn: c, observed: &observedMuxedConn{trace: t}}, nil
}

func (u *privateUpgrader) UpgradeGatedMaListener(tr transport.Transport, l transport.GatedMaListener) transport.Listener {
	return &privateListener{Listener: u.Upgrader.UpgradeGatedMaListener(tr, l), observer: u.observer}
}

// For inbound connections, observe the capable connection returned by Accept.
// UpgradeGatedMaListener returns a transport.Listener, so wrapping that return
// avoids reimplementing the donor's listener queue or handshake timeouts.
type privateListener struct {
	transport.Listener
	observer *upgradeObserver
}

func privateWrapCapable(c transport.CapableConn, o *upgradeObserver, direction network.Direction) transport.CapableConn {
	o.mu.Lock()
	defer o.mu.Unlock()
	if len(o.connections) == upgradeConnectionLimit {
		o.overflow = true
		return c
	}
	state := c.ConnState()
	t := &upgradeConnection{owner: o, privateProfile: true, stage: 4,
		evidence: upgradeConnectionEvidence{ID: uint64(len(o.connections) + 1), Direction: direction.String(),
			LocalAddress: c.LocalMultiaddr().String(), RemoteAddress: c.RemoteMultiaddr().String(),
			LocalPeer: c.LocalPeer().String(), RemotePeer: c.RemotePeer().String(),
			Security: string(state.Security), Muxer: string(state.StreamMultiplexer), EarlyMuxer: state.UsedEarlyMuxerNegotiation,
			Events: []upgradeEvent{}}}
	t.eventLocked(upgradeEvent{Kind: "capable_conn_returned", Protocol: string(state.Security)})
	o.connections = append(o.connections, t)
	return &privateCapableConn{CapableConn: c, observed: &observedMuxedConn{trace: t}}
}
func (l *privateListener) Accept() (transport.CapableConn, error) {
	c, err := l.Listener.Accept()
	if err != nil {
		return nil, err
	}
	return privateWrapCapable(c, l.observer, network.DirInbound), nil
}
func (u *privateUpgrader) UpgradeListener(tr transport.Transport, l manet.Listener) transport.Listener {
	return &privateListener{Listener: u.Upgrader.UpgradeListener(tr, l), observer: u.observer}
}

func privateObservedTCP(o *upgradeObserver) func(transport.Upgrader, network.ResourceManager, *tcpreuse.ConnMgr) (*tcp.TcpTransport, error) {
	return func(u transport.Upgrader, r network.ResourceManager, m *tcpreuse.ConnMgr) (*tcp.TcpTransport, error) {
		return tcp.NewTCPTransport(&privateUpgrader{Upgrader: u, observer: o}, r, m)
	}
}

func privateProtocol(scenario string) (protocol.ID, error) {
	if len(scenario) >= 13 && scenario[:13] == "inline_muxer_" {
		return echoProtocol, nil
	}
	switch scenario {
	case "tcp_yamux_private_pnet", "multistream_select_private_pnet", "noise_identity_private_pnet", "tls_identity_private_pnet":
		return echoProtocol, nil
	case "ping_private_tcp_yamux_pnet":
		return "/ipfs/ping/1.0.0", nil
	case "identify_private_tcp_yamux_pnet":
		return "/ipfs/id/1.0.0", nil
	case "kademlia_amino_private_tcp_yamux_pnet":
		return "/ipfs/kad/1.0.0", nil
	default:
		return "", fmt.Errorf("unsupported private contract %s", scenario)
	}
}

func publicFrameReceipt(frame []byte, raw bool) map[string]any {
	sum := sha256.Sum256(frame)
	return map[string]any{"framed_hex": hex.EncodeToString(frame), "raw": raw,
		"read": map[string]any{"framed_bytes": len(frame), "framed_sha256": hex.EncodeToString(sum[:]),
			"frames": 1, "complete_frames": true, "invalid_or_over_limit": false}}
}
func publicFramed(data []byte) []byte { var b bytes.Buffer; _ = writeFrame(&b, data); return b.Bytes() }

func readPrivateFrame(s io.Reader) ([]byte, error) {
	prefix := []byte{}
	size := 0
	for {
		var b [1]byte
		if _, err := io.ReadFull(s, b[:]); err != nil {
			return nil, err
		}
		if len(prefix) == 3 {
			return nil, fmt.Errorf("private frame header exceeds bound")
		}
		size |= int(b[0]&127) << (7 * len(prefix))
		prefix = append(prefix, b[0])
		if b[0]&128 == 0 {
			if size == 0 || size > 8192 || len(prefix) > 1 && b[0] == 0 {
				return nil, fmt.Errorf("noncanonical or oversized private frame")
			}
			break
		}
	}
	body := make([]byte, size)
	if _, err := io.ReadFull(s, body); err != nil {
		return nil, err
	}
	return append(prefix, body...), nil
}

func verifyPrivateIdentify(frame []byte, remote peer.ID) error {
	offset := 0
	for {
		b := frame[offset]
		offset++
		if b&128 == 0 {
			break
		}
	}
	message := &idpb.Identify{}
	if err := proto.Unmarshal(frame[offset:], message); err != nil {
		return err
	}
	envelope, value, err := record.ConsumeEnvelope(message.GetSignedPeerRecord(), peer.PeerRecordEnvelopeDomain)
	if err != nil {
		return err
	}
	routing, ok := value.(*peer.PeerRecord)
	if !ok || routing.PeerID != remote || routing.Seq == 0 || len(routing.Addrs) == 0 {
		return fmt.Errorf("private Identify record peer or addresses mismatch")
	}
	signer, err := peer.IDFromPublicKey(envelope.PublicKey)
	if err != nil || signer != remote {
		return fmt.Errorf("private Identify signer differs from authenticated peer")
	}
	return nil
}

func privateObservation(o *upgradeObserver) upgradeEvidence {
	proof, _ := o.finish("", "", network.DirInbound)
	proof.Source = "go-libp2p.capable-conn.v1"
	return proof
}

func runPrivateProfile(opts options) (err error) {
	o := &upgradeObserver{}
	state := &pnetConnectionState{}
	config := []libp2p.Option{libp2p.NoTransports, libp2p.DisableRelay(),
		libp2p.Transport(privateObservedTCP(o)), libp2p.Muxer(yamux.ID, yamux.DefaultTransport),
		libp2p.ConnectionGater(&pnetConnectionGater{state: state}), libp2p.ListenAddrStrings("/ip4/127.0.0.1/tcp/0")}
	if opts.transport == "tcp-pnet-tls" || opts.transport == "tcp-tls" {
		config = append(config, libp2p.Security(sectls.ID, sectls.New))
	} else {
		config = append(config, libp2p.Security(noise.ID, noise.New))
	}
	if opts.pnetKeyFile != "" {
		key, failure := loadPnetKey(opts.pnetKeyFile)
		if failure != nil {
			return fmt.Errorf("invalid private key fixture")
		}
		config = append(config, libp2p.PrivateNetwork(key))
	}
	h, err := libp2p.New(config...)
	if err != nil {
		return err
	}
	h.Network().Notify(&network.NotifyBundle{ConnectedF: func(network.Network, network.Conn) { state.established.Add(1) }})
	installEchoHandler(h, state)
	dht, err := kad.New(context.Background(), h, kad.Mode(kad.ModeServer), kad.DisableAutoRefresh())
	if err != nil {
		_ = h.Close()
		return err
	}
	result := map[string]any{"implementation": "go", "role": "dialer", "scenario": opts.scenario,
		"local_peer_id": h.ID().String(), "status": "ok", "pnet_fingerprint": opts.pnetFingerprint}
	defer func() {
		err = errors.Join(err, dht.Close(), h.Close())
		result["private_observation"] = privateObservation(o)
		if err != nil {
			result["status"] = "error"
		}
		err = errors.Join(err, writeJSON(opts.resultFile, result))
	}()
	if opts.command == "listen" {
		result["role"] = "listener"
		addresses := []string{}
		for _, address := range h.Addrs() {
			addresses = append(addresses, address.String()+"/p2p/"+h.ID().String())
		}
		if err = writeJSON(opts.readyFile, map[string]any{"implementation": "go", "role": "listener", "status": "ready",
			"peer_id": h.ID().String(), "listen_addrs": addresses}); err != nil {
			return err
		}
		for {
			if _, failure := os.Stat(opts.stopFile); failure == nil {
				break
			}
			if opts.pnetControl == "" {
				result["private_observation"] = privateObservation(o)
				if err = writeJSON(opts.resultFile, result); err != nil {
					return err
				}
			}
			time.Sleep(25 * time.Millisecond)
		}
		if opts.pnetControl != "" {
			result = pnetRejection(opts, "listener", "", state)
		}
		return nil
	}
	if opts.command != "dial" {
		return fmt.Errorf("private fixture requires listen or dial")
	}
	a, err := ma.NewMultiaddr(opts.addr)
	if err != nil {
		return err
	}
	remote, err := peer.AddrInfoFromP2pAddr(a)
	if err != nil {
		return err
	}
	if remote.ID.String() != opts.peerID {
		return fmt.Errorf("private target peer differs from address")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	if err = h.Connect(ctx, *remote); err != nil {
		if opts.pnetControl == "" {
			return err
		}
		result = pnetRejection(opts, "dialer", opts.peerID, state)
		return nil
	}
	if opts.pnetControl != "" {
		return fmt.Errorf("private rejection control authenticated a peer")
	}
	id, err := privateProtocol(opts.scenario)
	if err != nil {
		return err
	}
	streamContext, binding := bindUpgradeStream(ctx)
	s, err := h.NewStream(streamContext, remote.ID, id)
	if err != nil {
		return err
	}
	binding.attach(s)
	if err = s.SetDeadline(time.Now().Add(10 * time.Second)); err != nil {
		_ = s.Reset()
		return err
	}
	result["connection_receipt"] = endpointConnectionReceipt(s.Conn())
	result["protocol"] = string(s.Protocol())
	var request, response []byte
	raw := id == "/ipfs/ping/1.0.0"
	switch id {
	case echoProtocol:
		request = publicFramed([]byte(opts.payload))
		_, err = s.Write(request)
		if err == nil {
			response, err = readPrivateFrame(s)
		}
	case "/ipfs/ping/1.0.0":
		request = make([]byte, 32)
		for i := range request {
			request[i] = byte(i + 1)
		}
		_, err = s.Write(request)
		response = make([]byte, 32)
		if err == nil {
			_, err = io.ReadFull(s, response)
		}
	case "/ipfs/id/1.0.0":
		response, err = readPrivateFrame(s)
		if err == nil {
			err = verifyPrivateIdentify(response, remote.ID)
			result["identify_verified"] = err == nil
		}
	case "/ipfs/kad/1.0.0":
		body, failure := proto.Marshal(kadpb.NewMessage(kadpb.Message_FIND_NODE, []byte(remote.ID), 0))
		if failure != nil {
			err = failure
			break
		}
		request = publicFramed(body)
		_, err = s.Write(request)
		if err == nil {
			response, err = readPrivateFrame(s)
		}
	}
	if err != nil {
		_ = s.Reset()
		return err
	}
	if id == echoProtocol || raw {
		if !bytes.Equal(request, response) {
			_ = s.Reset()
			return fmt.Errorf("private response differs from request")
		}
	}
	if len(response) == 0 || len(response) > 8192 {
		_ = s.Reset()
		return fmt.Errorf("private response outside bound")
	}
	binding.complete(s)
	if err = s.Close(); err != nil {
		return err
	}
	result["stream_id"], result["stream_closed"] = s.ID(), true
	result["response"] = publicFrameReceipt(response, raw)
	if len(request) > 0 {
		result["request"] = publicFrameReceipt(request, raw)
	}
	if opts.scenario == "multistream_select_private_pnet" {
		_, rejection := expectUnsupportedProtocol(ctx, h, remote.ID, "/forge/interop/private-unknown/1")
		if rejection != nil {
			return rejection
		}
		result["unknown_protocol_rejected"] = true
	}
	result["private_observation"] = privateObservation(o)
	if err = writeJSON(opts.resultFile, result); err != nil {
		return err
	}
	for {
		if _, failure := os.Stat(opts.stopFile); failure == nil {
			break
		}
		time.Sleep(25 * time.Millisecond)
	}
	return nil
}
