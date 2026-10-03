package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/binary"
	"fmt"
	"net"
	"strings"
	"testing"
	"time"

	libp2p "github.com/libp2p/go-libp2p"
	"github.com/libp2p/go-libp2p/core/crypto"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/core/protocol"
	"github.com/libp2p/go-libp2p/core/sec"
	tptu "github.com/libp2p/go-libp2p/p2p/net/upgrader"
	relayclient "github.com/libp2p/go-libp2p/p2p/protocol/circuitv2/client"
	relaypb "github.com/libp2p/go-libp2p/p2p/protocol/circuitv2/pb"
	"github.com/libp2p/go-libp2p/p2p/security/noise"
	sectls "github.com/libp2p/go-libp2p/p2p/security/tls"
	"google.golang.org/protobuf/proto"
)

func TestAutoRelayCapturedNativeHopFrame(t *testing.T) {
	message := &relaypb.HopMessage{Type: relaypb.HopMessage_STATUS.Enum(), Status: relaypb.Status_OK.Enum(),
		Reservation: &relaypb.Reservation{Expire: proto.Uint64(1700000008)}}
	payload, err := proto.Marshal(message)
	if err != nil {
		t.Fatal(err)
	}
	frame := binary.AppendUvarint(nil, uint64(len(payload)))
	frame = append(frame, payload...)
	decoded, err := autoRelayHopMessage(frame)
	if err != nil || decoded.GetStatus() != relaypb.Status_OK || decoded.GetReservation().GetExpire() != 1700000008 {
		t.Fatalf("native frame lost status/expiry: %v %v", decoded, err)
	}
	for _, bad := range [][]byte{nil, frame[:len(frame)-1], append(append([]byte{}, frame...), 0), {0x80},
		append([]byte{byte(len(payload)) | 0x80, 0}, payload...), binary.AppendUvarint(nil, 4097)} {
		if _, err := autoRelayHopMessage(bad); err == nil {
			t.Fatalf("accepted malformed/noncanonical/truncated capture %x", bad)
		}
	}
}

func TestAutoRelayCaptureSnapshotIsBoundedAndIndependent(t *testing.T) {
	stream := &autoRelayObservedStream{}
	stream.input.Write([]byte{1, 2, 3})
	stream.output.Write([]byte{4, 5})
	input, output, overflow := stream.capture()
	if overflow || !stream.finished || !bytes.Equal(input, []byte{1, 2, 3}) || !bytes.Equal(output, []byte{4, 5}) {
		t.Fatal("capture did not close/snapshot delegated observations")
	}
	stream.input.Bytes()[0] = 9
	if input[0] != 1 {
		t.Fatal("native input snapshot aliases mutable capture")
	}
	trace := &autoRelayTrace{}
	for index := 0; index < 129; index++ {
		trace.record(map[string]any{"kind": "measured"})
	}
	events, overflow := trace.snapshot()
	if len(events) != 128 || !overflow {
		t.Fatal("trace overflow was not fail-closed")
	}
}

func TestAutoRelayShutdownClosesHandlerAdmissionBeforeJoining(t *testing.T) {
	trace := &autoRelayTrace{}
	if !trace.admit() {
		t.Fatal("native observer handler admission failed before shutdown")
	}
	trace.closeAdmission()
	if trace.admit() {
		t.Fatal("native observer admitted work after shutdown")
	}
	trace.handlers.Done()
	trace.handlers.Wait()
}

func TestAutoRelayTCPTLSSecurityIsOptInAndTLSFirst(t *testing.T) {
	for _, tc := range []struct {
		name string
		auto *autoRelayHostConfig
		ids  []protocol.ID
	}{
		{"legacy", nil, []protocol.ID{sectls.ID}},
		{"client_or_observer", &autoRelayHostConfig{}, []protocol.ID{sectls.ID, noise.ID}},
		{"relay_service", &autoRelayHostConfig{service: true}, []protocol.ID{sectls.ID, noise.ID}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			config := libp2p.Config{}
			for _, option := range autoRelayTLSSecurityOptions(tc.auto) {
				if err := option(&config); err != nil {
					t.Fatal(err)
				}
			}
			if len(config.SecurityTransports) != len(tc.ids) || config.Insecure {
				t.Fatalf("wrong native security composition: %+v", config.SecurityTransports)
			}
			for index, id := range tc.ids {
				if config.SecurityTransports[index].ID != id {
					t.Fatalf("native security preference %d = %s, want %s", index, config.SecurityTransports[index].ID, id)
				}
			}
		})
	}
}

type autoRelayTestNoiseTransport struct {
	sec.SecureTransport
	completed chan<- autoRelayTestNoiseHandshake
}

type autoRelayTestNoiseHandshake struct {
	localPeer  peer.ID
	remotePeer peer.ID
	relay      string
	remote     string
	security   protocol.ID
}

func (s *autoRelayTestNoiseTransport) SecureOutbound(ctx context.Context, raw net.Conn, remote peer.ID) (sec.SecureConn, error) {
	conn, err := s.SecureTransport.SecureOutbound(ctx, raw, remote)
	if err != nil {
		return conn, err
	}
	handshake := autoRelayTestNoiseHandshake{localPeer: conn.LocalPeer(), remotePeer: conn.RemotePeer(), security: s.ID()}
	if address, ok := raw.RemoteAddr().(*relayclient.NetAddr); ok {
		handshake.relay, handshake.remote = address.Relay, address.Remote
	}
	select {
	case s.completed <- handshake:
		return conn, nil
	default:
		conn.Close()
		return nil, fmt.Errorf("unexpected extra native Noise handshake")
	}
}

func TestAutoRelayTCPTLSOffersCircuitNoise(t *testing.T) {
	relay, err := newFixtureHost("tcp-tls", "", "", &autoRelayHostConfig{
		service: true, ttl: 8 * time.Second, trace: &autoRelayTrace{},
	})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := relay.Close(); err != nil {
			t.Error(err)
		}
	})
	completed := make(chan autoRelayTestNoiseHandshake, 1)
	source, err := newFixtureHost("tcp-tls", "", "", &autoRelayHostConfig{}, func(config *libp2p.Config) error {
		if len(config.SecurityTransports) != 2 || config.SecurityTransports[0].ID != sectls.ID ||
			config.SecurityTransports[1].ID != noise.ID {
			return fmt.Errorf("expected unmodified native TLS-first/Noise configuration")
		}
		delegate, ok := config.SecurityTransports[1].Constructor.(func(protocol.ID, crypto.PrivKey, []tptu.StreamMuxer) (sec.SecureTransport, error))
		if !ok {
			return fmt.Errorf("unexpected native Noise constructor")
		}
		config.SecurityTransports[1].Constructor = func(id protocol.ID, key crypto.PrivKey, muxers []tptu.StreamMuxer) (sec.SecureTransport, error) {
			transport, err := delegate(id, key, muxers)
			if err != nil {
				return nil, err
			}
			return &autoRelayTestNoiseTransport{SecureTransport: transport, completed: completed}, nil
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := source.Close(); err != nil {
			t.Error(err)
		}
	})
	// This mixed counterparty uses outer Noise too; it is not a PR9 TLS-role receipt.
	// No direct listener or subsequent dial can bypass the established circuit.
	target, err := newFixtureHost("tcp", "", "", &autoRelayHostConfig{}, libp2p.NoListenAddrs)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := target.Close(); err != nil {
			t.Error(err)
		}
	})
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	relayInfo := peer.AddrInfo{ID: relay.ID(), Addrs: relay.Addrs()}
	if err := target.Connect(ctx, relayInfo); err != nil {
		t.Fatal(err)
	}
	targetControls := target.Network().ConnsToPeer(relay.ID())
	if len(targetControls) != 1 || targetControls[0].ConnState().Security != protocol.ID(noise.ID) ||
		targetControls[0].ConnState().Transport != "tcp" || targetControls[0].Stat().Limited {
		t.Fatal("mixed Noise-only counterparty-to-relay outer TCP did not actually negotiate Noise")
	}
	t.Log("mixed regression counterparty-to-relay outer TCP uses Noise, not a TLS PR9 receipt")
	if _, err := relayclient.Reserve(ctx, target, relayInfo); err != nil {
		t.Fatal(err)
	}
	if err := source.Connect(ctx, relayInfo); err != nil {
		t.Fatal(err)
	}
	controls := source.Network().ConnsToPeer(relay.ID())
	if len(controls) != 1 || controls[0].ConnState().Security != protocol.ID(sectls.ID) ||
		controls[0].ConnState().Transport != "tcp" || controls[0].Stat().Limited {
		t.Fatal("outer source-to-relay TCP did not actually negotiate TLS")
	}
	address, err := relayedAddr(relay.Addrs()[0].String()+"/p2p/"+relay.ID().String(), target.ID())
	if err != nil {
		t.Fatal(err)
	}
	info, err := peer.AddrInfoFromP2pAddr(address)
	if err != nil {
		t.Fatal(err)
	}
	circuitCtx := network.WithAllowLimitedConn(ctx, "autorelay-tls-noise-regression")
	if err := source.Connect(circuitCtx, *info); err != nil {
		t.Fatal(err)
	}
	stream, err := source.NewStream(network.WithNoDial(circuitCtx, "use established circuit"), target.ID(), echoProtocol)
	if err != nil {
		t.Fatal(err)
	}
	defer stream.Close()
	deadline, _ := ctx.Deadline()
	if err := stream.SetDeadline(deadline); err != nil {
		t.Fatal(err)
	}
	if stream.Conn().RemotePeer() != target.ID() {
		t.Fatalf("circuit authenticated peer = %s, want %s", stream.Conn().RemotePeer(), target.ID())
	}
	if stream.Conn().ConnState().Transport != "p2p-circuit" || !stream.Conn().Stat().Limited ||
		!strings.Contains(stream.Conn().RemoteMultiaddr().String(), "/p2p/"+relay.ID().String()+"/p2p-circuit") {
		t.Fatalf("echo bypassed circuit: state=%+v stats=%+v addr=%s", stream.Conn().ConnState(), stream.Conn().Stat(), stream.Conn().RemoteMultiaddr())
	}
	// The pinned circuit ConnState omits Security; observe the successful native delegate instead.
	select {
	case handshake := <-completed:
		if handshake.security != noise.ID || handshake.localPeer != source.ID() || handshake.remotePeer != target.ID() ||
			handshake.relay != relay.ID().String() || handshake.remote != target.ID().String() {
			t.Fatalf("native Noise handshake does not bind source/target/circuit: %+v", handshake)
		}
	default:
		t.Fatal("no successful native Noise circuit handshake was observed")
	}
	payload := []byte("tls-outer-noise-inner")
	if err := writeFrame(stream, payload); err != nil {
		t.Fatal(err)
	}
	echoed, err := readFrame(bufio.NewReader(stream))
	if err != nil || !bytes.Equal(echoed, payload) {
		t.Fatalf("native relay echo mismatch: %x %v", echoed, err)
	}
	if controls[0].IsClosed() || controls[0].ConnState().Security != protocol.ID(sectls.ID) {
		t.Fatal("outer control was downgraded during circuit echo")
	}
}
