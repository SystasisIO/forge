package main

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"fmt"
	"net/netip"
	"os"
	"sync"
	"time"

	libp2p "github.com/libp2p/go-libp2p"
	"github.com/libp2p/go-libp2p/core/host"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/core/protocol"
	"github.com/libp2p/go-libp2p/p2p/host/autonat"
	v1pb "github.com/libp2p/go-libp2p/p2p/host/autonat/pb"
	"github.com/libp2p/go-libp2p/p2p/muxer/yamux"
	"github.com/libp2p/go-libp2p/p2p/protocol/autonatv2"
	"github.com/libp2p/go-libp2p/p2p/protocol/identify"
	"github.com/libp2p/go-libp2p/p2p/security/noise"
	sectls "github.com/libp2p/go-libp2p/p2p/security/tls"
	quic "github.com/libp2p/go-libp2p/p2p/transport/quic"
	"github.com/libp2p/go-libp2p/p2p/transport/tcp"
	ma "github.com/multiformats/go-multiaddr"
	manet "github.com/multiformats/go-multiaddr/net"
	"google.golang.org/protobuf/proto"
)

func isAutoNATScenario(s string) bool { return s == "autonat_v1" || s == "autonat_v2" }

func autoNATListenAddr(ip netip.Addr, transport string) ma.Multiaddr {
	family := "ip4"
	if ip.Is6() {
		family = "ip6"
	}
	tail := "/tcp/0"
	if transport == "quic" {
		tail = "/udp/0/quic-v1"
	}
	return ma.StringCast("/" + family + "/" + ip.String() + tail)
}

// Accept only a numeric, direct address for this fixture's one transport.
func autoNATAddress(value string, transport string, allowZero bool) (ma.Multiaddr, error) {
	a, err := ma.NewMultiaddr(value)
	if err != nil {
		return nil, err
	}
	parts := a.Protocols()
	want := []int{ma.P_IP4, ma.P_TCP}
	if len(parts) != 0 && parts[0].Code == ma.P_IP6 {
		want[0] = ma.P_IP6
	}
	if transport == "quic" {
		want = []int{want[0], ma.P_UDP, ma.P_QUIC_V1}
	}
	if len(parts) != len(want) {
		return nil, fmt.Errorf("AutoNAT requires a numeric direct %s address", transport)
	}
	for i := range want {
		if parts[i].Code != want[i] {
			return nil, fmt.Errorf("AutoNAT address has wrong transport: %s", a)
		}
	}
	port, err := a.ValueForProtocol(want[1])
	if err != nil || (!allowZero && port == "0") || !manet.IsPublicAddr(a) {
		return nil, fmt.Errorf("AutoNAT requires a public-classified address and nonzero probe port: %s", a)
	}
	return a, nil
}

// Both main and isolated probe hosts use the same transport/security/PSK/gater
// configuration and libp2p's default resource limits, but independent stores/keys.
func newAutoNATHost(opts options, listen []ma.Multiaddr, state *pnetConnectionState) (host.Host, error) {
	config := []libp2p.Option{libp2p.NoTransports, libp2p.NoListenAddrs,
		libp2p.DisableRelay(), libp2p.DisableIdentifyAddressDiscovery()}
	switch opts.transport {
	case "quic":
		config = append(config, libp2p.Transport(quic.NewTransport))
	case "tcp", "tcp-tls", "tcp-pnet":
		config = append(config, libp2p.Transport(tcp.NewTCPTransport), libp2p.Muxer(yamux.ID, yamux.DefaultTransport))
		if opts.transport == "tcp" {
			config = append(config, libp2p.Security(noise.ID, noise.New))
		} else {
			config = append(config, libp2p.Security(sectls.ID, sectls.New))
		}
	default:
		return nil, fmt.Errorf("unsupported AutoNAT transport %s", opts.transport)
	}
	if opts.transport == "tcp-pnet" {
		key, err := loadPnetKey(opts.pnetKeyFile)
		if err != nil {
			return nil, err
		}
		config = append(config, libp2p.PrivateNetwork(key), libp2p.ConnectionGater(&pnetConnectionGater{state: state}))
	}
	h, err := libp2p.New(config...)
	if err != nil {
		return nil, err
	}
	// Stop the automatically constructed ambient client before connecting. Only
	// the explicitly requested donor exchange below may generate test traffic.
	ambient, ok := h.(interface{ GetAutoNat() autonat.AutoNAT })
	if !ok {
		return nil, errors.Join(fmt.Errorf("host does not expose its ambient AutoNAT owner"), h.Close())
	}
	if a := ambient.GetAutoNat(); a != nil {
		if err := a.Close(); err != nil {
			return nil, errors.Join(err, h.Close())
		}
	}
	if len(listen) != 0 {
		if err := h.Network().Listen(listen...); err != nil {
			return nil, errors.Join(err, h.Close())
		}
	}
	return h, nil
}

type autoNATConnection struct {
	ID            string `json:"connection_id"`
	Local         string `json:"local_addr"`
	Remote        string `json:"remote_addr"`
	Peer          string `json:"authenticated_peer"`
	Authenticated bool   `json:"authenticated"`
	Direction     string `json:"direction"`
}

type autoNATTrace struct {
	mu          sync.Mutex
	connections []autoNATConnection
	completed   []autonatv2.EventDialRequestCompleted
	overflow    error
	failed      chan struct{}
}

const (
	autoNATConnectionTraceLimit = 128
	autoNATCompletedTraceLimit  = 64
	autoNATHandlerLimit         = 64
	// The pinned v1 service deliberately waits out its 15s dial timeout after
	// failure, even when its stream has been reset during shutdown.
	autoNATHandlerJoinTimeout = 20 * time.Second
)

func newAutoNATTrace() *autoNATTrace { return &autoNATTrace{failed: make(chan struct{})} }

func (t *autoNATTrace) failLocked(message string) {
	if t.overflow == nil {
		t.overflow = errors.New(message)
		close(t.failed)
	}
}

func (t *autoNATTrace) failure() error {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.overflow
}

func (t *autoNATTrace) connected(_ network.Network, c network.Conn) {
	authenticated := false
	if key := c.RemotePublicKey(); key != nil {
		p, err := peer.IDFromPublicKey(key)
		authenticated = err == nil && p == c.RemotePeer()
	}
	direction := "outbound"
	if c.Stat().Direction == network.DirInbound {
		direction = "inbound"
	}
	t.mu.Lock()
	defer t.mu.Unlock()
	if len(t.connections) == autoNATConnectionTraceLimit {
		t.failLocked("AutoNAT connection trace overflow")
		return
	}
	t.connections = append(t.connections, autoNATConnection{ID: c.ID(), Local: c.LocalMultiaddr().String(),
		Remote: c.RemoteMultiaddr().String(), Peer: c.RemotePeer().String(),
		Authenticated: authenticated, Direction: direction})
}

func (t *autoNATTrace) snapshot() []autoNATConnection {
	t.mu.Lock()
	defer t.mu.Unlock()
	return append([]autoNATConnection{}, t.connections...)
}

func (t *autoNATTrace) CompletedRequest(e autonatv2.EventDialRequestCompleted) {
	t.mu.Lock()
	defer t.mu.Unlock()
	if len(t.completed) == autoNATCompletedTraceLimit {
		t.failLocked("AutoNAT completed-request trace overflow")
		return
	}
	t.completed = append(t.completed, e)
}

func (*autoNATTrace) ClientCompletedRequest([]autonatv2.Request, autonatv2.Result, error) {}

// Admission and completion share one mutex. The drained channel replaces a
// WaitGroup waiter goroutine, so a timed-out join cannot leak such a waiter and
// there is no Add-versus-Wait race when a registered callback starts late.
type autoNATHandlerGroup struct {
	mu      sync.Mutex
	closed  bool
	active  map[string]network.Stream
	drained chan struct{}
	failure error
	failed  chan struct{}
}

func newAutoNATHandlerGroup() *autoNATHandlerGroup {
	return &autoNATHandlerGroup{active: make(map[string]network.Stream),
		drained: make(chan struct{}), failed: make(chan struct{})}
}

func (g *autoNATHandlerGroup) wrap(handler network.StreamHandler) network.StreamHandler {
	return func(s network.Stream) {
		g.mu.Lock()
		if g.closed || g.failure != nil {
			g.mu.Unlock()
			_ = s.Reset()
			return
		}
		if len(g.active) == autoNATHandlerLimit || g.active[s.ID()] != nil {
			if g.failure == nil {
				g.failure = errors.New("AutoNAT handler admission overflow or duplicate stream id")
				close(g.failed)
			}
			g.mu.Unlock()
			_ = s.Reset()
			return
		}
		g.active[s.ID()] = s
		g.mu.Unlock()
		defer func() {
			g.mu.Lock()
			defer g.mu.Unlock()
			delete(g.active, s.ID())
			if g.closed && len(g.active) == 0 {
				close(g.drained)
			}
		}()
		handler(s)
	}
}

func (g *autoNATHandlerGroup) closeAdmission() error {
	g.mu.Lock()
	if g.closed {
		g.mu.Unlock()
		return nil
	}
	g.closed = true
	streams := make([]network.Stream, 0, len(g.active))
	for _, s := range g.active {
		streams = append(streams, s)
	}
	if len(streams) == 0 {
		close(g.drained)
	}
	g.mu.Unlock()
	var err error
	for _, s := range streams {
		err = errors.Join(err, s.Reset())
	}
	return err
}

func (g *autoNATHandlerGroup) join(deadline time.Time) error {
	select {
	case <-g.drained:
		return nil
	default:
	}
	timer := time.NewTimer(time.Until(deadline))
	defer timer.Stop()
	select {
	case <-g.drained:
		return nil
	case <-timer.C:
		select {
		case <-g.drained:
			return nil
		default:
			return errors.New("AutoNAT handler join timed out; trace is not final")
		}
	}
}

func (g *autoNATHandlerGroup) err() error {
	g.mu.Lock()
	defer g.mu.Unlock()
	return g.failure
}

// Observe the exact v1 reply without replacing the donor's request or decoder.
// DialBack's public API hides the reply address, so an API return alone is not
// sufficient address evidence. The copy is bounded by the donor's frame limit.
type autoNATRequestHost struct {
	host.Host
	reply    *bytes.Buffer
	handlers *autoNATHandlerGroup
}

func (h *autoNATRequestHost) SetStreamHandler(id protocol.ID, handler network.StreamHandler) {
	h.Host.SetStreamHandler(id, h.handlers.wrap(handler))
}

func (h *autoNATRequestHost) SetStreamHandlerMatch(id protocol.ID, match func(protocol.ID) bool, handler network.StreamHandler) {
	h.Host.SetStreamHandlerMatch(id, match, h.handlers.wrap(handler))
}

type autoNATRequestStream struct {
	network.Stream
	reply *bytes.Buffer
	stop  func() bool
	done  chan struct{}
	once  sync.Once
}

func (h *autoNATRequestHost) NewStream(ctx context.Context, p peer.ID, ids ...protocol.ID) (network.Stream, error) {
	s, err := h.Host.NewStream(ctx, p, ids...)
	if err != nil {
		return nil, err
	}
	out := &autoNATRequestStream{Stream: s, reply: h.reply, done: make(chan struct{})}
	out.stop = context.AfterFunc(ctx, func() { _ = s.Reset(); close(out.done) })
	return out, nil
}

func (s *autoNATRequestStream) Read(p []byte) (int, error) {
	n, err := s.Stream.Read(p)
	if s.reply == nil {
		return n, err
	}
	if s.reply.Len()+n > 4106 {
		return n, fmt.Errorf("AutoNAT v1 reply exceeds bounded evidence buffer")
	}
	s.reply.Write(p[:n])
	return n, err
}

func (s *autoNATRequestStream) Close() error {
	err := s.Stream.Close()
	s.joinCancellation()
	return err
}

func (s *autoNATRequestStream) Reset() error {
	err := s.Stream.Reset()
	s.joinCancellation()
	return err
}

func (s *autoNATRequestStream) joinCancellation() {
	s.once.Do(func() {
		if s.stop() {
			close(s.done)
		}
	})
	<-s.done
}

func autoNATV1Request(ctx context.Context, h host.Host, observer peer.ID, requested ma.Multiaddr) (ma.Multiaddr, error) {
	observed := &autoNATRequestHost{Host: h, reply: &bytes.Buffer{}}
	client := autonat.NewAutoNATClient(observed, func() []ma.Multiaddr { return []ma.Multiaddr{requested} }, nil)
	if err := client.DialBack(ctx, observer); err != nil {
		return nil, err
	}
	payload, err := readFrame(bufio.NewReader(bytes.NewReader(observed.reply.Bytes())))
	if err != nil {
		return nil, err
	}
	var response v1pb.Message
	if err := proto.Unmarshal(payload, &response); err != nil {
		return nil, err
	}
	if response.GetType() != v1pb.Message_DIAL_RESPONSE || response.GetDialResponse() == nil ||
		response.GetDialResponse().GetStatus() != v1pb.Message_OK {
		return nil, fmt.Errorf("AutoNAT v1 response did not report success")
	}
	return ma.NewMultiaddrBytes(response.GetDialResponse().GetAddr())
}

func autoNATIdentify(ctx context.Context, h host.Host, observer peer.ID, id protocol.ID) error {
	ids, ok := h.(interface{ IDService() identify.IDService })
	if !ok {
		return fmt.Errorf("host does not expose Identify service")
	}
	connections := h.Network().ConnsToPeer(observer)
	if len(connections) != 1 {
		return fmt.Errorf("expected exactly one fresh observer control connection, got %d", len(connections))
	}
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-ids.IDService().IdentifyWait(connections[0]):
	}
	// IdentifyWait also closes on failure. Require the actual Identify protocol
	// inventory, even when policy forbids checking for an AutoNAT service.
	protocols, err := h.Peerstore().GetProtocols(observer)
	if err != nil {
		return err
	}
	if len(protocols) == 0 {
		return fmt.Errorf("observer Identify did not supply a protocol inventory")
	}
	if id == "" {
		return nil
	}
	supported, err := h.Peerstore().SupportsProtocols(observer, id)
	if err != nil {
		return err
	}
	if len(supported) != 1 {
		return fmt.Errorf("observer Identify does not advertise %s", id)
	}
	return nil
}

func autoNATV2Request(ctx context.Context, a *autonatv2.AutoNAT, requested ma.Multiaddr, calls *int) (autonatv2.Result, error) {
	// Identify's completion and AutoNAT's subscriber run independently. ErrNoPeers
	// means no RPC was started; never retry a refused or failed real request.
	ticker := time.NewTicker(10 * time.Millisecond)
	defer ticker.Stop()
	for {
		*calls += 1
		result, err := a.GetReachability(ctx, []autonatv2.Request{{Addr: requested, SendDialData: true}})
		if !errors.Is(err, autonatv2.ErrNoPeers) {
			return result, err
		}
		select {
		case <-ctx.Done():
			return result, ctx.Err()
		case <-ticker.C:
		}
	}
}

func autoNATBoundAddress(h host.Host, desired ma.Multiaddr) (ma.Multiaddr, error) {
	wantedIP, err := manet.ToIP(desired)
	if err != nil {
		return nil, err
	}
	portProtocol := ma.P_TCP
	if _, err := desired.ValueForProtocol(ma.P_UDP); err == nil {
		portProtocol = ma.P_UDP
	}
	wantedPort, _ := desired.ValueForProtocol(portProtocol)
	for _, addr := range h.Network().ListenAddresses() {
		ip, err := manet.ToIP(addr)
		port, portErr := addr.ValueForProtocol(portProtocol)
		if err == nil && portErr == nil && ip.Equal(wantedIP) && (wantedPort == "0" || port == wantedPort) {
			return addr, nil
		}
	}
	return nil, fmt.Errorf("requested probe address has no real listener: %s", desired)
}

func autoNATResponseAddress(addr ma.Multiaddr, local peer.ID, transport string) (ma.Multiaddr, error) {
	if addr == nil {
		return nil, fmt.Errorf("missing AutoNAT response address")
	}
	if _, err := addr.ValueForProtocol(ma.P_P2P); err == nil {
		info, err := peer.AddrInfoFromP2pAddr(addr)
		if err != nil || info.ID != local || len(info.Addrs) != 1 {
			return nil, fmt.Errorf("AutoNAT response has wrong authenticated target identity")
		}
		addr = info.Addrs[0]
	}
	return autoNATAddress(addr.String(), transport, false)
}

func runAutoNAT(opts options) (retErr error) {
	probeAPICalls := 0
	version, wireProtocol := 1, protocol.ID(autonat.AutoNATProto)
	if opts.scenario == "autonat_v2" {
		version, wireProtocol = 2, autonatv2.DialProtocol
	}
	role := "dialer"
	if opts.command == "listen" {
		role = "listener"
	}
	result := map[string]any{
		"implementation": "go", "role": role, "scenario": opts.scenario,
		"version": version, "protocol": string(wireProtocol), "transport": opts.transport,
		"internet_egress": opts.internetEgress, "requested_addr": nil, "response_addr": nil,
		"reached": false, "fresh_inbound_count": 0, "actual_connections": []autoNATConnection{},
		"authenticated_peer": nil, "nonce_proof": "not_exposed_by_donor_api",
		"dial_data_bytes": nil,
		"donor_revision":  "9cfe2cc00be5b20a0be737f002c99f81b92255c5",
		"donor_api_basis": "", "status": "ok",
		"hosts_created": 0, "hosts_closed": true,
	}
	defer func() {
		result["autonat_probe_api_calls"] = probeAPICalls
		if retErr != nil {
			result["status"], result["error"] = "error", retErr.Error()
			result["reached"] = false
		}
		if opts.resultFile != "" {
			retErr = errors.Join(retErr, writeJSON(opts.resultFile, result))
		}
	}()
	bind, err := netip.ParseAddr(opts.bindIP)
	if err != nil || bind.Zone() != "" || bind.Is4In6() {
		return fmt.Errorf("AutoNAT requires --bind-ip with a numeric IPv4/IPv6 address")
	}
	if opts.internetEgress != "" && opts.internetEgress != "allow" && opts.internetEgress != "deny" {
		return fmt.Errorf("--internet-egress must be allow or deny")
	}
	if opts.transport == "tcp-pnet" && (opts.internetEgress == "" || opts.pnetKeyFile == "") {
		return fmt.Errorf("PSK AutoNAT requires --internet-egress allow|deny and --pnet-key-file")
	}
	if opts.command == "listen" && (opts.readyFile == "" || opts.stopFile == "") {
		return fmt.Errorf("AutoNAT listener requires ready and stop files")
	}
	if opts.command == "dial" && opts.resultFile == "" {
		return fmt.Errorf("AutoNAT client requires a result file")
	}
	denied := opts.transport == "tcp-pnet" && opts.internetEgress == "deny"
	listenAddr, err := autoNATAddress(autoNATListenAddr(bind, opts.transport).String(), opts.transport, true)
	if err != nil {
		return err
	}
	desired := listenAddr
	if opts.probeAddr != "" {
		desired, err = autoNATAddress(opts.probeAddr, opts.transport, true)
		if err != nil {
			return err
		}
	}
	if version == 1 {
		ip, _ := manet.ToIP(desired)
		if ip.String() != bind.String() {
			return fmt.Errorf("AutoNAT v1 probe listener must use the observed control IP")
		}
	}
	listen := []ma.Multiaddr{listenAddr}
	if !desired.Equal(listenAddr) {
		listen = append(listen, desired)
	}
	state := &pnetConnectionState{}
	h, err := newAutoNATHost(opts, listen, state)
	if err != nil {
		return err
	}
	result["hosts_created"], result["hosts_closed"] = 1, false
	trace, probeTrace := newAutoNATTrace(), newAutoNATTrace()
	handlers := newAutoNATHandlerGroup()
	notify := &network.NotifyBundle{ConnectedF: trace.connected}
	h.Network().Notify(notify)
	var probe host.Host
	var v1 autonat.AutoNAT
	var v2 *autonatv2.AutoNAT
	v2Started := false
	defer func() {
		joinDeadline := time.Now().Add(autoNATHandlerJoinTimeout)
		closeErr := handlers.closeAdmission()
		// Close request connections before the service owner, so pending handlers
		// cannot wait on a remote that is also shutting down.
		for _, c := range h.Network().Conns() {
			closeErr = errors.Join(closeErr, c.Close())
		}
		if v2Started {
			v2.Close()
		}
		if v1 != nil {
			closeErr = errors.Join(closeErr, v1.Close())
		}
		if probe != nil {
			closeErr = errors.Join(closeErr, probe.Close())
		}
		closeErr = errors.Join(closeErr, h.Close())
		h.Network().StopNotify(notify)
		joinErr := handlers.join(joinDeadline)
		handlerErr := handlers.err()
		traceErr := errors.Join(trace.failure(), probeTrace.failure())
		retErr = errors.Join(retErr, closeErr, joinErr, handlerErr, traceErr)
		result["handlers_joined"] = joinErr == nil
		result["hosts_closed"] = closeErr == nil
		result["trace_complete"] = joinErr == nil && handlerErr == nil && traceErr == nil
		if handlerErr != nil {
			result["handler_error"] = handlerErr.Error()
		}
		if traceErr != nil {
			result["trace_error"] = traceErr.Error()
		}
		if joinErr != nil {
			result["handler_join_error"] = joinErr.Error()
			result["actual_connections"], result["probe_connections"] = nil, nil
			result["service_completed_requests"] = nil
			return
		}
		result["actual_connections"] = trace.snapshot()
		result["probe_connections"] = probeTrace.snapshot()
		if opts.command == "listen" {
			trace.mu.Lock()
			defer trace.mu.Unlock()
			completed := []map[string]any{}
			for _, e := range trace.completed {
				entry := map[string]any{"response_status": e.ResponseStatus.String(),
					"dial_status": e.DialStatus.String(), "dial_data_required": e.DialDataRequired}
				if e.DialedAddr != nil {
					entry["dialed_addr"] = e.DialedAddr.String()
				}
				if e.Error != nil {
					entry["error"] = e.Error.Error()
					retErr = errors.Join(retErr, fmt.Errorf("AutoNAT service request failed: %w", e.Error))
				}
				completed = append(completed, entry)
			}
			result["service_completed_requests"] = completed
		}
	}()
	result["local_peer_id"] = h.ID().String()
	requested, err := autoNATBoundAddress(h, desired)
	if err != nil {
		return err
	}
	result["requested_addr"] = requested.String()
	if !denied && (version == 2 || opts.command == "listen") {
		probe, err = newAutoNATHost(opts, nil, state)
		if err != nil {
			return err
		}
		result["hosts_created"] = 2
		if probe.ID() == h.ID() {
			return fmt.Errorf("AutoNAT probe host must have an independent identity")
		}
		probe.Network().Notify(&network.NotifyBundle{ConnectedF: probeTrace.connected})
		result["probe_peer_id"] = probe.ID().String()
		if version == 1 {
			// This is configured service activation, never measured client evidence.
			v1, err = autonat.New(&autoNATRequestHost{Host: h, handlers: handlers}, autonat.EnableService(probe.Network()),
				autonat.WithReachability(network.ReachabilityPublic))
			result["service_forced_public"] = true
		} else {
			v2, err = autonatv2.New(probe, autonatv2.WithMetricsTracer(trace))
			if err == nil {
				err = v2.Start(&autoNATRequestHost{Host: h, handlers: handlers})
				v2Started = err == nil
			}
			if err == nil && opts.command == "dial" {
				h.RemoveStreamHandler(autonatv2.DialProtocol)
			}
		}
		if err != nil {
			return err
		}
	}
	if opts.command == "listen" {
		result["requested_addr"] = nil
		result["donor_api_basis"] = "configured_donor_service_not_client_reachability"
		result["service_enabled"] = !denied
		addrs := []string{}
		for _, a := range h.Network().ListenAddresses() {
			addrs = append(addrs, a.String()+"/p2p/"+h.ID().String())
		}
		if err := writeJSON(opts.readyFile, map[string]any{
			"implementation": "go", "role": role, "status": "ready", "peer_id": h.ID().String(),
			"listen_addrs": addrs, "version": version, "protocol": string(wireProtocol),
			"service_enabled": !denied, "internet_egress": opts.internetEgress,
			"service_forced_public": !denied && version == 1,
		}); err != nil {
			return err
		}
		ticker := time.NewTicker(100 * time.Millisecond)
		defer ticker.Stop()
		for {
			select {
			case <-trace.failed:
				return trace.failure()
			case <-probeTrace.failed:
				return probeTrace.failure()
			case <-handlers.failed:
				return handlers.err()
			case <-ticker.C:
			}
			if _, err := os.Stat(opts.stopFile); err == nil {
				return nil
			} else if !os.IsNotExist(err) {
				return err
			}
		}
	}
	observer, err := peer.Decode(opts.peerID)
	if err != nil {
		return err
	}
	remote, err := ma.NewMultiaddr(opts.addr)
	if err != nil {
		return err
	}
	if _, err := remote.ValueForProtocol(ma.P_P2P); err == nil {
		info, err := peer.AddrInfoFromP2pAddr(remote)
		if err != nil || info.ID != observer || len(info.Addrs) != 1 {
			return fmt.Errorf("observer address and peer id disagree")
		}
		remote = info.Addrs[0]
	}
	if _, err := autoNATAddress(remote.String(), opts.transport, false); err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	if err := h.Connect(ctx, peer.AddrInfo{ID: observer, Addrs: []ma.Multiaddr{remote}}); err != nil {
		return err
	}
	control := h.Network().ConnsToPeer(observer)
	if len(control) != 1 {
		return fmt.Errorf("expected one observer connection")
	}
	key := control[0].RemotePublicKey()
	if key == nil {
		return fmt.Errorf("control connection has no authenticated remote key")
	}
	verifiedPeer, err := peer.IDFromPublicKey(key)
	if err != nil || verifiedPeer != observer || control[0].RemotePeer() != observer {
		return fmt.Errorf("control connection authentication does not match observer")
	}
	controlIP, err := manet.ToIP(control[0].LocalMultiaddr())
	if err != nil || controlIP.String() != bind.String() {
		return fmt.Errorf("control connection did not use --bind-ip; fix isolated namespace source routing")
	}
	result["control_local_addr"] = control[0].LocalMultiaddr().String()
	result["control_remote_addr"] = control[0].RemoteMultiaddr().String()
	result["control_connection_id"] = control[0].ID()
	result["control_authenticated_peer"] = verifiedPeer.String()
	result["observer_peer_id"] = observer.String()
	identifyProtocol := wireProtocol
	if denied {
		identifyProtocol = ""
	}
	if err := autoNATIdentify(ctx, h, observer, identifyProtocol); err != nil {
		return err
	}
	result["control_identify_completed"] = true
	if denied {
		result["status"], result["policy_denied"] = "rejected", true
		result["authenticated_peer"] = verifiedPeer.String()
		result["policy_basis"] = "fixture_policy_not_donor_feature"
		result["donor_api_basis"] = "fixtureegresspolicy_after_authenticated_control"
		return nil
	}
	baseline := len(trace.snapshot())
	var response ma.Multiaddr
	if version == 1 {
		probeAPICalls++
		response, err = autoNATV1Request(ctx, h, observer, requested)
		result["donor_api_basis"] = "NewAutoNATClient.DialBack + bounded_copy_of_actual_v1_response"
	} else {
		var r autonatv2.Result
		r, err = autoNATV2Request(ctx, v2, requested, &probeAPICalls)
		if err == nil && (r.AllAddrsRefused || r.Reachability != network.ReachabilityPublic || r.Idx != 0) {
			err = fmt.Errorf("AutoNAT v2 did not verify the requested public address: %+v", r)
		}
		response = r.Addr
		result["response_index"] = r.Idx
		result["donor_api_basis"] = "autonatv2.New/Start/GetReachability: donor_verified_result"
	}
	if err != nil {
		return err
	}
	response, err = autoNATResponseAddress(response, h.ID(), opts.transport)
	if err != nil {
		return err
	}
	result["response_addr"] = response.String()
	if !response.Equal(requested) {
		return fmt.Errorf("AutoNAT response address differs from requested listener: %s != %s", response, requested)
	}
	fresh := []autoNATConnection{}
	for _, c := range trace.snapshot()[baseline:] {
		if c.Direction == "inbound" && c.Local == response.String() && c.Authenticated {
			fresh = append(fresh, c)
		}
	}
	result["fresh_inbound_count"] = len(fresh)
	result["fresh_inbound_connections"] = fresh
	if len(fresh) == 0 {
		return fmt.Errorf("successful AutoNAT response without a fresh authenticated inbound connection")
	}
	result["authenticated_peer"] = fresh[0].Peer
	result["observer_peer_id"] = observer.String()
	result["reached"] = true
	return nil
}
