package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"crypto/tls"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"net"
	"os"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"unicode/utf8"

	pubsubpb "github.com/libp2p/go-libp2p-pubsub/pb"
	"github.com/libp2p/go-libp2p/core/connmgr"
	"github.com/libp2p/go-libp2p/core/crypto"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/core/pnet"
	"github.com/libp2p/go-libp2p/core/transport"
	libp2pquic "github.com/libp2p/go-libp2p/p2p/transport/quic"
	"github.com/libp2p/go-libp2p/p2p/transport/quicreuse"
	ma "github.com/multiformats/go-multiaddr"
	quic "github.com/quic-go/quic-go"
)

// These promoted public methods exist on libp2p's native QUIC MuxedStream,
// before Swarm hides that delegate. Context describes only the send direction.
type pubsubQUICNativeStream interface {
	Context() context.Context
	StreamID() quic.StreamID
}

type pubsubQUICKey struct {
	connection *quic.Conn
	stream     quic.StreamID
}

type pubsubQUICContext struct {
	done  bool
	cause error
}

func pubsubQUICSample(ctx context.Context) pubsubQUICContext {
	if ctx == nil || ctx.Err() == nil {
		return pubsubQUICContext{}
	}
	return pubsubQUICContext{done: true, cause: context.Cause(ctx)}
}

func (sample pubsubQUICContext) fields() map[string]any {
	value := map[string]any{"done": sample.done, "cause_type": nil, "error_code": nil,
		"remote": nil, "native_stream_id": nil, "error": nil}
	if sample.cause != nil {
		value["cause_type"], value["error"] = fmt.Sprintf("%T", sample.cause), pubsubQUICDiagnostic(sample.cause)
	}
	switch cause := sample.cause.(type) {
	case *quic.StreamError:
		if cause != nil {
			value["error_code"], value["remote"], value["native_stream_id"] = uint64(cause.ErrorCode), cause.Remote, int64(cause.StreamID)
		}
	case *quic.ApplicationError:
		if cause != nil {
			value["error_code"], value["remote"] = uint64(cause.ErrorCode), cause.Remote
		}
	}
	return value
}

type pubsubQUICObserver struct {
	observation  *pubsubScoringObserver
	gater        *pathDialObserver
	mu           sync.Mutex
	connections  map[*quic.Conn]*pubsubQUICConn
	streams      map[pubsubQUICKey]*pubsubQUICStream
	baseline     map[pubsubQUICKey]pubsubQUICContext
	connBase     map[*quic.Conn]pubsubQUICContext
	prepareAck   int
	publishedAck atomic.Int64
	active       int
	changed      chan struct{}
}

func newPubsubQUICObserver(o *pubsubScoringObserver, g *pathDialObserver) *pubsubQUICObserver {
	return &pubsubQUICObserver{observation: o, gater: g, connections: map[*quic.Conn]*pubsubQUICConn{},
		streams: map[pubsubQUICKey]*pubsubQUICStream{}, baseline: map[pubsubQUICKey]pubsubQUICContext{},
		connBase: map[*quic.Conn]pubsubQUICContext{}, changed: make(chan struct{}, 1)}
}

func pubsubQUICReceipt(o *pubsubScoringObserver, kind, source string, fields map[string]any) int {
	o.mu.Lock()
	defer o.mu.Unlock()
	previous := len(o.events)
	o.emitLocked(kind, source, fields)
	if len(o.events) == previous {
		return 0
	}
	return previous + 1
}

func (q *pubsubQUICObserver) begin() int {
	q.mu.Lock()
	defer q.mu.Unlock()
	q.active++
	return int(q.publishedAck.Load())
}

func (q *pubsubQUICObserver) end() {
	q.mu.Lock()
	q.active--
	q.mu.Unlock()
	select {
	case q.changed <- struct{}{}:
	default:
	}
}

// Called under the observation mutex at the actual prepare ACK boundary.
// No stream mutex, callback scheduling timestamp, or previous I/O cause is used.
func (q *pubsubQUICObserver) prepare(ack int) []map[string]any {
	q.mu.Lock()
	defer q.mu.Unlock()
	q.prepareAck = ack
	values := []map[string]any{}
	for native, c := range q.connections {
		q.connBase[native] = pubsubQUICSample(c.connectionContext)
		if sample := q.connBase[native]; sample.done && q.observation.failure == nil {
			q.observation.failure = sample.cause
			q.observation.notifyScoreLocked()
		}
	}
	for key, s := range q.streams {
		send := pubsubQUICSample(s.sendContext)
		connection := q.connBase[key.connection]
		q.baseline[key] = send
		if s.pubsub.Load() && !pubsubQUICValidSendContext(send, key.stream) && q.observation.failure == nil {
			q.observation.failure = send.cause
			q.observation.notifyScoreLocked()
		}
		values = append(values, map[string]any{"native_connection_id": s.connection.secured.id,
			"native_stream_id": int64(key.stream), "send_context": send.fields(), "connection_context": connection.fields()})
	}
	if q.observation.failure != nil {
		q.prepareAck = 0
	}
	return values
}

func (q *pubsubQUICObserver) baselineFor(s *pubsubQUICStream) (pubsubQUICContext, pubsubQUICContext, bool) {
	q.mu.Lock()
	defer q.mu.Unlock()
	key := pubsubQUICKey{s.connection.native, s.nativeID}
	send, exists := q.baseline[key]
	connection, connectionExists := q.connBase[key.connection]
	return send, connection, exists && connectionExists && q.prepareAck != 0
}

func (q *pubsubQUICObserver) constructor() func(crypto.PrivKey, *quicreuse.ConnManager, pnet.PSK,
	connmgr.ConnectionGater, network.ResourceManager) (transport.Transport, error) {
	return func(key crypto.PrivKey, manager *quicreuse.ConnManager, psk pnet.PSK,
		gater connmgr.ConnectionGater, resources network.ResourceManager) (transport.Transport, error) {
		native, err := libp2pquic.NewTransport(key, manager, psk, gater, resources)
		if err != nil {
			return native, err
		}
		return &pubsubQUICTransport{Transport: native, owner: q}, nil
	}
}

type pubsubQUICTransport struct {
	transport.Transport
	owner *pubsubQUICObserver
}

func (t *pubsubQUICTransport) Dial(ctx context.Context, address ma.Multiaddr, p peer.ID) (transport.CapableConn, error) {
	t.owner.begin()
	defer t.owner.end()
	c, err := t.Transport.Dial(ctx, address, p)
	if err != nil {
		return c, err
	}
	return t.owner.connection(c), err
}

func (t *pubsubQUICTransport) Listen(address ma.Multiaddr) (transport.Listener, error) {
	l, err := t.Transport.Listen(address)
	if err != nil {
		return l, err
	}
	return &pubsubQUICListener{Listener: l, owner: t.owner}, err
}

func (t *pubsubQUICTransport) Close() error { return t.Transport.(io.Closer).Close() }
func (t *pubsubQUICTransport) ListenOrder() int {
	return t.Transport.(interface{ ListenOrder() int }).ListenOrder()
}

type pubsubQUICListener struct {
	transport.Listener
	owner *pubsubQUICObserver
}

func (l *pubsubQUICListener) Accept() (transport.CapableConn, error) {
	l.owner.begin()
	defer l.owner.end()
	c, err := l.Listener.Accept()
	if err != nil {
		return c, err
	}
	return l.owner.connection(c), err
}

func (q *pubsubQUICObserver) connection(raw transport.CapableConn) transport.CapableConn {
	var native *quic.Conn
	if raw == nil || q.gater == nil || raw.ConnState().Transport != "quic-v1" || !raw.As(&native) || native == nil {
		q.observation.fail(fmt.Errorf("lower QUIC connection lacks its native capable owner"))
		return raw
	}
	q.gater.mu.Lock()
	secured, exists := q.gater.secured[native]
	captureErr := q.gater.captureErr
	q.gater.mu.Unlock()
	key, local, remote := raw.RemotePublicKey(), raw.LocalMultiaddr(), raw.RemoteMultiaddr()
	if captureErr != nil || !exists || secured.id == "" || secured.key == nil || key == nil || local == nil || remote == nil ||
		secured.localPeer != raw.LocalPeer() || secured.peer != raw.RemotePeer() || !secured.key.Equals(key) ||
		secured.local != local.String() || secured.remote != remote.String() {
		q.observation.fail(fmt.Errorf("lower QUIC connection lacks its own authenticated secured callback"))
		return raw
	}
	connectionContext := native.Context()
	if connectionContext == nil {
		q.observation.fail(fmt.Errorf("native QUIC connection lacks its public context"))
		return raw
	}
	publicKey, keyErr := key.Raw()
	if keyErr != nil {
		q.observation.fail(fmt.Errorf("cannot inspect native QUIC remote public key"))
		return raw
	}
	digest := sha256.Sum256(publicKey)
	q.mu.Lock()
	if previous := q.connections[native]; previous != nil {
		q.mu.Unlock()
		<-previous.ready
		return previous
	}
	if len(q.connections) == pubsubScoringPeers {
		q.mu.Unlock()
		q.observation.fail(fmt.Errorf("lower QUIC connection capture exceeds bound"))
		return raw
	}
	c := &pubsubQUICConn{CapableConn: raw, owner: q, native: native, secured: secured,
		connectionContext: connectionContext, remoteKeyDigest: hex.EncodeToString(digest[:]), ready: make(chan struct{})}
	q.connections[native] = c
	ack := q.prepareAck
	q.mu.Unlock()
	fields := c.fields()
	fields["authentication_basis"] = "same_native_quic_Conn_and_InterceptSecured_capable_output"
	fields["secured_callback"] = secured.receipt()
	fields["prepare_ack_sequence"], fields["connection_context_at_capable_return"] = ack, pubsubQUICSample(connectionContext).fields()
	c.receipt = pubsubQUICReceipt(q.observation, "native_quic_connection", "go.quic.transport.capable_output", fields)
	close(c.ready)
	return c
}

type pubsubQUICConn struct {
	transport.CapableConn
	owner             *pubsubQUICObserver
	native            *quic.Conn
	connectionContext context.Context
	remoteKeyDigest   string
	secured           pathQUICSecured
	receipt           int
	ready             chan struct{}
	mu                sync.Mutex
	context           pubsubQUICContext
	contextEvent      int
}

func (c *pubsubQUICConn) fields() map[string]any {
	return map[string]any{"native_connection_id": c.secured.id, "local_peer_id": c.LocalPeer().String(),
		"remote_peer_id": c.RemotePeer().String(), "local_address": c.LocalMultiaddr().String(),
		"remote_address": c.RemoteMultiaddr().String(), "remote_public_key_sha256": c.remoteKeyDigest,
		"native_connection_basis": "CapableConn.As(**quic.Conn)"}
}

func (c *pubsubQUICConn) As(target any) bool {
	if out, ok := target.(**pubsubQUICConn); ok {
		*out = c
		return true
	}
	return c.CapableConn.As(target)
}

func (c *pubsubQUICConn) OpenStream(ctx context.Context) (network.MuxedStream, error) {
	// Getters/registration can lag RETURN; admission precedes the native call.
	beginAck := c.owner.begin()
	defer c.owner.end()
	s, err := c.CapableConn.OpenStream(ctx)
	if err != nil {
		c.recordContext()
		return s, err
	}
	return c.stream(s, network.DirOutbound, beginAck), err
}

func (c *pubsubQUICConn) AcceptStream() (network.MuxedStream, error) {
	beginAck := c.owner.begin()
	defer c.owner.end()
	s, err := c.CapableConn.AcceptStream()
	if err != nil {
		c.recordContext()
		return s, err
	}
	return c.stream(s, network.DirInbound, beginAck), err
}

func (c *pubsubQUICConn) recordContext() {
	sample := pubsubQUICSample(c.connectionContext)
	if !sample.done {
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.contextEvent != 0 {
		return
	}
	fields := c.fields()
	fields["connection_receipt_sequence"], fields["context"] = c.receipt, sample.fields()
	c.owner.mu.Lock()
	baseline, existed := c.owner.connBase[c.native]
	c.owner.mu.Unlock()
	c.owner.observation.mu.Lock()
	prepared := c.owner.observation.prepared && c.owner.observation.prepareAck != 0
	ack := c.owner.observation.prepareAck
	c.owner.observation.mu.Unlock()
	fields["prepare_ack_sequence"], fields["context_at_prepare"] = ack, baseline.fields()
	fields["prepare_baseline_present"] = existed
	c.context = sample
	c.contextEvent = pubsubQUICReceipt(c.owner.observation, "native_quic_connection_context", "go.quic.Conn.Context", fields)
	cause, typed := sample.cause.(*quic.ApplicationError)
	if !prepared || !existed || baseline.done || !typed || cause == nil || cause.ErrorCode != 0 {
		c.owner.observation.fail(sample.cause)
	}
}

func (c *pubsubQUICConn) Close() error {
	c.owner.begin()
	defer c.owner.end()
	err := c.CapableConn.Close()
	c.recordContext()
	if err != nil {
		c.owner.observation.fail(err)
	}
	return err
}

func (c *pubsubQUICConn) CloseWithError(code network.ConnErrorCode) error {
	c.owner.begin()
	defer c.owner.end()
	err := c.CapableConn.CloseWithError(code)
	c.recordContext()
	if err != nil {
		c.owner.observation.fail(err)
	}
	return err
}

func (c *pubsubQUICConn) stream(raw network.MuxedStream, direction network.Direction, beginAck int) network.MuxedStream {
	native, ok := raw.(pubsubQUICNativeStream)
	if !ok || native.Context() == nil || native.StreamID() < 0 || native.StreamID()&2 != 0 {
		c.owner.observation.fail(fmt.Errorf("native QUIC stream lacks public send context/bidirectional StreamID"))
		return raw
	}
	s := &pubsubQUICStream{MuxedStream: raw, connection: c, nativeID: native.StreamID(),
		sendContext: native.Context(), direction: direction, nativeBeginAck: beginAck, pending: []pubsubQUICPending{}}
	key := pubsubQUICKey{c.native, s.nativeID}
	c.owner.mu.Lock()
	if _, exists := c.owner.streams[key]; exists || len(c.owner.streams) == pubsubScoringStreams {
		c.owner.mu.Unlock()
		c.owner.observation.fail(fmt.Errorf("duplicate/over-limit native QUIC stream capture"))
		return raw
	}
	c.owner.streams[key] = s
	ack := c.owner.prepareAck
	c.owner.mu.Unlock()
	s.createdAck, s.createdContext = ack, pubsubQUICSample(s.sendContext)
	fields := s.fields()
	fields["prepare_ack_sequence"] = ack
	fields["native_call_begin_prepare_ack_sequence"] = beginAck
	fields["native_call_begin_observation_basis"] = "published_Prepare_ACK_before_native_CapableConn_call"
	fields["send_context_at_stream_return"] = s.createdContext.fields()
	s.receipt = pubsubQUICReceipt(c.owner.observation, "native_quic_stream", "go.quic.CapableConn.stream_return", fields)
	return s
}

type pubsubQUICDirection struct {
	buffer []byte
	header bool
	paused bool
	token  string
	tail   []byte
}

type pubsubQUICOperation struct {
	name     string
	started  uint64
	ack      int
	reset    *pubsubQUICReset
	terminal *pubsubQUICReset
}

type pubsubQUICReset struct {
	started, returned uint64
	ack, sequence     int
	completed         bool
	err               error
}

type pubsubQUICPending struct {
	sequence, ack   int
	contextSequence int
	returned        uint64
	err             error
	fields          map[string]any
	reset           *pubsubQUICReset
	connectionCause *quic.ApplicationError
}

type pubsubQUICCleanup struct {
	sequence, errorSequence, writeSequence, contextSequence, ack int
	peerHeaderSequence, disposalSequence                         int
	framingSequence                                              int
	returned                                                     uint64
	err                                                          error
	cause                                                        *quic.ApplicationError
	finished                                                     bool
}

type pubsubQUICCleanupDisposal struct {
	readReturn, started, returned uint64
	ack, sequence                 int
}

type pubsubQUICStreamAbort struct {
	sequence, errorSequence, ack, framingSequence int
	returned                                      uint64
	err                                           error
	reset                                         *pubsubQUICReset
	ioSequences                                   []int
	close                                         *pubsubQUICPending
	closePending                                  int
	finished                                      bool
}

type pubsubQUICStream struct {
	network.MuxedStream
	connection        *pubsubQUICConn
	nativeID          quic.StreamID
	sendContext       context.Context
	direction         network.Direction
	receipt           int
	nativeBeginAck    int
	createdAck        int
	createdContext    pubsubQUICContext
	mu                sync.Mutex
	order             uint64
	activeIO          [2]int
	activeClose       int
	ending            [2]bool
	read, write       pubsubScoringDecoder
	negotiation       [2]pubsubQUICDirection
	frames            []int
	proposals         int
	proposal, reply   string
	selected          string
	touchedPubsub     bool
	pubsub            atomic.Bool
	failed            bool
	earlyError        error
	finalized         bool
	disposal          int
	reset, terminal   *pubsubQUICReset
	pending           []pubsubQUICPending
	candidateWrite    int
	cleanup           *pubsubQUICCleanup
	cleanupRead       *pubsubQUICReturn
	cleanupDisposal   *pubsubQUICCleanupDisposal
	abort             *pubsubQUICStreamAbort
	abortReturn       *pubsubQUICReturn
	abortClose        *pubsubQUICPending
	abortClosePending int
}

func pubsubQUICIsPubsub(protocol string) bool {
	return protocol == "/meshsub/1.0.0" || protocol == "/meshsub/1.1.0"
}

func pubsubQUICValidSendContext(sample pubsubQUICContext, stream quic.StreamID) bool {
	if !sample.done || sample.cause == context.Canceled {
		return true
	}
	cause, typed := sample.cause.(*quic.StreamError)
	return typed && cause != nil && cause.ErrorCode == 0 && cause.StreamID == stream
}

func (s *pubsubQUICStream) fields() map[string]any {
	return map[string]any{"native_connection_id": s.connection.secured.id, "native_stream_id": int64(s.nativeID),
		"connection_receipt_sequence": s.connection.receipt, "native_stream_receipt_sequence": s.receipt,
		"remote_peer_id": s.connection.RemotePeer().String(), "stream_direction": s.direction.String(),
		"protocol": s.selected, "owner_basis": "same_native_CapableConn_returned_MuxedStream"}
}

func (s *pubsubQUICStream) failLocked(err error) {
	s.failed = true
	s.connection.owner.observation.fail(err)
}

// Mirrors the existing upgrade observer's proposal/NA/ACK state machine, but
// retains bounded PubSub tails until selection is proven by both actual sides.
func (s *pubsubQUICStream) feedLocked(side int, data []byte) {
	if s.failed {
		return
	}
	if s.selected != "" {
		s.bodyLocked(side, data)
		return
	}
	d := &s.negotiation[side]
	for len(data) > 0 {
		if d.paused {
			if d.token == "na" || len(d.tail)+len(data) > pubsubScoringFrame+binary.MaxVarintLen64 {
				s.failLocked(fmt.Errorf("unpaired/over-limit native QUIC negotiation tail"))
				return
			}
			d.tail = append(d.tail, data...)
			return
		}
		d.buffer = append(d.buffer, data[0])
		data = data[1:]
		length, header := binary.Uvarint(d.buffer)
		if header == 0 {
			if len(d.buffer) == binary.MaxVarintLen64 {
				s.failLocked(fmt.Errorf("invalid native QUIC multistream length"))
				return
			}
			continue
		}
		var canonical [binary.MaxVarintLen64]byte
		if header < 0 || length == 0 || length > 256 || header != binary.PutUvarint(canonical[:], length) ||
			!bytes.Equal(d.buffer[:header], canonical[:header]) {
			s.failLocked(fmt.Errorf("noncanonical/over-limit native QUIC negotiation"))
			return
		}
		if len(d.buffer)-header < int(length) {
			continue
		}
		token := string(d.buffer[header:])
		if (!d.header && token != "/multistream/1.0.0\n") || (d.header && token == "/multistream/1.0.0\n") ||
			(!validUpgradeToken(token) && token != "na\n") {
			s.failLocked(fmt.Errorf("malformed native QUIC negotiation token"))
			return
		}
		protocol := strings.TrimSuffix(token, "\n")
		s.touchedPubsub = s.touchedPubsub || pubsubQUICIsPubsub(protocol)
		if len(s.frames) == 18 {
			s.failLocked(fmt.Errorf("native QUIC negotiation attempt bound"))
			return
		}
		fields := s.fields()
		fields["direction"], fields["protocol"] = []string{"read", "write"}[side], protocol
		fields["receipt"] = pubsubQUICWireReceipt(d.buffer, side)
		sequence := pubsubQUICReceipt(s.connection.owner.observation, "multistream_frame", "go.quic.native_stream.multistream", fields)
		s.frames = append(s.frames, sequence)
		d.buffer = nil
		if !d.header {
			d.header = true
			continue
		}
		d.token, d.paused = protocol, true
		proposer := (side == 0) == (s.direction == network.DirInbound)
		if proposer {
			s.proposals++
			if protocol == "na" || s.proposals > 8 {
				s.failLocked(fmt.Errorf("invalid native QUIC multistream proposal"))
				return
			}
			s.proposal = protocol
		} else {
			s.reply = protocol
		}
		if s.proposal != "" && s.reply != "" {
			if s.reply == "na" {
				if len(s.negotiation[0].tail)+len(s.negotiation[1].tail) != 0 {
					s.failLocked(fmt.Errorf("native QUIC bytes followed rejected proposal"))
					return
				}
				s.proposal, s.reply = "", ""
				for index := range s.negotiation {
					s.negotiation[index].paused, s.negotiation[index].token = false, ""
				}
			} else if s.proposal != s.reply {
				s.failLocked(fmt.Errorf("native QUIC proposal/ACK mismatch"))
				return
			} else {
				s.selected = s.proposal
				s.pubsub.Store(pubsubQUICIsPubsub(s.selected))
				if pubsubQUICIsPubsub(s.selected) && s.earlyError != nil {
					s.failLocked(s.earlyError)
				}
				fields := s.fields()
				fields["negotiation_frame_sequences"] = append([]int{}, s.frames...)
				pubsubQUICReceipt(s.connection.owner.observation, "protocol", "go.quic.native_stream.selected", fields)
				for index := range s.negotiation {
					s.bodyLocked(index, s.negotiation[index].tail)
					s.negotiation[index].tail = nil
				}
				s.bodyLocked(side, data)
				return
			}
		}
	}
}

func pubsubQUICWireReceipt(frame []byte, side int) map[string]any {
	digest := sha256.Sum256(frame)
	return map[string]any{"framed_hex": hex.EncodeToString(frame), []string{"read", "write"}[side]: map[string]any{
		"framed_bytes": len(frame), "framed_sha256": hex.EncodeToString(digest[:]), "frames": 1,
		"complete_frames": true, "invalid_or_over_limit": false}}
}

func (s *pubsubQUICStream) bodyLocked(side int, data []byte) {
	if !pubsubQUICIsPubsub(s.selected) {
		return // Identify/Ping and other native handlers remain delegated, not PubSub RPCs.
	}
	d := &s.read
	if side == 1 {
		d = &s.write
	}
	d.feed(data, func(frame []byte, _ *pubsubpb.RPC) {
		fields := s.fields()
		fields["direction"], fields["receipt"] = []string{"read", "write"}[side], pubsubQUICWireReceipt(frame, side)
		o := s.connection.owner.observation
		o.mu.Lock()
		if o.wireBytes+len(frame) > pubsubScoringWireBytes {
			o.mu.Unlock()
			s.failLocked(fmt.Errorf("native QUIC PubSub wire capture bound"))
			return
		}
		o.wireBytes += len(frame)
		o.emitLocked("rpc", "go.quic.native_stream."+[]string{"read", "write"}[side], fields)
		o.mu.Unlock()
	}, s.failLocked)
}

func (s *pubsubQUICStream) begin(name string, side int) pubsubQUICOperation {
	q := s.connection.owner
	ack := int(q.publishedAck.Load())
	q.begin()
	s.mu.Lock()
	defer s.mu.Unlock()
	s.order++
	op := pubsubQUICOperation{name: name, started: s.order, ack: ack}
	if s.finalized && (pubsubQUICIsPubsub(s.selected) || s.cleanup != nil || s.abort != nil) {
		s.failLocked(fmt.Errorf("native QUIC operation after observation join"))
	}
	if side >= 0 {
		if s.abortReturn != nil {
			s.failLocked(fmt.Errorf("native QUIC I/O after bounded negotiation abort"))
		}
		s.activeIO[side]++
		if s.activeIO[side] > 1 {
			s.failLocked(fmt.Errorf("concurrent native QUIC prefix observations are ambiguous"))
		}
	} else {
		s.activeClose++
		if name == "stream_close" && s.reset != nil && s.reset.completed && s.reset.err == nil {
			op.reset = s.reset
		}
		if name == "stream_reset" {
			s.reset = &pubsubQUICReset{started: op.started, ack: ack}
			op.reset = s.reset
		} else if name == "stream_reset_with_error" {
			s.reset = nil
		}
		if name == "stream_close" || name == "stream_close_read" || name == "stream_reset" {
			s.terminal = &pubsubQUICReset{started: op.started, ack: ack}
			op.terminal = s.terminal
		} else if name == "stream_reset_with_error" {
			s.terminal = nil
		}
	}
	return op
}

func pubsubQUICDiagnostic(err error) string {
	switch value := err.(type) {
	case *quic.StreamError:
		if value == nil {
			return "typed-nil QUIC StreamError"
		}
	case *quic.ApplicationError:
		if value == nil {
			return "typed-nil QUIC ApplicationError"
		}
	case *network.StreamError:
		if value == nil {
			return "typed-nil network StreamError"
		}
		if inner, typed := value.TransportError.(*quic.StreamError); typed && inner == nil {
			return "network StreamError with typed-nil QUIC cause"
		}
	case *network.ConnError:
		if value == nil {
			return "typed-nil network ConnError"
		}
		if inner, typed := value.TransportError.(*quic.ApplicationError); typed && inner == nil {
			return "network ConnError with typed-nil QUIC cause"
		}
	}
	return pubsubScoringDiagnostic(err)
}

func pubsubQUICErrorFields(err error) map[string]any {
	fields := map[string]any{"error": nil, "error_type": nil, "outcome": "ok", "typed_cause": "none"}
	if err != nil {
		fields["error"], fields["error_type"] = pubsubQUICDiagnostic(err), fmt.Sprintf("%T", err)
		fields["outcome"], fields["typed_cause"] = "error", "opaque"
	}
	return fields
}

type pubsubQUICReturn struct {
	order           uint64
	ack             int
	reset, terminal *pubsubQUICReset
	resetSucceeded  bool
	selected        string
	parentContext   pubsubQUICContext
}

// Record the delegate return before context sampling or event publication.
// A subsequent terminal call cannot become authority for an earlier I/O return.
func (s *pubsubQUICStream) nativeReturned(op pubsubQUICOperation, n int, err error) pubsubQUICReturn {
	ack := int(s.connection.owner.publishedAck.Load())
	parent := pubsubQUICSample(s.connection.connectionContext)
	s.mu.Lock()
	defer s.mu.Unlock()
	s.order++
	result := pubsubQUICReturn{order: s.order, ack: ack, selected: s.selected, parentContext: parent}
	if s.cleanupRead == nil && op.name == "stream_read" && n == 0 && result.selected == "" &&
		ack != 0 && ack == s.createdAck && op.ack == ack && s.nativeBeginAck == ack && parent.done {
		if outer, ok := err.(*network.ConnError); ok && outer != nil && outer.ErrorCode == 0 {
			if cause, ok := outer.TransportError.(*quic.ApplicationError); ok && cause != nil && cause.ErrorCode == 0 &&
				cause.Remote == outer.Remote && parent.cause == cause {
				s.cleanupRead = &result
			}
		}
	}
	// One immutable successful full-disposal slot follows the sealed Read
	// RETURN, even if that Read's observation publication is still paused.
	if read := s.cleanupRead; read != nil && s.cleanupDisposal == nil && err == nil &&
		(op.name == "stream_close" || op.name == "stream_reset") && result.selected == "" &&
		op.ack == read.ack && result.ack == read.ack && op.started > read.order {
		s.cleanupDisposal = &pubsubQUICCleanupDisposal{readReturn: read.order, started: op.started,
			returned: result.order, ack: read.ack}
	}
	if op.name == "stream_reset" {
		op.reset.completed, op.reset.returned, op.reset.err = true, result.order, err
	}
	if op.terminal != nil {
		op.terminal.completed, op.terminal.returned, op.terminal.err = true, result.order, err
	}
	if ack != 0 {
		qualifies := func(attempt *pubsubQUICReset) bool {
			return attempt != nil && attempt.ack == ack && attempt.started < result.order &&
				(!attempt.completed || attempt.err == nil)
		}
		if qualifies(s.terminal) {
			result.terminal = s.terminal
		}
		if qualifies(s.reset) {
			result.reset = s.reset
			result.resetSucceeded = s.reset.completed && s.reset.err == nil && s.reset.returned < result.order
		}
	}
	// Seal the actual error RETURN independently of later context/event publication.
	// This slot is not acceptance: the complete prefix and original cause must
	// still pass queueStreamAbortLocked and the actual disposal/native join.
	if s.abortReturn == nil && n == 0 && result.selected == "" && ack != 0 && ack == s.createdAck &&
		op.ack == ack && !parent.done {
		if outer, ok := err.(*network.StreamError); ok && outer != nil && outer.ErrorCode == 0 {
			if cause, typed := outer.TransportError.(*quic.StreamError); typed && cause != nil && cause.ErrorCode == 0 &&
				cause.StreamID == s.nativeID && cause.Remote == outer.Remote &&
				((op.name == "stream_read" && s.direction == network.DirOutbound && !cause.Remote && result.resetSucceeded) ||
					(op.name == "stream_write" && s.direction == network.DirInbound && cause.Remote)) {
				s.abortReturn = &result
			}
		}
	}
	return result
}

func (s *pubsubQUICStream) returned(op pubsubQUICOperation, result pubsubQUICReturn, err error, sample pubsubQUICContext) map[string]any {
	fields := s.fields()
	for key, value := range pubsubQUICErrorFields(err) {
		fields[key] = value
	}
	fields["operation"], fields["started_order"], fields["returned_order"] = op.name, op.started, result.order
	fields["prepare_ack_sequence"] = op.ack
	fields["send_context"] = sample.fields()
	return fields
}

func (s *pubsubQUICStream) Read(p []byte) (int, error)  { return s.io(p, 0) }
func (s *pubsubQUICStream) Write(p []byte) (int, error) { return s.io(p, 1) }

func pubsubQUICCapturedBytes(data []byte, complete bool) map[string]any {
	digest := sha256.Sum256(data)
	fields := map[string]any{"bytes": len(data), "sha256": hex.EncodeToString(digest[:]),
		"hex": nil, "capture_complete": complete}
	if complete {
		fields["hex"] = hex.EncodeToString(data)
	}
	return fields
}

// Parser snapshots are diagnostics, never proof of a selected protocol or a
// complete RPC. In particular, lazy application bytes can precede the ACK.
func (s *pubsubQUICStream) negotiationSnapshotLocked(prefix []byte) map[string]any {
	bytes := len(prefix)
	for _, direction := range s.negotiation {
		bytes += len(direction.buffer) + len(direction.tail)
	}
	o := s.connection.owner.observation
	o.mu.Lock()
	complete := o.wireBytes+bytes <= pubsubScoringWireBytes
	if complete {
		o.wireBytes += bytes
	}
	o.mu.Unlock()
	if !complete {
		s.failLocked(fmt.Errorf("native QUIC negotiation snapshot capture bound"))
	}
	fields := map[string]any{"capture_complete": complete, "selected_protocol": s.selected,
		"proposal": s.proposal, "reply": s.reply, "touched_pubsub": s.touchedPubsub,
		"parser_failed": s.failed, "proposals": s.proposals,
		"frame_sequences": append([]int{}, s.frames...),
		"snapshot_basis":  "parser_state_after_successful_prefix_observation"}
	if prefix != nil {
		fields["successful_prefix"] = pubsubQUICCapturedBytes(prefix, complete)
	}
	for side, direction := range s.negotiation {
		fields[[]string{"read", "write"}[side]] = map[string]any{
			"header_seen": direction.header, "paused": direction.paused, "token": direction.token,
			"partial_frame": pubsubQUICCapturedBytes(direction.buffer, complete),
			"lazy_tail":     pubsubQUICCapturedBytes(direction.tail, complete)}
	}
	return fields
}

func (s *pubsubQUICStream) passiveReturnLocked(fields map[string]any, result pubsubQUICReturn,
	err error, sample, connectionSample pubsubQUICContext, prefix []byte) {
	q := s.connection.owner
	q.mu.Lock()
	key := pubsubQUICKey{s.connection.native, s.nativeID}
	baseline, streamExists := q.baseline[key]
	connectionBase, connectionExists := q.connBase[key.connection]
	baselineAck := q.prepareAck
	q.mu.Unlock()
	fields["observation_phase"] = "unselected_at_native_return"
	fields["protocol_at_native_return"] = result.selected
	fields["terminal_prepare_ack_sequence"], fields["prepare_snapshot_ack_sequence"] = result.ack, baselineAck
	fields["prepare_baseline_present"] = streamExists && connectionExists && baselineAck != 0
	fields["stream_prepare_baseline_present"], fields["connection_prepare_baseline_present"] = streamExists, connectionExists
	fields["send_context_at_prepare"], fields["connection_context_at_prepare"] = baseline.fields(), connectionBase.fields()
	fields["connection_context"] = connectionSample.fields()
	fields["context_observation_basis"] = "send_context_after_seal_and_parent_context_sealed_at_native_return"
	snapshot := s.negotiationSnapshotLocked(prefix)
	if fields["successful_prefix_valid"] == true && prefix == nil {
		snapshot["successful_prefix"] = pubsubQUICCapturedBytes(nil, snapshot["capture_complete"] == true)
	}
	fields["negotiation_snapshot"] = snapshot
	// Only direct public native types are described. Nothing here authorizes an
	// error, borrows an earlier stream's cause, or changes the delegate result.
	switch outer := err.(type) {
	case *network.StreamError:
		if outer != nil {
			fields["error_code"], fields["remote"] = uint32(outer.ErrorCode), outer.Remote
			fields["transport_error_type"] = fmt.Sprintf("%T", outer.TransportError)
			if inner, typed := outer.TransportError.(*quic.StreamError); typed && inner != nil {
				fields["typed_cause"] = "libp2p_quic_stream_error"
				fields["transport_error_code"], fields["transport_error_remote"] = uint64(inner.ErrorCode), inner.Remote
				fields["transport_native_stream_id"] = int64(inner.StreamID)
				fields["same_send_context_cause"] = sample.done && sample.cause == inner
			}
		}
	case *network.ConnError:
		if outer != nil {
			fields["error_code"], fields["remote"] = uint32(outer.ErrorCode), outer.Remote
			fields["transport_error_type"] = fmt.Sprintf("%T", outer.TransportError)
			if inner, typed := outer.TransportError.(*quic.ApplicationError); typed && inner != nil {
				fields["typed_cause"] = "libp2p_quic_application_error"
				fields["transport_error_code"], fields["transport_error_remote"] = uint64(inner.ErrorCode), inner.Remote
				fields["same_native_connection_context_cause"] = connectionSample.done && connectionSample.cause == inner
			}
		}
	}
}

func pubsubQUICEvent(o *pubsubScoringObserver, sequence int, kind, source string) map[string]any {
	o.mu.Lock()
	defer o.mu.Unlock()
	if sequence <= 0 || sequence > len(o.events) {
		return nil
	}
	event := o.events[sequence-1]
	if event["sequence"] != sequence || event["kind"] != kind || event["source"] != source {
		return nil
	}
	return event
}

func pubsubQUICSnapshotBytes(value any) ([]byte, bool) {
	fields, ok := value.(map[string]any)
	if !ok || len(fields) != 4 || fields["capture_complete"] != true {
		return nil, false
	}
	size, ok := fields["bytes"].(int)
	wire, encoded := fields["hex"].(string)
	if !ok || !encoded || size < 0 || size > pubsubScoringFrame+512 || len(wire) != 2*size {
		return nil, false
	}
	raw, err := hex.DecodeString(wire)
	digest := sha256.Sum256(raw)
	return raw, err == nil && hex.EncodeToString(raw) == wire && fields["sha256"] == hex.EncodeToString(digest[:])
}

// Decode candidate bytes without feeding the selected stream decoder or
// emitting RPC authority. Marshal equality rejects duplicate/noncanonical
// protobuf fields and booleans that Unmarshal would otherwise normalize.
func pubsubQUICSubscriptionBytes(raw []byte, topic string) bool {
	decoder := pubsubScoringDecoder{}
	count, valid := 0, false
	decoder.feed(raw, func(frame []byte, rpc *pubsubpb.RPC) {
		count++
		if count != 1 || len(frame) != len(raw) || len(rpc.Subscriptions) != 1 || len(rpc.Publish) != 0 ||
			rpc.Control != nil || rpc.Partial != nil || rpc.TestExtension != nil || len(rpc.XXX_unrecognized) != 0 {
			return
		}
		sub := rpc.Subscriptions[0]
		if sub == nil || sub.Subscribe == nil || !*sub.Subscribe || sub.Topicid == nil || *sub.Topicid != topic ||
			len(topic) == 0 || len(topic) > 255 || !utf8.ValidString(topic) || len(sub.XXX_unrecognized) != 0 ||
			(sub.RequestsPartial != nil && *sub.RequestsPartial) || (sub.SupportsSendingPartial != nil && *sub.SupportsSendingPartial) {
			return
		}
		_, header := binary.Uvarint(frame)
		canonical, err := rpc.Marshal()
		valid = err == nil && bytes.Equal(canonical, frame[header:])
	}, func(error) { valid = false })
	return valid && count == 1 && !decoder.failed && len(decoder.buffer) == 0
}

func (s *pubsubQUICStream) ownsFields(event map[string]any, streamRecord bool) bool {
	if event == nil {
		return false
	}
	fields := s.fields()
	if streamRecord {
		fields["native_stream_receipt_sequence"] = 0
	}
	for _, key := range []string{"native_connection_id", "native_stream_id", "connection_receipt_sequence",
		"native_stream_receipt_sequence", "remote_peer_id", "stream_direction", "owner_basis"} {
		if event[key] != fields[key] {
			return false
		}
	}
	return true
}

func (s *pubsubQUICStream) latePreparedOwner() bool {
	q := s.connection.owner
	q.mu.Lock()
	key := pubsubQUICKey{s.connection.native, s.nativeID}
	base, parentExists := q.connBase[key.connection]
	_, streamExisted := q.baseline[key]
	owned := q.connections[key.connection] == s.connection && q.streams[key] == s && parentExists && !base.done &&
		!streamExisted && q.prepareAck == s.createdAck
	q.mu.Unlock()
	if !owned || s.createdAck == 0 || s.nativeBeginAck != s.createdAck || s.createdContext.done || s.direction != network.DirOutbound ||
		s.connection.receipt <= 0 || s.connection.receipt >= s.createdAck || s.receipt <= s.createdAck {
		return false
	}
	o := q.observation
	o.mu.Lock()
	healthy := o.prepared && o.prepareAck == s.createdAck && o.failure == nil && !o.overflow
	o.mu.Unlock()
	ack := pubsubQUICEvent(o, s.createdAck, "shutdown_prepared", "go.fixture.prepare_shutdown")
	created := pubsubQUICEvent(o, s.receipt, "native_quic_stream", "go.quic.CapableConn.stream_return")
	return healthy && ack != nil && ack["actor"] == o.actor && ack["case_token"] == o.token &&
		ack["local_peer_id"] == o.local.String() && ack["admission_closed"] == true && ack["pending_commands"] == 0 &&
		s.ownsFields(created, true) && created["protocol"] == "" && created["prepare_ack_sequence"] == s.createdAck &&
		created["native_call_begin_prepare_ack_sequence"] == s.nativeBeginAck &&
		created["native_call_begin_observation_basis"] == "published_Prepare_ACK_before_native_CapableConn_call"
}

func (s *pubsubQUICStream) negotiationFrameLocked(reference int, direction, token string) ([]byte, bool) {
	frame := pubsubQUICEvent(s.connection.owner.observation, reference, "multistream_frame", "go.quic.native_stream.multistream")
	if !s.ownsFields(frame, false) || frame["direction"] != direction || frame["protocol"] != token {
		return nil, false
	}
	receipt, ok := frame["receipt"].(map[string]any)
	if !ok || len(receipt) != 2 {
		return nil, false
	}
	completion, ok := receipt[direction].(map[string]any)
	if !ok || len(completion) != 5 {
		return nil, false
	}
	wire, ok := receipt["framed_hex"].(string)
	raw, err := hex.DecodeString(wire)
	digest := sha256.Sum256(raw)
	length, header := binary.Uvarint(raw)
	var canonical [binary.MaxVarintLen64]byte
	valid := ok && err == nil && hex.EncodeToString(raw) == wire && completion["framed_bytes"] == len(raw) &&
		completion["framed_sha256"] == hex.EncodeToString(digest[:]) && completion["frames"] == 1 &&
		completion["complete_frames"] == true && completion["invalid_or_over_limit"] == false &&
		header > 0 && length <= 256 && header == binary.PutUvarint(canonical[:], length) &&
		len(raw) == header+int(length) && bytes.Equal(raw[:header], canonical[:header]) &&
		bytes.Equal(raw[header:], []byte(token+"\n"))
	return raw, valid
}

// A complete peer header is negotiation progress only, never a protocol ACK.
func (s *pubsubQUICStream) cleanupPeerHeaderLocked() (int, bool) {
	read := s.negotiation[0]
	if len(read.buffer)+len(read.tail) != 0 || read.paused || read.token != "" {
		return 0, false
	}
	if len(s.frames) == 2 {
		return 0, !read.header
	}
	if len(s.frames) != 3 || !read.header || s.frames[2] <= s.frames[1] {
		return 0, false
	}
	_, valid := s.negotiationFrameLocked(s.frames[2], "read", "/multistream/1.0.0")
	return s.frames[2], valid
}

func (s *pubsubQUICStream) subscriptionWriteLocked(event map[string]any) bool {
	peerHeader, headerOK := s.cleanupPeerHeaderLocked()
	if !s.latePreparedOwner() || s.failed || s.earlyError != nil || s.selected != "" || s.reply != "" ||
		!pubsubQUICIsPubsub(s.proposal) || s.proposals != 1 || !headerOK || !s.ownsFields(event, false) ||
		event["protocol"] != "" || event["protocol_at_native_return"] != "" || event["operation"] != "stream_write" ||
		event["outcome"] != "ok" || event["error"] != nil || event["error_type"] != nil ||
		event["prepare_ack_sequence"] != s.createdAck || event["terminal_prepare_ack_sequence"] != s.createdAck ||
		event["successful_prefix_valid"] != true {
		return false
	}
	snapshot, ok := event["negotiation_snapshot"].(map[string]any)
	if !ok || snapshot["capture_complete"] != true || snapshot["parser_failed"] != false ||
		snapshot["selected_protocol"] != "" || snapshot["proposal"] != s.proposal || snapshot["reply"] != "" {
		return false
	}
	refs, ok := snapshot["frame_sequences"].([]int)
	if !ok || len(refs) != 2 || refs[0] != s.frames[0] || refs[1] != s.frames[1] {
		return false
	}
	sequence, ok := event["sequence"].(int)
	if !ok || refs[0] <= s.receipt || refs[1] <= refs[0] || sequence <= refs[1] ||
		(peerHeader != 0 && peerHeader <= sequence) {
		return false
	}
	prefix, ok := pubsubQUICSnapshotBytes(snapshot["successful_prefix"])
	if !ok || len(prefix) == 0 || event["requested_bytes"] != len(prefix) || event["successful_prefix_bytes"] != len(prefix) {
		return false
	}
	for side, direction := range s.negotiation {
		captured, typed := snapshot[[]string{"read", "write"}[side]].(map[string]any)
		if !typed {
			return false
		}
		partial, partialOK := pubsubQUICSnapshotBytes(captured["partial_frame"])
		tail, tailOK := pubsubQUICSnapshotBytes(captured["lazy_tail"])
		if !partialOK || !tailOK || len(partial) != 0 || len(direction.buffer) != 0 ||
			(side == 0 && (captured["header_seen"] != false || captured["paused"] != false ||
				captured["token"] != "" || len(tail) != 0)) ||
			(side == 1 && (!bytes.Equal(tail, direction.tail) || captured["header_seen"] != direction.header ||
				captured["paused"] != direction.paused || captured["token"] != direction.token ||
				!direction.header || !direction.paused || direction.token != s.proposal ||
				!pubsubQUICSubscriptionBytes(tail, s.connection.owner.observation.topic))) {
			return false
		}
	}
	remaining := prefix
	for index, reference := range refs {
		token := "/multistream/1.0.0"
		if index == 1 {
			token = s.proposal
		}
		raw, valid := s.negotiationFrameLocked(reference, "write", token)
		if !valid || !bytes.HasPrefix(remaining, raw) {
			return false
		}
		remaining = remaining[len(raw):]
	}
	return bytes.Equal(remaining, s.negotiation[1].tail)
}

func (s *pubsubQUICStream) cleanupReadSnapshotLocked(event map[string]any) bool {
	if !s.ownsFields(event, false) || event["protocol"] != "" || event["protocol_at_native_return"] != "" ||
		event["operation"] != "stream_read" || event["direction"] != "read" || event["outcome"] != "error" ||
		event["successful_prefix_valid"] != true || event["successful_prefix_bytes"] != 0 {
		return false
	}
	snapshot, ok := event["negotiation_snapshot"].(map[string]any)
	if !ok || snapshot["capture_complete"] != true || snapshot["parser_failed"] != false ||
		snapshot["selected_protocol"] != "" || snapshot["proposal"] != s.proposal || snapshot["reply"] != "" {
		return false
	}
	refs, ok := snapshot["frame_sequences"].([]int)
	if !ok || len(refs) != len(s.frames) {
		return false
	}
	sequence, sequenceOK := event["sequence"].(int)
	for index, reference := range refs {
		if !sequenceOK || reference != s.frames[index] || reference >= sequence {
			return false
		}
	}
	prefix, ok := pubsubQUICSnapshotBytes(snapshot["successful_prefix"])
	if !ok || len(prefix) != 0 {
		return false
	}
	for side, direction := range s.negotiation {
		captured, ok := snapshot[[]string{"read", "write"}[side]].(map[string]any)
		if !ok || captured["header_seen"] != direction.header || captured["paused"] != direction.paused ||
			captured["token"] != direction.token {
			return false
		}
		partial, partialOK := pubsubQUICSnapshotBytes(captured["partial_frame"])
		tail, tailOK := pubsubQUICSnapshotBytes(captured["lazy_tail"])
		if !partialOK || !tailOK || len(partial) != 0 || len(direction.buffer) != 0 || !bytes.Equal(tail, direction.tail) {
			return false
		}
	}
	return true
}

func (s *pubsubQUICStream) queueNegotiationCleanupLocked(op pubsubQUICOperation, result pubsubQUICReturn,
	sequence, n int, err error) bool {
	outer, ok := err.(*network.ConnError)
	if !ok || outer == nil || outer.ErrorCode != 0 || op.name != "stream_read" || n != 0 ||
		s.cleanup != nil || s.cleanupRead == nil || s.cleanupRead.order != result.order || s.cleanupRead.ack != result.ack ||
		result.selected != "" || result.ack != s.createdAck || op.ack != result.ack || !result.parentContext.done {
		return false
	}
	cause, ok := outer.TransportError.(*quic.ApplicationError)
	if !ok || cause == nil || cause.ErrorCode != 0 || cause.Remote != outer.Remote || result.parentContext.cause != cause {
		return false
	}
	write := pubsubQUICEvent(s.connection.owner.observation, s.candidateWrite, "native_quic_negotiation_io_return", "go.quic.native_stream.io_return")
	original := pubsubQUICEvent(s.connection.owner.observation, sequence, "native_quic_negotiation_io_return", "go.quic.native_stream.io_return")
	peerHeader, headerOK := s.cleanupPeerHeaderLocked()
	if !headerOK || !s.subscriptionWriteLocked(write) || !s.cleanupReadSnapshotLocked(original) {
		return false
	}
	writeReturn, ok := write["returned_order"].(uint64)
	writeStart, startOK := write["started_order"].(uint64)
	s.connection.mu.Lock()
	contextSequence, current := s.connection.contextEvent, s.connection.context
	s.connection.mu.Unlock()
	if !ok || !startOK || writeStart == 0 || writeStart >= writeReturn || writeReturn >= result.order ||
		s.candidateWrite >= sequence || sequence == 0 || contextSequence == 0 || contextSequence >= sequence ||
		!current.done || current.cause != cause {
		return false
	}
	fields := s.fields()
	fields["operation_receipt_sequence"], fields["candidate_write_receipt_sequence"] = sequence, s.candidateWrite
	fields["connection_context_receipt_sequence"], fields["prepare_ack_sequence"] = contextSequence, result.ack
	fields["candidate_protocol"], fields["candidate_protocol_frame_sequence"] = s.proposal, s.frames[1]
	fields["peer_header_frame_sequence"] = peerHeader
	fields["candidate_bytes_complete"], fields["selected_rpc_authority"] = true, false
	fields["negotiation_complete"], fields["framing_clean"] = false, false
	fields["terminal_outcome"] = "negotiation_aborted_cleanup_pending"
	fields["same_native_connection_context_cause"] = true
	pendingSequence := pubsubQUICReceipt(s.connection.owner.observation, "native_quic_negotiation_cleanup_pending",
		"go.quic.native_stream.negotiation_cleanup", fields)
	if pendingSequence == 0 {
		return false
	}
	s.cleanup = &pubsubQUICCleanup{sequence: pendingSequence, errorSequence: sequence, writeSequence: s.candidateWrite,
		contextSequence: contextSequence, peerHeaderSequence: peerHeader, ack: result.ack, returned: result.order, err: err, cause: cause}
	s.pinCleanupDisposalLocked()
	return true
}

// Validate the entire unselected owner, not merely its terminal error or tail.
func (s *pubsubQUICStream) streamAbortShapeLocked() ([]int, bool) {
	return s.streamAbortPrefixLocked(1)
}

func (s *pubsubQUICStream) streamAbortPrefixLocked(expectedErrors int) ([]int, bool) {
	if s.failed || s.earlyError != nil || s.selected != "" || s.reply != "" || s.proposals != 1 || !pubsubQUICIsPubsub(s.proposal) {
		return nil, false
	}
	q := s.connection.owner
	q.mu.Lock()
	base, exists := q.connBase[s.connection.native]
	registered := q.streams[pubsubQUICKey{s.connection.native, s.nativeID}] == s && q.connections[s.connection.native] == s.connection
	q.mu.Unlock()
	if !registered || !exists || base.done || s.createdAck == 0 || s.createdContext.done || s.receipt <= s.createdAck ||
		(s.direction == network.DirOutbound && !s.latePreparedOwner()) {
		return nil, false
	}
	o := q.observation
	o.mu.Lock()
	healthy := o.prepared && o.prepareAck == s.createdAck && o.failure == nil && !o.overflow
	events := append([]map[string]any{}, o.events...)
	o.mu.Unlock()
	if !healthy {
		return nil, false
	}
	var observed [2][]byte
	errorsObserved := 0
	refs := []int{}
	for _, event := range events {
		if event["kind"] != "native_quic_negotiation_io_return" || !s.ownsFields(event, false) {
			continue
		}
		if event["protocol"] != "" || event["protocol_at_native_return"] != "" || event["prepare_ack_sequence"] != s.createdAck ||
			event["terminal_prepare_ack_sequence"] != s.createdAck || event["successful_prefix_valid"] != true {
			return nil, false
		}
		snapshot, ok := event["negotiation_snapshot"].(map[string]any)
		if !ok || snapshot["capture_complete"] != true || snapshot["parser_failed"] != false || snapshot["selected_protocol"] != "" || snapshot["reply"] != "" {
			return nil, false
		}
		prefix, ok := pubsubQUICSnapshotBytes(snapshot["successful_prefix"])
		if !ok || event["successful_prefix_bytes"] != len(prefix) {
			return nil, false
		}
		if event["error"] != nil {
			errorsObserved++
			if event["outcome"] != "error" || event["error_type"] != "*network.StreamError" || len(prefix) != 0 {
				return nil, false
			}
		}
		side := 0
		if event["direction"] == "write" {
			side = 1
		}
		if len(prefix) != 0 && (event["outcome"] != "ok" || event["error"] != nil ||
			(side == 1 && event["requested_bytes"] != len(prefix))) {
			return nil, false
		}
		if len(observed[side])+len(prefix) > pubsubScoringFrame+512 {
			return nil, false
		}
		observed[side] = append(observed[side], prefix...)
		refs = append(refs, event["sequence"].(int))
	}
	var expected [2][]byte
	var frameCounts [2]int
	for _, ref := range s.frames {
		frame := pubsubQUICEvent(o, ref, "multistream_frame", "go.quic.native_stream.multistream")
		if !s.ownsFields(frame, false) {
			return nil, false
		}
		direction, ok := frame["direction"].(string)
		token, typed := frame["protocol"].(string)
		if !ok || !typed || (direction != "read" && direction != "write") {
			return nil, false
		}
		side := 0
		if direction == "write" {
			side = 1
		}
		wanted := "/multistream/1.0.0"
		proposalSide := (s.direction == network.DirOutbound && side == 1) || (s.direction == network.DirInbound && side == 0)
		if proposalSide && len(expected[side]) != 0 {
			wanted = s.proposal
		}
		if token != wanted {
			return nil, false
		}
		raw, valid := s.negotiationFrameLocked(ref, direction, token)
		if !valid {
			return nil, false
		}
		expected[side] = append(expected[side], raw...)
		frameCounts[side]++
	}
	for side, direction := range s.negotiation {
		if len(direction.buffer) != 0 {
			return nil, false
		}
		proposalSide := (s.direction == network.DirOutbound && side == 1) || (s.direction == network.DirInbound && side == 0)
		if proposalSide {
			if !direction.header || !direction.paused || direction.token != s.proposal {
				return nil, false
			}
			if s.direction == network.DirOutbound {
				if !pubsubQUICSubscriptionBytes(direction.tail, o.topic) {
					return nil, false
				}
			} else if len(direction.tail) != 0 {
				return nil, false
			}
		} else if direction.paused || direction.token != "" || len(direction.tail) != 0 {
			return nil, false
		}
		if !bytes.Equal(observed[side], append(expected[side], direction.tail...)) {
			return nil, false
		}
	}
	return refs, errorsObserved == expectedErrors && ((s.direction == network.DirOutbound && frameCounts[1] == 2 && frameCounts[0] <= 1) ||
		(s.direction == network.DirInbound && frameCounts[0] == 2 && frameCounts[1] == 1))
}

func (s *pubsubQUICStream) abortFields() map[string]any {
	fields := s.fields()
	o := s.connection.owner.observation
	fields["actor"], fields["case_token"], fields["local_peer_id"], fields["pid"] = o.actor, o.token, o.local.String(), os.Getpid()
	return fields
}

func (s *pubsubQUICStream) queueStreamAbortLocked(op pubsubQUICOperation, result pubsubQUICReturn, sequence, n int, err error, sample pubsubQUICContext) bool {
	outer, ok := err.(*network.StreamError)
	if !ok || outer == nil || outer.ErrorCode != 0 || n != 0 || s.abort != nil || s.cleanup != nil || result.selected != "" ||
		result.ack == 0 || result.ack != s.createdAck || op.ack != result.ack || result.parentContext.done ||
		s.abortReturn == nil || s.abortReturn.order != result.order {
		return false
	}
	cause, ok := outer.TransportError.(*quic.StreamError)
	if !ok || cause == nil || cause.ErrorCode != 0 || cause.StreamID != s.nativeID || cause.Remote != outer.Remote {
		return false
	}
	localRead := s.direction == network.DirOutbound && op.name == "stream_read" && !cause.Remote && result.resetSucceeded
	peerWrite := s.direction == network.DirInbound && op.name == "stream_write" && cause.Remote && sample.done && sample.cause == cause
	if !localRead && !peerWrite {
		return false
	}
	refs, valid := s.streamAbortShapeLocked()
	if !valid || sequence == 0 {
		return false
	}
	fields := s.abortFields()
	fields["operation_receipt_sequence"], fields["prepare_ack_sequence"], fields["negotiation_io_receipt_sequences"] = sequence, result.ack, refs
	fields["negotiation_complete"], fields["framing_clean"], fields["selected_rpc_authority"], fields["candidate_bytes_complete"] = false, false, false, true
	fields["terminal_outcome"], fields["terminal_state_cause"] = "negotiation_stream_reset_abort_pending", "unknown"
	fields["owned_reset_started_order"], fields["owned_reset_returned_order"] = uint64(0), uint64(0)
	var reset *pubsubQUICReset
	if localRead {
		reset = result.reset
		fields["owned_reset_started_order"], fields["owned_reset_returned_order"] = reset.started, reset.returned
	}
	sequencePending := pubsubQUICReceipt(s.connection.owner.observation, "native_quic_negotiation_abort_pending", "go.quic.native_stream.negotiation_cleanup", fields)
	if sequencePending == 0 {
		return false
	}
	s.abort = &pubsubQUICStreamAbort{sequence: sequencePending, errorSequence: sequence, ack: result.ack, returned: result.order,
		err: err, reset: reset, ioSequences: refs, close: s.abortClose, closePending: s.abortClosePending}
	return true
}

func (s *pubsubQUICStream) streamAbortValidLocked() bool {
	abort := s.abort
	if abort == nil || s.activeIO[0]+s.activeIO[1]+s.activeClose != 0 || !s.ending[0] || !s.ending[1] ||
		len(s.read.buffer)+len(s.write.buffer) != 0 || s.read.failed || s.write.failed || len(s.pending) != 0 {
		return false
	}
	refs, valid := s.streamAbortShapeLocked()
	if !valid || len(refs) != len(abort.ioSequences) {
		return false
	}
	for index := range refs {
		if refs[index] != abort.ioSequences[index] {
			return false
		}
	}
	reset := s.reset
	if reset == nil || !reset.completed || reset.err != nil || reset.ack != abort.ack || reset.sequence == 0 {
		return false
	}
	if abort.reset != nil {
		if !abort.reset.completed || abort.reset.err != nil || abort.reset.sequence == 0 || abort.reset.returned >= abort.returned {
			return false
		}
	} else if reset.started <= abort.returned {
		return false
	}
	if close := abort.close; close != nil {
		if close.reset == nil || !close.reset.completed || close.reset.err != nil || close.reset.sequence == 0 || close.contextSequence == 0 || abort.closePending == 0 {
			return false
		}
	}
	return true
}

func (s *pubsubQUICStream) finishStreamAbort(joinSequence int) {
	s.mu.Lock()
	defer s.mu.Unlock()
	abort := s.abort
	if abort == nil || abort.finished {
		return
	}
	o := s.connection.owner.observation
	join := pubsubQUICEvent(o, joinSequence, "native_quic_join", "go.quic.fixture.owned_observation_join")
	accepted := s.finalized && abort.framingSequence != 0 && joinSequence > abort.framingSequence &&
		join != nil && join["active_native_calls"] == 0 && s.streamAbortValidLocked()
	disposal, closeFinal := 0, 0
	if s.reset != nil {
		disposal = s.reset.sequence
	}
	if close := abort.close; close != nil {
		fields := s.abortFields()
		fields["operation_receipt_sequence"], fields["negotiation_abort_pending_receipt_sequence"] = close.sequence, abort.sequence
		fields["pending_receipt_sequence"] = abort.closePending
		fields["framing_receipt_sequence"], fields["native_join_receipt_sequence"] = abort.framingSequence, joinSequence
		fields["owner_disposal_receipt_sequence"], fields["owned_terminal_receipt_sequence"] = disposal, close.reset.sequence
		fields["prepare_ack_sequence"], fields["send_context_receipt_sequence"] = abort.ack, close.contextSequence
		fields["terminal_outcome"], fields["terminal_state_cause"], fields["native_close_succeeded"] = "negotiation_abort_close_pending", "unknown", false
		fields["accepted"] = accepted
		closeFinal = pubsubQUICReceipt(o, "native_quic_terminal_finalized", "go.quic.native_stream.finalize", fields)
		if closeFinal == 0 || !accepted {
			o.fail(close.err)
		}
	}
	fields := s.abortFields()
	fields["pending_receipt_sequence"], fields["operation_receipt_sequence"] = abort.sequence, abort.errorSequence
	fields["prepare_ack_sequence"], fields["negotiation_io_receipt_sequences"] = abort.ack, abort.ioSequences
	fields["framing_receipt_sequence"], fields["native_join_receipt_sequence"] = abort.framingSequence, joinSequence
	fields["owner_disposal_receipt_sequence"], fields["first_owner_disposal_receipt_sequence"] = disposal, s.disposal
	fields["owned_reset_receipt_sequence"], fields["close_finalization_receipt_sequence"] = 0, closeFinal
	if abort.reset != nil {
		fields["owned_reset_receipt_sequence"] = abort.reset.sequence
	}
	fields["terminal_outcome"], fields["terminal_state_cause"] = "negotiation_stream_reset_abort", "unknown"
	fields["negotiation_complete"], fields["framing_clean"], fields["selected_rpc_authority"], fields["candidate_bytes_complete"] = false, false, false, accepted
	fields["accepted"] = accepted && (abort.close == nil || closeFinal != 0)
	if pubsubQUICReceipt(o, "native_quic_negotiation_abort_finalized", "go.quic.native_stream.negotiation_cleanup", fields) == 0 || !accepted {
		o.fail(abort.err)
	}
	abort.finished = true
}

func (s *pubsubQUICStream) pinCleanupDisposalLocked() {
	pending, disposal := s.cleanup, s.cleanupDisposal
	if pending != nil && pending.disposalSequence == 0 && disposal != nil && disposal.sequence != 0 &&
		disposal.readReturn == pending.returned && disposal.ack == pending.ack &&
		disposal.started > pending.returned && disposal.returned > disposal.started {
		pending.disposalSequence = disposal.sequence
	}
}

func (s *pubsubQUICStream) negotiationCleanupValidLocked() bool {
	pending := s.cleanup
	if pending == nil || s.activeIO[0]+s.activeIO[1]+s.activeClose != 0 || !s.ending[0] || !s.ending[1] ||
		s.failed || s.earlyError != nil || s.selected != "" || len(s.pending) != 0 ||
		len(s.read.buffer)+len(s.write.buffer) != 0 || s.read.failed || s.write.failed {
		return false
	}
	o := s.connection.owner.observation
	write := pubsubQUICEvent(o, pending.writeSequence, "native_quic_negotiation_io_return", "go.quic.native_stream.io_return")
	peerHeader, headerOK := s.cleanupPeerHeaderLocked()
	if !headerOK || peerHeader != pending.peerHeaderSequence || !s.subscriptionWriteLocked(write) {
		return false
	}
	original := pubsubQUICEvent(o, pending.errorSequence, "native_quic_negotiation_io_return", "go.quic.native_stream.io_return")
	disposal := pubsubQUICEvent(o, pending.disposalSequence, "native_stream_operation", "go.quic.native_stream.operation_return")
	if !s.cleanupReadSnapshotLocked(original) || original["protocol"] != "" || original["operation"] != "stream_read" ||
		original["outcome"] != "error" || original["successful_prefix_bytes"] != 0 ||
		original["returned_order"] != pending.returned || original["terminal_prepare_ack_sequence"] != pending.ack ||
		original["typed_cause"] != "libp2p_quic_application_error" || original["same_native_connection_context_cause"] != true ||
		!s.ownsFields(disposal, false) || disposal["protocol"] != "" || disposal["prepare_ack_sequence"] != pending.ack ||
		(disposal["operation"] != "stream_reset" && disposal["operation"] != "stream_close") ||
		disposal["outcome"] != "ok" || disposal["error"] != nil || disposal["error_type"] != nil || pending.disposalSequence == 0 {
		return false
	}
	start, startOK := disposal["started_order"].(uint64)
	returned, returnOK := disposal["returned_order"].(uint64)
	sealed := s.cleanupDisposal
	if !startOK || !returnOK || start <= pending.returned || returned <= start || sealed == nil ||
		sealed.readReturn != pending.returned || sealed.ack != pending.ack || sealed.sequence != pending.disposalSequence ||
		sealed.started != start || sealed.returned != returned {
		return false
	}
	current := pubsubQUICSample(s.connection.connectionContext)
	s.connection.mu.Lock()
	contextSequence, contextSample := s.connection.contextEvent, s.connection.context
	s.connection.mu.Unlock()
	if contextSequence != pending.contextSequence || !current.done || current.cause != pending.cause ||
		!contextSample.done || contextSample.cause != pending.cause {
		return false
	}
	send := pubsubQUICSample(s.sendContext)
	return pubsubQUICValidSendContext(send, s.nativeID) || (send.done && send.cause == pending.cause)
}

// This is not a selected-stream terminal. Publication waits for the existing
// native join receipt, while the original I/O error and incomplete ACK remain.
func (s *pubsubQUICStream) finishNegotiationCleanup(joinSequence int) {
	s.mu.Lock()
	defer s.mu.Unlock()
	pending := s.cleanup
	if pending == nil || pending.finished {
		return
	}
	o := s.connection.owner.observation
	joined := pubsubQUICEvent(o, joinSequence, "native_quic_join", "go.quic.fixture.owned_observation_join")
	accepted := s.finalized && pending.framingSequence != 0 && joinSequence > pending.framingSequence &&
		joined != nil && joined["active_native_calls"] == 0 &&
		joined["joined_scope"] == "fixture_lower_stream_IO_and_operations_not_all_donor_goroutines" &&
		s.negotiationCleanupValidLocked()
	fields := s.fields()
	fields["pending_receipt_sequence"], fields["operation_receipt_sequence"] = pending.sequence, pending.errorSequence
	fields["candidate_write_receipt_sequence"], fields["connection_context_receipt_sequence"] = pending.writeSequence, pending.contextSequence
	fields["prepare_ack_sequence"], fields["candidate_protocol"] = pending.ack, s.proposal
	fields["candidate_protocol_frame_sequence"] = s.frames[1]
	fields["peer_header_frame_sequence"] = pending.peerHeaderSequence
	fields["framing_receipt_sequence"], fields["owner_disposal_receipt_sequence"] = pending.framingSequence, pending.disposalSequence
	fields["first_owner_disposal_receipt_sequence"] = s.disposal
	fields["native_join_receipt_sequence"], fields["accepted"] = joinSequence, accepted
	fields["candidate_bytes_complete"] = s.subscriptionWriteLocked(pubsubQUICEvent(o, pending.writeSequence,
		"native_quic_negotiation_io_return", "go.quic.native_stream.io_return"))
	fields["selected_rpc_authority"], fields["negotiation_complete"], fields["framing_clean"] = false, false, false
	fields["terminal_outcome"] = "negotiation_aborted_cleanup"
	fields["same_native_connection_context_cause"] = pubsubQUICSample(s.connection.connectionContext).cause == pending.cause
	if pubsubQUICReceipt(o, "native_quic_negotiation_cleanup_finalized", "go.quic.native_stream.negotiation_cleanup", fields) == 0 || !accepted {
		o.fail(pending.err)
	}
	pending.finished = true
}

func (s *pubsubQUICStream) io(p []byte, side int) (int, error) {
	op := s.begin([]string{"stream_read", "stream_write"}[side], side)
	var n int
	var err error
	if side == 0 {
		n, err = s.MuxedStream.Read(p)
	} else {
		n, err = s.MuxedStream.Write(p)
	}
	result := s.nativeReturned(op, n, err)
	sample := pubsubQUICSample(s.sendContext)
	connectionSample := result.parentContext
	if outer, ok := err.(*network.ConnError); ok && outer != nil && outer.ErrorCode == 0 && side == 0 && n == 0 &&
		result.ack != 0 && result.ack == s.createdAck {
		if cause, ok := outer.TransportError.(*quic.ApplicationError); ok && cause != nil &&
			cause.ErrorCode == 0 && result.parentContext.done && result.parentContext.cause == cause {
			s.connection.recordContext() // Public current Context only; never a previous stream's Read cause.
		}
	}
	q := s.connection.owner
	baseline, connectionBase, existed := q.baselineFor(s)
	ack, returned := result.ack, result.order
	s.mu.Lock()
	fields := s.returned(op, result, err, sample)
	if n < 0 || n > len(p) {
		s.failLocked(fmt.Errorf("invalid native QUIC successful prefix count"))
	} else {
		s.feedLocked(side, p[:n])
	}
	if side == 1 && n < len(p) && err == nil && pubsubQUICIsPubsub(s.selected) {
		s.failLocked(io.ErrShortWrite)
	}
	passiveSequence := 0
	if result.selected == "" {
		passive := s.returned(op, result, err, sample)
		passive["direction"], passive["requested_bytes"], passive["successful_prefix_bytes"] = []string{"read", "write"}[side], len(p), n
		validPrefix := n >= 0 && n <= len(p)
		passive["successful_prefix_valid"] = validPrefix
		var prefix []byte
		if validPrefix {
			prefix = p[:n]
		}
		s.passiveReturnLocked(passive, result, err, sample, connectionSample, prefix)
		passiveSequence = pubsubQUICReceipt(q.observation, "native_quic_negotiation_io_return", "go.quic.native_stream.io_return", passive)
		if side == 1 && err == nil && s.candidateWrite == 0 {
			captured := pubsubQUICEvent(q.observation, passiveSequence, "native_quic_negotiation_io_return", "go.quic.native_stream.io_return")
			if s.subscriptionWriteLocked(captured) {
				s.candidateWrite = passiveSequence
			}
		}
	}
	if err != nil && pubsubQUICIsPubsub(s.selected) {
		fields["protocol"] = s.selected
		fields["direction"], fields["successful_prefix_bytes"] = []string{"read", "write"}[side], n
		fields["pending_frame_bytes"] = len(s.read.buffer)
		if side == 1 {
			fields["pending_frame_bytes"] = len(s.write.buffer)
		}
		fields["terminal_prepare_ack_sequence"] = ack
		fields["prepare_baseline_present"] = existed
		fields["send_context_at_prepare"], fields["connection_context_at_prepare"] = baseline.fields(), connectionBase.fields()
		pending := pubsubQUICPending{ack: ack, returned: returned, err: err, fields: fields}
		candidate := false
		if err == io.EOF && side == 0 {
			fields["outcome"], fields["typed_cause"] = "eof", "io_eof"
		} else if ack != 0 {
			if outer, ok := err.(*network.StreamError); ok && outer != nil && outer.ErrorCode == 0 {
				if inner, ok := outer.TransportError.(*quic.StreamError); ok && inner != nil && inner.ErrorCode == 0 && inner.StreamID == s.nativeID && inner.Remote == outer.Remote {
					fields["typed_cause"], fields["error_code"], fields["remote"] = "libp2p_quic_stream_error", uint64(inner.ErrorCode), inner.Remote
					fields["transport_error_type"], fields["transport_native_stream_id"] = fmt.Sprintf("%T", inner), int64(inner.StreamID)
					fields["transport_error_code"], fields["transport_error_remote"] = uint64(inner.ErrorCode), inner.Remote
					fields["same_send_context_cause"] = sample.done && sample.cause == inner
					if inner.Remote {
						candidate = existed && !baseline.done && !connectionBase.done &&
							(side == 0 || (sample.done && sample.cause == inner))
						fields["outcome"], fields["peer_reset_reason"] = "peer_zero_reset_pending", "unknown"
					} else if side == 0 {
						terminal := result.terminal
						if result.resetSucceeded {
							terminal = result.reset // Successful full Reset sealed before this RETURN.
						}
						if terminal != nil {
							candidate, pending.reset = true, terminal
							fields["outcome"] = "owned_read_terminal_pending"
						}
					} else if result.terminal != nil { // Preserve the existing Write gate.
						if reset := result.reset; reset != nil && sample.cause == inner {
							candidate, pending.reset = true, reset
							fields["outcome"] = "owned_reset_pending"
						}
					}
				}
			} else if outer, ok := err.(*network.ConnError); ok && outer != nil && outer.ErrorCode == 0 {
				if inner, ok := outer.TransportError.(*quic.ApplicationError); ok && inner != nil && inner.ErrorCode == 0 && inner.Remote == outer.Remote {
					candidate, pending.connectionCause = true, inner
					fields["outcome"], fields["typed_cause"] = "connection_application_close_pending", "libp2p_quic_application_error"
					fields["error_code"], fields["remote"] = uint64(inner.ErrorCode), inner.Remote
					fields["transport_error_type"] = fmt.Sprintf("%T", inner)
					fields["transport_error_code"], fields["transport_error_remote"] = uint64(inner.ErrorCode), inner.Remote
				}
			}
		}
		if !candidate && err != io.EOF {
			fields["outcome"] = "error"
		}
		sequence := pubsubQUICReceipt(q.observation, "stream_io_terminal", "go.quic.native_stream.io_return", fields)
		if candidate {
			pending.sequence = sequence
			s.queueLocked(pending)
		} else if err != io.EOF || side != 0 {
			q.observation.fail(err)
		}
	} else if err != nil && err != io.EOF && s.selected == "" && s.earlyError == nil {
		if !s.queueNegotiationCleanupLocked(op, result, passiveSequence, n, err) &&
			!s.queueStreamAbortLocked(op, result, passiveSequence, n, err, sample) {
			s.earlyError = err
			if s.touchedPubsub {
				s.failLocked(err)
			}
		}
	}
	if err != nil {
		s.ending[side] = true
	}
	s.activeIO[side]--
	s.mu.Unlock()
	q.end()
	return n, err
}

func pubsubQUICCloseErrorClassification(err error) string {
	if err == nil {
		return "none"
	}
	// These public types are causes, not the pinned send-stream Close's
	// nonspecific fmt error. A zero send context cannot authorize another cause.
	switch err.(type) {
	case syscall.Errno, net.Error, tls.AlertError, *tls.AlertError, tls.RecordHeaderError, *tls.RecordHeaderError,
		*network.StreamError, *network.ConnError,
		*quic.StreamError, *quic.ApplicationError, *quic.TransportError,
		*quic.VersionNegotiationError, *quic.StatelessResetError,
		*quic.IdleTimeoutError, *quic.HandshakeTimeoutError,
		quic.StreamLimitReachedError, *quic.StreamLimitReachedError,
		*quic.DatagramTooLargeError:
		return "public_typed_error"
	}
	switch err {
	case context.Canceled, context.DeadlineExceeded, io.EOF, io.ErrClosedPipe,
		io.ErrShortWrite, io.ErrShortBuffer, io.ErrUnexpectedEOF, io.ErrNoProgress,
		fs.ErrClosed, fs.ErrInvalid, fs.ErrPermission, fs.ErrExist, fs.ErrNotExist,
		errors.ErrUnsupported, network.ErrReset, quic.Err0RTTRejected:
		return "known_sentinel_error"
	}
	_, single := err.(interface{ Unwrap() error })
	_, joined := err.(interface{ Unwrap() []error })
	if single || joined {
		return "wrapped_or_joined_error"
	}
	return "opaque_unwrapped_native_error"
}

func (s *pubsubQUICStream) queueLocked(pending pubsubQUICPending) {
	if pending.sequence == 0 || len(s.pending) == 32 {
		s.failLocked(fmt.Errorf("native QUIC pending terminal capture bound"))
		return
	}
	s.pending = append(s.pending, pending)
}

func (s *pubsubQUICStream) close(name string, side int, code *network.StreamErrorCode) error {
	op := s.begin(name, -1)
	var err error
	switch name {
	case "stream_close":
		err = s.MuxedStream.Close()
	case "stream_close_read":
		err = s.MuxedStream.CloseRead()
	case "stream_close_write":
		err = s.MuxedStream.CloseWrite()
	case "stream_reset":
		err = s.MuxedStream.Reset()
	case "stream_reset_with_error":
		err = s.MuxedStream.ResetWithError(*code)
	}
	result := s.nativeReturned(op, -1, err)
	// This is the actual lower send context at Close RETURN, not a cached Read cause.
	sample := pubsubQUICSample(s.sendContext)
	connectionSample := result.parentContext
	baseline, _, existed := s.connection.owner.baselineFor(s)
	s.mu.Lock()
	returned := result.order
	fields := s.returned(op, result, err, sample)
	fields["requested_reset_code"] = nil
	if code != nil {
		fields["requested_reset_code"] = uint32(*code)
	}
	classification := pubsubQUICCloseErrorClassification(err)
	if name == "stream_close" {
		fields["native_close_error_classification"] = classification
	}
	if result.selected == "" {
		s.passiveReturnLocked(fields, result, err, sample, connectionSample, nil)
	}
	candidate := false
	abortClose := false
	if err != nil && pubsubQUICIsPubsub(s.selected) && name == "stream_close" && op.ack != 0 && classification == "opaque_unwrapped_native_error" {
		if cause, ok := sample.cause.(*quic.StreamError); sample.done && ok && cause != nil && cause.ErrorCode == 0 && cause.StreamID == s.nativeID {
			owned := op.reset != nil && op.reset.ack == op.ack && op.reset.completed && op.reset.err == nil && op.reset.returned < op.started
			candidate = owned || (cause.Remote && existed && !baseline.done)
			if candidate {
				fields["outcome"], fields["terminal_state_cause"] = "native_send_reset_close_pending", "observed_native_send_context_not_graceful_close"
				fields["send_context_at_prepare"] = baseline.fields()
				if !owned {
					op.reset = nil
				}
			}
		}
	}
	if err != nil && s.abortReturn != nil && s.abortClose == nil && name == "stream_close" && result.selected == "" &&
		op.ack == s.abortReturn.ack && s.abortReturn.order < op.started && classification == "opaque_unwrapped_native_error" && !result.parentContext.done &&
		op.reset != nil && op.reset.ack == op.ack && op.reset.completed && op.reset.err == nil && op.reset.returned < op.started {
		if cause, ok := sample.cause.(*quic.StreamError); sample.done && ok && cause != nil && !cause.Remote &&
			cause.ErrorCode == 0 && cause.StreamID == s.nativeID {
			expectedErrors := 0
			if s.abort != nil {
				expectedErrors = 1
			}
			_, abortClose = s.streamAbortPrefixLocked(expectedErrors)
		}
	}
	if abortClose {
		fields["terminal_state_cause"], fields["native_close_succeeded"] = "unknown", false
		fields["observed_reset_started_order"], fields["observed_reset_returned_order"] = op.reset.started, op.reset.returned
	}
	sequence := 0
	if pubsubQUICIsPubsub(s.selected) || result.selected == "" {
		contextSequence := 0
		if name == "stream_close" {
			contextFields := s.fields()
			contextFields["started_order"], contextFields["returned_order"] = op.started, returned
			contextFields["prepare_ack_sequence"], contextFields["send_context"] = op.ack, sample.fields()
			contextFields["observation_basis"] = "same_lower_delegate_Context_at_actual_Close_RETURN"
			if result.selected == "" {
				contextFields["observation_phase"], contextFields["protocol_at_native_return"] = "unselected_at_native_return", result.selected
				contextFields["connection_context"] = connectionSample.fields()
			}
			contextSequence = pubsubQUICReceipt(s.connection.owner.observation, "native_quic_send_context", "go.quic.Stream.Context.at_Close_return", contextFields)
			fields["send_context_receipt_sequence"] = contextSequence
		}
		sequence = pubsubQUICReceipt(s.connection.owner.observation, "native_stream_operation", "go.quic.native_stream.operation_return", fields)
		if candidate {
			s.queueLocked(pubsubQUICPending{sequence: sequence, ack: op.ack, returned: returned, err: err,
				fields: fields, reset: op.reset, contextSequence: contextSequence})
		} else if abortClose {
			s.abortClose = &pubsubQUICPending{sequence: sequence, ack: op.ack, returned: returned, err: err,
				fields: fields, reset: op.reset, contextSequence: contextSequence}
			pendingFields := s.abortFields()
			pendingFields["operation_receipt_sequence"], pendingFields["negotiation_abort_returned_order"] = sequence, s.abortReturn.order
			pendingFields["prepare_ack_sequence"], pendingFields["terminal_state_cause"], pendingFields["native_close_succeeded"] = op.ack, "unknown", false
			pendingFields["observed_reset_started_order"], pendingFields["observed_reset_returned_order"] = op.reset.started, op.reset.returned
			s.abortClosePending = pubsubQUICReceipt(s.connection.owner.observation, "native_quic_negotiation_abort_close_pending", "go.quic.native_stream.negotiation_cleanup", pendingFields)
			if s.abort != nil {
				s.abort.close, s.abort.closePending = s.abortClose, s.abortClosePending
			}
		} else if err != nil && pubsubQUICIsPubsub(s.selected) {
			s.connection.owner.observation.fail(err)
		}
		if code != nil && *code != 0 && pubsubQUICIsPubsub(s.selected) {
			s.failLocked(fmt.Errorf("native QUIC PubSub explicit nonzero reset code"))
		}
	}
	if err != nil && !abortClose && s.selected == "" && s.earlyError == nil {
		s.earlyError = err
		if s.touchedPubsub {
			s.failLocked(err)
		}
	}
	if s.abort != nil && name != "stream_reset" && name != "stream_close" {
		s.failLocked(fmt.Errorf("negotiation abort requires full Reset/Close, not a half-close or explicit reset code"))
	}
	if op.reset != nil && name == "stream_reset" {
		op.reset.sequence = sequence
	}
	if op.terminal != nil {
		op.terminal.sequence = sequence
	}
	if side < 0 && err == nil && s.disposal == 0 {
		s.disposal = sequence
	}
	// Fill only this exact sealed disposal's receipt, independently of whether
	// the Read has published its cleanup candidate. The first disposal is kept.
	if disposal := s.cleanupDisposal; disposal != nil && disposal.sequence == 0 &&
		disposal.started == op.started && disposal.returned == returned && err == nil {
		disposal.sequence = sequence
	}
	s.pinCleanupDisposalLocked()
	for index := range s.ending {
		if side < 0 || side == index {
			s.ending[index] = true
		}
	}
	s.activeClose--
	s.mu.Unlock()
	s.connection.owner.end()
	return err
}

func (s *pubsubQUICStream) Close() error      { return s.close("stream_close", -1, nil) }
func (s *pubsubQUICStream) CloseRead() error  { return s.close("stream_close_read", 0, nil) }
func (s *pubsubQUICStream) CloseWrite() error { return s.close("stream_close_write", 1, nil) }
func (s *pubsubQUICStream) Reset() error      { return s.close("stream_reset", -1, nil) }
func (s *pubsubQUICStream) ResetWithError(code network.StreamErrorCode) error {
	return s.close("stream_reset_with_error", -1, &code)
}

func (s *pubsubQUICStream) finalize() bool {
	q := s.connection.owner
	baseline, connectionBase, existed := q.baselineFor(s)
	s.connection.mu.Lock()
	connectionContext, connectionContextSequence := s.connection.context, s.connection.contextEvent
	s.connection.mu.Unlock()
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.activeIO[0]+s.activeIO[1]+s.activeClose != 0 {
		return false
	}
	if s.finalized {
		return true
	}
	if !s.touchedPubsub {
		s.finalized = true
		return true
	}
	send := pubsubQUICSample(s.sendContext)
	if !pubsubQUICValidSendContext(send, s.nativeID) &&
		!(send.done && connectionContextSequence != 0 && send.cause == connectionContext.cause) {
		s.failLocked(fmt.Errorf("native QUIC final send context has nonzero/foreign/unclassified cause"))
	}
	clean := pubsubQUICIsPubsub(s.selected) && !s.failed && s.ending[0] && s.ending[1] &&
		len(s.read.buffer)+len(s.write.buffer) == 0 && !s.read.failed && !s.write.failed
	fields := s.fields()
	fields["framing_clean"], fields["io_joined"] = clean, true
	fields["read_finalized"], fields["write_finalized"] = true, true
	fields["negotiation_complete"] = pubsubQUICIsPubsub(s.selected) && !s.failed
	fields["pending_read_frame_bytes"], fields["pending_write_frame_bytes"] = len(s.read.buffer), len(s.write.buffer)
	fields["native_owner_disposed"], fields["owner_disposal_receipt_sequence"] = s.disposal != 0, s.disposal
	if s.selected == "" {
		fields["negotiation_snapshot"] = s.negotiationSnapshotLocked(nil)
	}
	cleanup := s.cleanup != nil && s.negotiationCleanupValidLocked()
	abortClean := s.abort != nil && s.streamAbortValidLocked()
	if s.cleanup != nil {
		fields["candidate_bytes_complete"], fields["selected_rpc_authority"] = cleanup, false
		fields["negotiation_cleanup_pending_receipt_sequence"] = s.cleanup.sequence
		fields["peer_header_frame_sequence"] = s.cleanup.peerHeaderSequence
		fields["cleanup_owner_disposal_receipt_sequence"] = s.cleanup.disposalSequence
	}
	if s.abort != nil {
		fields["candidate_bytes_complete"], fields["selected_rpc_authority"] = abortClean, false
		fields["negotiation_abort_pending_receipt_sequence"] = s.abort.sequence
		fields["abort_owner_disposal_receipt_sequence"] = 0
		if s.reset != nil {
			fields["abort_owner_disposal_receipt_sequence"] = s.reset.sequence
		}
	}
	framingSequence := pubsubQUICReceipt(q.observation, "native_quic_framing_finalized", "go.quic.native_stream.finalize", fields)
	if s.cleanup != nil {
		s.cleanup.framingSequence = framingSequence
		if !cleanup || framingSequence == 0 {
			q.observation.fail(s.cleanup.err)
		}
	} else if s.abort != nil {
		s.abort.framingSequence = framingSequence
		if !abortClean || framingSequence == 0 {
			q.observation.fail(s.abort.err)
		}
	} else if !clean || s.disposal == 0 || framingSequence == 0 {
		q.observation.fail(fmt.Errorf("native QUIC PubSub negotiation/framing/disposal incomplete"))
	}
	for _, pending := range s.pending {
		q.observation.mu.Lock()
		accepted := clean && s.disposal != 0 && pending.sequence != 0 && pending.ack != 0 &&
			q.observation.prepared && q.observation.prepareAck == pending.ack && q.observation.failure == nil && !q.observation.overflow
		q.observation.mu.Unlock()
		if pending.reset != nil {
			reset := pending.reset
			accepted = accepted && reset.completed && reset.err == nil && reset.sequence != 0 && reset.ack == pending.ack && reset.started < pending.returned
		}
		if pending.connectionCause != nil {
			accepted = accepted && existed && !connectionBase.done && connectionContext.done &&
				connectionContextSequence != 0 && connectionContext.cause == pending.connectionCause
		}
		if pending.fields["outcome"] == "peer_zero_reset_pending" {
			accepted = accepted && existed && !baseline.done && !connectionBase.done
		}
		if pending.fields["outcome"] == "native_send_reset_close_pending" {
			accepted = accepted && pending.contextSequence != 0
		}
		value := s.fields()
		value["operation_receipt_sequence"], value["framing_receipt_sequence"] = pending.sequence, framingSequence
		value["owner_disposal_receipt_sequence"], value["accepted"] = s.disposal, accepted
		value["terminal_outcome"], value["prepare_ack_sequence"] = pending.fields["outcome"], pending.ack
		value["connection_context_receipt_sequence"] = connectionContextSequence
		value["send_context_receipt_sequence"] = pending.contextSequence
		if pending.connectionCause != nil {
			value["same_native_connection_context_cause"] = connectionContext.cause == pending.connectionCause
		}
		if pending.reset != nil {
			value["owned_terminal_receipt_sequence"] = pending.reset.sequence
		}
		pubsubQUICReceipt(q.observation, "native_quic_terminal_finalized", "go.quic.native_stream.finalize", value)
		if !accepted {
			q.observation.fail(pending.err)
		}
	}
	s.finalized = true
	s.read.finished, s.write.finished = true, true
	return true
}

func (q *pubsubQUICObserver) join(ctx context.Context) error {
	for {
		q.mu.Lock()
		active := q.active
		connections := make([]*pubsubQUICConn, 0, len(q.connections))
		streams := make([]*pubsubQUICStream, 0, len(q.streams))
		for _, c := range q.connections {
			connections = append(connections, c)
		}
		for _, s := range q.streams {
			streams = append(streams, s)
		}
		q.mu.Unlock()
		if active == 0 {
			for _, c := range connections {
				select {
				case <-c.connectionContext.Done():
					c.recordContext()
				case <-ctx.Done():
					return fmt.Errorf("native QUIC connection context did not finish: %w", ctx.Err())
				}
			}
			joined := true
			for _, s := range streams {
				joined = s.finalize() && joined
			}
			if joined {
				q.mu.Lock()
				unchanged := q.active == 0 && len(q.connections) == len(connections) && len(q.streams) == len(streams)
				q.mu.Unlock()
				if !unchanged {
					continue // Reuse the existing join wait; no native call or added worker.
				}
				sequence := pubsubQUICReceipt(q.observation, "native_quic_join", "go.quic.fixture.owned_observation_join", map[string]any{
					"active_native_calls": active, "observed_connections": len(connections), "observed_streams": len(streams),
					"joined_scope": "fixture_lower_stream_IO_and_operations_not_all_donor_goroutines"})
				for _, s := range streams {
					s.finishNegotiationCleanup(sequence)
					s.finishStreamAbort(sequence)
				}
				return nil
			}
		}
		select {
		case <-q.changed:
		case <-ctx.Done():
			return fmt.Errorf("native QUIC lower observations did not join: %w", ctx.Err())
		}
	}
}

// Swarm-level bookkeeping retains the existing fixture cleanup calls. It emits
// no RPC/context/terminal authority and never maps a Swarm ID to a QUIC ID.
type pubsubQUICHostStream struct {
	network.Stream
	drain    *pubsubScoringDrain
	mu       sync.Mutex
	active   int
	disposed bool
}

func (s *pubsubQUICHostStream) begin() {
	s.drain.mu.Lock()
	s.drain.active++
	s.drain.mu.Unlock()
	s.mu.Lock()
	s.active++
	s.mu.Unlock()
}

func (s *pubsubQUICHostStream) end(disposed bool) {
	s.mu.Lock()
	s.active--
	s.disposed = s.disposed || disposed
	release := s.active == 0 && s.disposed
	s.mu.Unlock()
	if release {
		s.drain.release(s)
	}
	s.drain.end()
}

func (s *pubsubQUICHostStream) framingJoined() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.active == 0 && s.disposed
}

func (s *pubsubQUICHostStream) Read(p []byte) (int, error) {
	s.begin()
	defer s.end(false)
	return s.Stream.Read(p)
}
func (s *pubsubQUICHostStream) Write(p []byte) (int, error) {
	s.begin()
	defer s.end(false)
	return s.Stream.Write(p)
}
func (s *pubsubQUICHostStream) Close() error {
	s.begin()
	err := s.Stream.Close()
	s.end(err == nil)
	return err
}
func (s *pubsubQUICHostStream) Reset() error {
	s.begin()
	err := s.Stream.Reset()
	s.end(err == nil)
	return err
}
func (s *pubsubQUICHostStream) ResetWithError(code network.StreamErrorCode) error {
	s.begin()
	err := s.Stream.ResetWithError(code)
	s.end(err == nil)
	return err
}

var _ transport.Transport = (*pubsubQUICTransport)(nil)
var _ transport.Listener = (*pubsubQUICListener)(nil)
var _ transport.CapableConn = (*pubsubQUICConn)(nil)
var _ network.MuxedStream = (*pubsubQUICStream)(nil)
var _ network.Stream = (*pubsubQUICHostStream)(nil)
