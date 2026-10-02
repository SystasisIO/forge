package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/binary"
	"strings"
	"testing"
	"time"

	libp2p "github.com/libp2p/go-libp2p"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/core/protocol"
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

func TestAutoRelayTCPTLSNativeOuterTLSAndInnerNoiseEcho(t *testing.T) {
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
	source, err := newFixtureHost("tcp-tls", "", "", &autoRelayHostConfig{})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := source.Close(); err != nil {
			t.Error(err)
		}
	})
	// A native Noise-only peer models the circuit endpoint, not a PR9 TLS-role receipt.
	target, err := newFixtureHost("tcp", "", "", &autoRelayHostConfig{})
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
	if _, err := relayclient.Reserve(ctx, target, relayInfo); err != nil {
		t.Fatal(err)
	}
	if err := source.Connect(ctx, relayInfo); err != nil {
		t.Fatal(err)
	}
	controls := source.Network().ConnsToPeer(relay.ID())
	if len(controls) != 1 || controls[0].ConnState().Security != protocol.ID(sectls.ID) {
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
	if err := source.Connect(ctx, *info); err != nil {
		t.Fatal(err)
	}
	stream, err := source.NewStream(network.WithAllowLimitedConn(ctx, "autorelay-tls-noise-regression"), target.ID(), echoProtocol)
	if err != nil {
		t.Fatal(err)
	}
	defer stream.Close()
	deadline, _ := ctx.Deadline()
	if err := stream.SetDeadline(deadline); err != nil {
		t.Fatal(err)
	}
	if stream.Conn().RemotePeer() != target.ID() || stream.Conn().ConnState().Security != protocol.ID(noise.ID) ||
		!strings.Contains(stream.Conn().RemoteMultiaddr().String(), "/p2p-circuit") {
		t.Fatal("inner connection lacks actual authenticated Noise circuit evidence")
	}
	payload := []byte("tls-outer-noise-inner")
	if err := writeFrame(stream, payload); err != nil {
		t.Fatal(err)
	}
	echoed, err := readFrame(bufio.NewReader(stream))
	if err != nil || !bytes.Equal(echoed, payload) {
		t.Fatalf("native relay echo mismatch: %x %v", echoed, err)
	}
	if controls[0].ConnState().Security != protocol.ID(sectls.ID) {
		t.Fatal("outer control was downgraded during circuit echo")
	}
}
