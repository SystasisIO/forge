package main

import (
	"bufio"
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/netip"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	libp2p "github.com/libp2p/go-libp2p"
	"github.com/libp2p/go-libp2p/core/crypto"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/core/peerstore"
	"github.com/libp2p/go-libp2p/core/protocol"
	"github.com/libp2p/go-libp2p/core/sec"
	"github.com/libp2p/go-libp2p/core/transport"
	"github.com/libp2p/go-libp2p/p2p/muxer/yamux"
	"github.com/libp2p/go-libp2p/p2p/net/upgrader"
	"github.com/libp2p/go-libp2p/p2p/security/noise"
	"github.com/libp2p/go-libp2p/p2p/transport/tcp"
	"github.com/libp2p/go-libp2p/p2p/transport/tcpreuse"
	reuseport "github.com/libp2p/go-reuseport"
	ma "github.com/multiformats/go-multiaddr"
	manet "github.com/multiformats/go-multiaddr/net"
)

const coordinatedScenario = "coordinated_dial_port_reuse"

func parseCoordinatedArgs(argv []string) (map[string]string, error) {
	required := map[string]bool{"scenario": true, "transport": true, "coord-role": true,
		"case-token": true, "bind-ip": true, "ready-file": true, "result-file": true,
		"stop-file": true, "control-file": true, "plan-file": true}
	args := map[string]string{}
	if len(argv)%2 != 0 {
		return nil, fmt.Errorf("coordinated-live requires flag/value pairs")
	}
	for i := 0; i < len(argv); i += 2 {
		name := strings.TrimPrefix(argv[i], "--")
		if !strings.HasPrefix(argv[i], "--") || (!required[name] && name != "timeout-ms" && name != "pnet-key-file" && name != "pnet-fingerprint") || args[name] != "" || argv[i+1] == "" {
			return nil, fmt.Errorf("invalid/duplicate coordinated-live flag")
		}
		args[name] = argv[i+1]
	}
	for name := range required {
		if args[name] == "" {
			return nil, fmt.Errorf("missing coordinated-live flag %s", name)
		}
	}
	if args["timeout-ms"] == "" {
		args["timeout-ms"] = "20000"
	}
	private := args["scenario"] == coordinatedScenario+"_private_pnet"
	ip, err := netip.ParseAddr(args["bind-ip"])
	budget, budgetErr := strconv.Atoi(args["timeout-ms"])
	if err != nil || !ip.Is4() || ip.IsUnspecified() || ip.IsMulticast() || budgetErr != nil || budget < 1000 || budget > 30000 ||
		(!private && args["scenario"] != coordinatedScenario) ||
		(private && (args["transport"] != "tcp-pnet-noise" || args["pnet-key-file"] == "" || !coordinatedHex(args["pnet-fingerprint"], 64))) ||
		(!private && (args["transport"] != "tcp" || args["pnet-key-file"] != "" || args["pnet-fingerprint"] != "")) ||
		(args["coord-role"] != "initiator" && args["coord-role"] != "responder") || !coordinatedToken(args["case-token"]) {
		return nil, fmt.Errorf("invalid coordinated-live identity/role/transport/budget contract")
	}
	return args, nil
}

func coordinatedHex(value string, size int) bool {
	if len(value) != size {
		return false
	}
	for _, c := range value {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}

func coordinatedToken(value string) bool { return coordinatedHex(value, 32) }

func coordinatedFields(path string) (map[string]string, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil || info.Size() > 8192 {
		return nil, fmt.Errorf("coordinated control exceeds bound")
	}
	fields := map[string]string{}
	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		key, value, ok := strings.Cut(scanner.Text(), "=")
		if !ok || key == "" || value == "" || strings.ContainsRune(scanner.Text(), '\x00') || fields[key] != "" || len(fields) == 8 {
			return nil, fmt.Errorf("invalid coordinated control field")
		}
		fields[key] = value
	}
	return fields, scanner.Err()
}

func coordinatedGate(args map[string]string, action string, sequence int) error {
	deadline := time.Now().Add(60 * time.Second)
	for time.Now().Before(deadline) {
		if _, err := os.Stat(args["stop-file"]); err == nil {
			return context.Canceled
		}
		fields, err := coordinatedFields(args["control-file"])
		if errors.Is(err, os.ErrNotExist) {
			time.Sleep(10 * time.Millisecond)
			continue
		}
		if err != nil {
			return err
		}
		n, err := strconv.Atoi(fields["sequence"])
		if err != nil || len(fields) != 3 || fields["case-token"] != args["case-token"] || n > sequence || n < 1 {
			return fmt.Errorf("coordinated gate sequence/token mismatch")
		}
		if n == sequence {
			if fields["action"] != action {
				return fmt.Errorf("coordinated gate action mismatch")
			}
			return nil
		}
		time.Sleep(10 * time.Millisecond)
	}
	return fmt.Errorf("coordinated start gate expired")
}

func writeCoordinatedJSON(path string, value any) error {
	data, err := json.Marshal(value)
	if err != nil {
		return err
	}
	pending := path + ".coordinated.tmp"
	if err := os.WriteFile(pending, append(data, '\n'), 0o644); err != nil {
		return err
	}
	return os.Rename(pending, path)
}

type coordinatedRoles struct {
	Security string `json:"security_role"`
	Yamux    string `json:"yamux_role"`
}

type coordinatedTrace struct {
	mu                  sync.Mutex
	source              *net.TCPAddr
	remote              ma.Multiaddr
	expected            peer.ID
	token               string
	client              bool
	dials               []map[string]any
	closed              bool
	handlers            sync.WaitGroup
	reads               int
	writes              int
	application         map[string]any
	nativeDials         sync.WaitGroup
	nativeClosed        bool
	nativeJoinCompleted bool
	capable             []*coordinatedCapableConn
}

// Delegate the real protector/security/muxer pipeline. The role is captured at
// the invoked security entry point and at Yamux construction, not from CLI flags.
type coordinatedSecurity struct{ sec.SecureTransport }
type coordinatedSecureConn struct {
	sec.SecureConn
	role string
}
type coordinatedMuxedConn struct {
	network.MuxedConn
	roles coordinatedRoles
}

func (c *coordinatedMuxedConn) As(target any) bool {
	if out, ok := target.(**coordinatedMuxedConn); ok {
		*out = c
		return true
	}
	return c.MuxedConn.As(target)
}

func (s *coordinatedSecurity) SecureInbound(ctx context.Context, c net.Conn, p peer.ID) (sec.SecureConn, error) {
	result, err := s.SecureTransport.SecureInbound(ctx, c, p)
	if err != nil {
		return nil, err
	}
	return &coordinatedSecureConn{SecureConn: result, role: "responder"}, nil
}

func (s *coordinatedSecurity) SecureOutbound(ctx context.Context, c net.Conn, p peer.ID) (sec.SecureConn, error) {
	result, err := s.SecureTransport.SecureOutbound(ctx, c, p)
	if err != nil {
		return nil, err
	}
	return &coordinatedSecureConn{SecureConn: result, role: "initiator"}, nil
}

type coordinatedMuxer struct{ network.Multiplexer }

func (m *coordinatedMuxer) NewConn(c net.Conn, server bool, scope network.PeerScope) (network.MuxedConn, error) {
	secure, ok := c.(*coordinatedSecureConn)
	if !ok {
		return nil, fmt.Errorf("Yamux lacks this connection's observed authenticated security role")
	}
	result, err := m.Multiplexer.NewConn(c, server, scope)
	if err != nil {
		return nil, err
	}
	role := "initiator"
	if server {
		role = "responder"
	}
	return &coordinatedMuxedConn{MuxedConn: result, roles: coordinatedRoles{Security: secure.role, Yamux: role}}, nil
}

func coordinatedSecureTransport() func(protocol.ID, crypto.PrivKey, []upgrader.StreamMuxer) (sec.SecureTransport, error) {
	return func(id protocol.ID, key crypto.PrivKey, muxers []upgrader.StreamMuxer) (sec.SecureTransport, error) {
		delegate, err := noise.New(id, key, muxers)
		if err != nil {
			return nil, err
		}
		return &coordinatedSecurity{SecureTransport: delegate}, nil
	}
}

// A custom donor TCP dialer makes the source constraint strict: unlike the
// donor's ordinary reuse path, it has no retry using an ephemeral source port.
type coordinatedDialer struct{ trace *coordinatedTrace }

func (d *coordinatedDialer) DialContext(ctx context.Context, networkName, address string) (net.Conn, error) {
	t := d.trace
	t.mu.Lock()
	source, remote, token, client := t.source, t.remote, t.token, t.client
	t.mu.Unlock()
	simultaneous, actualClient, reason := network.GetSimultaneousConnect(ctx)
	if source == nil || remote == nil || !simultaneous || actualClient != client || reason != token {
		return nil, fmt.Errorf("native TCP dial lacks prepared simultaneous-connect context")
	}
	rnet, raddr, err := manet.DialArgs(remote)
	if err != nil || networkName != rnet || address != raddr {
		return nil, fmt.Errorf("native TCP dial differs from prepared peer tuple")
	}
	dialer := net.Dialer{LocalAddr: source, Control: reuseport.Control}
	c, err := dialer.DialContext(ctx, networkName, address)
	if err != nil {
		return nil, err
	}
	if c.LocalAddr().String() != source.String() || c.RemoteAddr().String() != raddr {
		_ = c.Close()
		return nil, fmt.Errorf("native TCP socket differs from owned listener tuple")
	}
	t.mu.Lock()
	defer t.mu.Unlock()
	if len(t.dials) == 4 {
		_ = c.Close()
		return nil, fmt.Errorf("native coordinated dial exceeds bound")
	}
	t.dials = append(t.dials, map[string]any{"source": "net.Dialer.DialContext.returned-socket",
		"local": c.LocalAddr().String(), "remote": c.RemoteAddr().String(), "simultaneous_connect": true,
		"is_client": actualClient, "reason": reason})
	return c, nil
}

type coordinatedCapableConn struct {
	transport.CapableConn
	outgoing  bool
	closeOnce sync.Once
	closeErr  error
}

func (c *coordinatedCapableConn) Close() error {
	c.closeOnce.Do(func() { c.closeErr = c.CapableConn.Close() })
	return c.closeErr
}

func (c *coordinatedCapableConn) CloseWithError(code network.ConnErrorCode) error {
	c.closeOnce.Do(func() { c.closeErr = c.CapableConn.CloseWithError(code) })
	return c.closeErr
}

func (c *coordinatedCapableConn) As(target any) bool {
	if out, ok := target.(**coordinatedCapableConn); ok {
		*out = c
		return true
	}
	return c.CapableConn.As(target)
}

// Mark only the capable connection returned by the transport's outgoing Dial.
// Tuple equality alone cannot tell it apart from an accepted collision winner.
type coordinatedTCP struct {
	*tcp.TcpTransport
	trace     *coordinatedTrace
	closeOnce sync.Once
	closeErr  error
}

// DialPeer returning is not a join of the donor's background transport dials.
// Swarm shutdown invokes this transport hook after cancelling its dial owners.
func (t *coordinatedTCP) Close() error {
	t.closeOnce.Do(func() {
		t.trace.mu.Lock()
		t.trace.nativeClosed = true
		t.trace.mu.Unlock()
		t.trace.nativeDials.Wait()
		t.trace.mu.Lock()
		connections := append([]*coordinatedCapableConn{}, t.trace.capable...)
		t.trace.mu.Unlock()
		for _, c := range connections {
			t.closeErr = errors.Join(t.closeErr, c.Close())
		}
		t.trace.mu.Lock()
		t.trace.nativeJoinCompleted = t.closeErr == nil
		t.trace.mu.Unlock()
	})
	return t.closeErr
}

func (t *coordinatedTCP) Dial(ctx context.Context, addr ma.Multiaddr, p peer.ID) (transport.CapableConn, error) {
	return t.DialWithUpdates(ctx, addr, p, nil)
}

func (t *coordinatedTCP) DialWithUpdates(ctx context.Context, addr ma.Multiaddr, p peer.ID, updates chan<- transport.DialUpdate) (transport.CapableConn, error) {
	t.trace.mu.Lock()
	if t.trace.nativeClosed {
		t.trace.mu.Unlock()
		return nil, net.ErrClosed
	}
	t.trace.nativeDials.Add(1)
	t.trace.mu.Unlock()
	defer t.trace.nativeDials.Done()
	c, err := t.TcpTransport.DialWithUpdates(ctx, addr, p, updates)
	if err != nil {
		return nil, err
	}
	observed := &coordinatedCapableConn{CapableConn: c, outgoing: true}
	t.trace.mu.Lock()
	if len(t.trace.capable) == 4 {
		t.trace.mu.Unlock()
		_ = observed.Close()
		return nil, fmt.Errorf("native capable connection exceeds coordinated bound")
	}
	t.trace.capable = append(t.trace.capable, observed)
	t.trace.mu.Unlock()
	return observed, nil
}

func coordinatedTransport(t *coordinatedTrace) func(transport.Upgrader, network.ResourceManager, *tcpreuse.ConnMgr) (transport.Transport, error) {
	return func(u transport.Upgrader, r network.ResourceManager, m *tcpreuse.ConnMgr) (transport.Transport, error) {
		native, err := tcp.NewTCPTransport(u, r, m, tcp.WithDialerForAddr(func(ma.Multiaddr) (tcp.ContextDialer, error) {
			return &coordinatedDialer{trace: t}, nil
		}))
		if err != nil {
			return nil, err
		}
		return &coordinatedTCP{TcpTransport: native, trace: t}, nil
	}
}

func coordinatedWinner(c network.Conn, t *coordinatedTrace, sourceActor bool) (map[string]any, error) {
	var outgoing *coordinatedCapableConn
	physicalOutgoing := c.As(&outgoing) && outgoing.outgoing
	t.mu.Lock()
	defer t.mu.Unlock()
	if c.IsClosed() || t.source == nil || c.RemotePeer() != t.expected || c.LocalMultiaddr().String() != "/ip4/"+t.source.IP.String()+"/tcp/"+strconv.Itoa(t.source.Port) ||
		!c.RemoteMultiaddr().Equal(t.remote) || sourceActor && !physicalOutgoing {
		return nil, fmt.Errorf("winning connection lacks expected identity/owned tuple/outgoing source")
	}
	var muxed *coordinatedMuxedConn
	if !c.As(&muxed) {
		return nil, fmt.Errorf("winning connection lacks its own observed security/Yamux roles")
	}
	roles := muxed.roles
	want := "responder"
	if t.client {
		want = "initiator"
	}
	state := c.ConnState()
	if roles.Security != want || roles.Yamux != want || state.StreamMultiplexer != yamux.ID || state.Security != noise.ID || state.Transport != "tcp" {
		return nil, fmt.Errorf("winning authenticated connection lacks observed matching security/Yamux roles")
	}
	return map[string]any{"connection": endpointConnectionReceipt(c), "roles": roles,
		"role_source":            "security.SecureInbound/SecureOutbound+yamux.NewConn",
		"native_outgoing_winner": physicalOutgoing, "physical_direction": strings.ToLower(c.Stat().Direction.String()),
		"direction_source":    "go-libp2p.swarm.Conn.Stat",
		"native_dial_sockets": append([]map[string]any{}, t.dials...)}, nil
}

func runCoordinatedLive(args map[string]string) (err error) {
	trace := &coordinatedTrace{token: args["case-token"], client: args["coord-role"] == "initiator"}
	config := []libp2p.Option{libp2p.NoTransports, libp2p.DisableRelay(),
		libp2p.Transport(coordinatedTransport(trace)), libp2p.Security(noise.ID, coordinatedSecureTransport()),
		libp2p.Muxer(yamux.ID, &coordinatedMuxer{Multiplexer: yamux.DefaultTransport}),
		libp2p.ListenAddrStrings("/ip4/" + args["bind-ip"] + "/tcp/0")}
	if args["pnet-key-file"] != "" {
		psk, failure := loadPnetKey(args["pnet-key-file"])
		if failure != nil {
			return fmt.Errorf("invalid coordinated private-network key fixture")
		}
		fingerprint := sha256.New()
		_, _ = fingerprint.Write([]byte("forge.net.pnet.operational-fingerprint.v1\x00"))
		_, _ = fingerprint.Write(psk)
		if hex.EncodeToString(fingerprint.Sum(nil)) != args["pnet-fingerprint"] {
			return fmt.Errorf("coordinated PSK fingerprint differs from installed key")
		}
		config = append(config, libp2p.PrivateNetwork(psk))
	}
	h, err := libp2p.New(config...)
	if err != nil {
		return err
	}
	result := map[string]any{"schema_version": 1, "implementation": "go", "scenario": args["scenario"],
		"case_token": trace.token, "actor_role": args["coord-role"], "local_peer_id": h.ID().String(),
		"pnet_fingerprint": nil, "status": "error", "joined": false, "operation_admitted": false,
		"admission_source": "coordinated_actor.preflight", "native_dial_joined": false, "dial_call_returned": false,
		"receipt": nil, "error": nil}
	if args["pnet-fingerprint"] != "" {
		result["pnet_fingerprint"] = args["pnet-fingerprint"]
	}
	defer func() {
		trace.mu.Lock()
		trace.closed = true
		trace.mu.Unlock()
		err = errors.Join(err, h.Close())
		trace.handlers.Wait()
		result["connections_after_stop"] = len(h.Network().Conns())
		trace.mu.Lock()
		result["handler_reads"], result["handler_writes"] = trace.reads, trace.writes
		result["native_dial_joined"] = trace.nativeJoinCompleted
		trace.mu.Unlock()
		result["joined"] = true
		if result["connections_after_stop"] != 0 || result["native_dial_joined"] != true {
			err = errors.Join(err, fmt.Errorf("coordinated native shutdown did not join its transport owners"))
		}
		if err != nil {
			result["status"], result["error"] = "error", err.Error()
		}
		err = errors.Join(err, writeCoordinatedJSON(args["result-file"], result))
	}()
	listeners := h.Network().ListenAddresses()
	if len(listeners) != 1 {
		return fmt.Errorf("coordinated fixture requires exactly one real TCP listener")
	}
	_, local, err := manet.DialArgs(listeners[0])
	if err != nil {
		return err
	}
	trace.source, err = net.ResolveTCPAddr("tcp4", local)
	if err != nil || trace.source.Port == 0 {
		return fmt.Errorf("coordinated listener lacks actual source tuple")
	}
	h.SetStreamHandler(echoProtocol, func(s network.Stream) {
		trace.mu.Lock()
		if trace.closed {
			trace.mu.Unlock()
			_ = s.Reset()
			return
		}
		trace.handlers.Add(1)
		trace.mu.Unlock()
		defer trace.handlers.Done()
		defer s.Close()
		_ = s.SetDeadline(time.Now().Add(10 * time.Second))
		request, readErr := readPrivateFrame(s)
		response := publicFramed([]byte("coordinated:" + trace.token))
		if trace.client || readErr != nil || !bytes.Equal(request, response) {
			_ = s.Reset()
			return
		}
		proof, proofErr := coordinatedWinner(s.Conn(), trace, false)
		if proofErr != nil {
			_ = s.Reset()
			return
		}
		trace.mu.Lock()
		trace.reads++
		trace.mu.Unlock()
		written, writeErr := s.Write(response)
		if writeErr != nil || written != len(response) {
			_ = s.Reset()
			return
		}
		trace.mu.Lock()
		trace.writes++
		trace.mu.Unlock()
		proof["application"] = map[string]any{"protocol": string(echoProtocol), "connection_id": s.Conn().ID(), "fresh_dial": false,
			"request": publicFrameReceipt(request, false), "response": publicFrameReceipt(response, false)}
		trace.mu.Lock()
		trace.application = proof
		trace.mu.Unlock()
	})
	ready := map[string]any{"implementation": "go", "status": "ready", "case_token": trace.token,
		"peer_id": h.ID().String(), "listen_addrs": []string{listeners[0].String() + "/p2p/" + h.ID().String()},
		"listener_address": listeners[0].String(), "listener_port": trace.source.Port}
	if err = writeCoordinatedJSON(args["ready-file"], ready); err != nil {
		return err
	}
	if err = coordinatedGate(args, "start", 1); err != nil {
		return err
	}
	plan, err := coordinatedFields(args["plan-file"])
	if err != nil || len(plan) != 3 || plan["case-token"] != trace.token {
		return fmt.Errorf("invalid coordinated peer plan")
	}
	trace.expected, err = peer.Decode(plan["peer-id"])
	if err != nil || trace.expected == h.ID() {
		return fmt.Errorf("invalid coordinated expected peer")
	}
	address, err := ma.NewMultiaddr(plan["addr"])
	if err != nil {
		return err
	}
	info, err := peer.AddrInfoFromP2pAddr(address)
	if err != nil || info.ID != trace.expected || len(info.Addrs) != 1 || len(info.Addrs[0].Protocols()) != 2 ||
		info.Addrs[0].Protocols()[0].Code != ma.P_IP4 || info.Addrs[0].Protocols()[1].Code != ma.P_TCP {
		return fmt.Errorf("coordinated target must be an identity-bound concrete IPv4 TCP listener")
	}
	trace.remote = info.Addrs[0]
	_, tuple, err := manet.DialArgs(trace.remote)
	if err != nil {
		return err
	}
	remoteIP, err := netip.ParseAddrPort(tuple)
	if err != nil || remoteIP.Port() == 0 || remoteIP.Addr().IsUnspecified() || remoteIP.Addr().IsMulticast() {
		return fmt.Errorf("coordinated peer tuple is not concrete unicast TCP")
	}
	if len(h.Network().Conns()) != 0 {
		return fmt.Errorf("coordinated fixture has preexisting connections")
	}
	h.Peerstore().AddAddrs(info.ID, info.Addrs, peerstore.TempAddrTTL)
	result["status"], result["sequence"], result["operation_admitted"] = "started", 1, true
	result["expected_peer_id"], result["owned_source"] = info.ID.String(), listeners[0].String()
	if err = writeCoordinatedJSON(args["result-file"], result); err != nil {
		return err
	}
	budget, _ := strconv.Atoi(args["timeout-ms"])
	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(budget)*time.Millisecond)
	ctx = network.WithSimultaneousConnect(ctx, trace.client, trace.token)
	type dialResult struct {
		connection network.Conn
		err        error
	}
	done := make(chan dialResult, 1)
	go func() { c, failure := h.Network().DialPeer(ctx, info.ID); done <- dialResult{c, failure} }()
	dialReturned := false
	defer func() {
		cancel()
		if !dialReturned {
			<-done
		}
		result["dial_call_returned"] = true
	}()
	var c network.Conn
	for c == nil {
		select {
		case completed := <-done:
			dialReturned = true
			result["dial_call_returned"] = true
			if completed.err != nil {
				return completed.err
			}
			c = completed.connection
		case <-time.After(10 * time.Millisecond):
			if _, failure := os.Stat(args["stop-file"]); failure == nil {
				cancel()
				return context.Canceled
			}
		}
	}
	proof, err := coordinatedWinner(c, trace, trace.client)
	if err != nil {
		return err
	}
	if len(h.Network().ConnsToPeer(info.ID)) != 1 {
		return fmt.Errorf("coordinated winner is not unique")
	}
	result["receipt"], result["status"] = proof, "connected"
	if err = writeCoordinatedJSON(args["result-file"], result); err != nil {
		return err
	}
	deadline := time.Now().Add(30 * time.Second)
	probed := false
	lastSequence := "1"
	for time.Now().Before(deadline) {
		if _, failure := os.Stat(args["stop-file"]); failure == nil {
			if !probed {
				return fmt.Errorf("coordinated actor stopped before retained-connection probe")
			}
			result["status"] = "ok"
			return nil
		}
		control, failure := coordinatedFields(args["control-file"])
		if failure != nil {
			return failure
		}
		if len(control) != 3 || control["case-token"] != trace.token || (control["sequence"] != "1" && control["sequence"] != "2") ||
			control["sequence"] < lastSequence || (control["sequence"] == "1" && control["action"] != "start") ||
			(control["sequence"] == "2" && (!trace.client || control["action"] != "probe")) {
			return fmt.Errorf("coordinated probe sequence/token mismatch")
		}
		lastSequence = control["sequence"]
		if control["sequence"] == "2" && !probed {
			if !trace.client || control["action"] != "probe" {
				return fmt.Errorf("probe is initiator-only")
			}
			probeCtx, probeCancel := context.WithTimeout(context.Background(), time.Duration(budget)*time.Millisecond)
			s, failure := h.NewStream(network.WithNoDial(probeCtx, trace.token), info.ID, echoProtocol)
			if failure != nil {
				probeCancel()
				return failure
			}
			if s.Conn().ID() != c.ID() {
				_ = s.Reset()
				probeCancel()
				return fmt.Errorf("coordinated echo changed native connection")
			}
			_ = s.SetDeadline(time.Now().Add(time.Duration(budget) * time.Millisecond))
			request := publicFramed([]byte("coordinated:" + trace.token))
			var written int
			written, failure = s.Write(request)
			if failure == nil && written != len(request) {
				failure = io.ErrShortWrite
			}
			var response []byte
			if failure == nil {
				response, failure = readPrivateFrame(s)
			}
			probeCancel()
			if failure != nil || !bytes.Equal(request, response) {
				_ = s.Reset()
				return fmt.Errorf("coordinated native Yamux echo failed")
			}
			if failure = s.Close(); failure != nil {
				return failure
			}
			if c.IsClosed() || len(h.Network().ConnsToPeer(info.ID)) != 1 {
				return fmt.Errorf("probe lost its native winner")
			}
			proof["application"] = map[string]any{"protocol": string(echoProtocol), "connection_id": c.ID(), "fresh_dial": false,
				"request": publicFrameReceipt(request, false), "response": publicFrameReceipt(response, false)}
			probed = true
			result["sequence"] = 2
		}
		if !trace.client && !probed {
			trace.mu.Lock()
			application := trace.application
			trace.mu.Unlock()
			if application != nil {
				connection := application["connection"].(map[string]any)
				if connection["connection_id"] != c.ID() {
					return fmt.Errorf("responder probe changed native winner")
				}
				proof = application
				result["receipt"] = proof
				probed = true
			}
		}
		if probed && result["status"] != "exchanged" {
			result["status"] = "exchanged"
			if err = writeCoordinatedJSON(args["result-file"], result); err != nil {
				return err
			}
		}
		time.Sleep(10 * time.Millisecond)
	}
	return fmt.Errorf("coordinated probe/stop gate expired")
}
