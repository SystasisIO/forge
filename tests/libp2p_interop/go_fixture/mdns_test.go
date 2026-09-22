package main

import (
	"context"
	"encoding/hex"
	"errors"
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"

	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	ma "github.com/multiformats/go-multiaddr"
)

func mdnsTestOptions() options {
	return options{command: "dial", scenario: "mdns", bindIP: "192.0.2.1", transport: "tcp",
		payload: "challenge", readyFile: "ready", resultFile: "result", stopFile: "stop"}
}

func TestMDNSFingerprintPinnedRustGolden(t *testing.T) {
	key, err := hex.DecodeString("6189c5cf0b87fb800c1a9feeda73c6ab5e998db48fb9e6a978575c770ceef683")
	if err != nil {
		t.Fatal(err)
	}
	name, err := mdnsServiceName(key)
	if err != nil || name != "_p2p-45fc986bbc9388a11d939df26f730f0c._udp" {
		t.Fatalf("%q: %v", name, err)
	}
	if public, err := mdnsServiceName(nil); err != nil || public != "_p2p._udp" {
		t.Fatalf("%q: %v", public, err)
	}
	if _, err := mdnsServiceName(key[:31]); err == nil {
		t.Fatal("accepted malformed PSK")
	}
}

func TestMDNSRejectsRemoteCoordinatesAndUnusableBind(t *testing.T) {
	for _, transport := range []string{"tcp", "tcp-tls", "quic", "tcp-pnet"} {
		opts := mdnsTestOptions()
		opts.transport = transport
		if transport == "tcp-pnet" {
			opts.pnetKeyFile = "swarm.key"
		}
		if _, err := mdnsListenAddress(opts); err != nil {
			t.Fatal(err)
		}
	}
	for _, mutate := range []func(*options){
		func(o *options) { o.peerID = "remote" }, func(o *options) { o.addr = "/ip4/192.0.2.2/tcp/1" },
		func(o *options) { o.seedFile = "seeds" }, func(o *options) { o.seedPeerID = "remote" },
		func(o *options) { o.seedAddr = "remote" }, func(o *options) { o.targetPeerID = "remote" },
		func(o *options) { o.relayAddr = "remote" }, func(o *options) { o.relayPeerID = "remote" },
		func(o *options) { o.dnsServer = "192.0.2.2:53" }, func(o *options) { o.pnetFingerprint = "supplied" },
		func(o *options) { o.transport = "tcp-pnet" }, func(o *options) { o.pnetKeyFile = "swarm.key" },
	} {
		opts := mdnsTestOptions()
		mutate(&opts)
		if _, err := mdnsListenAddress(opts); err == nil {
			t.Fatalf("accepted forbidden input: %+v", opts)
		}
	}
	for _, bind := range []string{"127.0.0.1", "0.0.0.0", "224.0.0.251", "::1", "fe80::1%7", "example.org", "192.0.2.1\x00"} {
		opts := mdnsTestOptions()
		opts.bindIP = bind
		if _, err := mdnsListenAddress(opts); err == nil {
			t.Fatalf("accepted bind %q", bind)
		}
	}
}

func TestMDNSDiscoveryBoundAndCancellation(t *testing.T) {
	d := &mdnsDiscovery{changed: make(chan struct{}, 1)}
	addr := ma.StringCast("/ip4/192.0.2.2/tcp/4001")
	d.HandlePeerFound(peer.AddrInfo{ID: peer.ID("remote"), Addrs: []ma.Multiaddr{addr, addr}})
	snapshot, err := d.snapshot()
	if err != nil || len(snapshot[peer.ID("remote")]) != 1 {
		t.Fatal("failed dedup", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := d.waitPeer(ctx, peer.ID("missing")); err == nil {
		t.Fatal("ignored cancellation")
	}
	for i := 0; i < 64; i++ {
		d.HandlePeerFound(peer.AddrInfo{ID: peer.ID([]byte{byte(i), 1}), Addrs: []ma.Multiaddr{addr}})
	}
	if _, err := d.snapshot(); err == nil {
		t.Fatal("peer overflow was not sticky")
	}
	d = &mdnsDiscovery{changed: make(chan struct{}, 1)}
	for port := 1; port <= 17; port++ {
		// Independent wire port values avoid duplicate-address dedup hiding the bound.
		address := ma.StringCast("/ip4/192.0.2.2/tcp/" + strconv.Itoa(port))
		d.HandlePeerFound(peer.AddrInfo{ID: peer.ID("remote"), Addrs: []ma.Multiaddr{address}})
	}
	if _, err := d.snapshot(); err == nil {
		t.Fatal("address overflow was not sticky")
	}
}

type mdnsTestConn struct {
	network.Conn
	state     network.ConnectionState
	direction network.Direction
}

func (c mdnsTestConn) ID() string                    { return "actual-connection-7" }
func (c mdnsTestConn) LocalPeer() peer.ID            { return peer.ID("local") }
func (c mdnsTestConn) RemotePeer() peer.ID           { return peer.ID("remote") }
func (c mdnsTestConn) LocalMultiaddr() ma.Multiaddr  { return ma.StringCast("/ip4/192.0.2.1/tcp/4001") }
func (c mdnsTestConn) RemoteMultiaddr() ma.Multiaddr { return ma.StringCast("/ip4/192.0.2.2/tcp/5001") }
func (c mdnsTestConn) Stat() network.ConnStats {
	return network.ConnStats{Stats: network.Stats{Direction: c.direction}}
}
func (c mdnsTestConn) ConnState() network.ConnectionState { return c.state }

func TestMDNSReceiptUsesActualConnectionAndDirection(t *testing.T) {
	c := mdnsTestConn{state: network.ConnectionState{Transport: "tcp", Security: "/noise", StreamMultiplexer: "/yamux/1.0.0"}, direction: network.DirInbound}
	proof, err := mdnsConnection(protocolExchange{connection: c}, false)
	if err != nil {
		t.Fatal(err)
	}
	if proof["transport"] != "tcp" || proof["id"] != c.ID() || proof["remote_peer_id"] != c.RemotePeer().String() || proof["security"] != "/noise" {
		t.Fatal(proof)
	}
	if _, err := mdnsConnection(protocolExchange{connection: c}, true); err == nil {
		t.Fatal("accepted wrong direction")
	}
	c.state.Transport = "unobserved"
	if _, err := mdnsConnection(protocolExchange{connection: c}, false); err == nil {
		t.Fatal("accepted invented transport")
	}
	if _, err := mdnsConnection(protocolExchange{}, false); err == nil {
		t.Fatal("accepted missing connection")
	}
}

func TestMDNSQuietIsExplicitAndOtherOutcomesAreRejected(t *testing.T) {
	previous := os.Args
	defer func() { os.Args = previous }()
	os.Args = []string{"fixture", "listen", "--scenario", "mdns", "--mdns-outcome", "quiet"}
	parsed, err := parseArgs()
	if err != nil || parsed.mdnsOutcome != "quiet" {
		t.Fatal("quiet CLI did not reach the mDNS options", parsed, err)
	}
	os.Args = []string{"fixture", "listen", "--scenario", "echo", "--mdns-outcome", "quiet"}
	if _, err := parseArgs(); err == nil {
		t.Fatal("mDNS outcome accepted for another scenario")
	}
	for _, outcome := range []string{"", "echo", "quiet"} {
		opts := mdnsTestOptions()
		opts.mdnsOutcome = outcome
		if _, err := mdnsListenAddress(opts); err != nil {
			t.Fatal(outcome, err)
		}
	}
	opts := mdnsTestOptions()
	opts.mdnsOutcome = "timeout-is-success"
	if _, err := mdnsListenAddress(opts); err == nil {
		t.Fatal("accepted implicit timeout success")
	}
}

func TestMDNSQuietCountsEmptyAndWithdrawnDiscoveryCallbacks(t *testing.T) {
	d := &mdnsDiscovery{changed: make(chan struct{}, 1)}
	c := &mdnsQuietCounters{}
	if _, err := c.snapshot(d); err != nil {
		t.Fatal(err)
	}
	initial, _ := c.snapshot(d)
	if initial["donor_callback_join"] != "unavailable_native_detached_notifications" ||
		initial["discovery_callback_scope"] != "entered_callbacks_through_final_snapshot" {
		t.Fatal("quiet receipt hid the native callback ownership limitation", initial)
	}
	d.HandlePeerFound(peer.AddrInfo{ID: peer.ID("remote")})
	proof, err := c.snapshot(d)
	if err == nil || proof["discovery_callbacks"] != uint64(1) || proof["discovered_peers"] != 0 {
		t.Fatal("empty-address notification disappeared from lifetime evidence", proof, err)
	}
	d.HandlePeerFound(peer.AddrInfo{ID: peer.ID("remote"), Addrs: []ma.Multiaddr{ma.StringCast("/ip4/10.231.77.2/tcp/4001")}})
	d.mu.Lock()
	delete(d.peers, peer.ID("remote"))
	d.mu.Unlock()
	proof, err = c.snapshot(d)
	if err == nil || proof["discovery_callbacks"] != uint64(2) {
		t.Fatal("withdrawal erased lifetime discovery evidence", proof, err)
	}
}

func TestMDNSQuietCountsDisconnectedAuthenticationAndEcho(t *testing.T) {
	d := &mdnsDiscovery{changed: make(chan struct{}, 1)}
	c := &mdnsQuietCounters{}
	notifier := c.notifiee()
	notifier.Connected(nil, nil)
	notifier.Disconnected(nil, nil)
	proof, err := c.snapshot(d)
	if err == nil || proof["authenticated_connections"] != uint64(1) {
		t.Fatal("disconnect erased authentication evidence", proof, err)
	}
	c = &mdnsQuietCounters{}
	c.echoStreams.Add(1)
	if _, err := c.snapshot(d); err == nil {
		t.Fatal("echo admission passed quiet check")
	}
}

func TestMDNSQuietStopBeforeEchoAndDeadlineIsNeverSuccess(t *testing.T) {
	stop := filepath.Join(t.TempDir(), "stop")
	d := &mdnsDiscovery{changed: make(chan struct{}, 1)}
	c := &mdnsQuietCounters{}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	ticks := make(chan time.Time, 1)
	done := make(chan error, 1)
	go func() { done <- waitMDNSQuiet(ctx, stop, d, c, ticks) }()
	if err := os.WriteFile(stop, []byte("stop\n"), 0600); err != nil {
		t.Fatal(err)
	}
	ticks <- time.Now()
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-ctx.Done():
		t.Fatal("quiet stop did not join without echo")
	}
	expired, expire := context.WithDeadline(context.Background(), time.Unix(0, 0))
	defer expire()
	if err := waitMDNSQuiet(expired, stop, d, c, ticks); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatal("existing stop overrode expired deadline", err)
	}
	if _, err := c.snapshot(d); err != nil {
		t.Fatal(err)
	}
	// Finalization must check again after joins, even after a clean stop check.
	c.notifiee().Connected(nil, nil)
	if _, err := c.snapshot(d); err == nil {
		t.Fatal("late authentication passed final quiet evidence")
	}
}

func TestMDNSQuietStopRejectsActivityOverflowAndNonFile(t *testing.T) {
	stop := filepath.Join(t.TempDir(), "stop")
	if err := os.WriteFile(stop, []byte("stop\n"), 0600); err != nil {
		t.Fatal(err)
	}
	d := &mdnsDiscovery{changed: make(chan struct{}, 1), overflow: true}
	if err := waitMDNSQuiet(context.Background(), stop, d, &mdnsQuietCounters{}, nil); err == nil {
		t.Fatal("overflow passed quiet stop")
	}
	d = &mdnsDiscovery{changed: make(chan struct{}, 1)}
	if err := waitMDNSQuiet(context.Background(), filepath.Dir(stop), d, &mdnsQuietCounters{}, nil); err == nil {
		t.Fatal("directory accepted as stop request")
	}
}
