package main

import (
	"bufio"
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	libp2p "github.com/libp2p/go-libp2p"
	"github.com/libp2p/go-libp2p/core/control"
	"github.com/libp2p/go-libp2p/core/crypto"
	"github.com/libp2p/go-libp2p/core/event"
	"github.com/libp2p/go-libp2p/core/host"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/core/protocol"
	relayclient "github.com/libp2p/go-libp2p/p2p/protocol/circuitv2/client"
	relayv2 "github.com/libp2p/go-libp2p/p2p/protocol/circuitv2/relay"
	"github.com/libp2p/go-libp2p/p2p/protocol/holepunch"
	holepb "github.com/libp2p/go-libp2p/p2p/protocol/holepunch/pb"
	"github.com/libp2p/go-libp2p/p2p/protocol/identify"
	ma "github.com/multiformats/go-multiaddr"
	manet "github.com/multiformats/go-multiaddr/net"
	msmux "github.com/multiformats/go-multistream"
	quic "github.com/quic-go/quic-go"
	"google.golang.org/protobuf/proto"
)

const pathEchoProtocol = protocol.ID("/forge/interop/path-echo/1")
const pathEventLimit = 256

// The tracer is supplemental native evidence. It cannot synthesize wire frames,
// connection correlation, or an operation_finished success from counters.
type pathObserver struct {
	mu          sync.Mutex
	token       string
	local       peer.ID
	expected    peer.ID
	events      []map[string]any
	overflow    bool
	relayEcho   bool
	released    bool
	observed    map[string]ma.Multiaddr
	connections map[string]bool
}

func newPathObserver(token string, local peer.ID) (*pathObserver, error) {
	decoded, err := hex.DecodeString(token)
	if err != nil || len(decoded) != 16 || strings.ToLower(token) != token {
		return nil, fmt.Errorf("invalid path token")
	}
	return &pathObserver{token: token, local: local, observed: make(map[string]ma.Multiaddr), connections: make(map[string]bool)}, nil
}

func (o *pathObserver) record(kind, source string, fields map[string]any) {
	o.mu.Lock()
	defer o.mu.Unlock()
	if fields == nil {
		fields = map[string]any{}
	}
	encoded, err := json.Marshal(fields)
	if err != nil || len(encoded) > 32*1024 || !strings.HasPrefix(source, "go.") || len(o.events) == pathEventLimit {
		o.overflow = true
		return
	}
	copy := make(map[string]any, len(fields)+4)
	decoder := json.NewDecoder(bytes.NewReader(encoded))
	decoder.UseNumber()
	if err := decoder.Decode(&copy); err != nil {
		o.overflow = true
		return
	}
	copy["sequence"] = len(o.events) + 1
	copy["mono_ns"] = time.Since(pathClockOrigin).Nanoseconds() + 1
	copy["kind"], copy["source"] = kind, source
	o.events = append(o.events, copy)
}

var pathClockOrigin = time.Now()

func (o *pathObserver) Trace(event *holepunch.Event) {
	if event == nil {
		return
	}
	fields := map[string]any{"local_peer_id": event.Peer.String(), "remote_peer_id": event.Remote.String(),
		"native_type": event.Type, "native_unix_ns": event.Timestamp}
	switch value := event.Evt.(type) {
	case *holepunch.StartHolePunchEvt:
		fields["rtt_ns"], fields["addresses"] = value.RTT.Nanoseconds(), append([]string(nil), value.RemoteAddrs...)
	case *holepunch.HolePunchAttemptEvt:
		fields["native_attempt"] = value.Attempt
	case *holepunch.EndHolePunchEvt:
		fields["success"], fields["elapsed_ns"], fields["error"] = value.Success, value.EllapsedTime.Nanoseconds(), value.Error
	case *holepunch.DirectDialEvt:
		fields["success"], fields["elapsed_ns"] = value.Success, value.EllapsedTime.Nanoseconds()
		if value.Success {
			o.record("ordinary_direct_success", "go.holepunch.tracer", fields)
			return
		}
	case *holepunch.ProtocolErrorEvt:
		fields["error"] = value.Error
	default:
		return
	}
	o.record("holepunch_trace", "go.holepunch.tracer", fields)
}

func (o *pathObserver) bind(h host.Host, expected peer.ID) error {
	o.mu.Lock()
	if o.expected != "" {
		o.mu.Unlock()
		return fmt.Errorf("path peer already bound")
	}
	o.expected = expected
	o.mu.Unlock()
	ids := []string{}
	for _, c := range h.Network().ConnsToPeer(expected) {
		if !strings.Contains(c.RemoteMultiaddr().String(), "/p2p-circuit") {
			ids = append(ids, c.ID())
		}
	}
	o.record("baseline", "go.network.connections", map[string]any{"remote_peer_id": expected.String(), "direct_connection_ids": ids})
	if len(ids) != 0 {
		return fmt.Errorf("preexisting direct connection")
	}
	return nil
}

func (o *pathObserver) identifyObserved(h host.Host, evt event.EvtPeerIdentificationCompleted, relay string) {
	if evt.Conn == nil || evt.ObservedAddr == nil || evt.Conn.RemotePublicKey() == nil || evt.Conn.RemotePeer() != evt.Peer || evt.Peer.String() != relay ||
		evt.Conn.ConnState().Transport != "quic-v1" || !manet.IsPublicAddr(evt.ObservedAddr) {
		return
	}
	if _, err := evt.ObservedAddr.ValueForProtocol(ma.P_QUIC_V1); err != nil {
		return
	}
	if _, err := evt.ObservedAddr.ValueForProtocol(ma.P_UDP); err != nil {
		return
	}
	protocols := evt.ObservedAddr.Protocols()
	if len(protocols) != 3 || (protocols[0].Code != ma.P_IP4 && protocols[0].Code != ma.P_IP6) ||
		protocols[1].Code != ma.P_UDP || protocols[2].Code != ma.P_QUIC_V1 {
		return
	}
	local := evt.Conn.LocalMultiaddr()
	bound := false
	for _, listener := range h.Network().ListenAddresses() {
		if listener.Equal(local) {
			bound = true
			break
		}
	}
	if !bound {
		return
	}
	o.mu.Lock()
	_, exists := o.observed[evt.ObservedAddr.String()]
	if !exists && len(o.observed) >= 16 {
		o.overflow = true
		o.mu.Unlock()
		return
	}
	o.observed[evt.ObservedAddr.String()] = evt.ObservedAddr
	o.mu.Unlock()
	if exists {
		return
	}
	o.record("identify_observed_address", "go.event.EvtPeerIdentificationCompleted", map[string]any{
		"connection_id": evt.Conn.ID(), "observer_peer_id": evt.Peer.String(),
		"listener_address": local.String(), "connection_local_address": local.String(),
		"observed_address": evt.ObservedAddr.String()})
}

func (o *pathObserver) holePunchAddrs() []ma.Multiaddr {
	o.mu.Lock()
	defer o.mu.Unlock()
	addresses := make([]ma.Multiaddr, 0, len(o.observed))
	for _, address := range o.observed {
		addresses = append(addresses, address)
	}
	sort.Slice(addresses, func(i, j int) bool { return addresses[i].String() < addresses[j].String() })
	return addresses
}

func waitPathHolePunchReady(ctx context.Context, protocols func() []protocol.ID) ([]protocol.ID, error) {
	ticker := time.NewTicker(10 * time.Millisecond)
	defer ticker.Stop()
	for {
		if err := ctx.Err(); err != nil {
			return nil, fmt.Errorf("native DCUtR handler readiness: %w", err)
		}
		registered := protocols()
		if err := ctx.Err(); err != nil {
			return nil, fmt.Errorf("native DCUtR handler readiness: %w", err)
		}
		for _, id := range registered {
			if id == holepunch.Protocol {
				return append([]protocol.ID(nil), registered...), nil
			}
		}
		select {
		case <-ctx.Done():
			return nil, fmt.Errorf("native DCUtR handler readiness: %w", ctx.Err())
		case <-ticker.C:
		}
	}
}

func (o *pathObserver) observeConnections(h host.Host, relay string) error {
	o.mu.Lock()
	expected := o.expected
	o.mu.Unlock()
	if expected == "" {
		return nil
	}
	for _, c := range h.Network().ConnsToPeer(expected) {
		o.mu.Lock()
		seen := o.connections[c.ID()]
		o.mu.Unlock()
		if seen {
			continue
		}
		if c.RemotePublicKey() == nil {
			return fmt.Errorf("missing native authenticated remote key")
		}
		authenticated, err := peer.IDFromPublicKey(c.RemotePublicKey())
		if err != nil || authenticated != expected {
			return fmt.Errorf("native authenticated key/expected peer mismatch")
		}
		state := c.ConnState()
		path, transport, security, basis := "direct", "quic", string(state.Security), "native_quic_authenticated_output"
		muxer := string(state.StreamMultiplexer)
		var relayPeer any
		if strings.Contains(c.RemoteMultiaddr().String(), "/p2p-circuit") {
			path, transport, basis, relayPeer = "relay", "circuit", "native_relay_inner_upgrade", relay
			observedSecurity, observedMuxer, err := pathInnerUpgrade(c, o.local, expected)
			if err != nil {
				return fmt.Errorf("missing native relay inner authentication/muxer: security=%q muxer=%q transport=%q: %w", state.Security, state.StreamMultiplexer, state.Transport, err)
			}
			security, muxer = string(observedSecurity), string(observedMuxer)
		} else {
			if state.Transport != "quic-v1" {
				return fmt.Errorf("direct output is not native QUIC")
			}
			// Native QUIC authenticates via TLS; ConnState deliberately has no
			// stream security protocol. Authenticated transport output is the basis.
			security = "/tls/1.0.0"
		}
		direction := "inbound"
		if c.Stat().Direction == network.DirOutbound {
			direction = "outbound"
		}
		o.mu.Lock()
		seen = o.connections[c.ID()]
		o.connections[c.ID()] = true
		o.mu.Unlock()
		if seen {
			continue
		}
		o.record("authenticated_connection", "go.network.Conn.authenticated_output", map[string]any{
			"connection_id": c.ID(), "remote_peer_id": c.RemotePeer().String(), "path": path, "authenticated": true,
			"local_address": c.LocalMultiaddr().String(), "remote_address": c.RemoteMultiaddr().String(),
			"security": security, "transport": transport, "muxer": muxer,
			"authentication_basis": basis, "relay_peer_id": relayPeer, "direction": direction})
	}
	return nil
}

func (o *pathObserver) release(c network.Conn) error {
	o.mu.Lock()
	if !o.relayEcho || o.released || c.RemotePeer() != o.expected || !strings.Contains(c.RemoteMultiaddr().String(), "/p2p-circuit") {
		o.mu.Unlock()
		return fmt.Errorf("release requires authenticated circuit challenge, exactly once")
	}
	o.released = true
	o.mu.Unlock()
	// Application observation only. It does not gate/change native protocol I/O.
	o.record("barrier_released", "go.path_echo.io", map[string]any{"connection_id": c.ID(),
		"remote_peer_id": c.RemotePeer().String(), "case_token": o.token})
	return nil
}

// Wrap only the fixture host supplied to the native holepunch.NewService. Native
// CONNECT/SYNC, NAT dialing and role selection are not reimplemented here.
type pathNativeHost struct {
	host.Host
	observer *pathObserver
	dials    *pathDialObserver
	drain    pathHandlerDrain
}

// Service.Close does not join its inbound stream handlers in the pinned donor.
// Track actual callback entry/return; after native close, reset only DCUtR streams
// still owned by those callbacks, never the relay/application connection.
type pathHandlerDrain struct {
	mu                 sync.Mutex
	closing            bool
	active             map[string]network.Stream
	entered, completed uint64
	done               chan struct{}
}

func (d *pathHandlerDrain) begin(s network.Stream) bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.closing {
		return false
	}
	if d.active == nil {
		d.active = make(map[string]network.Stream)
		d.done = make(chan struct{})
	}
	d.active[s.ID()] = s
	d.entered++
	return true
}

func (d *pathHandlerDrain) end(s network.Stream) {
	d.mu.Lock()
	defer d.mu.Unlock()
	delete(d.active, s.ID())
	d.completed++
	if d.closing && len(d.active) == 0 {
		close(d.done)
	}
}

func (d *pathHandlerDrain) wait(ctx context.Context) (map[string]any, error) {
	d.mu.Lock()
	if d.done == nil {
		d.done = make(chan struct{})
	}
	if !d.closing {
		d.closing = true
		if len(d.active) == 0 {
			close(d.done)
		}
	}
	streams := make([]network.Stream, 0, len(d.active))
	for _, s := range d.active {
		streams = append(streams, s)
	}
	done := d.done
	d.mu.Unlock()
	for _, s := range streams {
		_ = s.Reset()
	}
	select {
	case <-done:
		d.mu.Lock()
		defer d.mu.Unlock()
		return map[string]any{"entered": d.entered, "completed": d.completed, "active": len(d.active)}, nil
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}

func (h *pathNativeHost) NewStream(ctx context.Context, remote peer.ID, protocols ...protocol.ID) (network.Stream, error) {
	dcutr := false
	for _, id := range protocols {
		if id == holepunch.Protocol {
			dcutr = true
		}
	}
	s, err := h.Host.NewStream(ctx, remote, protocols...)
	if err != nil || !dcutr {
		return s, err
	}
	return &pathObservedStream{Stream: s, observer: h.observer}, nil
}

func (h *pathNativeHost) SetStreamHandler(id protocol.ID, handler network.StreamHandler) {
	if id != holepunch.Protocol {
		h.Host.SetStreamHandler(id, handler)
		return
	}
	h.Host.SetStreamHandler(id, func(s network.Stream) {
		if !h.drain.begin(s) {
			_ = s.Reset()
			return
		}
		defer h.drain.end(s)
		handler(&pathObservedStream{Stream: s, observer: h.observer})
	})
}

// Passive framing, above actual native security/muxer ownership. It never writes
// a CONNECT/SYNC message, changes candidates, or replaces native hole punching.
type pathObservedStream struct {
	network.Stream
	observer        *pathObserver
	readMu, writeMu sync.Mutex
	read, write     pathWireDecoder
	readTerminal    bool
	writeTerminal   bool
}

type pathWireDecoder struct {
	frame        []byte
	body, prefix int
	invalid      bool
	completed    int
}

func (d *pathWireDecoder) feed(bytes []byte, direction string, s *pathObservedStream) {
	for _, b := range bytes {
		if d.invalid {
			return
		}
		d.frame = append(d.frame, b)
		if d.prefix == 0 {
			if len(d.frame) > 2 {
				d.invalid = true
				return
			}
			if b&128 != 0 {
				continue
			}
			length, n := binary.Uvarint(d.frame)
			if n <= 0 || length == 0 || length > 4096 {
				d.invalid = true
				return
			}
			d.prefix, d.body = n, int(length)
		}
		if len(d.frame) != d.prefix+d.body {
			continue
		}
		var msg holepb.HolePunch
		if err := proto.Unmarshal(d.frame[d.prefix:], &msg); err != nil {
			d.invalid = true
			return
		}
		addresses := []string{}
		for _, bytes := range msg.ObsAddrs {
			address, err := ma.NewMultiaddrBytes(bytes)
			if err != nil {
				d.invalid = true
				return
			}
			addresses = append(addresses, address.String())
		}
		digest := sha256.Sum256(d.frame)
		s.observer.record("dcutr_frame", "go.native_dcutr.io", map[string]any{
			"connection_id": s.Conn().ID(), "stream_id": s.ID(), "remote_peer_id": s.Conn().RemotePeer().String(),
			"protocol": string(holepunch.Protocol), "direction": direction, "message_type": int(msg.GetType()),
			"addresses": addresses, "receipt": map[string]any{"framed_hex": hex.EncodeToString(d.frame),
				direction: map[string]any{"framed_bytes": len(d.frame), "framed_sha256": hex.EncodeToString(digest[:]),
					"frames": 1, "complete_frames": true, "invalid_or_over_limit": false}}})
		d.completed++
		d.frame, d.prefix, d.body = nil, 0, 0
	}
}

func (s *pathObservedStream) Read(bytes []byte) (int, error) {
	n, err := s.Stream.Read(bytes)
	s.readMu.Lock()
	s.read.feed(bytes[:n], "read", s)
	s.recordTerminal("read", &s.read, &s.readTerminal, n, err)
	s.readMu.Unlock()
	return n, err
}

func (s *pathObservedStream) Write(bytes []byte) (int, error) {
	n, err := s.Stream.Write(bytes)
	s.writeMu.Lock()
	s.write.feed(bytes[:n], "write", s)
	s.recordTerminal("write", &s.write, &s.writeTerminal, n, err)
	s.writeMu.Unlock()
	return n, err
}

func (s *pathObservedStream) recordTerminal(direction string, decoder *pathWireDecoder, recorded *bool, n int, err error) {
	if err == nil || *recorded {
		return
	}
	*recorded = true
	kind := "io_error"
	switch {
	case errors.Is(err, network.ErrReset):
		kind = "reset"
	case errors.Is(err, io.EOF):
		kind = "eof"
	case errors.Is(err, context.Canceled):
		kind = "canceled"
	case errors.Is(err, context.DeadlineExceeded), errors.Is(err, os.ErrDeadlineExceeded):
		kind = "deadline"
	}
	s.observer.record("dcutr_stream_terminal", "go.native_dcutr.io", map[string]any{
		"connection_id": s.Conn().ID(), "stream_id": s.ID(), "remote_peer_id": s.Conn().RemotePeer().String(),
		"protocol": string(holepunch.Protocol), "direction": direction, "error": err.Error(), "error_kind": kind,
		"io_bytes": n, "completed_frame_count": decoder.completed, "pending_frame_bytes": len(decoder.frame),
		"invalid_or_over_limit": decoder.invalid})
}

func (h *pathNativeHost) Connect(ctx context.Context, info peer.AddrInfo) error {
	simultaneous, client, reason := network.GetSimultaneousConnect(ctx)
	if !simultaneous {
		return h.Host.Connect(ctx, info)
	}
	requested := "listener"
	transportRole := "listener_udp_probe"
	if client {
		requested, transportRole = "dialer", "quic_client"
	}
	before := map[string]bool{}
	for _, c := range h.Network().ConnsToPeer(info.ID) {
		before[c.ID()] = true
	}
	h.observer.record("native_coordinated_dial", "go.native_host.Connect", map[string]any{
		"remote_peer_id": info.ID.String(), "requested_role": requested, "transport_role": transportRole,
		"simultaneous_connect_reason": reason, "direct_connection_ids_before": before})
	err := h.Host.Connect(ctx, info)
	if err != nil {
		return err
	}
	for _, c := range h.Network().ConnsToPeer(info.ID) {
		if !before[c.ID()] && !strings.Contains(c.RemoteMultiaddr().String(), "/p2p-circuit") {
			if h.dials == nil {
				return fmt.Errorf("native coordinated QUIC secured observer unavailable")
			}
			secured, err := h.dials.quicSecurity(c)
			if err != nil {
				return err
			}
			h.observer.record("native_coordinated_authenticated", "go.native_host.Connect", map[string]any{
				"remote_peer_id": c.RemotePeer().String(), "connection_id": c.ID(), "security_role": secured.role(),
				"security_role_basis": "native_quic_secured_callback", "requested_role": requested,
				"secured_receipt": secured.receipt(),
				"local_address":   c.LocalMultiaddr().String(), "remote_address": c.RemoteMultiaddr().String(),
				"native_transport": c.ConnState().Transport, "authenticated": c.RemotePublicKey() != nil})
			return nil
		}
	}
	return fmt.Errorf("native simultaneous connect returned without a new authenticated direct owner")
}

func pathFields(path string) (map[string]string, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	scanner := bufio.NewScanner(io.LimitReader(file, 8193))
	fields := map[string]string{}
	for scanner.Scan() {
		parts := strings.SplitN(scanner.Text(), "=", 2)
		if len(parts) != 2 || parts[0] == "" || parts[1] == "" || len(fields) >= 16 {
			return nil, fmt.Errorf("invalid path control")
		}
		if _, exists := fields[parts[0]]; exists {
			return nil, fmt.Errorf("duplicate path control")
		}
		fields[parts[0]] = parts[1]
	}
	return fields, scanner.Err()
}

func pathAtomic(path string, value []byte) error {
	if err := os.WriteFile(path+".tmp", value, 0600); err != nil {
		return err
	}
	return os.Rename(path+".tmp", path)
}

// Concrete shared-main dispatch target. Uses native libp2p relay and automatic
// NewService; there is deliberately no Service.DirectConnect invocation.
func runPathLive(args map[string]string) (failure error) {
	if args["transport"] != "quic" || args["scenario"] != "dcutr" || args["pnet-key-file"] != "" {
		return fmt.Errorf("path-live is native QUIC only")
	}
	role := args["path-role"]
	if role != "source" && role != "destination" && role != "relay" {
		return fmt.Errorf("invalid path role")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 55*time.Second)
	defer cancel()
	gater := &pathDialObserver{}
	options := []libp2p.Option{libp2p.ListenAddrStrings("/ip4/" + args["bind-ip"] + "/udp/0/quic-v1"), libp2p.EnableRelay(), libp2p.ConnectionGater(gater)}
	h, err := libp2p.New(append(options, pathUpgradeOptions()...)...)
	if err != nil {
		return err
	}
	o, err := newPathObserver(args["case-token"], h.ID())
	if err != nil {
		_ = h.Close()
		return err
	}
	var hp *holepunch.Service
	var nativeHost *pathNativeHost
	var relay *relayv2.Relay
	var identified event.Subscription
	var identifyOwner sync.WaitGroup
	var handlers sync.WaitGroup
	defer func() {
		cancel()
		if identified != nil {
			_ = identified.Close()
			identifyOwner.Wait()
		}
		joined := true
		if hp != nil {
			if err := hp.Close(); err != nil {
				joined = false
				if failure == nil {
					failure = err
				}
			}
		}
		if nativeHost != nil {
			drainCtx, stopDrain := context.WithTimeout(context.Background(), 5*time.Second)
			_, err := nativeHost.drain.wait(drainCtx)
			stopDrain()
			if err != nil {
				joined = false
				if failure == nil {
					failure = err
				}
			}
		}
		if relay != nil {
			if err := relay.Close(); err != nil {
				joined = false
				if failure == nil {
					failure = err
				}
			}
		}
		if err := h.Close(); err != nil {
			joined = false
			if failure == nil {
				failure = err
			}
		}
		handlers.Wait()
		result, err := o.result(true, joined, failure)
		if err == nil {
			err = pathAtomic(args["result-file"], result)
		}
		if failure == nil {
			failure = err
		}
	}()
	// The application handler proves actual read+write on this native stream.
	h.SetStreamHandler(pathEchoProtocol, func(s network.Stream) {
		handlers.Add(1)
		defer handlers.Done()
		defer s.Close()
		if err := o.observeConnections(h, args["relay-peer-id"]); err != nil {
			_ = s.Reset()
			return
		}
		for _, phase := range []string{"relay_before", "relay_after"} {
			if !strings.Contains(s.Conn().RemoteMultiaddr().String(), "/p2p-circuit") {
				phase = "direct_after"
			}
			if err := o.exchange(s, phase, true, gater.calls.Load); err != nil {
				_ = s.Reset()
				return
			}
			if phase == "direct_after" {
				return
			}
		}
	})
	if role == "relay" {
		relay, err = relayv2.New(h)
		if err != nil {
			return err
		}
		o.record("native_relay_started", "go.circuitv2.relay.New", map[string]any{"local_peer_id": h.ID().String()})
	} else {
		ids, ok := h.(interface{ IDService() identify.IDService })
		if !ok {
			return fmt.Errorf("native Identify service unavailable")
		}
		nativeHost = &pathNativeHost{Host: h, observer: o, dials: gater}
		// Observed public addresses are native Identify output. Listen addresses
		// are not forged as WAN candidates, and no address filter is bypassed.
		identified, err = h.EventBus().Subscribe(new(event.EvtPeerIdentificationCompleted))
		if err != nil {
			return err
		}
		identifyOwner.Add(1)
		go func() {
			defer identifyOwner.Done()
			for {
				select {
				case <-ctx.Done():
					return
				case raw, ok := <-identified.Out():
					if !ok {
						return
					}
					evt, ok := raw.(event.EvtPeerIdentificationCompleted)
					if ok {
						o.identifyObserved(h, evt, args["relay-peer-id"])
					}
				}
			}
		}()
		hp, err = holepunch.NewService(nativeHost, ids.IDService(), o.holePunchAddrs, holepunch.WithTracer(o))
		if err != nil {
			return err
		}
	}
	listen := h.Addrs()[0].Encapsulate(ma.StringCast("/p2p/" + h.ID().String())).String()
	ready := map[string]any{"implementation": "go", "status": "ready", "peer_id": h.ID().String(),
		"case_token": o.token, "listen_addrs": []string{listen}, "path_bindings": "actual_io_and_native_attempt_v1"}
	if role != "relay" {
		address, err := ma.NewMultiaddr(args["relay-addr"])
		if err != nil {
			return err
		}
		info, err := peer.AddrInfoFromP2pAddr(address)
		if err != nil {
			return err
		}
		if info.ID.String() != args["relay-peer-id"] {
			return fmt.Errorf("relay identity mismatch")
		}
		if err := h.Connect(ctx, *info); err != nil {
			return err
		}
		// The native service registers DCUtR asynchronously after observing a
		// public address. Match the donor's readiness predicate, not a delay.
		registered, err := waitPathHolePunchReady(ctx, h.Mux().Protocols)
		if err != nil {
			return err
		}
		readiness := map[string]any{"source": "go.host.Mux.Protocols", "protocol": string(holepunch.Protocol),
			"registered": true, "protocols": protocol.ConvertToStrings(registered)}
		o.record("native_dcutr_protocol_ready", "go.host.Mux.Protocols", readiness)
		ready["dcutr_protocol_readiness"] = readiness
		if role == "destination" {
			if _, err := relayclient.Reserve(ctx, h, *info); err != nil {
				return err
			}
			ready["circuit_addr"] = address.Encapsulate(ma.StringCast("/p2p-circuit/p2p/" + h.ID().String())).String()
		}
	}
	bytes, err := json.Marshal(ready)
	if err != nil {
		return err
	}
	if err := pathAtomic(args["ready-file"], bytes); err != nil {
		return err
	}
	var retained network.Stream
	sequence := uint64(0)
	ticker := time.NewTicker(25 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-ticker.C:
		}
		if _, err := os.Stat(args["stop-file"]); err == nil {
			if retained != nil {
				_ = retained.Close()
			}
			return nil
		}
		if err := o.observeConnections(h, args["relay-peer-id"]); err != nil {
			return err
		}
		if control, err := pathFields(args["control-file"]); err == nil {
			next, err := strconv.ParseUint(control["sequence"], 10, 64)
			if err != nil || next < sequence || next > sequence+1 {
				return fmt.Errorf("invalid path control sequence")
			}
			if next == sequence {
				result, err := o.result(false, false, nil)
				if err != nil {
					return err
				}
				if err := pathAtomic(args["result-file"], result); err != nil {
					return err
				}
				continue
			}
			if control["case-token"] != o.token {
				return fmt.Errorf("control token mismatch")
			}
			plan, err := pathFields(args["plan-file"])
			if err != nil {
				return err
			}
			if plan["case-token"] != o.token {
				return fmt.Errorf("plan token mismatch")
			}
			remote, err := peer.Decode(plan["peer-id"])
			if err != nil {
				return err
			}
			switch control["action"] {
			case "bind":
				if err := o.bind(h, remote); err != nil {
					return err
				}
			case "connect":
				address, err := ma.NewMultiaddr(plan["circuit-addr"])
				if err != nil {
					return err
				}
				info, err := peer.AddrInfoFromP2pAddr(address)
				if err != nil || info.ID != remote {
					return fmt.Errorf("circuit target mismatch")
				}
				if err := h.Connect(network.WithAllowLimitedConn(ctx, "path-connect"), *info); err != nil {
					return err
				}
			case "barrier", "direct_after", "relay_after":
				phase := control["action"]
				if phase == "barrier" {
					phase = "relay_before"
				}
				if phase == "relay_after" {
					if retained == nil {
						return fmt.Errorf("no retained relay stream")
					}
					if err := o.exchange(retained, phase, false, gater.calls.Load); err != nil {
						return err
					}
				} else {
					var chosen network.Conn
					for _, c := range h.Network().ConnsToPeer(remote) {
						if strings.Contains(c.RemoteMultiaddr().String(), "/p2p-circuit") == (phase == "relay_before") {
							chosen = c
							break
						}
					}
					if chosen == nil {
						return fmt.Errorf("no existing required path")
					}
					if err := o.observeConnections(h, args["relay-peer-id"]); err != nil {
						return err
					}
					before := gater.calls.Load()
					stream, err := pathOpenExisting(ctx, chosen)
					if err != nil {
						return err
					}
					after := gater.calls.Load()
					o.record("application_open", "go.network.Conn.NewStream", map[string]any{
						"stream_id": stream.ID(), "connection_id": chosen.ID(), "connected_before": true,
						"opening_basis": "native_existing_connection", "phase": phase,
						"dial_counter_basis":   "native_conn_new_stream_no_dial_api",
						"dial_attempts_before": before, "dial_attempts_after": after})
					if err := o.exchange(stream, phase, false, gater.calls.Load); err != nil {
						_ = stream.Reset()
						return err
					}
					if phase == "relay_before" {
						retained = stream
					} else {
						_ = stream.Close()
					}
				}
			case "release":
				for _, c := range h.Network().ConnsToPeer(remote) {
					if strings.Contains(c.RemoteMultiaddr().String(), "/p2p-circuit") {
						if err := o.release(c); err != nil {
							return err
						}
						break
					}
				}
			case "cancel":
				if hp == nil {
					return fmt.Errorf("no owned native holepunch service")
				}
				o.record("cancellation_requested", "go.holepunch.Service.Close.request", map[string]any{"remote_peer_id": remote.String()})
				if err := hp.Close(); err != nil {
					return err
				}
				hp = nil
				drainCtx, stopDrain := context.WithTimeout(ctx, 5*time.Second)
				drain, err := nativeHost.drain.wait(drainCtx)
				stopDrain()
				if err != nil {
					return err
				}
				drain["remote_peer_id"] = remote.String()
				o.record("native_handlers_drained", "go.native_dcutr.handler_return", drain)
				o.record("native_service_joined", "go.holepunch.Service.Close.and_handler_drain", map[string]any{"cancelled": true, "remote_peer_id": remote.String()})
			default:
				return fmt.Errorf("unsupported path control")
			}
			sequence = next
			o.record("control_completed", "go.path_control.native_call", map[string]any{"control_sequence": next, "action": control["action"]})
		}
		result, err := o.result(false, false, nil)
		if err != nil {
			return err
		}
		if err := pathAtomic(filepath.Clean(args["result-file"]), result); err != nil {
			return err
		}
	}
}

func pathReadFrame(stream io.Reader) ([]byte, error) {
	var header [4]byte
	if _, err := io.ReadFull(stream, header[:]); err != nil {
		return nil, err
	}
	size := binary.BigEndian.Uint32(header[:])
	if size == 0 || size > 128 {
		return nil, fmt.Errorf("path frame bound")
	}
	value := make([]byte, size)
	_, err := io.ReadFull(stream, value)
	return value, err
}

func pathWriteFrame(stream io.Writer, payload []byte) error {
	if len(payload) == 0 || len(payload) > 128 {
		return fmt.Errorf("path frame bound")
	}
	frame := make([]byte, 4+len(payload))
	binary.BigEndian.PutUint32(frame[:4], uint32(len(payload)))
	copy(frame[4:], payload)
	for len(frame) > 0 {
		n, err := stream.Write(frame)
		if err != nil {
			return err
		}
		if n <= 0 || n > len(frame) {
			return io.ErrShortWrite
		}
		frame = frame[n:]
	}
	return nil
}

func (o *pathObserver) exchange(s network.Stream, phase string, server bool, dialCount func() uint64) error {
	if phase != "relay_before" && phase != "relay_after" && phase != "direct_after" {
		return fmt.Errorf("invalid path phase")
	}
	o.mu.Lock()
	expected := o.expected
	o.mu.Unlock()
	c := s.Conn()
	if expected == "" || c.RemotePeer() != expected || s.Protocol() != pathEchoProtocol || dialCount == nil {
		return fmt.Errorf("path echo lacks authenticated stream/peer/no-dial owner binding")
	}
	path := "direct"
	if strings.Contains(c.RemoteMultiaddr().String(), "/p2p-circuit") {
		path = "relay"
	}
	if (phase == "direct_after") != (path == "direct") {
		return fmt.Errorf("echo selected wrong path")
	}
	limit := 3 * time.Second
	if server && phase == "relay_after" {
		limit = 40 * time.Second
	}
	if err := s.SetDeadline(time.Now().Add(limit)); err != nil {
		return err
	}
	payload := []byte("path:" + o.token + ":" + phase)
	before := dialCount()
	var read []byte
	var err error
	if server {
		read, err = pathReadFrame(s)
		if err == nil && string(read) == string(payload) {
			err = pathWriteFrame(s, read)
		}
	} else {
		err = pathWriteFrame(s, payload)
		if err == nil {
			read, err = pathReadFrame(s)
		}
	}
	if err != nil {
		return err
	}
	if string(read) != string(payload) {
		return fmt.Errorf("path echo mismatch")
	}
	after := dialCount()
	digest := sha256.Sum256(read)
	o.record("echo", "go.path_echo.io", map[string]any{"phase": phase, "protocol": string(s.Protocol()),
		"connection_id": c.ID(), "stream_id": s.ID(), "remote_peer_id": c.RemotePeer().String(), "path": path,
		"fresh_dial": false, "io_basis": "retained_native_stream", "server": server, "read_bytes": len(read), "write_bytes": len(payload), "read_sha256": hex.EncodeToString(digest[:]),
		"write_sha256": hex.EncodeToString(digest[:]), "dial_attempts_before": before, "dial_attempts_after": after})
	if phase == "relay_before" {
		o.mu.Lock()
		o.relayEcho = true
		o.mu.Unlock()
	}
	return nil
}

func pathOpenExisting(ctx context.Context, c network.Conn) (network.Stream, error) {
	// Open on this actual connection, not peer routing which may pick direct
	// instead of the retained circuit. Only application protocol negotiation occurs.
	ctx = network.WithAllowLimitedConn(network.WithNoDial(ctx, "path-proof"), "path-proof")
	s, err := c.NewStream(ctx)
	if err != nil {
		return nil, err
	}
	if err := s.SetDeadline(time.Now().Add(3 * time.Second)); err != nil {
		_ = s.Reset()
		return nil, err
	}
	selected, err := msmux.SelectOneOf([]protocol.ID{pathEchoProtocol}, s)
	if err != nil {
		_ = s.Reset()
		return nil, err
	}
	if err := s.SetProtocol(selected); err != nil {
		_ = s.Reset()
		return nil, err
	}
	if s.Conn().ID() != c.ID() {
		_ = s.Reset()
		return nil, fmt.Errorf("path echo selected a different connection")
	}
	return s, nil
}

func (o *pathObserver) result(finalized, joined bool, failure error) ([]byte, error) {
	o.mu.Lock()
	defer o.mu.Unlock()
	var message any
	if failure != nil {
		message = failure.Error()
	}
	return json.Marshal(map[string]any{"schema_version": 1, "implementation": "go", "local_peer_id": o.local.String(),
		"case_token": o.token, "events": o.events, "overflow": o.overflow, "finalized": finalized, "joined": joined, "error": message})
}

type pathQUICOwner interface {
	network.ConnSecurity
	network.ConnMultiaddrs
	As(any) bool
}

type pathQUICSecured struct {
	id              string
	localPeer, peer peer.ID
	key             crypto.PubKey
	local, remote   string
	direction       network.Direction
	observedMonoNS  int64
}

func (s pathQUICSecured) role() string {
	switch s.direction {
	case network.DirInbound:
		return "server"
	case network.DirOutbound:
		return "client"
	default:
		return ""
	}
}

func (s pathQUICSecured) receipt() map[string]any {
	direction := ""
	switch s.direction {
	case network.DirInbound:
		direction = "inbound"
	case network.DirOutbound:
		direction = "outbound"
	}
	return map[string]any{"source": "go.quic.transport.InterceptSecured", "native_connection_id": s.id,
		"native_connection_basis": "network.Conn.As(**quic.Conn)", "observed_mono_ns": s.observedMonoNS,
		"local_peer_id": s.localPeer.String(), "remote_peer_id": s.peer.String(), "local_address": s.local,
		"remote_address": s.remote, "direction": direction, "security_role": s.role()}
}

// QUIC reports the authenticated transport's direction before Swarm can relabel
// a server connection returned through holePunch/Dial as logically outbound.
type pathDialObserver struct {
	calls      atomic.Uint64
	mu         sync.Mutex
	secured    map[*quic.Conn]pathQUICSecured
	captureErr error
}

func (g *pathDialObserver) InterceptPeerDial(peer.ID) bool             { g.calls.Add(1); return true }
func (*pathDialObserver) InterceptAddrDial(peer.ID, ma.Multiaddr) bool { return true }
func (*pathDialObserver) InterceptAccept(network.ConnMultiaddrs) bool  { return true }
func (g *pathDialObserver) InterceptSecured(direction network.Direction, expected peer.ID, endpoints network.ConnMultiaddrs) bool {
	owner, ok := endpoints.(pathQUICOwner)
	if !ok || owner.ConnState().Transport != "quic-v1" {
		return true
	}
	var native *quic.Conn
	key := owner.RemotePublicKey()
	local, remote := owner.LocalMultiaddr(), owner.RemoteMultiaddr()
	valid := owner.As(&native) && native != nil && key != nil && local != nil && remote != nil &&
		owner.LocalPeer() != "" && owner.RemotePeer() == expected &&
		(direction == network.DirInbound || direction == network.DirOutbound)
	if valid {
		authenticated, err := peer.IDFromPublicKey(key)
		valid = err == nil && authenticated == expected
	}
	g.mu.Lock()
	defer g.mu.Unlock()
	if !valid {
		g.captureErr = fmt.Errorf("unbound native QUIC InterceptSecured callback")
		return true
	}
	if previous, exists := g.secured[native]; exists {
		if previous.direction != direction || previous.peer != expected || previous.localPeer != owner.LocalPeer() ||
			previous.local != local.String() || previous.remote != remote.String() || !previous.key.Equals(key) {
			g.captureErr = fmt.Errorf("conflicting native QUIC secured callbacks")
		}
		return true
	}
	if len(g.secured) == pathEventLimit {
		g.captureErr = fmt.Errorf("native QUIC secured capture exceeds bound")
		return true
	}
	if g.secured == nil {
		g.secured = make(map[*quic.Conn]pathQUICSecured)
	}
	g.secured[native] = pathQUICSecured{id: fmt.Sprintf("quic-%d", len(g.secured)+1),
		localPeer: owner.LocalPeer(), peer: expected, key: key, local: local.String(), remote: remote.String(),
		direction: direction, observedMonoNS: time.Since(pathClockOrigin).Nanoseconds() + 1}
	return true
}

func (g *pathDialObserver) quicSecurity(conn network.Conn) (pathQUICSecured, error) {
	var native *quic.Conn
	if conn.ConnState().Transport != "quic-v1" || !conn.As(&native) || native == nil {
		return pathQUICSecured{}, fmt.Errorf("coordinated connection lacks its native QUIC owner")
	}
	g.mu.Lock()
	defer g.mu.Unlock()
	if g.captureErr != nil {
		return pathQUICSecured{}, g.captureErr
	}
	secured, exists := g.secured[native]
	key := conn.RemotePublicKey()
	local, remote := conn.LocalMultiaddr(), conn.RemoteMultiaddr()
	if !exists || key == nil || local == nil || remote == nil || secured.localPeer != conn.LocalPeer() ||
		secured.peer != conn.RemotePeer() || !secured.key.Equals(key) ||
		secured.local != local.String() || secured.remote != remote.String() {
		return pathQUICSecured{}, fmt.Errorf("coordinated QUIC owner lacks its own authenticated secured callback")
	}
	return secured, nil
}

func (*pathDialObserver) InterceptUpgraded(network.Conn) (bool, control.DisconnectReason) {
	return true, 0
}
