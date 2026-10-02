package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/binary"
	"fmt"
	"os"
	"sync"
	"time"

	libp2p "github.com/libp2p/go-libp2p"
	"github.com/libp2p/go-libp2p/core/crypto"
	"github.com/libp2p/go-libp2p/core/event"
	"github.com/libp2p/go-libp2p/core/host"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/core/protocol"
	relaypb "github.com/libp2p/go-libp2p/p2p/protocol/circuitv2/pb"
	idpb "github.com/libp2p/go-libp2p/p2p/protocol/identify/pb"
	"github.com/libp2p/go-libp2p/p2p/security/noise"
	sectls "github.com/libp2p/go-libp2p/p2p/security/tls"
	ma "github.com/multiformats/go-multiaddr"
	"google.golang.org/protobuf/proto"
)

const autoRelayHop = protocol.ID("/libp2p/circuit/relay/0.2.0/hop")

type autoRelayTrace struct {
	mu       sync.Mutex
	handlers sync.WaitGroup
	events   []map[string]any
	overflow bool
	closing  bool
	joined   bool
}

func (t *autoRelayTrace) admit() bool {
	t.mu.Lock()
	defer t.mu.Unlock()
	if t.closing {
		return false
	}
	t.handlers.Add(1)
	return true
}

func (t *autoRelayTrace) closeAdmission() {
	t.mu.Lock()
	t.closing = true
	t.mu.Unlock()
}

func (t *autoRelayTrace) record(row map[string]any) {
	t.mu.Lock()
	defer t.mu.Unlock()
	if len(t.events) >= 128 {
		t.overflow = true
		return
	}
	row["unix_ms"] = time.Now().UnixMilli()
	t.events = append(t.events, row)
}

func (t *autoRelayTrace) snapshot() ([]map[string]any, bool) {
	t.mu.Lock()
	defer t.mu.Unlock()
	return append([]map[string]any{}, t.events...), t.overflow
}

type autoRelayHostConfig struct {
	service bool
	ttl     time.Duration
	trace   *autoRelayTrace
}

func autoRelayTLSSecurityOptions(auto *autoRelayHostConfig) []libp2p.Option {
	options := []libp2p.Option{libp2p.Security(sectls.ID, observedTLS)}
	if auto != nil {
		// The native circuit transport shares this upgrader; Forge/Rust use inner Noise.
		// TLS stays first for outer TCP, whose actual negotiation is checked separately.
		options = append(options, libp2p.Security(noise.ID, observedNoise))
	}
	return options
}

// Observe the selected stream while delegating every byte to the native handler.
type autoRelayObservedHost struct {
	host.Host
	trace *autoRelayTrace
}
type autoRelayObservedStream struct {
	network.Stream
	mu                 sync.Mutex
	input, output      bytes.Buffer
	overflow, finished bool
}

func (s *autoRelayObservedStream) Read(p []byte) (int, error) {
	n, err := s.Stream.Read(p)
	s.mu.Lock()
	if !s.finished {
		if s.input.Len()+n <= 8192 {
			s.input.Write(p[:n])
		} else {
			s.overflow = true
		}
	}
	s.mu.Unlock()
	return n, err
}
func (s *autoRelayObservedStream) Write(p []byte) (int, error) {
	n, err := s.Stream.Write(p)
	s.mu.Lock()
	if !s.finished {
		if s.output.Len()+n <= 8192 {
			s.output.Write(p[:n])
		} else {
			s.overflow = true
		}
	}
	s.mu.Unlock()
	return n, err
}

func (s *autoRelayObservedStream) capture() ([]byte, []byte, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.finished = true
	return append([]byte{}, s.input.Bytes()...), append([]byte{}, s.output.Bytes()...), s.overflow
}

func autoRelayHopMessage(raw []byte) (*relaypb.HopMessage, error) {
	size, prefix := binary.Uvarint(raw)
	if prefix <= 0 || size > 4096 || uint64(len(raw)-prefix) != size {
		return nil, fmt.Errorf("invalid captured hop frame")
	}
	var canonical [binary.MaxVarintLen64]byte
	if binary.PutUvarint(canonical[:], size) != prefix || !bytes.Equal(canonical[:prefix], raw[:prefix]) {
		return nil, fmt.Errorf("noncanonical captured hop frame")
	}
	message := new(relaypb.HopMessage)
	return message, proto.Unmarshal(raw[prefix:], message)
}

func autoRelayConnection(c network.Conn) map[string]any {
	state := c.ConnState()
	return map[string]any{"connection_id": c.ID(), "peer_id": c.RemotePeer().String(),
		"remote_addr": c.RemoteMultiaddr().String(), "local_addr": c.LocalMultiaddr().String(),
		"negotiated_transport": canonicalNegotiatedTransport(state.Transport),
		"negotiated_security":  string(state.Security), "negotiated_muxer": string(state.StreamMultiplexer)}
}

func (h *autoRelayObservedHost) SetStreamHandler(id protocol.ID, handler network.StreamHandler) {
	h.Host.SetStreamHandler(id, func(s network.Stream) {
		if !h.trace.admit() {
			s.Reset()
			return
		}
		defer h.trace.handlers.Done()
		row := autoRelayConnection(s.Conn())
		row["kind"], row["protocol"] = "hop_negotiated", string(s.Protocol())
		h.trace.record(row)
		observed := &autoRelayObservedStream{Stream: s}
		handler(observed)
		input, output, overflow := observed.capture()
		request, requestErr := autoRelayHopMessage(input)
		response, responseErr := autoRelayHopMessage(output)
		if requestErr == nil && request.GetType() == relaypb.HopMessage_RESERVE {
			row := autoRelayConnection(s.Conn())
			row["kind"], row["protocol"] = "reservation_response", string(s.Protocol())
			row["capture_complete"] = !overflow && responseErr == nil
			if responseErr == nil {
				row["status"] = int32(response.GetStatus())
				if response.GetReservation() != nil {
					row["expires_unix_ms"] = response.GetReservation().GetExpire() * 1000
				}
			}
			h.trace.record(row)
		}
	})
}

func autoRelayWrite(path string, value any) error {
	if err := writeJSON(path+".tmp", value); err != nil {
		return err
	}
	return os.Rename(path+".tmp", path)
}

func autoRelayIdentify(ctx context.Context, h host.Host, target peer.ID, revision string) (map[string]any, error) {
	s, err := h.NewStream(ctx, target, protocol.ID("/ipfs/id/1.0.0"))
	if err != nil {
		return nil, err
	}
	defer s.Close()
	if err := s.SetDeadline(time.Now().Add(5 * time.Second)); err != nil {
		return nil, err
	}
	raw, err := readFrame(bufio.NewReader(s))
	if err != nil {
		return nil, err
	}
	if len(raw) > 4096 {
		return nil, fmt.Errorf("oversized Identify")
	}
	message := new(idpb.Identify)
	if err := proto.Unmarshal(raw, message); err != nil {
		return nil, err
	}
	key, err := crypto.UnmarshalPublicKey(message.GetPublicKey())
	if err != nil {
		return nil, err
	}
	id, err := peer.IDFromPublicKey(key)
	if err != nil || id != target || s.Conn().RemotePeer() != target {
		return nil, fmt.Errorf("Identify authentication mismatch")
	}
	addresses := []string{}
	for _, encoded := range message.GetListenAddrs() {
		addr, err := ma.NewMultiaddrBytes(encoded)
		if err != nil {
			return nil, err
		}
		addresses = append(addresses, addr.String())
	}
	row := autoRelayConnection(s.Conn())
	row["kind"], row["protocol"] = "identify", string(s.Protocol())
	row["addresses"], row["revision"] = addresses, revision
	row["basis"] = "independent_authenticated_identify_stream"
	return row, nil
}

func runAutoRelay(opts options) (primary error) {
	if opts.command != "autorelay-relay" && opts.command != "autorelay-observe" {
		return fmt.Errorf("unknown AutoRelay command")
	}
	trace := &autoRelayTrace{}
	service := opts.command == "autorelay-relay"
	if service && opts.relayTTL == 0 {
		return fmt.Errorf("AutoRelay relay requires explicit native TTL")
	}
	h, err := newFixtureHost(opts.transport, "", "", &autoRelayHostConfig{service: service, ttl: time.Duration(opts.relayTTL) * time.Second, trace: trace})
	if err != nil {
		return err
	}
	result := func(complete bool) error {
		events, overflow := trace.snapshot()
		value := map[string]any{"schema_version": 1, "implementation": "go", "scenario": "autorelay", "transport": opts.transport,
			"peer_id": h.ID().String(), "role": "observer", "complete": complete, "overflow": overflow, "events": events,
			"native_ttl_seconds": opts.relayTTL}
		value["fixture_owned_handlers_joined"] = trace.joined
		if service {
			value["role"] = "service"
		}
		if primary != nil {
			value["error"] = primary.Error()
		}
		return autoRelayWrite(opts.resultFile, value)
	}
	defer func() {
		trace.closeAdmission()
		if err := h.Close(); primary == nil {
			primary = err
		}
		joined := make(chan struct{})
		go func() { trace.handlers.Wait(); close(joined) }()
		select {
		case <-joined:
			trace.joined = true
		case <-time.After(2 * time.Second):
			if primary == nil {
				primary = fmt.Errorf("observed native hop handlers did not join")
			}
		}
		if err := result(true); primary == nil {
			primary = err
		}
	}()
	addresses := []string{}
	for _, addr := range h.Addrs() {
		addresses = append(addresses, addr.String()+"/p2p/"+h.ID().String())
	}
	if err := autoRelayWrite(opts.readyFile, map[string]any{"implementation": "go", "peer_id": h.ID().String(), "status": "ready", "listen_addrs": addresses}); err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 55*time.Second)
	defer cancel()
	var target peer.ID
	var identification <-chan interface{}
	if !service {
		subscription, err := h.EventBus().Subscribe([]interface{}{new(event.EvtPeerProtocolsUpdated), new(event.EvtPeerIdentificationCompleted)})
		if err != nil {
			return err
		}
		defer subscription.Close()
		identification = subscription.Out()
		address, err := ma.NewMultiaddr(opts.addr)
		if err != nil {
			return err
		}
		info, err := peer.AddrInfoFromP2pAddr(address)
		if err != nil {
			return err
		}
		target = info.ID
		if err := h.Connect(ctx, *info); err != nil {
			return err
		}
	}
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	revision := ""
	nativeSequence := uint64(0)
	pendingPush := uint64(0)
	for {
		if _, err := os.Stat(opts.stopFile); err == nil {
			return nil
		}
		if !service {
			raw, err := os.ReadFile(opts.probeFile)
			if err == nil && string(raw) != revision {
				if len(raw) > 64 {
					return fmt.Errorf("probe revision limit")
				}
				row, err := autoRelayIdentify(ctx, h, target, string(raw))
				if err != nil {
					return err
				}
				trace.record(row)
				revision = string(raw)
			}
		}
		if err := result(false); err != nil {
			return err
		}
		select {
		case <-ctx.Done():
			return fmt.Errorf("AutoRelay fixture deadline")
		case <-ticker.C:
		case value, open := <-identification:
			if !open {
				return fmt.Errorf("native Identify subscription closed")
			}
			nativeSequence++
			switch identified := value.(type) {
			case event.EvtPeerProtocolsUpdated:
				if identified.Peer == target {
					pendingPush = nativeSequence
				}
			case event.EvtPeerIdentificationCompleted:
				if identified.Peer != target || identified.Conn == nil {
					continue
				}
				row := autoRelayConnection(identified.Conn)
				if identified.Conn.RemotePeer() != target {
					return fmt.Errorf("native Identify peer mismatch")
				}
				addresses := []string{}
				for _, address := range identified.ListenAddrs {
					addresses = append(addresses, address.String())
				}
				row["kind"], row["protocol"] = "native_identify", "/ipfs/id/1.0.0"
				if pendingPush != 0 {
					row["kind"], row["protocol"] = "identify_push", "/ipfs/id/push/1.0.0"
					row["basis"] = "native_EvtPeerProtocolsUpdated_then_EvtPeerIdentificationCompleted"
					row["native_update_sequence"] = pendingPush
					row["native_completed_sequence"] = nativeSequence
					pendingPush = 0
				}
				row["addresses"] = addresses
				trace.record(row)
			}
		}
	}
}
