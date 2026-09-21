package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/binary"
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
	"sync/atomic"
	"time"

	ds "github.com/ipfs/go-datastore"
	dssync "github.com/ipfs/go-datastore/sync"
	libp2p "github.com/libp2p/go-libp2p"
	kad "github.com/libp2p/go-libp2p-kad-dht"
	pubsub "github.com/libp2p/go-libp2p-pubsub"
	recpb "github.com/libp2p/go-libp2p-record/pb"
	"github.com/libp2p/go-libp2p/core/control"
	"github.com/libp2p/go-libp2p/core/event"
	"github.com/libp2p/go-libp2p/core/host"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/core/peerstore"
	corepnet "github.com/libp2p/go-libp2p/core/pnet"
	"github.com/libp2p/go-libp2p/core/protocol"
	"github.com/libp2p/go-libp2p/core/routing"
	"github.com/libp2p/go-libp2p/p2p/host/eventbus"
	"github.com/libp2p/go-libp2p/p2p/muxer/yamux"
	"github.com/libp2p/go-libp2p/p2p/net/swarm"
	relayclient "github.com/libp2p/go-libp2p/p2p/protocol/circuitv2/client"
	relayv2 "github.com/libp2p/go-libp2p/p2p/protocol/circuitv2/relay"
	"github.com/libp2p/go-libp2p/p2p/protocol/holepunch"
	"github.com/libp2p/go-libp2p/p2p/protocol/identify"
	"github.com/libp2p/go-libp2p/p2p/protocol/ping"
	"github.com/libp2p/go-libp2p/p2p/security/noise"
	sectls "github.com/libp2p/go-libp2p/p2p/security/tls"
	quic "github.com/libp2p/go-libp2p/p2p/transport/quic"
	"github.com/libp2p/go-libp2p/p2p/transport/tcp"
	"github.com/multiformats/go-base32"
	ma "github.com/multiformats/go-multiaddr"
	madns "github.com/multiformats/go-multiaddr-dns"
	manet "github.com/multiformats/go-multiaddr/net"
	"google.golang.org/protobuf/proto"
)

const echoProtocol = protocol.ID("/forge/interop/relay-echo/1")
const pubsubTopic = "forge.pubsub.interop"
const pubsubPayload = "forge-gossipsub-live"

type options struct {
	command         string
	scenario        string
	peerID          string
	addr            string
	relayAddr       string
	relayPeerID     string
	readyFile       string
	stopFile        string
	resultFile      string
	seedFile        string
	seedPeerID      string
	seedAddr        string
	targetPeerID    string
	payload         string
	transport       string
	dnsServer       string
	bindIP          string
	probeAddr       string
	internetEgress  string
	expected        int
	pnetKeyFile     string
	pnetFingerprint string
	pnetControl     string
	pnetCorrelation string
}

func parseArgs() (options, error) {
	if len(os.Args) < 2 {
		return options{}, fmt.Errorf("missing command")
	}
	out := options{command: os.Args[1]}
	for i := 2; i < len(os.Args); i++ {
		key := os.Args[i]
		if i+1 >= len(os.Args) {
			return options{}, fmt.Errorf("missing value for %s", key)
		}
		value := os.Args[i+1]
		i++
		switch key {
		case "--scenario":
			out.scenario = value
		case "--peer-id":
			out.peerID = value
		case "--addr":
			out.addr = value
		case "--relay-addr":
			out.relayAddr = value
		case "--relay-peer-id":
			out.relayPeerID = value
		case "--ready-file":
			out.readyFile = value
		case "--stop-file":
			out.stopFile = value
		case "--result-file":
			out.resultFile = value
		case "--seed-file":
			out.seedFile = value
		case "--seed-peer-id":
			out.seedPeerID = value
		case "--seed-addr":
			out.seedAddr = value
		case "--target-peer-id":
			out.targetPeerID = value
		case "--payload":
			out.payload = value
		case "--transport":
			out.transport = value
		case "--bind-ip":
			out.bindIP = value
		case "--probe-addr":
			out.probeAddr = value
		case "--internet-egress":
			out.internetEgress = value
		case "--dns-server":
			server, err := netip.ParseAddrPort(value)
			if err != nil || server.Port() == 0 || server.Addr().Zone() != "" {
				return options{}, fmt.Errorf("--dns-server requires numeric IP:port (bracket IPv6), port 1..65535")
			}
			out.dnsServer = server.String()
		case "--expected-messages":
			n, err := strconv.Atoi(value)
			if err != nil {
				return options{}, err
			}
			out.expected = n
		case "--pnet-key-file":
			out.pnetKeyFile = value
		case "--pnet-fingerprint":
			out.pnetFingerprint = value
		case "--pnet-control":
			out.pnetControl = value
		case "--pnet-correlation":
			out.pnetCorrelation = value
		case "--store-dir", "--features":
			// Accepted for CLI parity with the FORGE fixture.
		default:
			return options{}, fmt.Errorf("unknown argument %s", key)
		}
	}
	if out.payload == "" {
		out.payload = pubsubPayload
	}
	if out.transport == "" {
		out.transport = "quic"
	}
	if out.expected == 0 {
		out.expected = 1
	}
	return out, nil
}

func writeJSON(path string, value any) error {
	data, err := json.Marshal(value)
	if err != nil {
		return err
	}
	return os.WriteFile(path, append(data, '\n'), 0o644)
}

func writeFrame(w io.Writer, payload []byte) error {
	var prefix [binary.MaxVarintLen64]byte
	size := binary.PutUvarint(prefix[:], uint64(len(payload)))
	if _, err := w.Write(prefix[:size]); err != nil {
		return err
	}
	_, err := w.Write(payload)
	return err
}

func readFrame(r *bufio.Reader) ([]byte, error) {
	size, err := binary.ReadUvarint(r)
	if err != nil {
		return nil, err
	}
	if size == 0 || size > 256*1024 {
		return nil, fmt.Errorf("invalid frame size %d", size)
	}
	payload := make([]byte, size)
	if _, err := io.ReadFull(r, payload); err != nil {
		return nil, err
	}
	return payload, nil
}

func installEchoHandler(h host.Host, pnetState *pnetConnectionState) {
	h.SetStreamHandler(echoProtocol, func(s network.Stream) {
		if pnetState != nil {
			pnetState.applicationStreams.Add(1)
		}
		defer s.Close()
		payload, err := readFrame(bufio.NewReader(s))
		if err != nil {
			_ = s.Reset()
			return
		}
		if err := writeFrame(s, payload); err != nil {
			_ = s.Reset()
			return
		}
	})
}

type fixtureHost struct {
	host.Host
	holePunch *holepunch.Service
	kad       *kad.IpfsDHT
	dhtStore  ds.Batching
	pubsub    *pubsub.PubSub
	pnet      *pnetConnectionState
	upgrades  *upgradeObserver
}

type pnetConnectionState struct {
	attempted          atomic.Uint64
	established        atomic.Uint64
	identifyStreams    atomic.Uint64
	applicationStreams atomic.Uint64
	notifier           *network.NotifyBundle
}

type pnetConnectionGater struct {
	state *pnetConnectionState
}

func (g *pnetConnectionGater) InterceptPeerDial(peer.ID) bool { return true }

func (g *pnetConnectionGater) InterceptAddrDial(peer.ID, ma.Multiaddr) bool {
	g.state.attempted.Add(1)
	return true
}

func (g *pnetConnectionGater) InterceptAccept(network.ConnMultiaddrs) bool {
	g.state.attempted.Add(1)
	return true
}

func (g *pnetConnectionGater) InterceptSecured(network.Direction, peer.ID, network.ConnMultiaddrs) bool {
	return true
}

func (g *pnetConnectionGater) InterceptUpgraded(network.Conn) (bool, control.DisconnectReason) {
	return true, 0
}

func (h *fixtureHost) Close() error {
	var closeErr error
	if h.pnet != nil {
		h.Network().StopNotify(h.pnet.notifier)
	}
	if h.kad != nil {
		closeErr = errors.Join(closeErr, h.kad.Close())
	}
	if h.holePunch != nil {
		closeErr = errors.Join(closeErr, h.holePunch.Close())
	}
	return errors.Join(closeErr, h.Host.Close())
}

func loadPnetKey(path string) (corepnet.PSK, error) {
	input, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer input.Close()
	return corepnet.DecodeV1PSK(input)
}

func newHost(transport string, pnetKeyFile string, dnsServer string) (*fixtureHost, error) {
	var pnetState *pnetConnectionState
	var upgrades *upgradeObserver
	if transport == "tcp" || transport == "tcp-tls" {
		upgrades = &upgradeObserver{}
	}
	options := []libp2p.Option{
		libp2p.NoTransports,
		libp2p.ForceReachabilityPublic(),
	}
	if dnsServer != "" {
		resolver, err := madns.NewResolver(madns.WithDefaultResolver(&net.Resolver{
			PreferGo: true,
			Dial: func(ctx context.Context, network, _ string) (net.Conn, error) {
				return (&net.Dialer{}).DialContext(ctx, network, dnsServer)
			},
		}))
		if err != nil {
			return nil, err
		}
		options = append(options, libp2p.MultiaddrResolver(swarm.ResolverFromMaDNS{Resolver: resolver}))
	}
	if transport != "tcp-pnet" {
		options = append(options, libp2p.EnableRelay())
	}
	switch transport {
	case "quic", "":
		options = append(options,
			libp2p.Transport(quic.NewTransport),
			libp2p.ListenAddrStrings("/ip4/127.0.0.1/udp/0/quic-v1"),
		)
	case "tcp":
		options = append(options,
			libp2p.Transport(observedTCP(upgrades)),
			libp2p.Security(noise.ID, observedNoise),
			libp2p.Muxer(yamux.ID, observedYamux()),
			libp2p.ListenAddrStrings("/ip4/127.0.0.1/tcp/0"),
		)
	case "tcp-tls":
		options = append(options,
			libp2p.Transport(observedTCP(upgrades)),
			libp2p.Security(sectls.ID, observedTLS),
			libp2p.Muxer(yamux.ID, observedYamux()),
			libp2p.ListenAddrStrings("/ip4/127.0.0.1/tcp/0"),
		)
	case "tcp-pnet":
		pnetState = &pnetConnectionState{}
		options = append(options,
			libp2p.Transport(tcp.NewTCPTransport),
			libp2p.Security(sectls.ID, sectls.New),
			libp2p.Muxer(yamux.ID, yamux.DefaultTransport),
			libp2p.ConnectionGater(&pnetConnectionGater{state: pnetState}),
			libp2p.ListenAddrStrings("/ip4/127.0.0.1/tcp/0"),
		)
		if pnetKeyFile != "" {
			key, err := loadPnetKey(pnetKeyFile)
			if err != nil {
				return nil, fmt.Errorf("read pnet key: %w", err)
			}
			options = append(options, libp2p.PrivateNetwork(key))
		}
	default:
		return nil, fmt.Errorf("unsupported transport %s", transport)
	}
	h, err := libp2p.New(options...)
	if err != nil {
		return nil, err
	}
	if transport == "tcp-pnet" {
		pnetState.notifier = &network.NotifyBundle{
			ConnectedF: func(network.Network, network.Conn) {
				pnetState.established.Add(1)
			},
		}
		h.Network().Notify(pnetState.notifier)
		installEchoHandler(h, pnetState)
		return &fixtureHost{Host: h, pnet: pnetState}, nil
	}
	installEchoHandler(h, nil)
	if _, err := relayv2.New(h, relayv2.WithReservationAddressFilter(fixtureReservationAddressFilter(h))); err != nil {
		h.Close()
		return nil, err
	}
	dhtStore := dssync.MutexWrap(ds.NewMapDatastore())
	dht, err := kad.New(context.Background(), h, kad.Mode(kad.ModeServer), kad.DisableAutoRefresh(),
		kad.Datastore(dhtStore))
	if err != nil {
		h.Close()
		return nil, err
	}
	pubsubRouter, err := pubsub.NewGossipSub(
		context.Background(),
		h,
		pubsub.WithGossipSubProtocols(
			[]protocol.ID{pubsub.GossipSubID_v11, pubsub.GossipSubID_v10},
			pubsub.GossipSubDefaultFeatures,
		),
	)
	if err != nil {
		h.Close()
		return nil, err
	}
	type idServiceHost interface {
		IDService() identify.IDService
	}
	identityHost, ok := h.(idServiceHost)
	if !ok {
		h.Close()
		return nil, fmt.Errorf("host does not expose identify service")
	}
	holePunchService, err := holepunch.NewService(h, identityHost.IDService(), h.Addrs)
	if err != nil {
		h.Close()
		return nil, err
	}
	return &fixtureHost{Host: h, holePunch: holePunchService, kad: dht, dhtStore: dhtStore,
		pubsub: pubsubRouter, upgrades: upgrades}, nil
}

func fixtureReservationAddressFilter(h host.Host) relayv2.ReservationAddressFilterFunc {
	fixtureLoopbacks := make(map[string]struct{})
	for _, addr := range h.Addrs() {
		if value, err := addr.ValueForProtocol(ma.P_IP4); err == nil {
			if parsed, parseErr := netip.ParseAddr(value); parseErr == nil && parsed.IsLoopback() {
				fixtureLoopbacks[addr.String()] = struct{}{}
			}
		}
		if value, err := addr.ValueForProtocol(ma.P_IP6); err == nil {
			if parsed, parseErr := netip.ParseAddr(value); parseErr == nil && parsed.IsLoopback() {
				fixtureLoopbacks[addr.String()] = struct{}{}
			}
		}
	}
	return func(addr ma.Multiaddr) bool {
		if _, ok := fixtureLoopbacks[addr.String()]; ok {
			return true
		}
		return manet.IsPublicAddr(addr)
	}
}

func pnetEvidence(opts options) map[string]any {
	return map[string]any{
		"pnet_enabled":     true,
		"negotiated_pnet":  true,
		"pnet_fingerprint": opts.pnetFingerprint,
	}
}

func pnetRejection(opts options, role string, expectedPeer string, state *pnetConnectionState) map[string]any {
	attempted := uint64(0)
	established := uint64(0)
	identifyStreams := uint64(0)
	applicationStreams := uint64(0)
	if state != nil {
		attempted = state.attempted.Load()
		established = state.established.Load()
		identifyStreams = state.identifyStreams.Load()
		applicationStreams = state.applicationStreams.Load()
	}
	return map[string]any{
		"implementation":           "go",
		"role":                     role,
		"scenario":                 "pnet",
		"status":                   "rejected",
		"control_kind":             opts.pnetControl,
		"correlation_token":        opts.pnetCorrelation,
		"expected_peer_id":         expectedPeer,
		"counter_source":           "go-libp2p.connection-gater",
		"attempted_connections":    attempted,
		"established_connections":  established,
		"identify_streams":         identifyStreams,
		"application_streams":      applicationStreams,
		"rejected_before_identify": established == 0 && identifyStreams == 0 && applicationStreams == 0,
	}
}

func dhtValueFixture(scenario string) ([]byte, []byte, error) {
	const identityMultihash = "00240801122079b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664"
	keyPrefix := ""
	valueHex := ""
	switch scenario {
	case "dht_pk_put_get":
		keyPrefix = "2f706b2f"
		valueHex = "0801122079b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664"
	case "dht_ipns_put_get":
		keyPrefix = "2f69706e732f"
		valueHex = "0a1f2f697066732f6261666b716163336a6f627868676964736e3572777734796b1240b7be19b36e1955d2e1ccddd889d25c" +
			"4eaef61aa72763bc44db9696697be7587e35d2efb2a625e7ac19b05f8c348086114103ee042a5a4041683e39c4ac0c460118" +
			"00221e323033302d30312d30325430333a30343a30352e3132333435363738395a28073080f092cbdd0842408904024a1b09" +
			"b52636334f17b9098f648f9a00214e6c6c89bb954c01300b00f54d085ddcacbe42952f2f819d70a48ff453d13329bb775d66" +
			"e5a4b6165c38a40a4a76a56354544c1b00000045d964b8006556616c7565581f2f697066732f6261666b716163336a6f6278" +
			"68676964736e3572777734796b6853657175656e6365076856616c6964697479581e323033302d30312d30325430333a30343a" +
			"30352e3132333435363738395a6c56616c69646974795479706500"
	default:
		return nil, nil, fmt.Errorf("unknown DHT value fixture %s", scenario)
	}
	key, err := hex.DecodeString(keyPrefix + identityMultihash)
	if err != nil {
		return nil, nil, err
	}
	value, err := hex.DecodeString(valueHex)
	return key, value, err
}

func isDHTValueScenario(scenario string) bool {
	return scenario == "dht_pk_put_get" || scenario == "dht_ipns_put_get"
}

func hasDHTValue(ctx context.Context, h *fixtureHost, key []byte, expected []byte) (bool, error) {
	dsKey := ds.NewKey(base32.RawStdEncoding.EncodeToString(key))
	encoded, err := h.dhtStore.Get(ctx, dsKey)
	if errors.Is(err, ds.ErrNotFound) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	record := new(recpb.Record)
	if err := proto.Unmarshal(encoded, record); err != nil {
		return false, err
	}
	return bytes.Equal(record.GetKey(), key) && bytes.Equal(record.GetValue(), expected), nil
}

func addDHTPeer(h *fixtureHost, info *peer.AddrInfo) {
	h.Peerstore().AddAddrs(info.ID, info.Addrs, peerstore.PermanentAddrTTL)
	_, _ = h.kad.RoutingTable().TryAddPeer(info.ID, true, false)
}

func dhtQueryEvidence(ctx context.Context, query func(context.Context) error) (int, error) {
	queryCtx, cancel := context.WithCancel(ctx)
	defer cancel()
	queryCtx, events := routing.RegisterForQueryEvents(queryCtx)
	queries := make(chan int, 1)
	go func() {
		count := 0
		for event := range events {
			if event.Type == routing.SendingQuery {
				count++
			}
		}
		queries <- count
	}()
	err := query(queryCtx)
	cancel()
	count := <-queries
	return count, err
}

func connectDHTSeed(ctx context.Context, h *fixtureHost, seedAddr, seedPeerID string) (peer.ID, int, error) {
	addr, err := ma.NewMultiaddr(seedAddr)
	if err != nil {
		return "", 0, fmt.Errorf("parse DHT seed address: %w", err)
	}
	info, err := peer.AddrInfoFromP2pAddr(addr)
	if err != nil {
		return "", 0, fmt.Errorf("parse DHT seed peer: %w", err)
	}
	expected, err := peer.Decode(seedPeerID)
	if err != nil {
		return "", 0, fmt.Errorf("parse expected DHT seed peer: %w", err)
	}
	if info.ID != expected {
		return "", 0, fmt.Errorf("DHT seed address peer %s does not match supplied peer %s", info.ID, expected)
	}
	if err := h.Connect(ctx, *info); err != nil {
		return "", 0, fmt.Errorf("connect DHT seed: %w", err)
	}
	addDHTPeer(h, info)
	count, err := dhtQueryEvidence(ctx, func(queryCtx context.Context) error {
		_, err := h.kad.GetClosestPeers(queryCtx, "forge-hidden-route-seed")
		return err
	})
	if err != nil {
		return "", 0, fmt.Errorf("DHT seed query failed: %w", err)
	}
	if count == 0 {
		return "", 0, fmt.Errorf("DHT seed query did not negotiate /ipfs/kad/1.0.0")
	}
	return info.ID, count, nil
}

func waitPubSubPeer(ctx context.Context, ps *pubsub.PubSub, topic string, peerID peer.ID) bool {
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	for {
		for _, current := range ps.ListPeers(topic) {
			if current == peerID {
				return true
			}
		}
		select {
		case <-ctx.Done():
			return false
		case <-ticker.C:
		}
	}
}

func installPubSubListener(h *fixtureHost, resultFile string) error {
	if resultFile == "" {
		return fmt.Errorf("missing result file for gossipsub listener")
	}
	sub, err := h.pubsub.Subscribe(pubsubTopic, pubsub.WithBufferSize(8))
	if err != nil {
		return err
	}
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
		defer cancel()
		msg, err := sub.Next(ctx)
		if err != nil {
			_ = writeJSON(resultFile, map[string]any{
				"implementation": "go",
				"scenario":       "gossipsub_publish",
				"status":         "error",
				"error":          err.Error(),
			})
			return
		}
		status := "ok"
		if string(msg.Data) != pubsubPayload {
			status = "mismatch"
		}
		_ = writeJSON(resultFile, map[string]any{
			"implementation":     "go",
			"scenario":           "gossipsub_publish",
			"status":             status,
			"topic":              pubsubTopic,
			"payload":            string(msg.Data),
			"propagation_source": msg.ReceivedFrom.String(),
			"source":             msg.GetFrom().String(),
		})
	}()
	return nil
}

type pubsubStressState struct {
	mu         sync.Mutex
	payloads   map[string]struct{}
	duplicates int
}

func installPubSubStressListener(h *fixtureHost) (*pubsubStressState, error) {
	sub, err := h.pubsub.Subscribe(pubsubTopic, pubsub.WithBufferSize(64))
	if err != nil {
		return nil, err
	}
	state := &pubsubStressState{payloads: make(map[string]struct{})}
	go func() {
		for {
			msg, err := sub.Next(context.Background())
			if err != nil {
				return
			}
			payload := string(msg.Data)
			state.mu.Lock()
			if _, ok := state.payloads[payload]; ok {
				state.duplicates++
			}
			state.payloads[payload] = struct{}{}
			state.mu.Unlock()
		}
	}()
	return state, nil
}

func connectSeedPeers(ctx context.Context, h host.Host, seedFile string) error {
	data, err := os.ReadFile(seedFile)
	if err != nil {
		return err
	}
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		addr, err := ma.NewMultiaddr(line)
		if err != nil {
			continue
		}
		info, err := peer.AddrInfoFromP2pAddr(addr)
		if err != nil || info.ID == h.ID() {
			continue
		}
		if err := h.Connect(ctx, *info); err != nil {
			continue
		}
	}
	return nil
}

func writePubSubStressResult(opts options, state *pubsubStressState) error {
	state.mu.Lock()
	defer state.mu.Unlock()
	payloads := make([]string, 0, len(state.payloads))
	for payload := range state.payloads {
		payloads = append(payloads, payload)
	}
	status := "ok"
	if len(payloads) < opts.expected || state.duplicates != 0 {
		status = "mismatch"
	}
	return writeJSON(opts.resultFile, map[string]any{
		"implementation": "go",
		"scenario":       "gossipsub_mixed_mesh_stress",
		"status":         status,
		"received":       len(payloads),
		"expected":       opts.expected,
		"duplicates":     state.duplicates,
		"payloads":       payloads,
	})
}

func listen(opts options) (err error) {
	h, err := newHost(opts.transport, opts.pnetKeyFile, opts.dnsServer)
	if err != nil {
		return err
	}
	defer func() {
		err = errors.Join(err, h.Close())
		if h.upgrades != nil && opts.resultFile != "" && upgradeProofProtocol(opts.scenario) != "" {
			proof, proofErr := h.upgrades.finish(upgradeProofProtocol(opts.scenario), "", network.DirInbound)
			err = errors.Join(err, proofErr)
			result := map[string]any{"implementation": "go", "role": "listener", "scenario": opts.scenario,
				"local_peer_id": h.ID().String(), "status": "ok", "upgrade_observation": proof}
			if err != nil {
				result["status"], result["error"] = "error", err.Error()
			}
			err = errors.Join(err, writeJSON(opts.resultFile, result))
		}
	}()
	var stress *pubsubStressState
	if opts.scenario == "gossipsub_publish" {
		if err := installPubSubListener(h, opts.resultFile); err != nil {
			return err
		}
	} else if opts.scenario == "gossipsub_mixed_mesh_stress" {
		stress, err = installPubSubStressListener(h)
		if err != nil {
			return err
		}
	}
	var expectedKey, expectedValue []byte
	if isDHTValueScenario(opts.scenario) {
		expectedKey, expectedValue, err = dhtValueFixture(opts.scenario)
		if err != nil {
			return err
		}
	}

	out := make([]string, 0, len(h.Addrs()))
	for _, addr := range h.Addrs() {
		out = append(out, addr.String()+"/p2p/"+h.ID().String())
	}
	protocols := make([]string, 0, len(h.Mux().Protocols()))
	for _, protocolID := range h.Mux().Protocols() {
		protocols = append(protocols, string(protocolID))
	}
	if err := writeJSON(opts.readyFile, map[string]any{
		"implementation": "go",
		"role":           "listener",
		"peer_id":        h.ID().String(),
		"listen_addrs":   out,
		"protocols":      protocols,
		"transport":      opts.transport,
		"status":         "ready",
	}); err != nil {
		return err
	}
	seeded := false
	recordReported := false
	pnetReported := false
	for {
		if !pnetReported && opts.scenario == "pnet" && h.pnet != nil && h.pnet.established.Load() > 0 {
			result := map[string]any{
				"implementation": "go",
				"role":           "listener",
				"scenario":       "pnet",
				"status":         "ok",
			}
			for key, value := range pnetEvidence(opts) {
				result[key] = value
			}
			if err := writeJSON(opts.resultFile, result); err != nil {
				return err
			}
			pnetReported = true
		}
		if !recordReported && isDHTValueScenario(opts.scenario) {
			found, err := hasDHTValue(context.Background(), h, expectedKey, expectedValue)
			if err != nil {
				return err
			}
			if found {
				if err := writeJSON(opts.resultFile, map[string]any{
					"implementation":   "go",
					"role":             "listener",
					"scenario":         opts.scenario,
					"status":           "ok",
					"record_persisted": true,
				}); err != nil {
					return err
				}
				recordReported = true
			}
		}
		if !seeded && opts.scenario == "gossipsub_mixed_mesh_stress" && opts.seedFile != "" {
			if _, err := os.Stat(opts.seedFile); err == nil {
				ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
				_ = connectSeedPeers(ctx, h, opts.seedFile)
				cancel()
				seeded = true
			}
		}
		if !seeded && opts.scenario == "dht_hidden_find_peer" {
			if opts.seedPeerID == "" || opts.seedAddr == "" || opts.resultFile == "" {
				return fmt.Errorf("dht_hidden_find_peer routing listener requires --seed-peer-id, --seed-addr and --result-file")
			}
			ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
			seedPeer, queries, err := connectDHTSeed(ctx, h, opts.seedAddr, opts.seedPeerID)
			cancel()
			if err != nil {
				return err
			}
			if err := writeJSON(opts.resultFile, map[string]any{
				"implementation":      "go",
				"role":                "routing_listener",
				"scenario":            opts.scenario,
				"status":              "ok",
				"seed_peer_id":        seedPeer.String(),
				"dht_queries_delta":   queries,
				"negotiated_protocol": "/ipfs/kad/1.0.0",
				"authenticated_seed":  true,
			}); err != nil {
				return err
			}
			seeded = true
		}
		if _, err := os.Stat(opts.stopFile); err == nil {
			if !pnetReported && opts.scenario == "pnet" && opts.pnetControl != "" {
				return writeJSON(opts.resultFile, pnetRejection(opts, "listener", "", h.pnet))
			}
			if stress != nil {
				return writePubSubStressResult(opts, stress)
			}
			return nil
		}
		time.Sleep(100 * time.Millisecond)
	}
}

func destination(opts options) error {
	h, err := newHost(opts.transport, opts.pnetKeyFile, opts.dnsServer)
	if err != nil {
		return err
	}
	defer h.Close()

	relayAddr, err := ma.NewMultiaddr(opts.relayAddr)
	if err != nil {
		return err
	}
	relayInfo, err := peer.AddrInfoFromP2pAddr(relayAddr)
	if err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	if err := h.Connect(ctx, *relayInfo); err != nil {
		return fmt.Errorf("connect relay failed: %w", err)
	}
	reservation, err := relayclient.Reserve(ctx, h, *relayInfo)
	if err != nil {
		return fmt.Errorf("reserve relay failed: %w", err)
	}
	listenAddrs := make([]string, 0, len(h.Addrs()))
	for _, addr := range h.Addrs() {
		listenAddrs = append(listenAddrs, addr.String()+"/p2p/"+h.ID().String())
	}
	relayAddrs := make([]string, 0, len(reservation.Addrs))
	for _, addr := range reservation.Addrs {
		relayAddrs = append(relayAddrs, addr.String())
	}
	voucherPayloadBytes := 0
	voucherRelay := ""
	voucherPeer := ""
	voucherExpiration := int64(0)
	if reservation.Voucher != nil {
		if payload, err := reservation.Voucher.MarshalRecord(); err == nil {
			voucherPayloadBytes = len(payload)
		}
		voucherRelay = reservation.Voucher.Relay.String()
		voucherPeer = reservation.Voucher.Peer.String()
		voucherExpiration = reservation.Voucher.Expiration.Unix()
	}
	if err := writeJSON(opts.readyFile, map[string]any{
		"implementation":         "go",
		"role":                   "destination",
		"peer_id":                h.ID().String(),
		"listen_addrs":           listenAddrs,
		"relay_addrs":            relayAddrs,
		"relay_peer_id":          relayInfo.ID.String(),
		"native_relay_transport": true,
		"voucher":                reservation.Voucher != nil,
		"voucher_payload_bytes":  voucherPayloadBytes,
		"voucher_relay":          voucherRelay,
		"voucher_peer":           voucherPeer,
		"voucher_expiration":     voucherExpiration,
		"status":                 "ready",
	}); err != nil {
		return err
	}
	for {
		if _, err := os.Stat(opts.stopFile); err == nil {
			return nil
		}
		time.Sleep(100 * time.Millisecond)
	}
}

type protocolExchange struct {
	bytes      int
	connection network.Conn
	streamID   string
	protocol   protocol.ID
	target     upgradeTarget
}

func identifyOnExchangeConnection(identified event.EvtPeerIdentificationCompleted, exchange protocolExchange, expected peer.ID) bool {
	return identified.Peer == expected && identified.Conn != nil && exchange.connection != nil &&
		identified.Conn.ID() == exchange.connection.ID() && identified.Conn.RemotePeer() == expected &&
		exchange.connection.RemotePeer() == expected
}

func recordAutomaticIdentify(ctx context.Context, result map[string]any, events <-chan interface{}, exchange protocolExchange, expected peer.ID) error {
	for {
		select {
		case received, open := <-events:
			if !open {
				return fmt.Errorf("Identify completion subscription closed")
			}
			identified, ok := received.(event.EvtPeerIdentificationCompleted)
			if !ok || !identifyOnExchangeConnection(identified, exchange, expected) {
				continue
			}
			// Preserve the donor event; capability-specific validators require signed evidence.
			result["signed_peer_record"] = identified.SignedPeerRecord != nil
			result["identify_event_connection_id"] = identified.Conn.ID()
			result["identify_event_basis"] = "automatic_identify_separate_exchange_same_connection"
			return nil
		case <-ctx.Done():
			return fmt.Errorf("Identify completion event timed out: %w", ctx.Err())
		}
	}
}

func openRequiredProtocol(ctx context.Context, h host.Host, peer peer.ID, id protocol.ID) (protocolExchange, error) {
	ctx, binding := bindUpgradeStream(ctx)
	stream, err := h.NewStream(ctx, peer, id)
	if err != nil {
		return protocolExchange{}, err
	}
	defer stream.Close()
	binding.attach(stream)
	result := protocolExchange{connection: stream.Conn(), streamID: stream.ID(), protocol: stream.Protocol(), target: binding.target()}
	if id == protocol.ID("/ipfs/id/1.0.0") {
		payload, err := io.ReadAll(io.LimitReader(stream, 4097))
		if err != nil {
			return result, err
		}
		if len(payload) == 0 || len(payload) > 4096 {
			_ = stream.Reset()
			return result, fmt.Errorf("%s payload outside 1..4096 byte bound", id)
		}
		result.bytes = len(payload)
		binding.complete(stream)
	}
	return result, nil
}

func openEchoProtocol(ctx context.Context, h host.Host, peer peer.ID, payload []byte) (protocolExchange, error) {
	ctx, binding := bindUpgradeStream(ctx)
	stream, err := h.NewStream(ctx, peer, echoProtocol)
	if err != nil {
		return protocolExchange{}, err
	}
	defer stream.Close()
	binding.attach(stream)
	result := protocolExchange{connection: stream.Conn(), streamID: stream.ID(), protocol: stream.Protocol(), target: binding.target()}
	if err := writeFrame(stream, payload); err != nil {
		_ = stream.Reset()
		return result, err
	}
	echoed, err := readFrame(bufio.NewReader(stream))
	if err != nil {
		_ = stream.Reset()
		return result, err
	}
	if string(echoed) != string(payload) {
		return result, fmt.Errorf("echo mismatch: %q", string(echoed))
	}
	result.bytes = len(echoed)
	binding.complete(stream)
	return result, nil
}

func recordProtocolExchange(result map[string]any, exchange protocolExchange) {
	c := exchange.connection
	state := c.ConnState()
	result["application_protocol"] = string(exchange.protocol)
	result["application_connection_id"] = c.ID()
	result["application_stream_id"] = exchange.streamID
	result["authenticated_remote_peer_id"] = c.RemotePeer().String()
	result["connection_local_addr"] = c.LocalMultiaddr().String()
	result["connection_remote_addr"] = c.RemoteMultiaddr().String()
	// Actual connection state corroborates the trace; it never generates phase history.
	result["negotiated_transport"] = canonicalNegotiatedTransport(state.Transport)
	result["negotiated_security"] = string(state.Security)
	result["negotiated_muxer"] = string(state.StreamMultiplexer)
	var muxed *observedMuxedConn
	if c.As(&muxed) {
		result["application_connection_trace_id"] = muxed.trace.evidence.ID
		result["application_stream_trace_id"] = exchange.target.stream
	}
}

func canonicalNegotiatedTransport(transport string) string {
	switch transport {
	case "quic-v1", "/quic-v1":
		return "/quic-v1"
	default:
		return transport
	}
}

func connectionState(h host.Host, peer peer.ID) map[string]string {
	for _, conn := range h.Network().ConnsToPeer(peer) {
		state := conn.ConnState()
		return map[string]string{
			"negotiated_security":          string(state.Security),
			"negotiated_muxer":             string(state.StreamMultiplexer),
			"negotiated_transport":         canonicalNegotiatedTransport(state.Transport),
			"authenticated_remote_peer_id": conn.RemotePeer().String(),
		}
	}
	return map[string]string{}
}

func expectUnsupportedProtocol(ctx context.Context, h host.Host, peer peer.ID, id protocol.ID) (string, error) {
	stream, err := h.NewStream(ctx, peer, id)
	if err == nil {
		stream.Close()
		return "", fmt.Errorf("%s unexpectedly opened", id)
	}
	text := err.Error()
	if strings.Contains(text, "deadline exceeded") || strings.Contains(text, "context canceled") {
		return "", fmt.Errorf("%s failed without protocol rejection proof: %w", id, err)
	}
	return text, nil
}

func dial(opts options) (err error) {
	h, err := newHost(opts.transport, opts.pnetKeyFile, opts.dnsServer)
	if err != nil {
		return err
	}
	var result map[string]any
	var proofTarget upgradeTarget
	defer func() {
		if closeErr := h.Close(); closeErr != nil {
			if err != nil {
				err = fmt.Errorf("%w; dialer cleanup failed: %v", err, closeErr)
			} else {
				err = fmt.Errorf("dialer cleanup failed: %w", closeErr)
			}
		}
		if h.upgrades != nil && upgradeProofProtocol(opts.scenario) != "" {
			proof, proofErr := h.upgrades.finish(upgradeProofProtocol(opts.scenario), opts.peerID, network.DirOutbound, proofTarget)
			err = errors.Join(err, proofErr)
			if result == nil {
				result = map[string]any{"implementation": "go", "role": "dialer", "scenario": opts.scenario,
					"local_peer_id": h.ID().String(), "status": "ok"}
			}
			result["upgrade_observation"] = proof
			if err != nil {
				result["status"], result["error"] = "error", err.Error()
			}
			err = errors.Join(err, writeJSON(opts.resultFile, result))
		}
	}()
	var identifyEvents event.Subscription
	if opts.scenario == "identify" || opts.scenario == "pnet" || opts.scenario == "echo" || opts.scenario == "echo_large" {
		identifyEvents, err = h.EventBus().Subscribe(new(event.EvtPeerIdentificationCompleted), eventbus.BufSize(4))
		if err != nil {
			return fmt.Errorf("subscribe to Identify completion: %w", err)
		}
		defer identifyEvents.Close()
	}

	addr, err := ma.NewMultiaddr(opts.addr)
	if err != nil {
		return err
	}
	expectedPeer, err := peer.Decode(opts.peerID)
	if err != nil {
		return err
	}
	_, dnsErr := addr.ValueForProtocol(ma.P_DNSADDR)
	dnsRoot := dnsErr == nil
	if dnsRoot && opts.dnsServer == "" {
		return fmt.Errorf("DNSADDR fixture dial requires --dns-server")
	}
	info, err := peer.AddrInfoFromP2pAddr(addr)
	if err != nil {
		if !dnsRoot {
			return err
		}
		info = &peer.AddrInfo{ID: expectedPeer, Addrs: []ma.Multiaddr{addr}}
	}
	if info.ID != expectedPeer {
		return fmt.Errorf("address peer disagrees with --peer-id")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()

	if err := h.Connect(ctx, *info); err != nil {
		if opts.scenario == "pnet" && opts.pnetControl != "" {
			return writeJSON(opts.resultFile, pnetRejection(opts, "dialer", info.ID.String(), h.pnet))
		}
		return fmt.Errorf("connect failed: %w", err)
	}
	if opts.scenario == "pnet" && opts.pnetControl != "" {
		return fmt.Errorf("pnet rejection control unexpectedly established an authenticated session")
	}
	if h.kad != nil {
		addDHTPeer(h, info)
	}

	result = map[string]any{
		"implementation": "go",
		"role":           "dialer",
		"scenario":       opts.scenario,
		"local_peer_id":  h.ID().String(),
		"status":         "ok",
	}
	for key, value := range connectionState(h, info.ID) {
		result[key] = value
	}
	if dnsRoot {
		connections := h.Network().ConnsToPeer(expectedPeer)
		if len(connections) == 0 || connections[0].RemotePeer() != expectedPeer {
			return fmt.Errorf("DNSADDR dial has no authenticated expected-peer connection")
		}
		result["dns_input_address"] = opts.addr
		result["dns_resolver_configured"] = opts.dnsServer != ""
		result["expected_peer_id"] = expectedPeer.String()
		result["authenticated_remote_peer_id"] = connections[0].RemotePeer().String()
		result["dns_server"] = opts.dnsServer
		result["connection_remote_addr"] = connections[0].RemoteMultiaddr().String()
	}
	switch opts.scenario {
	case "ping":
		pingService := ping.NewPingService(h)
		ch := pingService.Ping(ctx, info.ID)
		select {
		case pong := <-ch:
			if pong.Error != nil {
				return fmt.Errorf("ping failed: %w", pong.Error)
			}
			result["rtt_ms"] = pong.RTT.Milliseconds()
		case <-ctx.Done():
			return fmt.Errorf("ping timed out: %w", ctx.Err())
		}
	case "identify":
		exchange, err := openRequiredProtocol(ctx, h, info.ID, protocol.ID("/ipfs/id/1.0.0"))
		if err != nil {
			return err
		}
		if err := recordAutomaticIdentify(ctx, result, identifyEvents.Out(), exchange, info.ID); err != nil {
			return err
		}
		result["payload_bytes"] = exchange.bytes
		proofTarget = exchange.target
		recordProtocolExchange(result, exchange)
	case "pnet":
		identified := false
		for !identified {
			select {
			case received := <-identifyEvents.Out():
				event, ok := received.(event.EvtPeerIdentificationCompleted)
				if ok && event.Peer == info.ID {
					h.pnet.identifyStreams.Add(1)
					result["signed_peer_record"] = event.SignedPeerRecord != nil
					result["identify_observed"] = true
					identified = true
				}
			case <-ctx.Done():
				return fmt.Errorf("pnet Identify completion event timed out: %w", ctx.Err())
			}
		}
		exchange, err := openEchoProtocol(ctx, h, info.ID, []byte(opts.payload))
		if err != nil {
			return err
		}
		h.pnet.applicationStreams.Add(1)
		result["protocol"] = string(echoProtocol)
		result["payload_bytes"] = exchange.bytes
		proofTarget = exchange.target
		recordProtocolExchange(result, exchange)
		result["echo_ok"] = true
		for key, value := range pnetEvidence(opts) {
			result[key] = value
		}
	case "echo", "echo_large":
		payload := []byte(opts.payload)
		if opts.scenario == "echo_large" {
			payload = make([]byte, 192*1024)
			for index := range payload {
				payload[index] = byte(index % 251)
			}
		}
		exchange, err := openEchoProtocol(ctx, h, info.ID, payload)
		if err != nil {
			return err
		}
		if err := recordAutomaticIdentify(ctx, result, identifyEvents.Out(), exchange, info.ID); err != nil {
			return err
		}
		result["protocol"] = string(echoProtocol)
		result["payload_bytes"] = exchange.bytes
		proofTarget = exchange.target
		recordProtocolExchange(result, exchange)
		result["echo_ok"] = true
	case "relay_reserve":
		reservation, err := relayclient.Reserve(ctx, h, *info)
		if err != nil {
			return err
		}
		addrs := make([]string, 0, len(reservation.Addrs))
		for _, addr := range reservation.Addrs {
			addrs = append(addrs, addr.String())
		}
		result["reservation_addrs"] = addrs
		result["limit_duration_ms"] = reservation.LimitDuration.Milliseconds()
		result["limit_data"] = reservation.LimitData
		result["voucher"] = reservation.Voucher != nil
	case "dcutr":
		if _, err = openRequiredProtocol(ctx, h, info.ID, protocol.ID("/libp2p/dcutr")); err != nil {
			return err
		}
		result["opened"] = true
	case "dht_find_peer":
		found, err := h.kad.FindPeer(ctx, info.ID)
		if err != nil {
			return fmt.Errorf("dht FindPeer failed: %w", err)
		}
		if found.ID != info.ID {
			return fmt.Errorf("dht FindPeer returned %s, expected %s", found.ID, info.ID)
		}
		result["found_peer"] = found.ID.String()
		result["addr_count"] = len(found.Addrs)
	case "dht_hidden_find_peer":
		target, err := peer.Decode(opts.targetPeerID)
		if err != nil {
			return fmt.Errorf("parse hidden target peer: %w", err)
		}
		if target == info.ID {
			return fmt.Errorf("hidden target must differ from the known routing peer")
		}
		preexisting := len(h.Peerstore().Addrs(target)) != 0 || len(h.Network().ConnsToPeer(target)) != 0
		if preexisting {
			return fmt.Errorf("hidden target %s was present before FindPeer", target)
		}
		var found peer.AddrInfo
		queries, err := dhtQueryEvidence(ctx, func(queryCtx context.Context) error {
			var queryErr error
			found, queryErr = h.kad.FindPeer(queryCtx, target)
			return queryErr
		})
		if err != nil {
			return fmt.Errorf("DHT hidden FindPeer failed: %w", err)
		}
		if queries == 0 {
			return fmt.Errorf("DHT hidden FindPeer did not issue /ipfs/kad/1.0.0 query")
		}
		if found.ID != target || len(found.Addrs) == 0 {
			return fmt.Errorf("DHT hidden FindPeer returned %s with %d addresses, expected %s", found.ID, len(found.Addrs), target)
		}
		result["preexisting_target"] = false
		result["found_peer"] = found.ID.String()
		result["addr_count"] = len(found.Addrs)
		result["dht_queries_delta"] = queries
		result["negotiated_protocol"] = "/ipfs/kad/1.0.0"
	case "dht_provide_find_provider":
		proof, err := independentProviderEvidence(ctx, h, info, opts.transport, opts.pnetKeyFile, opts.dnsServer)
		if err != nil {
			return err
		}
		result["network_proof"] = proof
	case "dht_pk_put_get", "dht_ipns_put_get":
		key, expected, err := dhtValueFixture(opts.scenario)
		if err != nil {
			return err
		}
		if opts.payload != "get_only" {
			if err := h.kad.PutValue(ctx, string(key), expected); err != nil {
				return fmt.Errorf("DHT PutValue failed: %w", err)
			}
		}
		if opts.payload == "put_only" {
			result["operation"] = "put_only"
			result["value_bytes"] = len(expected)
			break
		}
		selected, err := h.kad.GetValue(ctx, string(key))
		if err != nil {
			return fmt.Errorf("DHT GetValue failed: %w", err)
		}
		if !bytes.Equal(selected, expected) {
			return fmt.Errorf("DHT GetValue returned a different value")
		}
		if opts.payload == "get_only" {
			result["operation"] = "get_only"
			result["remote_get"] = true
		} else {
			result["operation"] = "put_get"
			result["remote_get"] = false
		}
		result["value_bytes"] = len(selected)
	case "gossipsub_publish", "gossipsub_mixed_mesh_stress":
		if _, err := h.pubsub.Subscribe(pubsubTopic, pubsub.WithBufferSize(8)); err != nil {
			return err
		}
		waitCtx, waitCancel := context.WithTimeout(context.Background(), 8*time.Second)
		defer waitCancel()
		meshPeer := waitPubSubPeer(waitCtx, h.pubsub, pubsubTopic, info.ID)
		if meshPeer {
			time.Sleep(500 * time.Millisecond)
		}
		publishAttempts := 1
		if opts.scenario == "gossipsub_publish" {
			publishAttempts = 4
		}
		for attempt := 0; attempt < publishAttempts; attempt++ {
			if err := h.pubsub.Publish(pubsubTopic, []byte(opts.payload)); err != nil {
				return fmt.Errorf("gossipsub publish failed: %w", err)
			}
			time.Sleep(500 * time.Millisecond)
		}
		time.Sleep(1 * time.Second)
		result["topic"] = pubsubTopic
		result["payload"] = opts.payload
		result["payload_bytes"] = len(opts.payload)
		result["mesh_peer"] = meshPeer
		result["publish_attempts"] = publishAttempts
	case "unknown_protocol":
		expected, err := expectUnsupportedProtocol(ctx, h, info.ID, protocol.ID("/forge/interop/unknown/1"))
		if err != nil {
			return err
		}
		result["expected_error"] = expected
	default:
		return fmt.Errorf("unknown scenario %s", opts.scenario)
	}
	if h.upgrades != nil && upgradeProofProtocol(opts.scenario) != "" {
		return nil
	}
	return writeJSON(opts.resultFile, result)
}

func relayedAddr(relayAddr string, target peer.ID) (ma.Multiaddr, error) {
	base, err := ma.NewMultiaddr(relayAddr)
	if err != nil {
		return nil, err
	}
	circuit, err := ma.NewMultiaddr("/p2p-circuit/p2p/" + target.String())
	if err != nil {
		return nil, err
	}
	return base.Encapsulate(circuit), nil
}

func waitDirectConnection(ctx context.Context, h host.Host, target peer.ID) bool {
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	for {
		for _, conn := range h.Network().ConnsToPeer(target) {
			if !strings.Contains(conn.RemoteMultiaddr().String(), "p2p-circuit") {
				return true
			}
		}
		select {
		case <-ctx.Done():
			return false
		case <-ticker.C:
		}
	}
}

func dialRelay(opts options) error {
	h, err := newHost(opts.transport, opts.pnetKeyFile, opts.dnsServer)
	if err != nil {
		return err
	}
	defer h.Close()

	targetPeer, err := peer.Decode(opts.peerID)
	if err != nil {
		return err
	}
	relayPeer, err := peer.Decode(opts.relayPeerID)
	if err != nil {
		return err
	}
	relayAddr, err := ma.NewMultiaddr(opts.relayAddr)
	if err != nil {
		return err
	}
	relayInfo, err := peer.AddrInfoFromP2pAddr(relayAddr)
	if err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	if err := h.Connect(ctx, *relayInfo); err != nil {
		return fmt.Errorf("connect relay failed: %w", err)
	}
	destinationAddr, err := relayedAddr(opts.relayAddr, targetPeer)
	if err != nil {
		return err
	}
	targetInfo, err := peer.AddrInfoFromP2pAddr(destinationAddr)
	if err != nil {
		return err
	}
	if err := h.Connect(ctx, *targetInfo); err != nil {
		return fmt.Errorf("connect relayed destination failed: %w", err)
	}

	result := map[string]any{
		"implementation": "go",
		"role":           "relay_dialer",
		"scenario":       opts.scenario,
		"local_peer_id":  h.ID().String(),
		"relay_peer":     relayPeer.String(),
		"target_peer":    targetPeer.String(),
		"relayed_addr":   destinationAddr.String(),
		"status":         "ok",
	}
	stream, err := h.NewStream(network.WithAllowLimitedConn(ctx, "relay-echo"), targetPeer, echoProtocol)
	if err != nil {
		return fmt.Errorf("open relayed echo failed: %w", err)
	}
	payload := []byte("relay-echo")
	if err := writeFrame(stream, payload); err != nil {
		_ = stream.Reset()
		return err
	}
	echoed, err := readFrame(bufio.NewReader(stream))
	if err != nil {
		_ = stream.Reset()
		return err
	}
	_ = stream.Close()
	if string(echoed) != string(payload) {
		return fmt.Errorf("relay echo mismatch: %q", string(echoed))
	}
	result["relay_echo"] = true
	if opts.scenario == "dcutr_relay_topology" {
		if h.holePunch == nil {
			return fmt.Errorf("hole punch service is unavailable")
		}
		if err := h.holePunch.DirectConnect(targetPeer); err != nil {
			return fmt.Errorf("DCUtR direct connect failed: %w", err)
		}
		result["direct_upgrade"] = waitDirectConnection(ctx, h, targetPeer)
		if !result["direct_upgrade"].(bool) {
			return fmt.Errorf("DCUtR did not produce a direct connection")
		}
	}
	return writeJSON(opts.resultFile, result)
}

func main() {
	opts, err := parseArgs()
	if err == nil {
		switch opts.command {
		case "listen":
			if isAutoNATScenario(opts.scenario) {
				err = runAutoNAT(opts)
			} else {
				err = listen(opts)
			}
		case "destination":
			err = destination(opts)
		case "dial":
			if isAutoNATScenario(opts.scenario) {
				err = runAutoNAT(opts)
			} else {
				err = dial(opts)
			}
		case "dial-relay":
			err = dialRelay(opts)
		default:
			err = fmt.Errorf("unknown command %s", opts.command)
		}
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
}
