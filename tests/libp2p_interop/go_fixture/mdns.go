package main

import (
	"bufio"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"net/netip"
	"os"
	"sync"
	"sync/atomic"
	"time"

	libp2p "github.com/libp2p/go-libp2p"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/p2p/discovery/mdns"
	ma "github.com/multiformats/go-multiaddr"
	"golang.org/x/crypto/salsa20"
	"golang.org/x/crypto/sha3"
)

const mdnsSchema = "forge.mdns.interop.v1"
const mdnsTimeout = 45 * time.Second

func mdnsListenAddress(opts options) (string, error) {
	if opts.mdnsOutcome != "" && opts.mdnsOutcome != "echo" && opts.mdnsOutcome != "quiet" {
		return "", fmt.Errorf("mDNS outcome must be echo or quiet")
	}
	if opts.command != "listen" && opts.command != "dial" {
		return "", fmt.Errorf("mDNS requires listen or dial")
	}
	for _, value := range []string{opts.peerID, opts.addr, opts.relayAddr, opts.relayPeerID,
		opts.seedFile, opts.seedPeerID, opts.seedAddr, opts.targetPeerID, opts.dnsServer,
		opts.probeAddr, opts.internetEgress, opts.pnetFingerprint, opts.pnetControl, opts.pnetCorrelation} {
		if value != "" {
			return "", fmt.Errorf("mDNS forbids remote coordinates and external discovery inputs")
		}
	}
	ip, err := netip.ParseAddr(opts.bindIP)
	if err != nil || ip.Zone() != "" || ip.IsUnspecified() || ip.IsLoopback() || ip.IsMulticast() || ip.IsLinkLocalUnicast() {
		return "", fmt.Errorf("mDNS requires a usable numeric local --bind-ip (not loopback or scoped)")
	}
	if len(opts.payload) == 0 || len(opts.payload) > 4096 || opts.readyFile == "" || opts.resultFile == "" || opts.stopFile == "" {
		return "", fmt.Errorf("mDNS requires payload 1..4096 bytes and ready/result/stop files")
	}
	if (opts.transport == "tcp-pnet") != (opts.pnetKeyFile != "") {
		return "", fmt.Errorf("mDNS private mode requires tcp-pnet and a PSK together")
	}
	family := "ip6"
	if ip.Is4() {
		family = "ip4"
	}
	base := fmt.Sprintf("/%s/%s", family, ip.String())
	switch opts.transport {
	case "tcp", "tcp-tls", "tcp-pnet":
		return base + "/tcp/0", nil
	case "quic":
		return base + "/udp/0/quic-v1", nil
	default:
		return "", fmt.Errorf("unsupported mDNS transport")
	}
}

func mdnsServiceName(key []byte) (string, error) {
	if len(key) == 0 {
		return mdns.ServiceName, nil
	}
	if len(key) != 32 {
		return "", fmt.Errorf("mDNS PSK must contain 32 bytes")
	}
	var fixed [32]byte
	copy(fixed[:], key)
	var stream [64]byte
	salsa20.XORKeyStream(stream[:], stream[:], []byte("finprint"), &fixed)
	xof := sha3.NewShake128()
	_, _ = xof.Write(stream[:])
	var fingerprint [16]byte
	if _, err := io.ReadFull(xof, fingerprint[:]); err != nil {
		return "", err
	}
	return "_p2p-" + hex.EncodeToString(fingerprint[:]) + "._udp", nil
}

// The donor invokes notifications concurrently. Retain only bounded metadata;
// never dial or block its callback. Overflow is a sticky evidence failure.
type mdnsDiscovery struct {
	mu        sync.Mutex
	peers     map[peer.ID][]ma.Multiaddr
	bytes     int
	overflow  bool
	callbacks uint64
	changed   chan struct{}
}

func (d *mdnsDiscovery) HandlePeerFound(info peer.AddrInfo) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.callbacks == ^uint64(0) {
		d.overflow = true
		return
	}
	d.callbacks++
	if d.peers == nil {
		d.peers = make(map[peer.ID][]ma.Multiaddr)
	}
	if _, exists := d.peers[info.ID]; !exists && len(d.peers) >= 64 {
		d.overflow = true
		return
	}
	for _, addr := range info.Addrs {
		duplicate := false
		for _, old := range d.peers[info.ID] {
			if old.Equal(addr) {
				duplicate = true
				break
			}
		}
		if duplicate {
			continue
		}
		if len(d.peers[info.ID]) == 16 || len(addr.Bytes()) > 4096 || d.bytes+len(addr.Bytes()) > 256*1024 {
			d.overflow = true
			break
		}
		d.peers[info.ID] = append(d.peers[info.ID], addr)
		d.bytes += len(addr.Bytes())
	}
	select {
	case d.changed <- struct{}{}:
	default:
	}
}

func (d *mdnsDiscovery) snapshot() (map[peer.ID][]ma.Multiaddr, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.overflow {
		return nil, fmt.Errorf("mDNS discovery bound exceeded")
	}
	out := make(map[peer.ID][]ma.Multiaddr, len(d.peers))
	for id, addresses := range d.peers {
		out[id] = append([]ma.Multiaddr(nil), addresses...)
	}
	return out, nil
}

func (d *mdnsDiscovery) waitPeer(ctx context.Context, id peer.ID) ([]ma.Multiaddr, error) {
	for {
		peers, err := d.snapshot()
		if err != nil {
			return nil, err
		}
		if len(peers[id]) != 0 {
			return peers[id], nil
		}
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-d.changed:
		}
	}
}

// Quiet counters retain events even after peers disconnect. Network notifications
// are installed before opening listeners; no final active-connection snapshot is
// presented as lifetime authentication evidence.
type mdnsQuietCounters struct {
	authenticated atomic.Uint64
	echoStreams   atomic.Uint64
}

func (c *mdnsQuietCounters) notifiee() *network.NotifyBundle {
	return &network.NotifyBundle{ConnectedF: func(network.Network, network.Conn) {
		c.authenticated.Add(1)
	}}
}

func (c *mdnsQuietCounters) snapshot(d *mdnsDiscovery) (map[string]any, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	proof := map[string]any{
		"basis":                    "lifetime_notifier_and_network_notifications",
		"discovery_callback_scope": "entered_callbacks_through_final_snapshot",
		"donor_callback_join":      "unavailable_native_detached_notifications",
		"discovery_callbacks":      d.callbacks, "discovered_peers": len(d.peers),
		"authenticated_connections": c.authenticated.Load(), "echo_streams": c.echoStreams.Load(),
		// Quiet mode never invokes Connect or application read/write. These are
		// application counts, not mDNS packet counters hidden by the donor API.
		"dial_attempts": uint64(0), "application_bytes_sent": uint64(0), "application_bytes_received": uint64(0),
		"discovery_overflow": d.overflow,
	}
	if d.overflow || d.callbacks != 0 || len(d.peers) != 0 || c.authenticated.Load() != 0 || c.echoStreams.Load() != 0 {
		return proof, fmt.Errorf("mDNS quiet observation contained discovery or connection activity")
	}
	return proof, nil
}

func waitMDNSQuiet(ctx context.Context, stopFile string, d *mdnsDiscovery, c *mdnsQuietCounters,
	ticks <-chan time.Time) error {
	for {
		// Deadline wins over a coincident stop request: expiry is never quiet success.
		if err := ctx.Err(); err != nil {
			return err
		}
		if _, err := c.snapshot(d); err != nil {
			return err
		}
		if info, err := os.Stat(stopFile); err == nil {
			if !info.Mode().IsRegular() {
				return fmt.Errorf("mDNS quiet stop must be a regular file")
			}
			return ctx.Err()
		} else if !os.IsNotExist(err) {
			return err
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-d.changed:
		case <-ticks:
		}
	}
}

func mdnsConnection(exchange protocolExchange, outbound bool) (map[string]any, error) {
	c := exchange.connection
	if c == nil {
		return nil, fmt.Errorf("missing actual echo connection")
	}
	direction := "inbound"
	if c.Stat().Direction == network.DirOutbound {
		direction = "outbound"
	}
	if c.Stat().Direction != network.DirInbound && c.Stat().Direction != network.DirOutbound {
		return nil, fmt.Errorf("unknown connection direction")
	}
	if (direction == "outbound") != outbound {
		return nil, fmt.Errorf("unexpected connection direction")
	}
	state := c.ConnState()
	transport := canonicalNegotiatedTransport(state.Transport)
	if transport != "tcp" && transport != "/quic-v1" {
		return nil, fmt.Errorf("unobserved transport %q", state.Transport)
	}
	return map[string]any{"id": c.ID(), "local_peer_id": c.LocalPeer().String(),
		"remote_peer_id": c.RemotePeer().String(), "direction": direction,
		"local_address": c.LocalMultiaddr().String(), "remote_address": c.RemoteMultiaddr().String(),
		"transport": transport, "security": string(state.Security), "muxer": string(state.StreamMultiplexer),
		"binding": "echo_stream_conn"}, nil
}

func runMDNS(opts options) (err error) {
	listen, err := mdnsListenAddress(opts)
	if err != nil {
		return err
	}
	quiet := opts.mdnsOutcome == "quiet"
	if quiet {
		if _, failure := os.Stat(opts.stopFile); failure == nil {
			return fmt.Errorf("mDNS quiet stop file must not predate readiness")
		} else if !os.IsNotExist(failure) {
			return failure
		}
	}
	role := "listener"
	if opts.command == "dial" {
		role = "dialer"
	}
	result := map[string]any{"schema": mdnsSchema, "implementation": "go", "role": role, "status": "error"}
	if quiet {
		result["outcome"] = "quiet"
	}
	defer func() {
		if err != nil {
			result["status"] = "error"
			result["error"] = err.Error()
		}
		if writeErr := writeJSON(opts.resultFile, result); err == nil {
			err = writeErr
		}
	}()
	var key []byte
	if opts.pnetKeyFile != "" {
		key, err = loadPnetKey(opts.pnetKeyFile)
		if err != nil {
			return err
		}
	}
	serviceName, err := mdnsServiceName(key)
	if err != nil {
		return err
	}
	result["service_name"] = serviceName + ".local"
	hostOptions := []libp2p.Option{libp2p.NoListenAddrs}
	if !quiet {
		hostOptions = append(hostOptions, libp2p.ListenAddrStrings(listen))
	}
	h, err := newHost(opts.transport, opts.pnetKeyFile, "", hostOptions...)
	if err != nil {
		return err
	}
	result["local_peer_id"] = h.ID().String()
	ctx, cancel := context.WithTimeout(context.Background(), mdnsTimeout)
	defer cancel()
	discovered := &mdnsDiscovery{changed: make(chan struct{}, 1)}
	quietCounters := &mdnsQuietCounters{}
	if quiet {
		h.Network().Notify(quietCounters.notifiee())
	}
	service := mdns.NewMdnsService(h, serviceName, discovered)
	type completion struct {
		exchange protocolExchange
		err      error
	}
	completed := make(chan completion, 1)
	var handlers sync.WaitGroup
	var admission sync.Mutex
	accepting := true
	busy := false
	defer func() {
		admission.Lock()
		accepting = false
		admission.Unlock()
		closeErr := service.Close()
		hostErr := h.Close()
		handlers.Wait()
		result["echo_handlers_joined"] = true
		if err == nil {
			err = closeErr
		}
		if err == nil {
			err = hostErr
		}
		if quiet {
			proof, failure := quietCounters.snapshot(discovered)
			for key, value := range proof {
				result[key] = value
			}
			result["capture_phase"] = "after_stop"
			result["cleanup_scope"] = "mdns_service_host_and_admitted_echo_handlers"
			result["cleanup_complete"] = closeErr == nil && hostErr == nil
			if err == nil {
				err = failure
			}
			if err == nil && result["stop_reason"] == "stop_file" {
				result["status"] = "ok"
			}
		}
	}()
	h.SetStreamHandler(echoProtocol, func(s network.Stream) {
		admission.Lock()
		if quiet && accepting {
			handlers.Add(1)
			admission.Unlock()
			defer handlers.Done()
			quietCounters.echoStreams.Add(1)
			_ = s.Reset()
			return
		}
		if !accepting || busy || opts.command != "listen" {
			admission.Unlock()
			_ = s.Reset()
			return
		}
		busy = true
		handlers.Add(1)
		admission.Unlock()
		defer handlers.Done()
		deadline, _ := ctx.Deadline()
		_ = s.SetDeadline(deadline)
		exchange := protocolExchange{connection: s.Conn(), streamID: s.ID(), protocol: s.Protocol()}
		payload, failure := readFrame(bufio.NewReader(io.LimitReader(s, 4106)))
		if failure == nil && string(payload) != opts.payload {
			failure = fmt.Errorf("echo challenge mismatch")
		}
		// Do not let the dialer terminate its advertiser before the listener has
		// independently discovered the authenticated peer.
		if failure == nil {
			_, failure = discovered.waitPeer(ctx, s.Conn().RemotePeer())
		}
		if failure == nil {
			failure = writeFrame(s, payload)
			exchange.bytes = len(payload)
		}
		if failure == nil {
			failure = s.Close()
		} else {
			_ = s.Reset()
		}
		completed <- completion{exchange, failure}
	})
	if quiet {
		address, failure := ma.NewMultiaddr(listen)
		if failure != nil {
			return failure
		}
		if err = h.Network().Listen(address); err != nil {
			return err
		}
	}
	if err = service.Start(); err != nil {
		return err
	}
	readyAt := time.Now()
	ready := map[string]any{"schema": mdnsSchema, "status": "ready", "role": role,
		"local_peer_id": h.ID().String(), "service_name": serviceName + ".local"}
	if quiet {
		ready["outcome"] = "quiet"
		ready["ready_at_unix_ms"] = readyAt.UnixMilli()
		result["ready_at_unix_ms"] = readyAt.UnixMilli()
	}
	if err = writeJSON(opts.readyFile, ready); err != nil {
		return err
	}
	if quiet {
		ticker := time.NewTicker(100 * time.Millisecond)
		defer ticker.Stop()
		if err = waitMDNSQuiet(ctx, opts.stopFile, discovered, quietCounters, ticker.C); err != nil {
			return err
		}
		result["stop_reason"] = "stop_file"
		result["stop_requested_at_unix_ms"] = time.Now().UnixMilli()
		result["observed_milliseconds"] = time.Since(readyAt).Milliseconds()
		return nil
	}
	var exchange protocolExchange
	if opts.command == "dial" {
		var candidate peer.AddrInfo
		for candidate.ID == "" {
			peers, failure := discovered.snapshot()
			if failure != nil {
				return failure
			}
			for id, addresses := range peers {
				if id != h.ID() && len(addresses) > 0 {
					candidate = peer.AddrInfo{ID: id, Addrs: addresses}
					break
				}
			}
			if candidate.ID == "" {
				select {
				case <-ctx.Done():
					return ctx.Err()
				case <-discovered.changed:
				}
			}
		}
		if err = h.Connect(ctx, candidate); err != nil {
			return err
		}
		connections := h.Network().ConnsToPeer(candidate.ID)
		if len(connections) != 1 {
			return fmt.Errorf("ambiguous discovered connection")
		}
		id := connections[0].ID()
		stopClose := context.AfterFunc(ctx, func() { _ = connections[0].Close() })
		exchange, err = openEchoProtocol(ctx, h, candidate.ID, []byte(opts.payload))
		stopClose()
		if err != nil {
			return err
		}
		if exchange.connection.ID() != id || exchange.connection.RemotePeer() != candidate.ID {
			return fmt.Errorf("echo moved away from authenticated discovered connection")
		}
	} else {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case done := <-completed:
			exchange, err = done.exchange, done.err
		}
		if err != nil {
			return err
		}
	}
	connection, err := mdnsConnection(exchange, opts.command == "dial")
	if err != nil {
		return err
	}
	addresses, err := discovered.waitPeer(ctx, exchange.connection.RemotePeer())
	if err != nil {
		return err
	}
	strings := make([]string, 0, len(addresses))
	for _, address := range addresses {
		strings = append(strings, address.String())
	}
	digest := sha256.Sum256([]byte(opts.payload))
	result["discovery"] = map[string]any{"source": "mdns", "peer_id": exchange.connection.RemotePeer().String(), "addresses": strings}
	result["connection"] = connection
	result["echo"] = map[string]any{"protocol": string(echoProtocol), "bytes": exchange.bytes,
		"sha256": hex.EncodeToString(digest[:]), "connection_id": exchange.connection.ID(),
		"remote_peer_id": exchange.connection.RemotePeer().String(), "stream_id": exchange.streamID}
	result["status"] = "ok"
	if err = writeJSON(opts.resultFile, result); err != nil {
		return err
	}
	// Keep the listener alive until the runner has collected the dialer's receipt.
	if opts.command == "listen" {
		ticker := time.NewTicker(100 * time.Millisecond)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return ctx.Err()
			case <-ticker.C:
				if _, failure := os.Stat(opts.stopFile); failure == nil {
					return nil
				} else if !os.IsNotExist(failure) {
					return failure
				}
			}
		}
	}
	return nil
}
