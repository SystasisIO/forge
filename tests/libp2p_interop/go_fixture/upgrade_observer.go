package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"hash"
	"io"
	"net"
	"strings"
	"sync"

	"github.com/libp2p/go-libp2p/core/crypto"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/core/protocol"
	"github.com/libp2p/go-libp2p/core/sec"
	"github.com/libp2p/go-libp2p/core/transport"
	"github.com/libp2p/go-libp2p/p2p/muxer/yamux"
	tptu "github.com/libp2p/go-libp2p/p2p/net/upgrader"
	"github.com/libp2p/go-libp2p/p2p/security/noise"
	sectls "github.com/libp2p/go-libp2p/p2p/security/tls"
	"github.com/libp2p/go-libp2p/p2p/transport/tcp"
	"github.com/libp2p/go-libp2p/p2p/transport/tcpreuse"
	manet "github.com/multiformats/go-multiaddr/net"
)

const upgradeConnectionLimit = 16
const upgradeStreamLimit = 32
const upgradeEventLimit = 256
const upgradeFrameLimit = 256
const upgradeProposalLimit = 8

// Only negotiation frames are retained. No security handshake or application payload is captured.
type upgradeEvent struct {
	Sequence        uint64 `json:"sequence"`
	Kind            string `json:"kind"`
	Phase           string `json:"phase,omitempty"`
	Direction       string `json:"direction,omitempty"`
	Protocol        string `json:"protocol,omitempty"`
	Stream          uint64 `json:"stream_trace_id,omitempty"`
	ConnectionID    string `json:"network_connection_id,omitempty"`
	NetworkStreamID string `json:"network_stream_id,omitempty"`
	FrameHex        string `json:"frame_hex,omitempty"`
	Error           string `json:"error,omitempty"`
}

type upgradeConnectionEvidence struct {
	ID            uint64                  `json:"connection_trace_id"`
	Direction     string                  `json:"direction"`
	LocalAddress  string                  `json:"local_address"`
	RemoteAddress string                  `json:"remote_address"`
	LocalPeer     string                  `json:"authenticated_local_peer_id"`
	RemotePeer    string                  `json:"authenticated_remote_peer_id"`
	Security      string                  `json:"selected_security"`
	Muxer         string                  `json:"selected_muxer"`
	EarlyMuxer    bool                    `json:"early_muxer_negotiation"`
	Closed        bool                    `json:"underlying_close_returned"`
	Failed        bool                    `json:"failed"`
	Events        []upgradeEvent          `json:"events"`
	Streams       []upgradeStreamEvidence `json:"streams"`
}

type upgradeStreamEvidence struct {
	ID                    uint64 `json:"stream_trace_id"`
	Direction             string `json:"direction"`
	Protocol              string `json:"protocol"`
	NetworkStreamID       string `json:"network_stream_id,omitempty"`
	ApplicationIOComplete bool   `json:"application_io_complete"`
	ResponseWriteComplete bool   `json:"response_write_complete"`
	Failed                bool   `json:"failed"`
	Reset                 bool   `json:"reset"`
	ReadFrames            uint64 `json:"read_frames"`
	WriteFrames           uint64 `json:"write_frames"`
	ReadBytes             uint64 `json:"read_framed_bytes"`
	WriteBytes            uint64 `json:"write_framed_bytes"`
	ReadSHA256            string `json:"read_framed_sha256,omitempty"`
	WriteSHA256           string `json:"write_framed_sha256,omitempty"`
}

type upgradeEvidence struct {
	Source           string                      `json:"source"`
	Finalized        bool                        `json:"finalized_after_host_close"`
	Complete         bool                        `json:"complete"`
	Overflow         bool                        `json:"overflow"`
	Connections      []upgradeConnectionEvidence `json:"connections"`
	TargetConnection uint64                      `json:"target_connection_trace_id,omitempty"`
	TargetStreams    []uint64                    `json:"target_stream_trace_ids"`
}

type upgradeTarget struct {
	connection uint64
	stream     uint64
}

type upgradeObserver struct {
	mu          sync.Mutex
	connections []*upgradeConnection
	overflow    bool
}

type upgradeConnection struct {
	owner        *upgradeObserver
	evidence     upgradeConnectionEvidence
	sequence     uint64
	stage        int
	streams      uint64
	streamStates []*selectionTrace
	security     selectionTrace
	muxer        selectionTrace
}

// All parser and trace mutation is serialized by the owning observer, never across delegated I/O.
type selectionTrace struct {
	connection         *upgradeConnection
	phase              string
	stream             uint64
	direction          string
	read               frameDecoder
	write              frameDecoder
	selected           string
	proposal           string
	reply              string
	proposals          int
	unconfirmedTail    bool
	failed             bool
	stopped            bool
	closed             bool
	writeClosed        bool
	reset              bool
	ioFailed           bool
	active             int
	boundID            string
	applicationDone    bool
	completionReported bool
	readBody           framedBodyDigest
	writeBody          framedBodyDigest
}

// Incremental framing/count/digest only. Body bytes never enter retained evidence or buffers.
type framedBodyDigest struct {
	digest     hash.Hash
	total      uint64
	frames     uint64
	length     uint64
	headerSize int
	remaining  uint64
	failed     bool
}

func (b *framedBodyDigest) feed(data []byte, limit uint64) {
	if b.failed {
		return
	}
	if uint64(len(data)) > limit-b.total {
		b.failed = true
		return
	}
	if b.digest == nil {
		b.digest = sha256.New()
	}
	b.total += uint64(len(data))
	b.digest.Write(data)
	for len(data) > 0 {
		if b.remaining > 0 {
			n := min(b.remaining, uint64(len(data)))
			b.remaining -= n
			data = data[n:]
			if b.remaining == 0 {
				b.frames++
			}
			continue
		}
		v := data[0]
		data = data[1:]
		if b.headerSize == 10 || (b.headerSize == 9 && v > 1) || b.frames == 4 {
			b.failed = true
			return
		}
		b.length |= uint64(v&127) << (7 * b.headerSize)
		b.headerSize++
		if v&128 != 0 {
			continue
		}
		if b.length == 0 || b.length > limit || (b.headerSize > 1 && v == 0) {
			b.failed = true
			return
		}
		b.remaining, b.length, b.headerSize = b.length, 0, 0
	}
}

func (b *framedBodyDigest) complete() bool {
	return !b.failed && b.frames > 0 && b.remaining == 0 && b.headerSize == 0
}

func (b *framedBodyDigest) sum() string {
	if b.digest == nil {
		return ""
	}
	return hex.EncodeToString(b.digest.Sum(nil))
}

type frameDecoder struct {
	header     [10]byte
	headerSize int
	length     uint64
	data       [upgradeFrameLimit]byte
	size       int
	stage      int
	selected   string
	paused     bool
}

func (o *upgradeObserver) connection(c manet.Conn, direction network.Direction) *upgradeConnection {
	o.mu.Lock()
	defer o.mu.Unlock()
	if len(o.connections) == upgradeConnectionLimit {
		o.overflow = true
		return nil
	}
	t := &upgradeConnection{owner: o, evidence: upgradeConnectionEvidence{
		ID: uint64(len(o.connections) + 1), Direction: direction.String(),
		LocalAddress: c.LocalMultiaddr().String(), RemoteAddress: c.RemoteMultiaddr().String(),
		Events: []upgradeEvent{},
	}}
	t.security = selectionTrace{connection: t, phase: "security_multistream"}
	t.muxer = selectionTrace{connection: t, phase: "muxer_multistream"}
	o.connections = append(o.connections, t)
	t.eventLocked(upgradeEvent{Kind: "tcp_connection"})
	return t
}

func (t *upgradeConnection) eventLocked(event upgradeEvent) {
	if len(t.evidence.Events) == upgradeEventLimit {
		t.owner.overflow = true
		return
	}
	t.sequence++
	event.Sequence = t.sequence
	t.evidence.Events = append(t.evidence.Events, event)
}

func (t *upgradeConnection) failureLocked(phase string, err error) {
	t.evidence.Failed = true
	// Error text is diagnostic, never protocol classification, and bounded independently.
	message := err.Error()
	if len(message) > upgradeFrameLimit {
		message = message[:upgradeFrameLimit]
	}
	t.eventLocked(upgradeEvent{Kind: "failure", Phase: phase, Error: message})
}

func (t *upgradeConnection) failure(phase string, err error) {
	if t == nil || err == nil {
		return
	}
	t.owner.mu.Lock()
	defer t.owner.mu.Unlock()
	t.failureLocked(phase, err)
}

func (s *selectionTrace) feed(read bool, data []byte) {
	if s == nil {
		return
	}
	t := s.connection
	t.owner.mu.Lock()
	defer t.owner.mu.Unlock()
	if s.stopped || s.failed || t.owner.overflow {
		return
	}
	d, direction := &s.write, "write"
	if read {
		d, direction = &s.read, "read"
	}
	for offset, b := range data {
		if d.paused {
			// A proposal pauses this direction only until its response is paired. A rejection
			// resumes framing; a positive acknowledgement ends negotiation. Coalesced tails
			// are never saved as frames, even when the other I/O callback is still pending.
			if d.selected == "na" {
				s.failLocked("unmatched continuation after rejection")
			} else {
				if s.selected == "" {
					s.unconfirmedTail = true
				}
				s.bodyLocked(read, data[offset:], d.selected)
			}
			break
		}
		if d.headerSize == 0 || d.header[d.headerSize-1]&128 != 0 {
			if d.headerSize == len(d.header) || (d.headerSize == 9 && b > 1) {
				s.failLocked("invalid multistream length")
				break
			}
			d.header[d.headerSize] = b
			d.length |= uint64(b&127) << (7 * d.headerSize)
			d.headerSize++
			if b&128 != 0 {
				continue
			}
			if d.length == 0 || d.length > upgradeFrameLimit || (d.headerSize > 1 && b == 0) {
				s.failLocked("multistream frame exceeds canonical bound")
				break
			}
			continue
		}
		d.data[d.size] = b
		d.size++
		if uint64(d.size) != d.length {
			continue
		}
		token := string(d.data[:d.size])
		if (d.stage == 0 && token != "/multistream/1.0.0\n") ||
			(d.stage != 0 && token == "/multistream/1.0.0\n") ||
			(!validUpgradeToken(token) && token != "na\n") {
			s.failLocked("unsupported or malformed multistream negotiation")
			break
		}
		wire := append(append([]byte{}, d.header[:d.headerSize]...), d.data[:d.size]...)
		t.eventLocked(upgradeEvent{Kind: "multistream_frame", Phase: s.phase, Stream: s.stream,
			Direction: direction, Protocol: strings.TrimSuffix(token, "\n"), FrameHex: hex.EncodeToString(wire)})
		d.headerSize, d.length, d.size = 0, 0, 0
		if d.stage == 0 {
			d.stage = 1
			continue
		}
		d.selected = strings.TrimSuffix(token, "\n")
		d.paused = true
		if read == s.proposerReadsLocked() {
			s.proposals++
			if d.selected == "na" || s.proposals > upgradeProposalLimit {
				s.failLocked("invalid or excessive multistream proposals")
				break
			}
			s.proposal = d.selected
		} else {
			s.reply = d.selected
		}
		s.reconcileSelectionLocked()
		if s.failed {
			break
		}
	}
}

func (s *selectionTrace) proposerReadsLocked() bool {
	direction := s.connection.evidence.Direction
	if s.stream != 0 {
		direction = s.direction
	}
	return direction == network.DirInbound.String()
}

func (s *selectionTrace) reconcileSelectionLocked() {
	if s.proposal == "" || s.reply == "" || s.selected != "" {
		return
	}
	if s.reply == "na" {
		if s.unconfirmedTail {
			s.failLocked("unmatched bytes followed a rejected proposal")
			return
		}
		s.connection.eventLocked(upgradeEvent{Kind: "protocol_rejected", Phase: s.phase, Stream: s.stream, Protocol: s.proposal})
		s.proposal, s.reply = "", ""
		s.read.paused, s.write.paused = false, false
		s.read.selected, s.write.selected = "", ""
		return
	}
	if s.proposal != s.reply {
		s.failLocked("multistream proposal/acknowledgement disagree")
		return
	}
	s.selected = s.proposal
	s.connection.eventLocked(upgradeEvent{Kind: "protocol_selected", Phase: s.phase, Stream: s.stream,
		Direction: s.direction, Protocol: s.selected})
}

func (s *selectionTrace) bodyLocked(read bool, data []byte, candidate string) {
	if s.stream == 0 {
		return
	}
	body := &s.writeBody
	if read {
		body = &s.readBody
	}
	// Digests may start before the opposite callback is observed; they are usable
	// only after an exact proposal/ack pair and successful stream completion.
	if candidate == "/ipfs/id/1.0.0" {
		body.feed(data, 4096)
	}
	if candidate == string(echoProtocol) {
		body.feed(data, 256*1024+10)
	}
}

func validUpgradeToken(token string) bool {
	if len(token) < 3 || token[0] != '/' || token[len(token)-1] != '\n' {
		return false
	}
	for _, b := range []byte(token[:len(token)-1]) {
		if b < 33 || b > 126 {
			return false
		}
	}
	return true
}

func (s *selectionTrace) failLocked(reason string) {
	s.failed = true
	// A rejected unrelated application stream does not invalidate another successful stream.
	if s.stream == 0 {
		s.connection.evidence.Failed = true
	}
	s.connection.eventLocked(upgradeEvent{Kind: "negotiation_incomplete", Phase: s.phase,
		Stream: s.stream, Error: reason})
}

type observedRawConn struct {
	manet.Conn
	trace *upgradeConnection
}

func (c *observedRawConn) Read(p []byte) (int, error) {
	n, err := c.Conn.Read(p)
	c.trace.security.feed(true, p[:n])
	return n, err
}

func (c *observedRawConn) Write(p []byte) (int, error) {
	n, err := c.Conn.Write(p)
	c.trace.security.feed(false, p[:n])
	return n, err
}

func (c *observedRawConn) Close() error {
	err := c.Conn.Close()
	c.trace.owner.mu.Lock()
	if !c.trace.evidence.Closed && err != nil {
		c.trace.failureLocked("underlying_close", err)
	}
	c.trace.evidence.Closed = true
	c.trace.owner.mu.Unlock()
	return err
}

type observedUpgrader struct {
	transport.Upgrader
	observer *upgradeObserver
}

func (u *observedUpgrader) wrap(c manet.Conn, direction network.Direction) manet.Conn {
	if t := u.observer.connection(c, direction); t != nil {
		return &observedRawConn{Conn: c, trace: t}
	}
	return c
}

func (u *observedUpgrader) Upgrade(ctx context.Context, tr transport.Transport, c manet.Conn,
	direction network.Direction, p peer.ID, scope network.ConnManagementScope) (transport.CapableConn, error) {
	wrapped := u.wrap(c, direction)
	result, err := u.Upgrader.Upgrade(ctx, tr, wrapped, direction, p, scope)
	if raw, ok := wrapped.(*observedRawConn); ok {
		raw.trace.failure("upgrade", err)
	}
	return result, err
}

type observedGatedListener struct {
	transport.GatedMaListener
	upgrader *observedUpgrader
}

func (l *observedGatedListener) Accept() (manet.Conn, network.ConnManagementScope, error) {
	c, scope, err := l.GatedMaListener.Accept()
	if err == nil {
		c = l.upgrader.wrap(c, network.DirInbound)
	}
	return c, scope, err
}

func (u *observedUpgrader) UpgradeGatedMaListener(tr transport.Transport, l transport.GatedMaListener) transport.Listener {
	return u.Upgrader.UpgradeGatedMaListener(tr, &observedGatedListener{GatedMaListener: l, upgrader: u})
}

func (u *observedUpgrader) UpgradeListener(tr transport.Transport, l manet.Listener) transport.Listener {
	return u.UpgradeGatedMaListener(tr, u.Upgrader.GateMaListener(l))
}

type observedSecurity struct{ sec.SecureTransport }

func (s *observedSecurity) secure(ctx context.Context, c net.Conn, p peer.ID, inbound bool) (sec.SecureConn, error) {
	raw, observed := c.(*observedRawConn)
	if observed {
		t := raw.trace
		t.owner.mu.Lock()
		t.security.stopped = true
		if t.stage != 0 || t.security.selected != string(s.ID()) || t.security.failed {
			t.failureLocked("security_order", fmt.Errorf("selected wire security does not match invoked delegate"))
		}
		t.stage = 1
		t.eventLocked(upgradeEvent{Kind: "security_enter", Protocol: string(s.ID())})
		t.owner.mu.Unlock()
	}
	var conn sec.SecureConn
	var err error
	if inbound {
		conn, err = s.SecureTransport.SecureInbound(ctx, c, p)
	} else {
		conn, err = s.SecureTransport.SecureOutbound(ctx, c, p)
	}
	if !observed {
		return conn, err
	}
	t := raw.trace
	if err != nil {
		t.failure("security", err)
		return conn, err
	}
	state := conn.ConnState()
	t.owner.mu.Lock()
	t.stage = 2
	t.evidence.Security = string(s.ID())
	t.evidence.LocalPeer, t.evidence.RemotePeer = conn.LocalPeer().String(), conn.RemotePeer().String()
	t.evidence.EarlyMuxer = state.UsedEarlyMuxerNegotiation
	t.eventLocked(upgradeEvent{Kind: "security_complete", Protocol: string(s.ID())})
	if state.StreamMultiplexer != "" {
		t.muxer.stopped = true
		t.muxer.selected = string(state.StreamMultiplexer)
		t.eventLocked(upgradeEvent{Kind: "security_handshake_muxer_selected", Protocol: string(state.StreamMultiplexer)})
	}
	if state.UsedEarlyMuxerNegotiation != (state.StreamMultiplexer != "") {
		t.failureLocked("early_muxer", fmt.Errorf("native early muxer fields disagree"))
	}
	t.owner.mu.Unlock()
	return &observedSecureConn{SecureConn: conn, trace: t}, nil
}

func (s *observedSecurity) SecureInbound(ctx context.Context, c net.Conn, p peer.ID) (sec.SecureConn, error) {
	return s.secure(ctx, c, p, true)
}

func (s *observedSecurity) SecureOutbound(ctx context.Context, c net.Conn, p peer.ID) (sec.SecureConn, error) {
	return s.secure(ctx, c, p, false)
}

type observedSecureConn struct {
	sec.SecureConn
	trace *upgradeConnection
}

func (c *observedSecureConn) Read(p []byte) (int, error) {
	n, err := c.SecureConn.Read(p)
	c.trace.muxer.feed(true, p[:n])
	return n, err
}

func (c *observedSecureConn) Write(p []byte) (int, error) {
	n, err := c.SecureConn.Write(p)
	c.trace.muxer.feed(false, p[:n])
	return n, err
}

type observedMultiplexer struct {
	network.Multiplexer
	id protocol.ID
}

func (m *observedMultiplexer) NewConn(c net.Conn, server bool, scope network.PeerScope) (network.MuxedConn, error) {
	secure, ok := c.(*observedSecureConn)
	if !ok {
		return m.Multiplexer.NewConn(c, server, scope)
	}
	t := secure.trace
	t.owner.mu.Lock()
	t.muxer.stopped = true
	if t.stage != 2 || t.muxer.failed || t.muxer.selected != string(m.id) {
		t.failureLocked("muxer_order", fmt.Errorf("selected muxer does not match invoked delegate"))
	}
	t.stage = 3
	t.eventLocked(upgradeEvent{Kind: "muxer_enter", Protocol: string(m.id)})
	t.owner.mu.Unlock()
	conn, err := m.Multiplexer.NewConn(c, server, scope)
	if err != nil {
		t.failure("muxer", err)
		return conn, err
	}
	t.owner.mu.Lock()
	t.stage = 4
	t.evidence.Muxer = string(m.id)
	t.eventLocked(upgradeEvent{Kind: "muxer_complete", Protocol: string(m.id)})
	t.owner.mu.Unlock()
	return &observedMuxedConn{MuxedConn: conn, trace: t}, nil
}

type observedMuxedConn struct {
	network.MuxedConn
	trace *upgradeConnection
}

func (c *observedMuxedConn) As(target any) bool {
	if out, ok := target.(**observedMuxedConn); ok {
		*out = c
		return true
	}
	return c.MuxedConn.As(target)
}

func (c *observedMuxedConn) stream(s network.MuxedStream, direction network.Direction) network.MuxedStream {
	t := c.trace
	t.owner.mu.Lock()
	defer t.owner.mu.Unlock()
	if t.streams == upgradeStreamLimit {
		t.owner.overflow = true
		return s
	}
	t.streams++
	if t.stage != 4 {
		t.failureLocked("application_order", fmt.Errorf("stream before completed muxer"))
	}
	selection := &selectionTrace{connection: t, phase: "application_multistream", stream: t.streams,
		direction: direction.String()}
	t.streamStates = append(t.streamStates, selection)
	t.eventLocked(upgradeEvent{Kind: "stream_open", Stream: t.streams, Direction: direction.String()})
	return &observedMuxedStream{MuxedStream: s, selection: selection}
}

func (c *observedMuxedConn) OpenStream(ctx context.Context) (network.MuxedStream, error) {
	s, err := c.MuxedConn.OpenStream(ctx)
	if err == nil {
		s = c.stream(s, network.DirOutbound)
		if binding, ok := ctx.Value(upgradeStreamBindingKey{}).(*upgradeStreamBinding); ok {
			if observed, ok := s.(*observedMuxedStream); ok {
				binding.claim(observed.selection)
			}
		}
	}
	return s, err
}

func (c *observedMuxedConn) AcceptStream() (network.MuxedStream, error) {
	s, err := c.MuxedConn.AcceptStream()
	if err == nil {
		s = c.stream(s, network.DirInbound)
	}
	return s, err
}

type observedMuxedStream struct {
	network.MuxedStream
	selection *selectionTrace
}

func (s *observedMuxedStream) Read(p []byte) (int, error) {
	s.beginIO()
	n, err := s.MuxedStream.Read(p)
	s.selection.feed(true, p[:n])
	s.endIO(err != nil && err != io.EOF)
	return n, err
}

func (s *observedMuxedStream) Write(p []byte) (int, error) {
	s.beginIO()
	n, err := s.MuxedStream.Write(p)
	s.selection.feed(false, p[:n])
	s.endIO(err != nil || n != len(p))
	return n, err
}

func (s *observedMuxedStream) beginIO() {
	t := s.selection.connection
	t.owner.mu.Lock()
	s.selection.active++
	t.owner.mu.Unlock()
}

func (s *observedMuxedStream) endIO(failed bool) {
	t := s.selection.connection
	t.owner.mu.Lock()
	s.selection.active--
	s.selection.ioFailed = s.selection.ioFailed || failed
	s.selection.reportCompletionLocked()
	t.owner.mu.Unlock()
}

func (s *selectionTrace) completedLocked() bool {
	if s.failed || s.ioFailed || s.reset || s.readBody.failed || s.writeBody.failed || s.active != 0 || !s.writeClosed || s.selected == "" {
		return false
	}
	if s.direction == network.DirOutbound.String() {
		return s.applicationDone && s.boundID != "" && s.readBody.complete() &&
			(s.selected == "/ipfs/id/1.0.0" || (s.selected == string(echoProtocol) && s.writeBody.complete() && s.writeBody.sum() == s.readBody.sum()))
	}
	if !s.writeBody.complete() {
		return false
	}
	if s.selected == "/ipfs/id/1.0.0" {
		return true
	}
	return s.selected == string(echoProtocol) && s.readBody.complete() && s.readBody.sum() == s.writeBody.sum()
}

func (s *selectionTrace) reportCompletionLocked() {
	if s.completionReported || !s.completedLocked() {
		return
	}
	s.completionReported = true
	kind := "response_write_complete"
	if s.direction == network.DirOutbound.String() {
		kind = "application_io_complete"
	}
	s.connection.eventLocked(upgradeEvent{Kind: kind, Stream: s.stream, Direction: s.direction,
		Protocol: s.selected, NetworkStreamID: s.boundID})
}

func (s *observedMuxedStream) closed(kind string, err error, reset bool) {
	t := s.selection.connection
	t.owner.mu.Lock()
	defer t.owner.mu.Unlock()
	s.selection.reset = s.selection.reset || reset
	s.selection.ioFailed = s.selection.ioFailed || err != nil
	s.selection.writeClosed = true
	if s.selection.closed {
		return
	}
	s.selection.closed = kind != "stream_close_write_returned"
	if s.selection.active == 0 && s.selection.selected == "" && !s.selection.failed {
		s.selection.failLocked("stream ended before complete negotiation")
	}
	event := upgradeEvent{Kind: kind, Stream: s.selection.stream}
	if err != nil {
		event.Error = "delegated stream cleanup returned an error"
	}
	t.eventLocked(event)
	s.selection.reportCompletionLocked()
}

func (s *observedMuxedStream) Close() error {
	err := s.MuxedStream.Close()
	s.closed("stream_close_returned", err, false)
	return err
}

func (s *observedMuxedStream) Reset() error {
	err := s.MuxedStream.Reset()
	s.closed("stream_reset_returned", err, true)
	return err
}

func (s *observedMuxedStream) ResetWithError(code network.StreamErrorCode) error {
	err := s.MuxedStream.ResetWithError(code)
	s.closed("stream_reset_returned", err, true)
	return err
}

func (s *observedMuxedStream) CloseWrite() error {
	err := s.MuxedStream.CloseWrite()
	s.closed("stream_close_write_returned", err, false)
	return err
}

type upgradeStreamBindingKey struct{}

// The pinned swarm passes NewStream's context to MuxedConn.OpenStream unchanged.
type upgradeStreamBinding struct {
	mu        sync.Mutex
	selection *selectionTrace
	invalid   bool
}

func bindUpgradeStream(ctx context.Context) (context.Context, *upgradeStreamBinding) {
	b := &upgradeStreamBinding{}
	return context.WithValue(ctx, upgradeStreamBindingKey{}, b), b
}

func (b *upgradeStreamBinding) claim(s *selectionTrace) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.selection != nil {
		b.invalid = true
		b.selection.connection.owner.mu.Lock()
		b.selection.ioFailed = true
		b.selection.connection.owner.mu.Unlock()
		s.connection.owner.mu.Lock()
		s.ioFailed = true
		s.connection.owner.mu.Unlock()
		return
	}
	b.selection = s
}

func (b *upgradeStreamBinding) attach(s network.Stream) {
	b.bind(s, false)
}

func (b *upgradeStreamBinding) complete(s network.Stream) {
	b.bind(s, true)
}

func (b *upgradeStreamBinding) target() upgradeTarget {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.invalid || b.selection == nil {
		return upgradeTarget{}
	}
	return upgradeTarget{connection: b.selection.connection.evidence.ID, stream: b.selection.stream}
}

func (b *upgradeStreamBinding) bind(s network.Stream, complete bool) {
	var muxed *observedMuxedConn
	if !s.Conn().As(&muxed) {
		return
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	t := muxed.trace
	t.owner.mu.Lock()
	defer t.owner.mu.Unlock()
	selection := b.selection
	if b.invalid || selection == nil || selection.connection != t ||
		s.ID() == "" || s.Protocol() == "" || s.Stat().Direction != network.DirOutbound ||
		s.Conn().RemotePeer().String() != t.evidence.RemotePeer || s.Conn().LocalPeer().String() != t.evidence.LocalPeer ||
		(complete && (selection.boundID != s.ID() || selection.selected != string(s.Protocol()) || selection.applicationDone)) ||
		(!complete && selection.boundID != "") {
		t.failureLocked("application_binding", fmt.Errorf("one-shot stream/context/identity binding disagrees"))
		return
	}
	if complete {
		selection.applicationDone = true
	} else {
		selection.boundID = s.ID()
	}
	t.eventLocked(upgradeEvent{Kind: "application_stream_binding", Stream: selection.stream, Protocol: string(s.Protocol()),
		Direction: s.Stat().Direction.String(), ConnectionID: s.Conn().ID(), NetworkStreamID: s.ID()})
	selection.reportCompletionLocked()
}

func upgradeProofProtocol(scenario string) protocol.ID {
	switch scenario {
	case "identify":
		return "/ipfs/id/1.0.0"
	case "echo":
		return echoProtocol
	default:
		return ""
	}
}

func (o *upgradeObserver) finish(application protocol.ID, remote string, direction network.Direction, target ...upgradeTarget) (upgradeEvidence, error) {
	o.mu.Lock()
	defer o.mu.Unlock()
	out := upgradeEvidence{Source: "go-libp2p.public-upgrade-hooks.v1", Finalized: true,
		Complete: !o.overflow, Overflow: o.overflow, Connections: []upgradeConnectionEvidence{}, TargetStreams: []uint64{}}
	if len(target) > 1 || (len(target) == 1 && (target[0].connection == 0 || target[0].stream == 0)) {
		out.Complete = false
	}
	if direction == network.DirOutbound && len(target) != 1 {
		out.Complete = false
	}
	candidates := 0
	for _, t := range o.connections {
		value := t.evidence
		value.Events = append([]upgradeEvent{}, value.Events...)
		value.Streams = []upgradeStreamEvidence{}
		streams := []uint64{}
		for _, s := range t.streamStates {
			complete := s.completedLocked()
			value.Streams = append(value.Streams, upgradeStreamEvidence{ID: s.stream, Direction: s.direction,
				Protocol: s.selected, NetworkStreamID: s.boundID, Failed: s.failed || s.ioFailed || s.readBody.failed || s.writeBody.failed,
				Reset: s.reset, ApplicationIOComplete: complete && s.direction == network.DirOutbound.String(),
				ResponseWriteComplete: complete && s.direction == network.DirInbound.String(),
				ReadFrames:            s.readBody.frames, WriteFrames: s.writeBody.frames, ReadBytes: s.readBody.total,
				WriteBytes: s.writeBody.total, ReadSHA256: s.readBody.sum(), WriteSHA256: s.writeBody.sum()})
			if complete && s.selected == string(application) && s.direction == direction.String() &&
				value.Direction == direction.String() &&
				(remote == "" || remote == value.RemotePeer) &&
				(len(target) == 0 || (target[0].connection == value.ID && target[0].stream == s.stream)) {
				streams = append(streams, s.stream)
			}
		}
		if len(streams) > 0 && !value.Failed && value.Closed && t.stage == 4 {
			candidates++
			out.TargetConnection, out.TargetStreams = value.ID, streams
		}
		out.Connections = append(out.Connections, value)
	}
	out.Complete = out.Complete && candidates == 1
	if candidates != 1 {
		out.TargetConnection, out.TargetStreams = 0, []uint64{}
	}
	if !out.Complete {
		return out, fmt.Errorf("native TCP upgrade observation incomplete")
	}
	return out, nil
}

func observedTCP(o *upgradeObserver) func(transport.Upgrader, network.ResourceManager, *tcpreuse.ConnMgr) (*tcp.TcpTransport, error) {
	return func(u transport.Upgrader, r network.ResourceManager, shared *tcpreuse.ConnMgr) (*tcp.TcpTransport, error) {
		return tcp.NewTCPTransport(&observedUpgrader{Upgrader: u, observer: o}, r, shared)
	}
}

func observedNoise(id protocol.ID, key crypto.PrivKey, muxers []tptu.StreamMuxer) (sec.SecureTransport, error) {
	delegate, err := noise.New(id, key, muxers)
	if err != nil {
		return nil, err
	}
	return &observedSecurity{SecureTransport: delegate}, nil
}

func observedTLS(id protocol.ID, key crypto.PrivKey, muxers []tptu.StreamMuxer) (sec.SecureTransport, error) {
	delegate, err := sectls.New(id, key, muxers)
	if err != nil {
		return nil, err
	}
	return &observedSecurity{SecureTransport: delegate}, nil
}

func observedYamux() network.Multiplexer {
	return &observedMultiplexer{Multiplexer: yamux.DefaultTransport, id: yamux.ID}
}
