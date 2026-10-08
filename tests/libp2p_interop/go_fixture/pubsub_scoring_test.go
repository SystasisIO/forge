package main

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"io"
	"math"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"

	libp2p "github.com/libp2p/go-libp2p"
	pubsub "github.com/libp2p/go-libp2p-pubsub"
	pubsubpb "github.com/libp2p/go-libp2p-pubsub/pb"
	"github.com/libp2p/go-libp2p/core/crypto"
	"github.com/libp2p/go-libp2p/core/host"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/core/protocol"
	"github.com/libp2p/go-libp2p/p2p/muxer/yamux"
	"github.com/libp2p/go-libp2p/p2p/security/noise"
	nativeyamux "github.com/libp2p/go-yamux/v5"
	ma "github.com/multiformats/go-multiaddr"
)

func pubsubScoringUnitArgs() []string {
	return []string{"--version", "1.1", "--transport", "tcp", "--actor", "victim", "--case-token", strings.Repeat("a", 32),
		"--ready-file", "ready", "--control-file", "control", "--result-file", "result", "--stop-file", "stop", "--store-dir", "store"}
}

func TestPubsubScoringPrepareAdmissionAndStickyFailure(t *testing.T) {
	key, _, err := crypto.GenerateEd25519Key(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	local, err := peer.IDFromPrivateKey(key)
	if err != nil {
		t.Fatal(err)
	}
	token := strings.Repeat("a", 32)
	command := pubsubScoringCommand{Sequence: 1, Kind: "prepare_shutdown", Actor: "victim", Token: token, Local: local.String()}
	encoded, err := json.Marshal(command)
	if err != nil {
		t.Fatal(err)
	}
	if _, err = decodePubsubScoringCommand(encoded, 1); err != nil {
		t.Fatal(err)
	}
	for _, state := range []string{"active_error", "overflow", "pending", "foreign_actor", "foreign_token", "foreign_identity"} {
		t.Run(state, func(t *testing.T) {
			o := newPubsubScoringObserver("victim", token)
			o.local = local
			input, pending := command, 0
			failure := errors.New("active native failure")
			switch state {
			case "active_error":
				o.fail(failure)
			case "overflow":
				o.overflow = true
			case "pending":
				pending = 1
			case "foreign_actor":
				input.Actor = "sink"
			case "foreign_token":
				input.Token = strings.Repeat("b", 32)
			case "foreign_identity":
				input.Local = "foreign"
			}
			if o.prepareShutdown(input, pending) == nil || o.prepared || len(o.events) != 0 {
				t.Fatal("failed preparation emitted an acknowledgement")
			}
			if state == "active_error" && !errors.Is(o.failure, failure) {
				t.Fatal("late prepare cleared active failure")
			}
		})
	}
	o := newPubsubScoringObserver("victim", token)
	o.local = local
	if err = o.prepareShutdown(command, 0); err != nil {
		t.Fatal(err)
	}
	if err = o.admitCommand(); err == nil {
		t.Fatal("command admission remained open")
	}
	if err = o.prepareShutdown(command, 0); err == nil || len(o.events) != 1 {
		t.Fatal("duplicate prepare acknowledgement")
	}
	ack := o.events[0]
	if ack["source"] != "go.fixture.prepare_shutdown" || ack["local_peer_id"] != local.String() ||
		ack["case_token"] != token || ack["command_sequence"] != 1 || ack["pending_commands"] != 0 || ack["admission_closed"] != true {
		t.Fatal("prepare acknowledgement lacks exact identity/control binding")
	}
	o.fail(errors.New("real teardown error"))
	if o.result(true, true, nil, 0)["error"] == nil {
		t.Fatal("prepared state masked teardown error")
	}
}

func TestPubsubScoringStrictFlags(t *testing.T) {
	if _, err := parsePubsubScoringArgs(pubsubScoringUnitArgs()); err != nil {
		t.Fatal(err)
	}
	for name, value := range map[string]string{"--version": "1.2", "--transport": "tcp-tls", "--actor": "mesh", "--case-token": strings.Repeat("A", 32)} {
		args := pubsubScoringUnitArgs()
		for i := 0; i < len(args); i += 2 {
			if args[i] == name {
				args[i+1] = value
			}
		}
		if _, err := parsePubsubScoringArgs(args); err == nil {
			t.Fatalf("accepted %s=%s", name, value)
		}
	}
	for _, tail := range [][]string{{"--version", "1.0"}, {"--unknown", "x"}, {"--pnet-key-file", "key"}, {"--odd"}} {
		if _, err := parsePubsubScoringArgs(append(pubsubScoringUnitArgs(), tail...)); err == nil {
			t.Fatalf("accepted invalid tail %v", tail)
		}
	}
	args := pubsubScoringUnitArgs()
	args[13] = "ready"
	if _, err := parsePubsubScoringArgs(args); err == nil {
		t.Fatal("accepted overlapping ready/control paths")
	}
}

func TestPubsubScoringPrivateFlags(t *testing.T) {
	args := pubsubScoringUnitArgs()
	args[3] = "tcp-pnet-noise"
	if _, err := parsePubsubScoringArgs(args); err == nil {
		t.Fatal("accepted missing private credentials")
	}
	args = append(args, "--pnet-key-file", "key", "--pnet-fingerprint", strings.Repeat("b", 64))
	if _, err := parsePubsubScoringArgs(args); err != nil {
		t.Fatal(err)
	}
	args[len(args)-1] = strings.Repeat("B", 64)
	if _, err := parsePubsubScoringArgs(args); err == nil {
		t.Fatal("accepted noncanonical fingerprint")
	}
}

func TestPubsubScoringCommandsRejectAmbiguityAndBounds(t *testing.T) {
	for _, line := range []string{
		`{"sequence":1,"kind":"sample","label":"before","label":"after"}`,
		`{"sequence":1,"sequence":2,"kind":"sample","label":"before"}`,
		`{"sequence":2,"kind":"sample","label":"before"}`,
		`{"sequence":1,"kind":"sample","label":"before","payload":"hidden"}`,
		`{"sequence":1,"kind":"sample","label":"before","unknown":0}`,
		`{"sequence":1,"kind":"sample","label":null}`,
		`{"sequence":1,"kind":"sample","label":"periodic"}`,
		`{"sequence":1,"kind":"sample","label":"before"} {}`,
		`{"sequence":1,"kind":"graft","label":"forced"}`,
		`{"sequence":1,"kind":"publish","payload":""}`,
		`{"sequence":1,"kind":"publish","payload":"` + strings.Repeat("x", pubsubScoringPayload+1) + `"}`,
		`{"sequence":1,"kind":"connect","peer_id":"not-a-peer","address":"/ip4/127.0.0.1/tcp/1"}`,
	} {
		if _, err := decodePubsubScoringCommand([]byte(line), 1); err == nil {
			t.Fatalf("accepted invalid command %s", line)
		}
	}
	line := []byte(`{"sequence":1,"kind":"sample","label":"before"}`)
	if _, err := decodePubsubScoringCommand(line, 1); err != nil {
		t.Fatal(err)
	}
	if _, err := decodePubsubScoringCommand(bytes.Repeat([]byte{' '}, pubsubScoringLine+1), 1); err == nil {
		t.Fatal("accepted oversized line")
	}
	if _, err := decodePubsubScoringCommand(line, pubsubScoringCommands+1); err == nil {
		t.Fatal("accepted excessive commands")
	}
}

func TestPubsubScoringControlReadsOnlyCompleteAppendOnlyLines(t *testing.T) {
	path := filepath.Join(t.TempDir(), "control.jsonl")
	control := pubsubScoringControl{}
	if values, err := control.read(path); err != nil || len(values) != 0 {
		t.Fatal(values, err)
	}
	first := `{"sequence":1,"kind":"sample","label":"before"}`
	if err := os.WriteFile(path, []byte(first), 0o600); err != nil {
		t.Fatal(err)
	}
	if values, err := control.read(path); err != nil || len(values) != 0 {
		t.Fatal("read unfinished line", values, err)
	}
	if err := os.WriteFile(path, []byte(first+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if values, err := control.read(path); err != nil || len(values) != 1 || values[0].Sequence != 1 {
		t.Fatal(values, err)
	}
	if values, err := control.read(path); err != nil || len(values) != 0 {
		t.Fatal("replayed command", values, err)
	}
	if err := os.WriteFile(path, []byte(first+"\n"+first+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := control.read(path); err == nil {
		t.Fatal("accepted duplicate sequence")
	}
	if err := os.WriteFile(path, []byte("{}\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := control.read(path); err == nil {
		t.Fatal("accepted rewritten prefix")
	}
}

func TestPubsubScoringVictimPredicateOnly(t *testing.T) {
	token := strings.Repeat("a", 32)
	for _, actor := range []string{"victim", "offender", "replacement", "sink"} {
		for prefix, want := range map[string]pubsub.ValidationResult{"reject": pubsub.ValidationReject, "ignore": pubsub.ValidationIgnore, "accept": pubsub.ValidationAccept} {
			if actor != "victim" {
				want = pubsub.ValidationAccept
			}
			if result := pubsubScoringValidator(actor, token, []byte(prefix+":"+token+":one")); result != want {
				t.Fatalf("%s %s: %v", actor, prefix, result)
			}
		}
	}
	if result := pubsubScoringValidator("victim", token, []byte("reject:"+strings.Repeat("b", 32)+":one")); result != pubsub.ValidationAccept {
		t.Fatal("rejected foreign token")
	}
}

func TestPubsubScoringContractParametersAcceptedByNativeRouter(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	h, err := libp2p.New(libp2p.NoListenAddrs, libp2p.DisableRelay())
	if err != nil {
		t.Fatal(err)
	}
	defer h.Close()
	params, score, thresholds := pubsubScoringParameters("unit-topic")
	if params.D != 2 || params.Dlo != 1 || params.Dhi != 4 || params.Dscore != 1 || params.Dout != 0 ||
		params.HeartbeatInterval != 250*time.Millisecond || params.PruneBackoff != time.Second {
		t.Fatal("router profile drift")
	}
	if score.Topics["unit-topic"].InvalidMessageDeliveriesWeight != -100 || score.Topics["unit-topic"].InvalidMessageDeliveriesDecay != 0.99 ||
		score.AppSpecificWeight != 0 || score.IPColocationFactorWeight != 0 || score.RetainScore != 60*time.Second {
		t.Fatal("score profile drift")
	}
	if _, err := pubsub.NewGossipSub(ctx, h, pubsub.WithGossipSubParams(params), pubsub.WithPeerScore(score, thresholds),
		pubsub.WithFloodPublish(false), pubsub.WithPeerExchange(false),
		pubsub.WithMessageIdFn(pubsubScoringMessageID),
		pubsub.WithGossipSubProtocols([]protocol.ID{pubsub.GossipSubID_v10}, pubsub.GossipSubDefaultFeatures)); err != nil {
		t.Fatal(err)
	}
	cancel()
}

func pubsubScoringUnitFrame(t *testing.T, backoff *uint64) []byte {
	t.Helper()
	topic := "forge-pr11:" + strings.Repeat("a", 32)
	rpc := &pubsubpb.RPC{Control: &pubsubpb.ControlMessage{Prune: []*pubsubpb.ControlPrune{{TopicID: &topic, Backoff: backoff}}}}
	body, err := rpc.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	var header [binary.MaxVarintLen64]byte
	n := binary.PutUvarint(header[:], uint64(len(body)))
	return append(append([]byte{}, header[:n]...), body...)
}

func TestPubsubScoringWireCapturesFragmentedAndCoalescedNativePrefixes(t *testing.T) {
	raw := pubsubScoringUnitFrame(t, nil)
	decoder := pubsubScoringDecoder{}
	frames := [][]byte{}
	record := func(value []byte, rpc *pubsubpb.RPC) {
		frames = append(frames, value)
		if rpc.Control.Prune[0].Backoff != nil {
			t.Fatal("invented backoff presence")
		}
	}
	fail := func(err error) { t.Fatal(err) }
	for _, b := range raw {
		decoder.feed([]byte{b}, record, fail)
	}
	decoder.feed(append(append([]byte{}, raw...), raw...), record, fail)
	if len(frames) != 3 || len(decoder.buffer) != 0 {
		t.Fatal("incorrect frame count")
	}
	for _, frame := range frames {
		if !bytes.Equal(frame, raw) {
			t.Fatal("frame was rewritten")
		}
	}
}

func TestPubsubScoringWireRejectsNoncanonicalOversizedAndMalformedFrames(t *testing.T) {
	var header [binary.MaxVarintLen64]byte
	n := binary.PutUvarint(header[:], pubsubScoringFrame+1)
	for _, raw := range [][]byte{{0x81, 0x00, 0x00}, append([]byte{}, header[:n]...), {0x01, 0xff}} {
		decoder := pubsubScoringDecoder{}
		failures, frames := 0, 0
		decoder.feed(raw, func([]byte, *pubsubpb.RPC) { frames++ }, func(error) { failures++ })
		decoder.feed(pubsubScoringUnitFrame(t, nil), func([]byte, *pubsubpb.RPC) { frames++ }, func(error) { failures++ })
		if !decoder.failed || failures != 1 || frames != 0 {
			t.Fatal("malformed capture recovered/claimed a frame")
		}
	}
}

// Mock streams below exercise passive observer mechanics only. They are not
// network/crypto acceptance receipts and are never exported by a live actor.
type pubsubScoringUnitConn struct {
	network.Conn
	local, remote peer.ID
	key           crypto.PubKey
	id            string
	state         network.ConnectionState
}

func (c *pubsubScoringUnitConn) ID() string                         { return c.id }
func (c *pubsubScoringUnitConn) LocalPeer() peer.ID                 { return c.local }
func (c *pubsubScoringUnitConn) RemotePeer() peer.ID                { return c.remote }
func (c *pubsubScoringUnitConn) RemotePublicKey() crypto.PubKey     { return c.key }
func (c *pubsubScoringUnitConn) ConnState() network.ConnectionState { return c.state }
func (c *pubsubScoringUnitConn) Stat() network.ConnStats {
	return network.ConnStats{Stats: network.Stats{Direction: network.DirOutbound}}
}
func (c *pubsubScoringUnitConn) LocalMultiaddr() ma.Multiaddr {
	value, _ := ma.NewMultiaddr("/ip4/127.0.0.1/tcp/1234")
	return value
}
func (c *pubsubScoringUnitConn) RemoteMultiaddr() ma.Multiaddr {
	value, _ := ma.NewMultiaddr("/ip4/127.0.0.1/tcp/5678")
	return value
}
func (c *pubsubScoringUnitConn) As(any) bool { return false }

func pubsubScoringUnitConnection(t *testing.T) *pubsubScoringUnitConn {
	t.Helper()
	_, key, err := crypto.GenerateEd25519Key(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	p, err := peer.IDFromPublicKey(key)
	if err != nil {
		t.Fatal(err)
	}
	return &pubsubScoringUnitConn{local: p, remote: p, key: key, id: "unit-connection",
		state: network.ConnectionState{Transport: "tcp", Security: noise.ID, StreamMultiplexer: yamux.ID}}
}

type pubsubScoringUnitStream struct {
	network.Stream
	conn      network.Conn
	id        string
	missingID bool
	protocol  protocol.ID
	input     *bytes.Reader
	output    bytes.Buffer
	writeN    int
	writeErr  error
	reset     chan struct{}
	closeErr  error
	resetErr  error
	resetCode network.StreamErrorCode
}

func (s *pubsubScoringUnitStream) ID() string {
	if s.missingID {
		return ""
	}
	if s.id != "" {
		return s.id
	}
	return "unit-stream"
}
func (s *pubsubScoringUnitStream) Conn() network.Conn { return s.conn }
func (s *pubsubScoringUnitStream) Protocol() protocol.ID {
	if s.protocol != "" {
		return s.protocol
	}
	return pubsub.GossipSubID_v10
}
func (s *pubsubScoringUnitStream) Stat() network.Stats {
	return network.Stats{Direction: network.DirInbound}
}
func (s *pubsubScoringUnitStream) Read(p []byte) (int, error) { return s.input.Read(p) }
func (s *pubsubScoringUnitStream) Write(p []byte) (int, error) {
	n := len(p)
	if s.writeN >= 0 && s.writeN < n {
		n = s.writeN
	}
	_, _ = s.output.Write(p[:n])
	return n, s.writeErr
}
func (s *pubsubScoringUnitStream) Close() error      { return s.closeErr }
func (s *pubsubScoringUnitStream) CloseRead() error  { return s.closeErr }
func (s *pubsubScoringUnitStream) CloseWrite() error { return s.closeErr }
func (s *pubsubScoringUnitStream) Reset() error {
	if s.reset != nil {
		select {
		case s.reset <- struct{}{}:
		default:
		}
	}
	return s.resetErr
}
func (s *pubsubScoringUnitStream) ResetWithError(code network.StreamErrorCode) error {
	s.resetCode = code
	return s.Reset()
}

func TestPubsubScoringPassiveStreamPreservesDelegationAndExactReceipt(t *testing.T) {
	raw := pubsubScoringUnitFrame(t, nil)
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	drain := newPubsubScoringDrain()
	delegate := &pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t), input: bytes.NewReader(raw), writeN: -1}
	s := &pubsubScoringStream{Stream: delegate, observer: o, drain: drain}
	got := make([]byte, len(raw))
	if n, err := s.Read(got); n != len(raw) || err != nil || !bytes.Equal(got, raw) {
		t.Fatal("read changed delegate output", n, err)
	}
	if n, err := s.Write(raw); n != len(raw) || err != nil || !bytes.Equal(delegate.output.Bytes(), raw) {
		t.Fatal("write changed delegate input", n, err)
	}
	if len(o.events) != 2 {
		t.Fatal("missing actual prefix receipts")
	}
	for i, direction := range []string{"read", "write"} {
		event := o.events[i]
		receipt := event["receipt"].(map[string]any)
		meta := receipt[direction].(map[string]any)
		digest := sha256.Sum256(raw)
		if event["kind"] != "rpc" || event["connection_id"] != "unit-connection" || event["stream_id"] != "unit-stream" ||
			receipt["framed_hex"] != hex.EncodeToString(raw) || meta["framed_sha256"] != hex.EncodeToString(digest[:]) ||
			meta["framed_bytes"] != len(raw) || meta["frames"] != 1 || meta["complete_frames"] != true {
			t.Fatal("incorrect native owner/prefix receipt")
		}
	}
}

func TestPubsubScoringPartialWriteErrorDoesNotInventCompletion(t *testing.T) {
	raw := pubsubScoringUnitFrame(t, nil)
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	failure := errors.New("unit native write error")
	delegate := &pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t), writeN: len(raw) - 1, writeErr: failure}
	s := &pubsubScoringStream{Stream: delegate, observer: o, drain: newPubsubScoringDrain()}
	if n, err := s.Write(raw); n != len(raw)-1 || err != failure {
		t.Fatal("native write failure changed")
	}
	if len(o.events) != 2 || o.events[0]["kind"] != "stream_io_terminal" || s.write.buffer != nil || !s.write.finished {
		t.Fatal("invented completed RPC")
	}
	pubsubScoringUnitIncomplete(t, o, "write", "stream_write", len(raw)-1, false)
	o.local = delegate.Conn().LocalPeer()
	command := pubsubScoringCommand{Sequence: 1, Kind: "prepare_shutdown", Actor: o.actor, Token: o.token, Local: o.local.String()}
	if o.prepareShutdown(command, 0) == nil || o.prepared || len(o.events) != 2 {
		t.Fatal("partial native RPC permitted preparation/ACK")
	}
}

func pubsubScoringUnitIncomplete(t *testing.T, o *pubsubScoringObserver, direction, operation string, pending int, prepared bool) {
	t.Helper()
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.failure == nil {
		t.Fatal("incomplete native RPC did not set sticky failure")
	}
	count := 0
	for _, event := range o.events {
		if event["kind"] == "rpc" {
			t.Fatal("incomplete prefix became a completed RPC")
		}
		if event["kind"] != "incomplete_rpc_frame" {
			continue
		}
		count++
		if event["source"] != "go.pubsub.native_stream.finalize" || event["typed_cause"] != "incomplete_rpc_frame" ||
			event["direction"] != direction || event["operation"] != operation || event["pending_frame_bytes"] != pending ||
			event["prepared"] != prepared || event["connection_id"] != "unit-connection" || event["stream_id"] != "unit-stream" ||
			event["protocol"] != string(pubsub.GossipSubID_v10) || event["remote_peer_id"] == "" {
			t.Fatal("incomplete RPC lacks exact bounded native attribution", event)
		}
	}
	if count != 1 {
		t.Fatal("missing/duplicate incomplete RPC record", count)
	}
}

func TestPubsubScoringAllFramingTerminalsFailBeforeAndAfterPrepare(t *testing.T) {
	raw := pubsubScoringUnitFrame(t, nil)
	for _, prepared := range []bool{false, true} {
		for _, direction := range []string{"read", "write"} {
			for _, operation := range []string{"eof", "close", "half_close", "reset", "reset_with_error", "drain"} {
				if direction == "write" && operation == "eof" {
					continue
				}
				for _, prefix := range [][]byte{{0x80}, raw[:len(raw)-1]} {
					t.Run(fmt.Sprintf("%t/%s/%s/%d", prepared, direction, operation, len(prefix)), func(t *testing.T) {
						o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
						delegate := &pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t), input: bytes.NewReader(prefix), writeN: -1}
						drain := newPubsubScoringDrain()
						s := &pubsubScoringStream{Stream: delegate, observer: o, drain: drain}
						if retained, err := drain.retain(s); !retained || err != nil {
							t.Fatal("wrapper not retained")
						}
						o.local = delegate.Conn().LocalPeer()
						command := pubsubScoringCommand{Sequence: 1, Kind: "prepare_shutdown", Actor: o.actor, Token: o.token, Local: o.local.String()}
						if prepared {
							if err := o.prepareShutdown(command, 0); err != nil {
								t.Fatal(err)
							}
						}
						if direction == "read" {
							buffer := make([]byte, len(prefix))
							if n, err := s.Read(buffer); n != len(prefix) || err != nil || !bytes.Equal(buffer, prefix) {
								t.Fatal("read prefix changed", n, err)
							}
						} else if n, err := s.Write(prefix); n != len(prefix) || err != nil {
							t.Fatal("write prefix changed", n, err)
						}
						expected := "stream_" + operation
						var err error
						switch operation {
						case "eof":
							if n, native := s.Read(make([]byte, 1)); n != 0 || native != io.EOF {
								t.Fatal("EOF changed", n, native)
							}
							expected = "stream_read"
						case "close":
							err = s.Close()
						case "half_close":
							if direction == "read" {
								err = s.CloseRead()
							} else {
								err = s.CloseWrite()
							}
							expected = "stream_close_" + direction
						case "reset":
							err = s.Reset()
						case "reset_with_error":
							err = s.ResetWithError(network.StreamErrorCode(7))
							if delegate.resetCode != 7 {
								t.Fatal("native reset code changed")
							}
						case "drain":
							ctx, cancel := context.WithTimeout(context.Background(), time.Second)
							defer cancel()
							err = drain.stop(ctx)
							expected = "stream_reset"
						}
						if err != nil {
							t.Fatal("native close/reset result changed", err)
						}
						pubsubScoringUnitIncomplete(t, o, direction, expected, len(prefix), prepared)
						first := o.failure
						if o.result(true, true, nil, 0)["error"] == nil {
							t.Fatal("terminal result masked framing failure")
						}
						if !prepared && (o.prepareShutdown(command, 0) == nil || o.prepared) {
							t.Fatal("late prepare accepted incomplete RPC")
						}
						if err := s.Reset(); err != nil {
							t.Fatal(err)
						}
						pubsubScoringUnitIncomplete(t, o, direction, expected, len(prefix), prepared)
						if o.failure != first || s.read.buffer != nil || s.write.buffer != nil {
							t.Fatal("first failure/buffer finalization changed")
						}
					})
				}
			}
		}
	}
}

func TestPubsubScoringEmptyEOFAndNativeCloseErrorsPreserveTheirMeaning(t *testing.T) {
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	delegate := &pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t), input: bytes.NewReader(nil), writeN: -1}
	s := &pubsubScoringStream{Stream: delegate, observer: o, drain: newPubsubScoringDrain()}
	if n, err := s.Read(make([]byte, 1)); n != 0 || err != io.EOF || o.failure != nil {
		t.Fatal("empty EOF became framing failure", n, err)
	}
	if err := s.CloseRead(); err != nil {
		t.Fatal(err)
	}
	if n, err := s.Write([]byte{0}); n != 1 || err != nil || o.failure != nil {
		t.Fatal("half-close terminated opposite direction", n, err)
	}
	if err := s.Close(); err != nil || !s.framingJoined() || o.failure != nil {
		t.Fatal("complete stream did not finalize neutrally", err)
	}
	for _, operation := range []string{"close", "close_read", "close_write", "reset", "reset_with_error"} {
		t.Run(operation, func(t *testing.T) {
			o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
			failure := errors.New("native close sentinel")
			delegate := &pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t), closeErr: failure, resetErr: failure}
			s := &pubsubScoringStream{Stream: delegate, observer: o, drain: newPubsubScoringDrain()}
			var err error
			switch operation {
			case "close":
				err = s.Close()
			case "close_read":
				err = s.CloseRead()
			case "close_write":
				err = s.CloseWrite()
			case "reset":
				err = s.Reset()
			case "reset_with_error":
				err = s.ResetWithError(7)
			}
			if err != failure || o.failure != failure || s.framingJoined() {
				t.Fatal("native error changed or falsely joined", err)
			}
		})
	}
}

func TestPubsubScoringFramingFailureDoesNotReplacePrimaryAfterPrepare(t *testing.T) {
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	delegate := &pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t), input: bytes.NewReader(nil), writeN: -1}
	s := &pubsubScoringStream{Stream: delegate, observer: o, drain: newPubsubScoringDrain()}
	o.local = delegate.Conn().LocalPeer()
	command := pubsubScoringCommand{Sequence: 1, Kind: "prepare_shutdown", Actor: o.actor, Token: o.token, Local: o.local.String()}
	if err := o.prepareShutdown(command, 0); err != nil {
		t.Fatal(err)
	}
	if n, err := s.Read(make([]byte, 1)); n != 0 || err != io.EOF || o.failure != nil {
		t.Fatal("prepared empty EOF failed", n, err)
	}
	primary := errors.New("original active/native owner failure")
	o.fail(primary)
	if n, err := s.Write([]byte{0x80}); n != 1 || err != nil {
		t.Fatal("native partial prefix changed", n, err)
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	pubsubScoringUnitIncomplete(t, o, "write", "stream_close", 1, true)
	if o.failure != primary || o.result(true, true, nil, 0)["error"] != primary.Error() {
		t.Fatal("framing finalization replaced primary result")
	}
}

func pubsubScoringUnitWrapped(t *testing.T, delegate network.Stream) (*pubsubScoringStream, *pubsubScoringObserver) {
	t.Helper()
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	o.local = delegate.Conn().LocalPeer()
	h := &pubsubScoringHost{observer: o, drain: newPubsubScoringDrain(), protocol: pubsub.GossipSubID_v10}
	wrapped, err := h.wrap(delegate)
	if err != nil {
		t.Fatal(err)
	}
	return wrapped.(*pubsubScoringStream), o
}

func pubsubScoringUnitPrepare(t *testing.T, o *pubsubScoringObserver) {
	t.Helper()
	if err := o.prepareShutdown(pubsubScoringCommand{Sequence: 1, Kind: "prepare_shutdown",
		Actor: o.actor, Token: o.token, Local: o.local.String()}, 0); err != nil {
		t.Fatal(err)
	}
}

func pubsubScoringUnitEvents(o *pubsubScoringObserver, kind string) []map[string]any {
	o.mu.Lock()
	defer o.mu.Unlock()
	var result []map[string]any
	for _, event := range o.events {
		if event["kind"] == kind {
			result = append(result, event)
		}
	}
	return result
}

func pubsubScoringUnitLocalReset() error {
	return &network.StreamError{ErrorCode: 0, Remote: false,
		TransportError: &nativeyamux.StreamError{ErrorCode: 0, Remote: false}}
}

func pubsubScoringUnitPeerReset() error {
	return &network.StreamError{ErrorCode: 0, Remote: true,
		TransportError: &nativeyamux.StreamError{ErrorCode: 0, Remote: true}}
}

func pubsubScoringUnitDisposal(t *testing.T, o *pubsubScoringObserver, final map[string]any) {
	t.Helper()
	reference, ok := final["owner_disposal_receipt_sequence"].(int)
	if !ok || reference <= 0 || reference >= final["sequence"].(int) {
		t.Fatal("terminal finalizer lacks an earlier indexed owner disposal", final)
	}
	for _, event := range pubsubScoringUnitEvents(o, "native_stream_operation") {
		if event["sequence"] != reference {
			continue
		}
		if event["outcome"] != "ok" || event["error"] != nil || event["typed_cause"] != "none" ||
			(event["operation"] != "stream_close" && event["operation"] != "stream_reset" && event["operation"] != "stream_reset_with_error") {
			t.Fatal("owner disposal is not a successful full native operation", event)
		}
		for _, key := range []string{"connection_id", "stream_id", "remote_peer_id", "protocol", "native_yamux"} {
			if event[key] != final[key] {
				t.Fatal("owner disposal borrowed a foreign native wrapper", key, event, final)
			}
		}
		if event["started_order"].(uint64) >= event["returned_order"].(uint64) {
			t.Fatal("owner disposal lacks its actual native RETURN", event)
		}
		return
	}
	t.Fatal("owner disposal references no actual native receipt", reference)
}

type pubsubScoringResetUnitStream struct {
	pubsubScoringUnitStream
	ioStarted, ioReturn, resetStarted, resetReturn chan struct{}
	prefix                                         []byte
	nativeErr                                      error
}

func (s *pubsubScoringResetUnitStream) Read(p []byte) (int, error) {
	close(s.ioStarted)
	<-s.ioReturn
	return copy(p, s.prefix), s.nativeErr
}

func (s *pubsubScoringResetUnitStream) Write(p []byte) (int, error) {
	close(s.ioStarted)
	<-s.ioReturn
	n := len(s.prefix)
	_, _ = s.output.Write(p[:n])
	return n, s.nativeErr
}

func (s *pubsubScoringResetUnitStream) Reset() error {
	close(s.resetStarted)
	<-s.resetReturn
	return s.resetErr
}

func pubsubScoringUnitWait(t *testing.T, signal <-chan struct{}) {
	t.Helper()
	select {
	case <-signal:
	case <-time.After(2 * time.Second):
		t.Fatal("bounded native unit operation did not reach checkpoint")
	}
}

func TestPubsubScoringOwnedResetIOPinsPreparedResetNotIOBegin(t *testing.T) {
	for _, direction := range []string{"read", "write"} {
		for _, ioFirst := range []bool{false, true} {
			for _, mode := range []string{"clean", "header", "body", "malformed", "failed_reset", "sticky"} {
				t.Run(fmt.Sprintf("%s/%t/%s", direction, ioFirst, mode), func(t *testing.T) {
					delegate := &pubsubScoringResetUnitStream{pubsubScoringUnitStream: pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t)},
						ioStarted: make(chan struct{}), ioReturn: make(chan struct{}), resetStarted: make(chan struct{}), resetReturn: make(chan struct{}),
						nativeErr: pubsubScoringUnitLocalReset()}
					t.Cleanup(func() {
						for _, signal := range []chan struct{}{delegate.ioReturn, delegate.resetReturn} {
							select {
							case <-signal:
							default:
								close(signal)
							}
						}
					})
					s, o := pubsubScoringUnitWrapped(t, delegate)
					var primary error
					switch mode {
					case "header":
						delegate.prefix = []byte{0x80}
					case "body":
						frame := pubsubScoringUnitFrame(t, nil)
						delegate.prefix = frame[:len(frame)-1]
					case "malformed":
						delegate.prefix = []byte{0x01, 0xff}
					case "failed_reset":
						delegate.resetErr = errors.New("native failed Reset")
					}
					ioDone := make(chan struct{})
					var n int
					var native error
					buffer := make([]byte, len(delegate.prefix)+1)
					if direction == "write" {
						copy(buffer, delegate.prefix)
					}
					go func() {
						if direction == "read" {
							n, native = s.Read(buffer)
						} else {
							n, native = s.Write(buffer)
						}
						close(ioDone)
					}()
					pubsubScoringUnitWait(t, delegate.ioStarted)
					pubsubScoringUnitPrepare(t, o)
					if mode == "sticky" {
						primary = syscall.ECONNRESET
						o.fail(primary)
					}
					resetDone := make(chan struct{})
					var resetResult error
					go func() { resetResult = s.Reset(); close(resetDone) }()
					pubsubScoringUnitWait(t, delegate.resetStarted)
					var before []byte
					if ioFirst {
						close(delegate.ioReturn)
						pubsubScoringUnitWait(t, ioDone)
						before, _ = json.Marshal(pubsubScoringUnitEvents(o, "stream_io_terminal"))
					} else {
						close(delegate.resetReturn)
						pubsubScoringUnitWait(t, resetDone)
					}
					if len(pubsubScoringUnitEvents(o, "native_stream_io_finalized")) != 0 || s.framingJoined() {
						t.Fatal("pending native Reset/I/O claimed finalization or join")
					}
					if ioFirst {
						close(delegate.resetReturn)
						pubsubScoringUnitWait(t, resetDone)
					} else {
						close(delegate.ioReturn)
						pubsubScoringUnitWait(t, ioDone)
					}
					if n != len(delegate.prefix) || native != delegate.nativeErr || resetResult != delegate.resetErr ||
						(direction == "read" && !bytes.Equal(buffer[:n], delegate.prefix)) ||
						(direction == "write" && !bytes.Equal(delegate.output.Bytes(), delegate.prefix)) {
						t.Fatal("native I/O or Reset result changed")
					}
					raw := pubsubScoringUnitEvents(o, "stream_io_terminal")
					final := pubsubScoringUnitEvents(o, "native_stream_io_finalized")
					resets := pubsubScoringUnitEvents(o, "native_stream_operation")
					candidate := mode != "failed_reset" || ioFirst
					if len(raw) != 1 || len(resets) != 1 || raw[0]["prepared"] != false || raw[0]["prepare_ack_sequence"] != 0 ||
						raw[0]["reset_receipt_sequence"] != 0 || raw[0]["reset_returned_order"] != uint64(0) ||
						resets[0]["prepared"] != true || resets[0]["prepare_ack_sequence"] != o.prepareAck ||
						raw[0]["started_order"].(uint64) >= resets[0]["started_order"].(uint64) ||
						(resets[0]["returned_order"].(uint64) > raw[0]["returned_order"].(uint64)) != ioFirst {
						t.Fatal("lost immutable native I/O / causal Prepared Reset ownership")
					}
					if ioFirst {
						after, _ := json.Marshal(raw)
						if !bytes.Equal(before, after) {
							t.Fatal("native I/O receipt was rewritten after Reset returned")
						}
					}
					if candidate {
						if len(final) != 1 || raw[0]["outcome"] != "owned_reset_pending" || raw[0]["reset_started_order"] != resets[0]["started_order"] ||
							final[0]["operation_receipt_sequence"] != raw[0]["sequence"] || final[0]["causal_reset_receipt_sequence"] != resets[0]["sequence"] {
							t.Fatal("missing exact pending/finalized Reset evidence")
						}
					} else if len(final) != 0 || raw[0]["outcome"] != "error" {
						t.Fatal("failed Reset manufactured a candidate")
					}
					accepted := mode == "clean" || mode == "sticky"
					if (candidate && final[0]["accepted"] != accepted) || (mode == "clean" && o.failure != nil) ||
						(mode != "clean" && o.failure == nil) || (mode == "sticky" && o.failure != primary) ||
						(mode == "failed_reset" && o.failure != delegate.resetErr) || s.read.buffer != nil || s.write.buffer != nil ||
						s.framingJoined() != (mode != "failed_reset") {
						t.Fatal("causal finalization masked failure, residue, or native owner")
					}
				})
			}
		}
	}
}

func TestPubsubScoringResetIORejectsNonExactCausesAndForeignOrLateReset(t *testing.T) {
	zero := pubsubScoringUnitLocalReset()
	causes := map[string]error{"bare": network.ErrReset, "nonzero": &network.StreamError{ErrorCode: 7, TransportError: &nativeyamux.StreamError{ErrorCode: 7}},
		"remote_nonzero": &network.StreamError{ErrorCode: 7, Remote: true, TransportError: &nativeyamux.StreamError{ErrorCode: 7, Remote: true}},
		"nil_inner":      &network.StreamError{}, "inner_nonzero": &network.StreamError{TransportError: &nativeyamux.StreamError{ErrorCode: 7}},
		"inner_remote": &network.StreamError{TransportError: &nativeyamux.StreamError{Remote: true}},
		"opaque":       errors.New("stream reset, error code: 0"), "wrapped": fmt.Errorf("wrapped: %w", zero),
		"joined": errors.Join(zero, syscall.ECONNRESET), "tcp": fmt.Errorf("%w: %w", network.ErrReset, syscall.ECONNRESET),
		"inner_joined":  &network.StreamError{TransportError: errors.Join(&nativeyamux.StreamError{}, syscall.ECONNRESET)},
		"inner_wrapped": &network.StreamError{TransportError: fmt.Errorf("wrapped: %w", &nativeyamux.StreamError{})},
		"tcp_inner":     &network.StreamError{TransportError: &net.OpError{Op: "read", Net: "tcp", Err: &os.SyscallError{Syscall: "read", Err: syscall.ECONNRESET}}},
		"raw_yamux":     &nativeyamux.StreamError{}}
	type failureCase struct {
		cause     error
		ownership string
	}
	tests := make(map[string]failureCase)
	for name, cause := range causes {
		tests[name] = failureCase{cause, "own"}
	}
	for _, ownership := range []string{"foreign", "late", "active_reset", "unadmitted", "absent", "failed_reset", "reset_with_error"} {
		tests[ownership] = failureCase{zero, ownership}
	}
	for _, direction := range []string{"read", "write"} {
		for name, input := range tests {
			t.Run(direction+"/"+name, func(t *testing.T) {
				delegate := &pubsubScoringErrorUnitStream{pubsubScoringUnitStream: pubsubScoringUnitStream{
					conn: pubsubScoringUnitConnection(t), writeN: 0, writeErr: input.cause}, nativeErr: input.cause}
				s, o := pubsubScoringUnitWrapped(t, delegate)
				if input.ownership == "active_reset" {
					if err := s.Reset(); err != nil {
						t.Fatal(err)
					}
				}
				pubsubScoringUnitPrepare(t, o)
				primary := input.cause
				switch input.ownership {
				case "own", "unadmitted", "failed_reset":
					if input.ownership == "failed_reset" {
						delegate.resetErr = syscall.ENOSYS
						primary = delegate.resetErr
					}
					if err := s.Reset(); err != delegate.resetErr {
						t.Fatal("native Reset changed")
					}
					if input.ownership == "unadmitted" {
						s.nativeYamux = false
					}
				case "foreign":
					h := &pubsubScoringHost{observer: o, drain: s.drain, protocol: pubsub.GossipSubID_v10}
					other, err := h.wrap(&pubsubScoringUnitStream{conn: delegate.Conn(), id: "foreign"})
					if err != nil {
						t.Fatal(err)
					}
					if err := other.Reset(); err != nil {
						t.Fatal(err)
					}
				case "reset_with_error":
					if err := s.ResetWithError(0); err != nil {
						t.Fatal(err)
					}
				}
				var n int
				var err error
				if direction == "read" {
					n, err = s.Read(make([]byte, 1))
				} else {
					n, err = s.Write([]byte{0})
				}
				if n != 0 || err != input.cause || o.failure != primary {
					t.Fatal("unknown/unowned native error changed or accepted")
				}
				if input.ownership == "late" {
					if err := s.Reset(); err != nil {
						t.Fatal(err)
					}
				}
				if o.failure != primary || len(pubsubScoringUnitEvents(o, "native_stream_io_finalized")) != 0 {
					t.Fatal("later/foreign Reset reinterpreted failure")
				}
			})
		}
	}
}

type pubsubScoringErrorUnitStream struct {
	pubsubScoringUnitStream
	nativeErr error
}

func (s *pubsubScoringErrorUnitStream) Read([]byte) (int, error) { return 0, s.nativeErr }

func TestPubsubScoringPeerZeroResetUsesReturnPreparationAndCleanOwnedJoin(t *testing.T) {
	for _, direction := range []string{"read", "write"} {
		for _, mode := range []string{"clean", "header", "body", "malformed"} {
			t.Run(direction+"/"+mode, func(t *testing.T) {
				prefix := pubsubScoringUnitFrame(t, nil)
				switch mode {
				case "header":
					prefix = []byte{0x80}
				case "body":
					prefix = prefix[:len(prefix)-1]
				case "malformed":
					prefix = []byte{1, 0xff}
				}
				delegate := &pubsubScoringResetUnitStream{pubsubScoringUnitStream: pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t)},
					ioStarted: make(chan struct{}), ioReturn: make(chan struct{}), prefix: prefix, nativeErr: pubsubScoringUnitPeerReset()}
				t.Cleanup(func() {
					select {
					case <-delegate.ioReturn:
					default:
						close(delegate.ioReturn)
					}
				})
				s, o := pubsubScoringUnitWrapped(t, delegate)
				buffer := append([]byte(nil), prefix...)
				var n int
				var native error
				done := make(chan struct{})
				go func() {
					if direction == "read" {
						n, native = s.Read(buffer)
					} else {
						n, native = s.Write(buffer)
					}
					close(done)
				}()
				pubsubScoringUnitWait(t, delegate.ioStarted)
				pubsubScoringUnitPrepare(t, o)
				close(delegate.ioReturn)
				pubsubScoringUnitWait(t, done)
				if n != len(prefix) || native != delegate.nativeErr ||
					(direction == "write" && !bytes.Equal(delegate.output.Bytes(), prefix)) {
					t.Fatal("peer cancellation changed native I/O prefix/result")
				}
				raw := pubsubScoringUnitEvents(o, "stream_io_terminal")
				if len(raw) != 1 || raw[0]["outcome"] != "peer_zero_reset_pending" || raw[0]["prepared"] != false ||
					raw[0]["prepare_ack_sequence"] != 0 || raw[0]["terminal_prepare_ack_sequence"] != o.prepareAck ||
					raw[0]["peer_reset_reason"] != "unknown" || raw[0]["reset_started_order"] != uint64(0) ||
					len(pubsubScoringUnitEvents(o, "native_stream_io_finalized")) != 0 || s.framingJoined() {
					t.Fatal("peer cancellation invented preparation-at-BEGIN, cleanup cause, or early join")
				}
				before, _ := json.Marshal(raw)
				if err := s.Close(); err != nil {
					t.Fatal(err)
				}
				final := pubsubScoringUnitEvents(o, "native_stream_io_finalized")
				after, _ := json.Marshal(raw)
				if len(final) != 1 || final[0]["accepted"] != (mode == "clean") ||
					final[0]["operation_receipt_sequence"] != raw[0]["sequence"] || !bytes.Equal(before, after) ||
					(mode == "clean" && o.failure != nil) || (mode != "clean" && o.failure == nil) {
					t.Fatal("peer cancellation bypassed immutable framing finalization")
				}
				if _, claimed := final[0]["causal_reset_receipt_sequence"]; claimed {
					t.Fatal("peer cancellation fabricated a causal remote/local Reset")
				}
				pubsubScoringUnitDisposal(t, o, final[0])
			})
		}
	}
}

func TestPubsubScoringOwnedReadTerminalPinsSuccessfulNativeReturnAndDisposal(t *testing.T) {
	for _, operation := range []string{"close", "close_read", "reset"} {
		for _, mode := range []string{"clean", "header", "body", "malformed", "sticky_before", "sticky_after"} {
			if mode == "sticky_after" && operation != "close_read" {
				continue
			}
			t.Run(operation+"/"+mode, func(t *testing.T) {
				prefix := pubsubScoringUnitFrame(t, nil)
				switch mode {
				case "header":
					prefix = []byte{0x80}
				case "body":
					prefix = prefix[:len(prefix)-1]
				case "malformed":
					prefix = []byte{1, 0xff}
				}
				native := fmt.Errorf("%w: %w", network.ErrReset, nativeyamux.ErrStreamReset)
				delegate := &pubsubScoringResetUnitStream{pubsubScoringUnitStream: pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t)},
					ioStarted: make(chan struct{}), ioReturn: make(chan struct{}), resetStarted: make(chan struct{}), resetReturn: make(chan struct{}),
					prefix: prefix, nativeErr: native}
				t.Cleanup(func() {
					for _, signal := range []chan struct{}{delegate.ioReturn, delegate.resetReturn} {
						select {
						case <-signal:
						default:
							close(signal)
						}
					}
				})
				s, o := pubsubScoringUnitWrapped(t, delegate)
				buffer := make([]byte, len(prefix)+1)
				var n int
				var result error
				done := make(chan struct{})
				go func() { n, result = s.Read(buffer); close(done) }()
				pubsubScoringUnitWait(t, delegate.ioStarted)
				pubsubScoringUnitPrepare(t, o)
				var terminalResult error
				switch operation {
				case "close":
					terminalResult = s.Close()
				case "close_read":
					terminalResult = s.CloseRead()
				case "reset":
					close(delegate.resetReturn)
					terminalResult = s.Reset()
				}
				if terminalResult != nil {
					t.Fatal("native terminal result changed", terminalResult)
				}
				if mode == "sticky_before" {
					o.fail(syscall.ECONNRESET)
				}
				close(delegate.ioReturn)
				pubsubScoringUnitWait(t, done)
				if n != len(prefix) || result != native || !bytes.Equal(buffer[:n], prefix) {
					t.Fatal("owned terminal altered the native Read error object or successful prefix")
				}
				raw := pubsubScoringUnitEvents(o, "stream_io_terminal")
				if len(raw) != 1 || raw[0]["prepared"] != false || raw[0]["prepare_ack_sequence"] != 0 {
					t.Fatal("Read BEGIN preparation was rewritten")
				}
				before, _ := json.Marshal(raw)
				if mode == "sticky_after" {
					o.fail(syscall.ECONNRESET)
				}
				if operation == "close_read" {
					if s.framingJoined() || len(pubsubScoringUnitEvents(o, "native_stream_io_finalized")) != 0 {
						t.Fatal("CloseRead alone invented full owner disposal")
					}
					if err := s.Close(); err != nil {
						t.Fatal(err)
					}
				}
				final := pubsubScoringUnitEvents(o, "native_stream_io_finalized")
				if mode == "sticky_before" {
					if raw[0]["outcome"] != "error" || len(final) != 0 || o.failure != syscall.ECONNRESET {
						t.Fatal("prior sticky failure authorized a new terminal candidate")
					}
					return
				}
				accepted := mode == "clean"
				if len(final) != 1 || final[0]["accepted"] != accepted || raw[0]["outcome"] != "owned_read_terminal_pending" ||
					raw[0]["typed_cause"] != "yamux_reset_sentinel_pair" || raw[0]["terminal_state_cause"] != "unknown" ||
					raw[0]["error_code"] != nil || raw[0]["remote"] != nil || raw[0]["transport_error_type"] != nil ||
					raw[0]["terminal_returned_order"].(uint64) >= raw[0]["returned_order"].(uint64) {
					t.Fatal("owned read terminal lost exact return ordering or invented a remote/code cause", raw, final)
				}
				terminalRef := final[0]["observed_terminal_receipt_sequence"]
				var terminal map[string]any
				for _, event := range pubsubScoringUnitEvents(o, "native_stream_operation") {
					if event["sequence"] == terminalRef {
						terminal = event
					}
				}
				if terminal == nil || terminal["prepared"] != true || terminal["prepare_ack_sequence"] != o.prepareAck ||
					terminal["outcome"] != "ok" || terminal["started_order"] != raw[0]["terminal_started_order"] ||
					terminal["returned_order"] != raw[0]["terminal_returned_order"] {
					t.Fatal("owned terminal finalizer lacks its real native operation receipt")
				}
				pubsubScoringUnitDisposal(t, o, final[0])
				after, _ := json.Marshal(pubsubScoringUnitEvents(o, "stream_io_terminal"))
				if !bytes.Equal(before, after) || (accepted && o.failure != nil) || (!accepted && o.failure == nil) ||
					(mode == "sticky_after" && o.failure != syscall.ECONNRESET) {
					t.Fatal("finalization changed immutable I/O or cleared the first failure")
				}
			})
		}
	}
}

func TestPubsubScoringOwnedReadTerminalRejectsNonExactPairAndUnownedOrLateTerminal(t *testing.T) {
	pair := fmt.Errorf("%w: %w", network.ErrReset, nativeyamux.ErrStreamReset)
	causes := map[string]error{
		"extra_child":  fmt.Errorf("%w: %w: %w", network.ErrReset, nativeyamux.ErrStreamReset, syscall.ECONNRESET),
		"wrapped_pair": fmt.Errorf("outer: %w", pair), "wrapped_child": fmt.Errorf("%w: %w", network.ErrReset, fmt.Errorf("inner: %w", nativeyamux.ErrStreamReset)),
		"errno_child": fmt.Errorf("%w: %w", network.ErrReset, syscall.ECONNRESET), "joined_errno": errors.Join(pair, syscall.ECONNRESET),
		"reversed": fmt.Errorf("%w: %w", nativeyamux.ErrStreamReset, network.ErrReset), "single": network.ErrReset,
		"yamux_only": nativeyamux.ErrStreamReset, "opaque": errors.New("stream reset: stream reset"),
	}
	for _, mode := range []string{"write", "preprepared_terminal", "preprepared_read", "late_close", "foreign", "failed", "superseded_failed", "inflight", "reset_with_error", "unadmitted", "sticky", "overflow"} {
		causes[mode] = pair
	}
	for mode, native := range causes {
		t.Run(mode, func(t *testing.T) {
			delegate := &pubsubScoringErrorUnitStream{pubsubScoringUnitStream: pubsubScoringUnitStream{
				conn: pubsubScoringUnitConnection(t), writeErr: native}, nativeErr: native}
			s, o := pubsubScoringUnitWrapped(t, delegate)
			if mode == "preprepared_terminal" {
				if err := s.CloseRead(); err != nil {
					t.Fatal(err)
				}
			}
			if mode != "preprepared_read" {
				pubsubScoringUnitPrepare(t, o)
			}
			primary := native
			var inflight pubsubScoringOperation
			switch mode {
			case "preprepared_terminal", "preprepared_read", "late_close":
			case "foreign":
				h := &pubsubScoringHost{observer: o, drain: s.drain, protocol: pubsub.GossipSubID_v10}
				other, err := h.wrap(&pubsubScoringUnitStream{conn: delegate.Conn(), id: "foreign"})
				if err != nil {
					t.Fatal(err)
				}
				if err := other.Close(); err != nil {
					t.Fatal(err)
				}
			case "failed":
				delegate.closeErr = syscall.ENOSYS
				primary = delegate.closeErr
				if err := s.CloseRead(); err != primary {
					t.Fatal("failed native terminal changed")
				}
			default:
				if err := s.CloseRead(); err != nil {
					t.Fatal(err)
				}
				switch mode {
				case "superseded_failed":
					delegate.resetErr = syscall.ENOSYS
					primary = delegate.resetErr
					if err := s.Reset(); err != primary {
						t.Fatal("failed superseding Reset changed")
					}
				case "inflight":
					inflight = s.beginOperation("stream_close_read", true)
				case "reset_with_error":
					if err := s.ResetWithError(0); err != nil {
						t.Fatal(err)
					}
				case "unadmitted":
					s.nativeYamux = false
				case "sticky":
					primary = syscall.ECONNRESET
					o.fail(primary)
				case "overflow":
					for index := 0; index < pubsubScoringEvents; index++ {
						o.emit("snapshot", "go.unit.overflow", nil)
					}
					primary = o.failure
				}
			}
			var n int
			var result error
			if mode == "write" {
				n, result = s.Write([]byte{0})
			} else {
				n, result = s.Read(make([]byte, 1))
			}
			if n != 0 || result != native || o.failure != primary {
				t.Fatal("unowned/nonexact terminal error changed or accepted", n, result, o.failure)
			}
			before, _ := json.Marshal(pubsubScoringUnitEvents(o, "stream_io_terminal"))
			if mode == "inflight" {
				s.endClose(inflight, 0, s.returnedOperation(inflight, delegate.CloseRead()), nil)
			}
			delegate.closeErr = nil
			if err := s.Close(); err != nil {
				t.Fatal(err)
			}
			after, _ := json.Marshal(pubsubScoringUnitEvents(o, "stream_io_terminal"))
			if !bytes.Equal(before, after) || o.failure != primary || len(pubsubScoringUnitEvents(o, "native_stream_io_finalized")) != 0 {
				t.Fatal("later successful disposal retroactively authorized the native Read failure")
			}
		})
	}
}

func TestPubsubScoringReadTerminalAndPeerZeroResetRequireFullDisposalReceipt(t *testing.T) {
	for _, outcome := range []string{"owned_read_terminal_pending", "peer_zero_reset_pending"} {
		for _, disposal := range []string{"close", "reset", "half_only", "failed_close"} {
			t.Run(outcome+"/"+disposal, func(t *testing.T) {
				native := pubsubScoringUnitPeerReset()
				if outcome == "owned_read_terminal_pending" {
					native = fmt.Errorf("%w: %w", network.ErrReset, nativeyamux.ErrStreamReset)
				}
				delegate := &pubsubScoringErrorUnitStream{pubsubScoringUnitStream: pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t)}, nativeErr: native}
				s, o := pubsubScoringUnitWrapped(t, delegate)
				pubsubScoringUnitPrepare(t, o)
				if err := s.CloseRead(); err != nil {
					t.Fatal(err)
				}
				if n, err := s.Read(make([]byte, 1)); n != 0 || err != native || o.failure != nil {
					t.Fatal("native Read result changed", n, err, o.failure)
				}
				if s.framingJoined() || len(pubsubScoringUnitEvents(o, "native_stream_io_finalized")) != 0 {
					t.Fatal("half-close fabricated owner disposal")
				}
				switch disposal {
				case "close":
					if err := s.Close(); err != nil {
						t.Fatal(err)
					}
				case "reset":
					if err := s.Reset(); err != nil {
						t.Fatal(err)
					}
				case "half_only":
					if err := s.CloseWrite(); err != nil {
						t.Fatal(err)
					}
				case "failed_close":
					delegate.closeErr = syscall.ENOSYS
					if err := s.Close(); err != delegate.closeErr {
						t.Fatal("native failed Close changed")
					}
				}
				final := pubsubScoringUnitEvents(o, "native_stream_io_finalized")
				accepted := disposal == "close" || disposal == "reset"
				if len(final) != 1 || final[0]["outcome"] != outcome || final[0]["accepted"] != accepted ||
					final[0]["native_owner_disposed"] != accepted || s.framingJoined() != accepted {
					t.Fatal("full owner disposal lacks its truthful finalizer", final)
				}
				if accepted {
					pubsubScoringUnitDisposal(t, o, final[0])
				} else if final[0]["owner_disposal_receipt_sequence"] != 0 || o.failure == nil ||
					(disposal == "failed_close" && o.failure != delegate.closeErr) {
					t.Fatal("missing/failed full disposal accepted or replaced the first error")
				}
			})
		}
	}
}

func TestPubsubScoringOwnedReadTerminalPinsUnpublishedNativeReturnWithoutOldAttemptFallback(t *testing.T) {
	for _, newerFailed := range []bool{false, true} {
		t.Run(fmt.Sprintf("newer_failed=%t", newerFailed), func(t *testing.T) {
			native := fmt.Errorf("%w: %w", network.ErrReset, nativeyamux.ErrStreamReset)
			delegate := &pubsubScoringErrorUnitStream{pubsubScoringUnitStream: pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t)}, nativeErr: native}
			s, o := pubsubScoringUnitWrapped(t, delegate)
			pubsubScoringUnitPrepare(t, o)
			// Separate the actual delegate's return registration from its later receipt publication.
			terminal := s.beginOperation("stream_close", true)
			result := s.returnedOperation(terminal, delegate.Close())
			if result.err != nil || !terminal.readTerminal.completed || terminal.readTerminal.sequence != 0 {
				t.Fatal("native terminal RETURN depends on publication")
			}
			if newerFailed {
				delegate.resetErr = syscall.ENOSYS
				if err := s.Reset(); err != delegate.resetErr {
					t.Fatal("superseding failed Reset changed")
				}
			}
			if n, err := s.Read(make([]byte, 1)); n != 0 || err != native {
				t.Fatal("native Read result changed")
			}
			raw := pubsubScoringUnitEvents(o, "stream_io_terminal")
			if len(raw) != 1 || len(pubsubScoringUnitEvents(o, "native_stream_io_finalized")) != 0 {
				t.Fatal("unpublished native operation falsely finalized")
			}
			before, _ := json.Marshal(raw)
			s.endClose(terminal, -1, result, nil)
			final := pubsubScoringUnitEvents(o, "native_stream_io_finalized")
			if newerFailed {
				if len(final) != 0 || o.failure != delegate.resetErr || s.readTerminal == terminal.readTerminal {
					t.Fatal("late publication borrowed or restored an old terminal attempt")
				}
			} else {
				if len(final) != 1 || final[0]["accepted"] != true || o.failure != nil || raw[0]["terminal_receipt_sequence"] != 0 ||
					final[0]["observed_terminal_receipt_sequence"] != terminal.readTerminal.sequence ||
					terminal.readTerminal.sequence <= raw[0]["sequence"].(int) {
					t.Fatal("native return ordering was replaced by receipt publication order")
				}
				pubsubScoringUnitDisposal(t, o, final[0])
			}
			after, _ := json.Marshal(pubsubScoringUnitEvents(o, "stream_io_terminal"))
			if !bytes.Equal(before, after) {
				t.Fatal("late publication rewrote the Read receipt")
			}
		})
	}
}

func TestPubsubScoringPeerZeroResetRejectsUnknownCauseOrPreparation(t *testing.T) {
	zero := pubsubScoringUnitPeerReset()
	cases := map[string]error{
		"preprepare": zero, "sticky": zero, "overflow": zero, "unadmitted": zero,
		"outer_nonzero":  &network.StreamError{ErrorCode: 7, Remote: true, TransportError: &nativeyamux.StreamError{Remote: true}},
		"inner_nonzero":  &network.StreamError{Remote: true, TransportError: &nativeyamux.StreamError{Remote: true, ErrorCode: 7}},
		"outer_mismatch": &network.StreamError{TransportError: &nativeyamux.StreamError{Remote: true}},
		"inner_mismatch": &network.StreamError{Remote: true, TransportError: &nativeyamux.StreamError{}},
		"nil_inner":      &network.StreamError{Remote: true}, "bare": network.ErrReset,
		"raw_yamux": &nativeyamux.StreamError{Remote: true}, "opaque": errors.New("stream reset by remote, error code: 0"),
		"wrapped": fmt.Errorf("wrapped: %w", zero), "joined": errors.Join(zero, syscall.ECONNRESET),
		"inner_wrapped": &network.StreamError{Remote: true, TransportError: fmt.Errorf("wrapped: %w", &nativeyamux.StreamError{Remote: true})},
		"inner_joined":  &network.StreamError{Remote: true, TransportError: errors.Join(&nativeyamux.StreamError{Remote: true}, syscall.ECONNRESET)},
		"tcp":           &network.StreamError{Remote: true, TransportError: &net.OpError{Op: "read", Net: "tcp", Err: syscall.ECONNRESET}},
		"errno38":       syscall.ENOSYS,
	}
	for _, direction := range []string{"read", "write"} {
		for name, cause := range cases {
			t.Run(direction+"/"+name, func(t *testing.T) {
				delegate := &pubsubScoringErrorUnitStream{pubsubScoringUnitStream: pubsubScoringUnitStream{
					conn: pubsubScoringUnitConnection(t), writeErr: cause}, nativeErr: cause}
				s, o := pubsubScoringUnitWrapped(t, delegate)
				if name != "preprepare" {
					pubsubScoringUnitPrepare(t, o)
				}
				primary := cause
				if name == "sticky" {
					primary = syscall.ENOSYS
					o.fail(primary)
				} else if name == "overflow" {
					for index := 0; index < pubsubScoringEvents; index++ {
						o.emit("snapshot", "go.unit.overflow", nil)
					}
					primary = o.failure
				} else if name == "unadmitted" {
					s.nativeYamux = false
				}
				var n int
				var native error
				if direction == "read" {
					n, native = s.Read(make([]byte, 1))
				} else {
					n, native = s.Write([]byte{0})
				}
				if n != 0 || native != cause || o.failure == nil || o.failure != primary ||
					len(pubsubScoringUnitEvents(o, "native_stream_io_finalized")) != 0 {
					t.Fatal("unknown/unprepared peer error changed or accepted")
				}
				before, _ := json.Marshal(pubsubScoringUnitEvents(o, "stream_io_terminal"))
				if name == "preprepare" {
					if err := o.prepareShutdown(pubsubScoringCommand{Sequence: 1, Kind: "prepare_shutdown",
						Actor: o.actor, Token: o.token, Local: o.local.String()}, 0); err == nil || o.prepared {
						t.Fatal("future prepare retroclassified observed peer cancellation")
					}
				}
				if err := s.Close(); err != nil {
					t.Fatal(err)
				}
				after, _ := json.Marshal(pubsubScoringUnitEvents(o, "stream_io_terminal"))
				if !bytes.Equal(before, after) || o.failure != primary {
					t.Fatal("close/future preparation changed earlier native failure")
				}
			})
		}
	}
}

type pubsubScoringPeerJoinUnitStream struct {
	pubsubScoringErrorUnitStream
	writeStarted, writeReturn chan struct{}
}

func (s *pubsubScoringPeerJoinUnitStream) Write(p []byte) (int, error) {
	close(s.writeStarted)
	<-s.writeReturn
	return s.pubsubScoringUnitStream.Write(p)
}

func TestPubsubScoringPeerZeroResetWaitsForOppositeIOWithoutBlockingNativeClose(t *testing.T) {
	for _, mode := range []string{"clean", "partial", "sticky"} {
		t.Run(mode, func(t *testing.T) {
			frame := pubsubScoringUnitFrame(t, nil)
			delegate := &pubsubScoringPeerJoinUnitStream{pubsubScoringErrorUnitStream: pubsubScoringErrorUnitStream{
				pubsubScoringUnitStream: pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t), writeN: len(frame)},
				nativeErr:               pubsubScoringUnitPeerReset()}, writeStarted: make(chan struct{}), writeReturn: make(chan struct{})}
			if mode == "partial" {
				delegate.writeN--
			}
			t.Cleanup(func() {
				select {
				case <-delegate.writeReturn:
				default:
					close(delegate.writeReturn)
				}
			})
			s, o := pubsubScoringUnitWrapped(t, delegate)
			written := make(chan struct{})
			var n int
			var native error
			go func() { n, native = s.Write(frame); close(written) }()
			pubsubScoringUnitWait(t, delegate.writeStarted)
			pubsubScoringUnitPrepare(t, o)
			if count, err := s.Read(make([]byte, 1)); count != 0 || err != delegate.nativeErr {
				t.Fatal("native peer Read result changed")
			}
			closed := make(chan error, 1)
			go func() { closed <- s.Close() }()
			select {
			case err := <-closed:
				if err != nil {
					t.Fatal(err)
				}
			case <-time.After(2 * time.Second):
				t.Fatal("native Close waited on an I/O gate")
			}
			if s.framingJoined() || len(pubsubScoringUnitEvents(o, "native_stream_io_finalized")) != 0 {
				t.Fatal("peer cancellation accepted unfinished opposite I/O")
			}
			if mode == "sticky" {
				o.fail(syscall.ENOSYS)
			}
			close(delegate.writeReturn)
			pubsubScoringUnitWait(t, written)
			final := pubsubScoringUnitEvents(o, "native_stream_io_finalized")
			if n != delegate.writeN || native != nil || !bytes.Equal(delegate.output.Bytes(), frame[:n]) ||
				len(final) != 1 || final[0]["accepted"] != (mode == "clean") || (o.failure != nil) != (mode != "clean") ||
				(mode == "sticky" && o.failure != syscall.ENOSYS) {
				t.Fatal("peer finalizer lost actual opposite prefix/join verdict")
			}
			pubsubScoringUnitDisposal(t, o, final[0])
		})
	}
}

func TestPubsubScoringRepeatCloseRequiresPriorSuccessAndCleanJoinedFraming(t *testing.T) {
	for _, mode := range []string{"valid", "valid_remote", "absent", "failed_reset", "superseded_reset", "active", "unadmitted", "partial", "nonzero", "bare", "opaque", "joined", "wrapped", "outer_network", "sticky"} {
		t.Run(mode, func(t *testing.T) {
			native := error(&nativeyamux.StreamError{})
			switch mode {
			case "nonzero":
				native = &nativeyamux.StreamError{ErrorCode: 7}
			case "bare":
				native = network.ErrReset
			case "opaque":
				native = errors.New("stream reset, error code: 0")
			case "joined":
				native = errors.Join(native, syscall.ECONNRESET)
			}
			switch mode {
			case "valid_remote":
				native = &nativeyamux.StreamError{Remote: true}
			case "wrapped":
				native = fmt.Errorf("wrapped: %w", native)
			case "outer_network":
				native = pubsubScoringUnitLocalReset()
			}
			delegate := &pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t), closeErr: native, writeN: -1}
			s, o := pubsubScoringUnitWrapped(t, delegate)
			if mode != "active" {
				pubsubScoringUnitPrepare(t, o)
			}
			if mode == "partial" {
				if n, err := s.Write([]byte{0x80}); n != 1 || err != nil {
					t.Fatal(n, err)
				}
			}
			if mode == "failed_reset" {
				delegate.resetErr = syscall.ECONNRESET
			}
			if mode != "absent" {
				if err := s.Reset(); err != delegate.resetErr {
					t.Fatal("Reset result changed")
				}
			}
			if mode == "unadmitted" {
				s.nativeYamux = false
			}
			if mode == "superseded_reset" {
				if err := s.ResetWithError(0); err != nil {
					t.Fatal(err)
				}
			}
			primary := o.failure
			if mode == "sticky" {
				primary = syscall.ECONNRESET
				o.fail(primary)
			}
			if err := s.Close(); err != native {
				t.Fatal("repeat native Close error changed")
			}
			final := pubsubScoringUnitEvents(o, "native_stream_close_finalized")
			valid := mode == "valid" || mode == "valid_remote"
			if valid || mode == "sticky" || mode == "partial" {
				if len(final) != 1 || final[0]["accepted"] != (mode != "partial") {
					t.Fatal("repeat Close lacks exact framing verdict")
				}
			} else if len(final) != 0 {
				t.Fatal("unknown Close accepted as repeated Reset")
			}
			if (valid && o.failure != nil) || (!valid && o.failure == nil) || (primary != nil && o.failure != primary) {
				t.Fatal("repeat Close cleared/changed first failure")
			}
		})
	}
}

func TestPubsubScoringRepeatCloseDoesNotWaitForIOOrBorrowInFlightReset(t *testing.T) {
	for _, resetInFlight := range []bool{false, true} {
		t.Run(fmt.Sprintf("reset_in_flight=%t", resetInFlight), func(t *testing.T) {
			nativeClose := error(&nativeyamux.StreamError{})
			delegate := &pubsubScoringResetUnitStream{pubsubScoringUnitStream: pubsubScoringUnitStream{
				conn: pubsubScoringUnitConnection(t), closeErr: nativeClose},
				ioStarted: make(chan struct{}), ioReturn: make(chan struct{}), resetStarted: make(chan struct{}), resetReturn: make(chan struct{}),
				nativeErr: pubsubScoringUnitLocalReset()}
			t.Cleanup(func() {
				for _, signal := range []chan struct{}{delegate.ioReturn, delegate.resetReturn} {
					select {
					case <-signal:
					default:
						close(signal)
					}
				}
			})
			s, o := pubsubScoringUnitWrapped(t, delegate)
			ioDone := make(chan struct{})
			var ioResult error
			go func() { _, ioResult = s.Read(make([]byte, 1)); close(ioDone) }()
			pubsubScoringUnitWait(t, delegate.ioStarted)
			pubsubScoringUnitPrepare(t, o)
			resetDone := make(chan struct{})
			var resetResult error
			go func() { resetResult = s.Reset(); close(resetDone) }()
			pubsubScoringUnitWait(t, delegate.resetStarted)
			if !resetInFlight {
				close(delegate.resetReturn)
				pubsubScoringUnitWait(t, resetDone)
			}
			closeDone := make(chan error, 1)
			go func() { closeDone <- s.Close() }()
			select {
			case err := <-closeDone:
				if err != nativeClose {
					t.Fatal("native Close result changed")
				}
			case <-time.After(2 * time.Second):
				t.Fatal("Close waited for an I/O or Reset gate")
			}
			if s.framingJoined() || len(pubsubScoringUnitEvents(o, "native_stream_close_finalized")) != 0 {
				t.Fatal("Close accepted unfinished I/O")
			}
			if resetInFlight {
				close(delegate.resetReturn)
				pubsubScoringUnitWait(t, resetDone)
			}
			close(delegate.ioReturn)
			pubsubScoringUnitWait(t, ioDone)
			if resetResult != nil || ioResult != delegate.nativeErr {
				t.Fatal("native results changed")
			}
			final := pubsubScoringUnitEvents(o, "native_stream_close_finalized")
			if resetInFlight {
				if len(final) != 1 || final[0]["accepted"] != true || final[0]["outcome"] != "concurrent_reset_close_pending" ||
					final[0]["terminal_state_cause"] != "unknown" || final[0]["native_close_succeeded"] != false || o.failure != nil {
					t.Fatal("joined overlap lacks its distinct unknown-cause observation")
				}
				if _, causal := final[0]["causal_reset_receipt_sequence"]; causal {
					t.Fatal("overlapping Reset was promoted to a prior causal Reset")
				}
			} else if len(final) != 1 || final[0]["accepted"] != true || o.failure != nil {
				t.Fatal("joined clean repeat Close not resolved")
			}
		})
	}
}

func TestPubsubScoringConcurrentResetCloseKeepsUnknownCauseAndStrictFailures(t *testing.T) {
	for _, mode := range []string{"clean", "partial", "remote", "nonzero", "wrapped", "joined", "outer_network", "half_close", "errno", "preprepare", "reset_preprepare",
		"sticky", "sticky_after", "quiesced", "quiesced_after", "quiesced_sticky", "overflow", "unadmitted", "reset_failure", "wrong_ack", "superseded", "explicit_reset"} {
		t.Run(mode, func(t *testing.T) {
			native := error(&nativeyamux.StreamError{})
			switch mode {
			case "remote":
				native = &nativeyamux.StreamError{Remote: true}
			case "nonzero":
				native = &nativeyamux.StreamError{ErrorCode: 7}
			case "wrapped":
				native = fmt.Errorf("wrapped: %w", native)
			case "joined":
				native = errors.Join(native, syscall.ECONNRESET)
			case "outer_network":
				native = pubsubScoringUnitLocalReset()
			case "errno":
				native = syscall.ECONNRESET
			}
			delegate := &pubsubScoringResetUnitStream{pubsubScoringUnitStream: pubsubScoringUnitStream{
				conn: pubsubScoringUnitConnection(t), closeErr: native}, nativeErr: io.EOF,
				ioStarted: make(chan struct{}), ioReturn: make(chan struct{}), resetStarted: make(chan struct{}), resetReturn: make(chan struct{})}
			if mode == "partial" {
				delegate.prefix = []byte{0x80}
			}
			if mode == "reset_failure" {
				delegate.resetErr = syscall.ENOSYS
			}
			s, o := pubsubScoringUnitWrapped(t, delegate)
			ioDone, resetDone, closeDone := make(chan struct{}), make(chan struct{}), make(chan struct{})
			var ioOnce, resetOnce sync.Once
			unblock := func() {
				ioOnce.Do(func() { close(delegate.ioReturn) })
				resetOnce.Do(func() { close(delegate.resetReturn) })
			}
			var readN int
			var readErr, resetErr, closeErr error
			go func() { defer close(ioDone); readN, readErr = s.Read(make([]byte, 1)) }()
			t.Cleanup(func() { unblock(); pubsubScoringUnitCleanupJoin(ioDone) })
			pubsubScoringUnitWait(t, delegate.ioStarted)
			if mode != "preprepare" && mode != "reset_preprepare" {
				pubsubScoringUnitPrepare(t, o)
			}
			go func() { defer close(resetDone); resetErr = s.Reset() }()
			t.Cleanup(func() { unblock(); pubsubScoringUnitCleanupJoin(resetDone) })
			pubsubScoringUnitWait(t, delegate.resetStarted)
			if mode == "reset_preprepare" {
				pubsubScoringUnitPrepare(t, o)
			}
			var primary error
			switch mode {
			case "sticky":
				primary = syscall.ECONNRESET
				o.fail(primary)
			case "quiesced":
				o.quiesced = true
			case "quiesced_sticky":
				o.quiesced = true
				primary = syscall.ENOSYS
				o.fail(primary)
			case "overflow":
				o.overflow = true
			case "unadmitted":
				s.nativeYamux = false
			case "wrong_ack":
				o.prepareAck++
			case "explicit_reset":
				if err := s.ResetWithError(0); err != nil {
					t.Fatal(err)
				}
			}
			go func() {
				defer close(closeDone)
				if mode == "half_close" {
					closeErr = s.CloseWrite()
				} else {
					closeErr = s.Close()
				}
			}()
			t.Cleanup(func() { unblock(); pubsubScoringUnitCleanupJoin(closeDone) })
			pubsubScoringUnitWait(t, closeDone)
			if closeErr != native || s.framingJoined() || len(pubsubScoringUnitEvents(o, "native_stream_close_finalized")) != 0 {
				t.Fatal("Close altered its native result or waited/invented join")
			}
			var observed map[string]any
			for _, event := range pubsubScoringUnitEvents(o, "native_stream_operation") {
				if event["operation"] == "stream_close" || mode == "half_close" && event["operation"] == "stream_close_write" {
					observed = event
				}
			}
			pending := mode == "clean" || mode == "partial" || mode == "reset_failure" || mode == "superseded" || mode == "sticky_after" || mode == "quiesced_after"
			if observed == nil || (observed["outcome"] == "concurrent_reset_close_pending") != pending {
				t.Fatal("unrelated Close error was deferred", observed)
			}
			before, _ := json.Marshal(observed)
			if mode == "sticky_after" {
				primary = syscall.ECONNRESET
				o.fail(primary)
			}
			if mode == "quiesced_after" {
				o.quiesced = true
			}
			if mode == "superseded" {
				newer := s.beginOperation("stream_reset", true)
				s.endClose(newer, -1, s.returnedOperation(newer, delegate.pubsubScoringUnitStream.Reset()), nil)
			}
			resetOnce.Do(func() { close(delegate.resetReturn) })
			pubsubScoringUnitWait(t, resetDone)
			if len(pubsubScoringUnitEvents(o, "native_stream_close_finalized")) != 0 {
				t.Fatal("successful Reset invented the unfinished I/O join")
			}
			ioOnce.Do(func() { close(delegate.ioReturn) })
			pubsubScoringUnitWait(t, ioDone)
			if resetErr != delegate.resetErr || readN != len(delegate.prefix) || readErr != io.EOF {
				t.Fatal("native Reset/Read results changed")
			}
			final := pubsubScoringUnitEvents(o, "native_stream_close_finalized")
			if pending {
				if len(final) != 1 || final[0]["accepted"] != (mode == "clean") || final[0]["native_close_succeeded"] != false ||
					final[0]["terminal_state_cause"] != "unknown" {
					t.Fatal("overlap bypassed full Reset/framing proof", final)
				}
				if mode == "clean" {
					pubsubScoringUnitDisposal(t, o, final[0])
					if final[0]["observed_reset_receipt_sequence"] != final[0]["owner_disposal_receipt_sequence"] {
						t.Fatal("observation used unrelated disposal")
					}
					seal, ok := final[0]["finalization_order"].(uint64)
					if !ok || seal <= final[0]["returned_order"].(uint64) || seal <= final[0]["observed_reset_returned_order"].(uint64) {
						t.Fatal("new finalizer lacks its strictly later native-order join seal")
					}
				}
				if _, causal := final[0]["causal_reset_receipt_sequence"]; causal {
					t.Fatal("unknown-cause observation fabricated a causal Reset")
				}
			} else if len(final) != 0 {
				t.Fatal("unrelated native error received an overlap finalizer")
			}
			after, _ := json.Marshal(observed)
			if !bytes.Equal(before, after) || (o.failure == nil) != (mode == "clean") || primary != nil && o.failure != primary ||
				mode == "reset_failure" && o.failure != delegate.resetErr {
				t.Fatal("finalization changed raw evidence or cleared/replaced the first error")
			}
			if _, mutated := observed["finalization_order"]; mutated {
				t.Fatal("finalization mutated the original native Close observation")
			}
		})
	}
}

func TestPubsubScoringConcurrentResetClosePinsLateSuccessfulResetPublication(t *testing.T) {
	delegate := &pubsubScoringResetUnitStream{pubsubScoringUnitStream: pubsubScoringUnitStream{
		conn: pubsubScoringUnitConnection(t), closeErr: &nativeyamux.StreamError{}},
		resetStarted: make(chan struct{}), resetReturn: make(chan struct{})}
	s, o := pubsubScoringUnitWrapped(t, delegate)
	pubsubScoringUnitPrepare(t, o)
	reset := s.beginOperation("stream_reset", true)
	returned := make(chan struct{})
	var resetResult pubsubScoringOperationReturn
	var releaseOnce sync.Once
	unblock := func() { releaseOnce.Do(func() { close(delegate.resetReturn) }) }
	go func() { defer close(returned); resetResult = s.returnedOperation(reset, delegate.Reset()) }()
	t.Cleanup(func() { unblock(); pubsubScoringUnitCleanupJoin(returned) })
	pubsubScoringUnitWait(t, delegate.resetStarted)
	closeOp := s.beginOperation("stream_close", true)
	closeResult := s.returnedOperation(closeOp, delegate.Close())
	if closeResult.err != delegate.closeErr || !closeOp.resetPending || closeOp.reset != reset.reset {
		t.Fatal("Close did not preserve its active Reset-at-BEGIN observation")
	}
	unblock()
	pubsubScoringUnitWait(t, returned)
	if resetResult.err != nil || reset.reset.sequence != 0 || resetResult.order <= closeResult.order {
		t.Fatal("mock native return order/result changed")
	}
	s.endClose(closeOp, -1, closeResult, nil)
	raw := pubsubScoringUnitEvents(o, "native_stream_operation")
	if len(raw) != 1 || raw[0]["outcome"] != "concurrent_reset_close_pending" || raw[0]["reset_returned_order"] != uint64(0) ||
		raw[0]["reset_receipt_sequence"] != 0 || len(pubsubScoringUnitEvents(o, "native_stream_close_finalized")) != 0 || s.framingJoined() {
		t.Fatal("delayed Reset publication fabricated completion", raw)
	}
	before, _ := json.Marshal(raw[0])
	s.endClose(reset, -1, resetResult, nil)
	final := pubsubScoringUnitEvents(o, "native_stream_close_finalized")
	after, _ := json.Marshal(raw[0])
	if len(final) != 1 || final[0]["accepted"] != true || final[0]["observed_reset_receipt_sequence"] != reset.reset.sequence ||
		final[0]["observed_reset_returned_order"] != resetResult.order || final[0]["owner_disposal_receipt_sequence"] != reset.reset.sequence ||
		reset.reset.sequence <= raw[0]["sequence"].(int) || !bytes.Equal(before, after) || o.failure != nil || !s.framingJoined() {
		t.Fatal("late publication changed immutable unknown-cause Close or lost exact full disposal")
	}
	seal, ok := final[0]["finalization_order"].(uint64)
	if !ok || seal <= closeResult.order || seal <= resetResult.order || seal != s.operationOrder {
		t.Fatal("joined/latest-Reset snapshot did not seal a unique native order")
	}
	pubsubScoringUnitDisposal(t, o, final[0])
}

type pubsubScoringPhaseUnitStream struct {
	pubsubScoringUnitStream
	closeStarted, closeReturn chan struct{}
	nativeReadErr             error
}

func (s *pubsubScoringPhaseUnitStream) Read([]byte) (int, error) { return 0, s.nativeReadErr }

func (s *pubsubScoringPhaseUnitStream) Close() error {
	close(s.closeStarted)
	<-s.closeReturn
	return s.closeErr
}

func TestPubsubScoringCloseNeverBorrowsResetStartedAfterItsBegin(t *testing.T) {
	delegate := &pubsubScoringPhaseUnitStream{pubsubScoringUnitStream: pubsubScoringUnitStream{
		conn: pubsubScoringUnitConnection(t), closeErr: &nativeyamux.StreamError{}},
		closeStarted: make(chan struct{}), closeReturn: make(chan struct{})}
	s, o := pubsubScoringUnitWrapped(t, delegate)
	pubsubScoringUnitPrepare(t, o)
	done := make(chan struct{})
	var result error
	var releaseOnce sync.Once
	unblock := func() { releaseOnce.Do(func() { close(delegate.closeReturn) }) }
	go func() { defer close(done); result = s.Close() }()
	t.Cleanup(func() { unblock(); pubsubScoringUnitCleanupJoin(done) })
	pubsubScoringUnitWait(t, delegate.closeStarted)
	if err := s.Reset(); err != nil {
		t.Fatal("native later Reset changed", err)
	}
	unblock()
	pubsubScoringUnitWait(t, done)
	if result != delegate.closeErr || o.failure != result || len(pubsubScoringUnitEvents(o, "native_stream_close_finalized")) != 0 {
		t.Fatal("future successful Reset reinterpreted unrelated Close")
	}
}

func TestPubsubScoringRepeatClosePinsNativeReturnBeforeResetReceiptPublication(t *testing.T) {
	for _, newerAfterBegin := range []bool{false, true} {
		t.Run(fmt.Sprintf("newer_after_begin=%t", newerAfterBegin), func(t *testing.T) {
			delegate := &pubsubScoringPhaseUnitStream{pubsubScoringUnitStream: pubsubScoringUnitStream{
				conn: pubsubScoringUnitConnection(t), closeErr: &nativeyamux.StreamError{}},
				closeStarted: make(chan struct{}), closeReturn: make(chan struct{}), nativeReadErr: pubsubScoringUnitLocalReset()}
			t.Cleanup(func() {
				select {
				case <-delegate.closeReturn:
				default:
					close(delegate.closeReturn)
				}
			})
			s, o := pubsubScoringUnitWrapped(t, delegate)
			pubsubScoringUnitPrepare(t, o)
			// Exercise the real return-phase helper with the delegate's native result.
			reset := s.beginOperation("stream_reset", true)
			result := s.returnedOperation(reset, delegate.Reset())
			if result.err != nil || !reset.reset.completed || reset.reset.err != nil ||
				reset.reset.returned != result.order || reset.reset.sequence != 0 {
				t.Fatal("native return phase depends on receipt publication")
			}
			if n, err := s.Read(make([]byte, 1)); n != 0 || err != delegate.nativeReadErr || o.failure != nil {
				t.Fatal("I/O lost completed but unpublished Reset", n, err)
			}
			closed := make(chan error, 1)
			go func() { closed <- s.Close() }()
			pubsubScoringUnitWait(t, delegate.closeStarted)
			if newerAfterBegin {
				if err := s.Reset(); err != nil {
					t.Fatal(err)
				}
			}
			close(delegate.closeReturn)
			select {
			case err := <-closed:
				if err != delegate.closeErr {
					t.Fatal("native Close result changed")
				}
			case <-time.After(2 * time.Second):
				t.Fatal("Close waited for Reset receipt publication")
			}
			var closeReceipt map[string]any
			for _, event := range pubsubScoringUnitEvents(o, "native_stream_operation") {
				if event["operation"] == "stream_close" {
					closeReceipt = event
				}
			}
			if closeReceipt == nil || closeReceipt["outcome"] != "repeat_close_pending" ||
				closeReceipt["reset_receipt_sequence"] != 0 || closeReceipt["reset_started_order"] != reset.started ||
				closeReceipt["reset_returned_order"] != result.order || o.failure != nil || s.framingJoined() ||
				len(pubsubScoringUnitEvents(o, "native_stream_close_finalized")) != 0 {
				t.Fatal("unpublished native Reset was lost or falsely joined")
			}
			before, _ := json.Marshal(closeReceipt)
			s.endClose(reset, -1, result, nil)
			final := pubsubScoringUnitEvents(o, "native_stream_close_finalized")
			ioFinal := pubsubScoringUnitEvents(o, "native_stream_io_finalized")
			after, _ := json.Marshal(closeReceipt)
			if len(final) != 1 || final[0]["accepted"] != true ||
				final[0]["causal_reset_receipt_sequence"] != reset.reset.sequence ||
				final[0]["causal_reset_returned_order"] != result.order ||
				final[0]["operation_receipt_sequence"] != closeReceipt["sequence"] ||
				reset.reset.sequence <= closeReceipt["sequence"].(int) || !bytes.Equal(before, after) ||
				o.failure != nil || !s.framingJoined() {
				t.Fatal("late Reset receipt changed pinned Close evidence")
			}
			if len(ioFinal) != 1 || ioFinal[0]["accepted"] != true ||
				ioFinal[0]["causal_reset_receipt_sequence"] != reset.reset.sequence {
				t.Fatal("I/O finalizer lost the same unpublished native Reset")
			}
			if newerAfterBegin && s.reset == reset.reset {
				t.Fatal("late publication overwrote the latest Reset START identity")
			}
		})
	}
}

func TestPubsubScoringDelayedResetPublicationNeverAllowsOldAttemptFallback(t *testing.T) {
	for _, mode := range []string{"new_start", "reset_with_error", "failed_reset", "partial", "sticky"} {
		t.Run(mode, func(t *testing.T) {
			delegate := &pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t),
				closeErr: &nativeyamux.StreamError{}, writeN: -1}
			s, o := pubsubScoringUnitWrapped(t, delegate)
			pubsubScoringUnitPrepare(t, o)
			if mode == "partial" {
				if n, err := s.Write([]byte{0x80}); n != 1 || err != nil {
					t.Fatal("native prefix changed", n, err)
				}
			}
			reset := s.beginOperation("stream_reset", true)
			result := s.returnedOperation(reset, delegate.Reset())
			if result.err != nil {
				t.Fatal(result.err)
			}
			var newer pubsubScoringOperation
			var newerResult pubsubScoringOperationReturn
			var primary error
			switch mode {
			case "new_start", "failed_reset":
				newer = s.beginOperation("stream_reset", true)
				if mode == "failed_reset" {
					delegate.resetErr = syscall.ENOSYS
					newerResult = s.returnedOperation(newer, delegate.Reset())
					primary = delegate.resetErr
					if !newer.reset.completed || newer.reset.err != primary || newer.reset.sequence != 0 || o.failure != primary {
						t.Fatal("failed native return was not captured atomically before publication")
					}
				}
			case "reset_with_error":
				if err := s.ResetWithError(0); err != nil {
					t.Fatal(err)
				}
			case "sticky":
				primary = syscall.ECONNRESET
				o.fail(primary)
			}
			if err := s.Close(); err != delegate.closeErr {
				t.Fatal("native Close result changed")
			}
			var closeReceipt map[string]any
			var before []byte
			if mode == "new_start" {
				for _, event := range pubsubScoringUnitEvents(o, "native_stream_operation") {
					if event["operation"] == "stream_close" {
						closeReceipt = event
					}
				}
				if closeReceipt == nil || closeReceipt["outcome"] != "concurrent_reset_close_pending" ||
					closeReceipt["reset_started_order"] != newer.started || closeReceipt["reset_returned_order"] != uint64(0) ||
					closeReceipt["reset_receipt_sequence"] != 0 || closeReceipt["native_close_succeeded"] != false ||
					closeReceipt["terminal_state_cause"] != "unknown" || o.failure != nil {
					t.Fatal("Close borrowed the old returned Reset instead of capturing the latest active attempt")
				}
				before, _ = json.Marshal(closeReceipt)
				// Even real old disposal/publication cannot authorize the still-pending new attempt.
				s.endClose(reset, -1, result, nil)
				if !s.disposed || s.disposalReceipt != reset.reset.sequence || s.framingJoined() ||
					len(pubsubScoringUnitEvents(o, "native_stream_close_finalized")) != 0 || o.failure != nil {
					t.Fatal("old successful Reset invented the pending new Reset's disposal/join authority")
				}
				newerResult = s.returnedOperation(newer, delegate.Reset())
			}
			if newer.reset != nil {
				s.endClose(newer, -1, newerResult, nil)
			}
			if mode != "new_start" {
				s.endClose(reset, -1, result, nil)
			}
			final := pubsubScoringUnitEvents(o, "native_stream_close_finalized")
			if mode == "new_start" {
				after, _ := json.Marshal(closeReceipt)
				if len(final) != 1 || final[0]["accepted"] != true || final[0]["outcome"] != "concurrent_reset_close_pending" ||
					final[0]["native_close_succeeded"] != false || final[0]["terminal_state_cause"] != "unknown" ||
					final[0]["error"] != closeReceipt["error"] || final[0]["error_type"] != closeReceipt["error_type"] ||
					final[0]["observed_reset_receipt_sequence"] != newer.reset.sequence ||
					final[0]["observed_reset_returned_order"] != newerResult.order ||
					final[0]["owner_disposal_receipt_sequence"] != newer.reset.sequence ||
					final[0]["owner_disposal_receipt_sequence"] == reset.reset.sequence ||
					final[0]["operation_receipt_sequence"] != closeReceipt["sequence"] ||
					!bytes.Equal(before, after) || !s.framingJoined() || o.failure != nil {
					t.Fatal("concurrent Close finalized using old/reset fallback instead of the exact new successful disposal")
				}
				if _, causal := final[0]["causal_reset_receipt_sequence"]; causal {
					t.Fatal("concurrent observation borrowed old causal Reset authority")
				}
				pubsubScoringUnitDisposal(t, o, final[0])
			} else if mode == "partial" || mode == "sticky" {
				if len(final) != 1 || final[0]["accepted"] != (mode == "sticky") {
					t.Fatal("delayed publication bypassed framing finalization")
				}
			} else if len(final) != 0 {
				t.Fatal("old successful Reset authorized a superseded Close")
			}
			if mode != "new_start" && (o.failure == nil || (primary != nil && o.failure != primary)) {
				t.Fatal("delayed publication cleared or changed first failure")
			}
		})
	}
}

func TestPubsubScoringObservedErrorCannotAcquireLatePrepareOrReset(t *testing.T) {
	primary := error(syscall.ECONNRESET)
	delegate := &pubsubScoringErrorUnitStream{pubsubScoringUnitStream: pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t)}, nativeErr: primary}
	s, o := pubsubScoringUnitWrapped(t, delegate)
	if _, err := s.Read(make([]byte, 1)); err != primary {
		t.Fatal("native error changed")
	}
	if err := s.Reset(); err != nil {
		t.Fatal(err)
	}
	if err := o.prepareShutdown(pubsubScoringCommand{Sequence: 1, Kind: "prepare_shutdown", Actor: o.actor, Token: o.token, Local: o.local.String()}, 0); err == nil || o.failure != primary || o.prepared || len(pubsubScoringUnitEvents(o, "shutdown_prepared")) != 0 {
		t.Fatal("late prepare cleared observed native failure")
	}
}

func TestPubsubScoringConnectionRejectsMissingWrongAndPrivateOwners(t *testing.T) {
	c := pubsubScoringUnitConnection(t)
	if _, err := pubsubScoringConnection(c, nil); err != nil {
		t.Fatal(err)
	}
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	if err := o.connection(c, nil, strings.Repeat("b", 64)); err == nil {
		t.Fatal("private config alone became owner proof")
	}
	c.key = nil
	if _, err := pubsubScoringConnection(c, nil); err == nil {
		t.Fatal("accepted missing key")
	}
	c = pubsubScoringUnitConnection(t)
	c.remote = peer.ID("foreign")
	if _, err := pubsubScoringConnection(c, nil); err == nil {
		t.Fatal("accepted key/peer mismatch")
	}
	c = pubsubScoringUnitConnection(t)
	c.state.Security = "/tls/1.0.0"
	if _, err := pubsubScoringConnection(c, nil); err == nil {
		t.Fatal("accepted configured unsupported TCP security")
	}
	c = pubsubScoringUnitConnection(t)
	c.state.Transport = "quic-v1"
	if _, err := pubsubScoringConnection(c, nil); err == nil {
		t.Fatal("accepted QUIC classification without an actual secured callback")
	}
	if _, err := pubsubScoringConnection(nil, nil); err == nil {
		t.Fatal("accepted no native connection")
	}
}

func TestPubsubScoringConnectionRecordedOnceAndConflictingIDRejected(t *testing.T) {
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	c := pubsubScoringUnitConnection(t)
	if err := o.connection(c, nil, ""); err != nil {
		t.Fatal(err)
	}
	if err := o.connection(c, nil, ""); err != nil {
		t.Fatal(err)
	}
	if len(o.events) != 1 || o.events[0]["kind"] != "connection" || o.failure != nil ||
		o.events[0]["source"] != "go.network.Conn.authenticated_output" ||
		o.events[0]["connection_state_source"] != "go-libp2p.network.Conn.ConnState" {
		t.Fatal("ambiguous connection receipt")
	}
	other := pubsubScoringUnitConnection(t)
	if err := o.connection(other, nil, ""); err == nil {
		t.Fatal("accepted reused foreign connection ID")
	}
}

func TestPubsubScoringEventProvenanceCannotBeOverwritten(t *testing.T) {
	for _, key := range []string{"source", "kind", "sequence", "mono_ns"} {
		o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
		o.emit("rpc", "go.pubsub.native_stream.read", map[string]any{key: "foreign"})
		if o.failure == nil || len(o.events) != 0 {
			t.Fatalf("reserved event provenance %s was replaced", key)
		}
	}
}

func TestPubsubScoringSampleDoesNotInventUninspectedScores(t *testing.T) {
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	o.Graft(peer.ID("unit-peer"), o.topic)
	ctx, cancel := context.WithDeadline(context.Background(), time.Now().Add(-time.Second))
	defer cancel()
	if err := o.sample(ctx, "before"); !errors.Is(err, context.DeadlineExceeded) || len(o.events) != 1 || o.scoreSequence != 0 {
		t.Fatal("sample invented scores before native inspection")
	}
}

type pubsubScoringUnitSampleContext struct {
	context.Context
	waiting chan struct{}
	once    sync.Once
}

func (c *pubsubScoringUnitSampleContext) Done() <-chan struct{} {
	c.once.Do(func() { close(c.waiting) })
	return c.Context.Done()
}

func pubsubScoringUnitSample(t *testing.T, o *pubsubScoringObserver, label string) (<-chan error, context.CancelFunc) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	t.Cleanup(cancel)
	observed := &pubsubScoringUnitSampleContext{Context: ctx, waiting: make(chan struct{})}
	done := make(chan error, 1)
	go func() { done <- o.sample(observed, label) }()
	select {
	case <-observed.waiting:
	case err := <-done:
		t.Fatalf("sample returned before awaiting a new inspection: %v", err)
	case <-ctx.Done():
		t.Fatal("sample did not start its bounded inspection wait")
	}
	return done, cancel
}

func pubsubScoringUnitSampleResult(t *testing.T, done <-chan error) error {
	t.Helper()
	select {
	case err := <-done:
		return err
	case <-time.After(3 * time.Second):
		t.Fatal("sample did not finish its bounded inspection wait")
		return nil
	}
}

func TestPubsubScoringSampleWaitsForFreshInspectionEvenWhenScoreIsUnchanged(t *testing.T) {
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	p := peer.ID("unit-peer")
	values := map[peer.ID]*pubsub.PeerScoreSnapshot{p: {Score: 0,
		Topics: map[string]*pubsub.TopicScoreSnapshot{o.topic: {InvalidMessageDeliveries: 0}}}}
	o.inspect(values)
	previous := o.scoreSequence
	done, _ := pubsubScoringUnitSample(t, o, "ignored")
	select {
	case err := <-done:
		t.Fatalf("sample reused the cached inspection: %v", err)
	default:
	}
	if len(o.events) != 2 || o.events[1]["label"] != "periodic" {
		t.Fatal("named sample was emitted without a fresh inspection")
	}
	o.inspect(values)
	if err := pubsubScoringUnitSampleResult(t, done); err != nil {
		t.Fatal(err)
	}
	snapshot := o.events[len(o.events)-1]
	sequence := snapshot["score_observation_sequence"].(int)
	if snapshot["label"] != "ignored" || sequence <= previous || sequence >= snapshot["sequence"].(int) ||
		o.events[sequence-1]["kind"] != "score" || o.events[sequence-1]["source"] != "go.pubsub.WithPeerScoreInspect" {
		t.Fatal("sample lacks its exact newer native score observation")
	}
	if snapshot["peer_scores"].([]map[string]any)[0]["value"] != float64(0) {
		t.Fatal("sample changed an unchanged inspected score")
	}
}

func TestPubsubScoringSampleDoesNotUseInspectionStartedBeforeRequest(t *testing.T) {
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	values := map[peer.ID]*pubsub.PeerScoreSnapshot{"unit": {Score: 0}}
	// Unit-only ordering model: callback entry precedes the request, publication follows it.
	older := o.inspectionStarted.Add(1)
	done, _ := pubsubScoringUnitSample(t, o, "ignored")
	o.inspectStarted(older, values)
	select {
	case err := <-done:
		t.Fatalf("pre-request callback satisfied the sample: %v", err)
	default:
	}
	if len(o.events) != 2 || o.events[1]["label"] != "periodic" {
		t.Fatal("pre-request inspection became a named snapshot")
	}
	previous := o.scoreSequence
	o.inspect(values)
	if err := pubsubScoringUnitSampleResult(t, done); err != nil {
		t.Fatal(err)
	}
	if o.events[len(o.events)-1]["score_observation_sequence"].(int) <= previous {
		t.Fatal("sample did not bind the post-request callback")
	}
}

func TestPubsubScoringOlderInspectionCompletionDoesNotReplaceNewerValues(t *testing.T) {
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	// Unit-only callback ordering model; both native-shaped snapshots remain in the raw trace.
	older := o.inspectionStarted.Add(1)
	newer := o.inspectionStarted.Add(1)
	o.inspectStarted(newer, map[peer.ID]*pubsub.PeerScoreSnapshot{"unit": {Score: -100}})
	sequence := o.scoreSequence
	o.inspectStarted(older, map[peer.ID]*pubsub.PeerScoreSnapshot{"unit": {Score: 0}})
	if o.scoreSequence != sequence || o.scoreInspection != newer || o.scores[0]["value"] != float64(-100) {
		t.Fatal("older callback completion replaced newer inspected values")
	}
	if len(o.events) != 3 || o.events[2]["kind"] != "score" ||
		o.events[2]["peer_scores"].([]map[string]any)[0]["value"] != float64(0) {
		t.Fatal("older actual observation disappeared from the raw trace")
	}
}

func TestPubsubScoringSampleCancellationDoesNotPublishCachedScores(t *testing.T) {
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	o.inspect(map[peer.ID]*pubsub.PeerScoreSnapshot{"unit": {Score: -100}})
	done, cancel := pubsubScoringUnitSample(t, o, "penalized")
	cancel()
	if err := pubsubScoringUnitSampleResult(t, done); !errors.Is(err, context.Canceled) || len(o.events) != 2 {
		t.Fatal("canceled sample published a stale named snapshot", err)
	}
}

func TestPubsubScoringSampleDeadlineDoesNotPublishCachedScores(t *testing.T) {
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	o.inspect(map[peer.ID]*pubsub.PeerScoreSnapshot{"unit": {Score: -100}})
	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()
	if err := o.sample(ctx, "penalized"); !errors.Is(err, context.DeadlineExceeded) || len(o.events) != 2 {
		t.Fatal("expired sample published a stale named snapshot", err)
	}
}

func TestPubsubScoringSampleInspectorFailureWakesWaitWithoutSnapshot(t *testing.T) {
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	done, _ := pubsubScoringUnitSample(t, o, "before")
	o.inspect(map[peer.ID]*pubsub.PeerScoreSnapshot{"unit": {Score: math.NaN()}})
	if err := pubsubScoringUnitSampleResult(t, done); err == nil || err != o.failure || len(o.events) != 0 {
		t.Fatal("failed inspection fabricated a named snapshot", err)
	}
}

func TestPubsubScoringSampleClosingObserverWakesWaitWithoutSnapshot(t *testing.T) {
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	done, _ := pubsubScoringUnitSample(t, o, "before")
	o.mu.Lock()
	o.closing = true
	o.notifyScoreLocked()
	o.mu.Unlock()
	if err := pubsubScoringUnitSampleResult(t, done); err == nil || len(o.events) != 0 {
		t.Fatal("closing observer fabricated a named snapshot", err)
	}
}

func TestPubsubScoringHostRequiresActualSelectedProtocolAndUniqueStream(t *testing.T) {
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	h := &pubsubScoringHost{observer: o, drain: newPubsubScoringDrain(), protocol: pubsub.GossipSubID_v11}
	s := &pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t)}
	if _, err := h.wrap(nil); err == nil {
		t.Fatal("accepted absent stream")
	}
	if _, err := h.wrap(s); err == nil || len(o.events) != 1 {
		t.Fatal("configured protocol became selection proof")
	}
	h.protocol = pubsub.GossipSubID_v10
	wrapped, err := h.wrap(s)
	if err != nil || wrapped == nil || len(o.events) != 2 || o.events[1]["source"] != "go.network.Stream.Protocol" {
		t.Fatal("missing native-selected stream owner", err)
	}
	if _, err = h.wrap(s); err == nil || len(o.events) != 2 {
		t.Fatal("duplicate stream became ambiguous protocol proof")
	}
}

func TestPubsubScoringSnapshotsAreDerivedAndInspectCopiesNativeValues(t *testing.T) {
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	p := peer.ID("unit-peer")
	o.Graft(p, o.topic)
	native := &pubsub.PeerScoreSnapshot{Score: -100, Topics: map[string]*pubsub.TopicScoreSnapshot{o.topic: {InvalidMessageDeliveries: 1}}}
	o.inspect(map[peer.ID]*pubsub.PeerScoreSnapshot{p: native})
	native.Score, native.Topics[o.topic].InvalidMessageDeliveries = 999, 999
	if o.events[2]["kind"] != "snapshot" || o.events[2]["label"] != "periodic" {
		t.Fatal("missing periodic native observation")
	}
	done, _ := pubsubScoringUnitSample(t, o, "penalized")
	fresh := &pubsub.PeerScoreSnapshot{Score: -100, Topics: map[string]*pubsub.TopicScoreSnapshot{o.topic: {InvalidMessageDeliveries: 1}}}
	o.inspect(map[peer.ID]*pubsub.PeerScoreSnapshot{p: fresh})
	fresh.Score, fresh.Topics[o.topic].InvalidMessageDeliveries = 999, 999
	if err := pubsubScoringUnitSampleResult(t, done); err != nil {
		t.Fatal(err)
	}
	snapshot := o.events[len(o.events)-1]
	values := snapshot["peer_scores"].([]map[string]any)
	if values[0]["value"] != float64(-100) || values[0]["invalid_deliveries"] != float64(1) ||
		!strings.HasPrefix(snapshot["mesh_basis"].(string), "derived_") || snapshot["score_observation_sequence"] != 4 {
		t.Fatal("snapshot fabricated/reused mutated native fields")
	}
	o.Prune(p, o.topic)
	done, _ = pubsubScoringUnitSample(t, o, "repaired")
	o.inspect(map[peer.ID]*pubsub.PeerScoreSnapshot{p: {Score: -100}})
	if err := pubsubScoringUnitSampleResult(t, done); err != nil {
		t.Fatal(err)
	}
	if len(o.events[len(o.events)-1]["mesh_peer_ids"].([]string)) != 0 {
		t.Fatal("native PRUNE did not update ledger")
	}
}

func TestPubsubScoringInvalidScoresAndOverflowStayFailed(t *testing.T) {
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	o.inspect(map[peer.ID]*pubsub.PeerScoreSnapshot{"unit": {Score: math.NaN()}})
	if o.failure == nil {
		t.Fatal("accepted nonfinite score")
	}
	first := o.failure
	o.fail(errors.New("later"))
	if o.failure != first {
		t.Fatal("first failure overwritten")
	}
	o = newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	o.emit("unit", "go.unit", map[string]any{"oversized": strings.Repeat("x", pubsubScoringEventBytes)})
	if !o.overflow || o.failure == nil || len(o.events) != 0 {
		t.Fatal("oversized event accepted")
	}
	o = newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	for i := 0; i <= pubsubScoringEvents; i++ {
		o.emit("unit", "go.unit", nil)
	}
	if !o.overflow || o.failure == nil || len(o.events) != pubsubScoringEvents {
		t.Fatal("event bound was not sticky")
	}
	o = newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	o.traceBytes = pubsubScoringTraceBytes
	o.emit("unit", "go.unit", nil)
	if !o.overflow || o.failure == nil || len(o.events) != 0 {
		t.Fatal("trace byte bound was bypassed")
	}
}

func TestPubsubScoringDrainWaitsForAdmittedWorkAndClosesAdmission(t *testing.T) {
	drain := newPubsubScoringDrain()
	delegate := &pubsubScoringUnitStream{reset: make(chan struct{}, 1)}
	s := &pubsubScoringStream{Stream: delegate, observer: newPubsubScoringObserver("victim", strings.Repeat("a", 32)), drain: drain}
	if retained, err := drain.retain(s); !retained || err != nil || !drain.begin() {
		t.Fatal("unit work not admitted")
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- drain.stop(ctx) }()
	select {
	case <-delegate.reset:
	case <-ctx.Done():
		t.Fatal("native reset not dispatched")
	}
	if drain.begin() {
		t.Fatal("new work admitted after stop")
	}
	select {
	case <-done:
		t.Fatal("false join while admitted work remains")
	default:
	}
	drain.end()
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-ctx.Done():
		t.Fatal("unit drain failed to join")
	}
}

func TestPubsubScoringDrainCancellationDoesNotClaimJoin(t *testing.T) {
	drain := newPubsubScoringDrain()
	if !drain.begin() {
		t.Fatal("unit work not admitted")
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := drain.stop(ctx); err == nil {
		t.Fatal("cancellation claimed outstanding join")
	}
	drain.end()
}

type pubsubScoringBlockingStream struct {
	pubsubScoringUnitStream
	started      [2]chan struct{}
	finish       chan struct{}
	prefix       []byte
	writeFailure error
}

func (s *pubsubScoringBlockingStream) Read(p []byte) (int, error) {
	close(s.started[0])
	<-s.finish
	return copy(p, s.prefix), io.EOF
}

func (s *pubsubScoringBlockingStream) Write(p []byte) (int, error) {
	close(s.started[1])
	<-s.finish
	n := len(s.prefix)
	_, _ = s.output.Write(p[:n])
	return n, s.writeFailure
}

func TestPubsubScoringDrainResetsAllWrappersBeforeJoiningAndFinalizingIO(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	raw := pubsubScoringUnitFrame(t, nil)
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	drain := newPubsubScoringDrain()
	delegates := make([]*pubsubScoringBlockingStream, 0, 2)
	results := make(chan error, 4)
	for index := 0; index < 2; index++ {
		delegate := &pubsubScoringBlockingStream{pubsubScoringUnitStream: pubsubScoringUnitStream{
			conn: pubsubScoringUnitConnection(t), id: fmt.Sprintf("blocked-%d", index), reset: make(chan struct{}, 1)},
			started: [2]chan struct{}{make(chan struct{}), make(chan struct{})}, finish: make(chan struct{}),
			prefix: raw[:len(raw)-1], writeFailure: errors.New("native blocked write sentinel")}
		delegates = append(delegates, delegate)
		t.Cleanup(func() {
			select {
			case <-delegate.finish:
			default:
				close(delegate.finish)
			}
		})
		s := &pubsubScoringStream{Stream: delegate, observer: o, drain: drain}
		if retained, err := drain.retain(s); !retained || err != nil {
			t.Fatal("wrapper admission failed")
		}
		go func() {
			buffer := make([]byte, len(raw))
			n, err := s.Read(buffer)
			if n != len(delegate.prefix) || err != io.EOF || !bytes.Equal(buffer[:n], delegate.prefix) {
				results <- fmt.Errorf("native blocked read changed")
			} else {
				results <- nil
			}
		}()
		go func() {
			n, err := s.Write(raw)
			if n != len(delegate.prefix) || err != delegate.writeFailure {
				results <- fmt.Errorf("native blocked write changed")
			} else {
				results <- nil
			}
		}()
		for _, started := range delegate.started {
			select {
			case <-started:
			case <-ctx.Done():
				t.Fatal("native I/O did not start")
			}
		}
	}
	done := make(chan error, 1)
	go func() { done <- drain.stop(ctx) }()
	for _, delegate := range delegates {
		select {
		case <-delegate.reset:
		case <-ctx.Done():
			t.Fatal("drain waited before resetting every native stream")
		}
	}
	select {
	case <-done:
		t.Fatal("drain falsely joined blocked native I/O")
	default:
	}
	// Reset notifies the delegate before its wrapper publishes the return receipt.
	for {
		o.mu.Lock()
		seen := make(map[string]bool, len(delegates))
		valid := o.failure == nil && len(o.events) <= len(delegates)
		for _, event := range o.events {
			id, ok := event["stream_id"].(string)
			started, startedOK := event["started_order"].(uint64)
			returned, returnedOK := event["returned_order"].(uint64)
			owned := false
			for _, delegate := range delegates {
				owned = owned || (id == delegate.ID() && event["connection_id"] == delegate.Conn().ID() &&
					event["remote_peer_id"] == delegate.Conn().RemotePeer().String())
			}
			valid = valid && ok && !seen[id] && owned && startedOK && returnedOK && started < returned &&
				event["kind"] == "native_stream_operation" && event["operation"] == "stream_reset" &&
				event["outcome"] == "ok" && event["error"] == nil && event["protocol"] == string(pubsub.GossipSubID_v10)
			seen[id] = true
		}
		complete := len(seen) == len(delegates)
		o.mu.Unlock()
		if !valid {
			t.Fatal("unexpected terminal/framing event before successful native prefixes arrived")
		}
		if complete {
			break
		}
		select {
		case <-ctx.Done():
			t.Fatal("successful native Reset receipts were not published")
		case <-time.After(time.Millisecond):
		}
	}
	for _, delegate := range delegates {
		close(delegate.finish)
	}
	for index := 0; index < 4; index++ {
		select {
		case err := <-results:
			if err != nil {
				t.Fatal(err)
			}
		case <-ctx.Done():
			t.Fatal("native I/O did not complete")
		}
	}
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-ctx.Done():
		t.Fatal("drain framing join deadlocked")
	}
	o.mu.Lock()
	defer o.mu.Unlock()
	count := 0
	for _, event := range o.events {
		if event["kind"] == "rpc" {
			t.Fatal("blocked partial prefixes became completed RPC")
		}
		if event["kind"] == "incomplete_rpc_frame" {
			count++
			if event["operation"] != "stream_reset" || event["pending_frame_bytes"] != len(raw)-1 {
				t.Fatal("drain lost residue attribution", event)
			}
		}
	}
	if count != 4 || o.failure == nil {
		t.Fatal("drain omitted a direction's incomplete RPC", count)
	}
}

func TestPubsubScoringDrainRetainsResetErrorAndDoesNotInventJoin(t *testing.T) {
	drain := newPubsubScoringDrain()
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	failure := errors.New("native reset sentinel")
	s := &pubsubScoringStream{Stream: &pubsubScoringUnitStream{resetErr: failure}, observer: o, drain: drain}
	if retained, err := drain.retain(s); !retained || err != nil {
		t.Fatal("wrapper not retained")
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := drain.stop(ctx); !errors.Is(err, failure) || !errors.Is(err, context.Canceled) || o.failure != failure || s.framingJoined() {
		t.Fatal("drain lost native reset failure or claimed join", err)
	}
}

func pubsubScoringUnitCleanupJoin(owners ...<-chan struct{}) {
	deadline := time.NewTimer(2 * time.Second)
	defer deadline.Stop()
	for _, done := range owners {
		if done == nil {
			continue
		}
		select {
		case <-done:
		case <-deadline.C:
			panic("owned PubSub unit operation failed to join after gate cleanup")
		}
	}
}

func TestPubsubScoringQuiesceDrainsBeforeCancelInBothReadResetOrders(t *testing.T) {
	for _, readFirst := range []bool{false, true} {
		t.Run(fmt.Sprintf("read_first=%v", readFirst), func(t *testing.T) {
			native := &pubsubScoringResetUnitStream{pubsubScoringUnitStream: pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t)},
				ioStarted: make(chan struct{}), ioReturn: make(chan struct{}), resetStarted: make(chan struct{}), resetReturn: make(chan struct{}),
				nativeErr: pubsubScoringUnitLocalReset()}
			wrapped, o := pubsubScoringUnitWrapped(t, native)
			readDone, quiesceDone, worker := make(chan struct{}), make(chan struct{}), make(chan struct{})
			readResult, result := make(chan error, 1), make(chan error, 1)
			root, cancelRoot := context.WithCancel(context.Background())
			child, cancelChild := context.WithCancel(root)
			var readOnce, resetOnce sync.Once
			unblockRead := func() { readOnce.Do(func() { close(native.ioReturn) }) }
			unblockReset := func() { resetOnce.Do(func() { close(native.resetReturn) }) }
			t.Cleanup(func() {
				unblockRead()
				unblockReset()
				cancelRoot()
				cancelChild()
				pubsubScoringUnitCleanupJoin(readDone, quiesceDone, worker)
			})
			go func() { <-child.Done(); close(worker) }()
			go func() { defer close(readDone); _, err := wrapped.Read(make([]byte, 1)); readResult <- err }()
			pubsubScoringUnitWait(t, native.ioStarted)
			pubsubScoringUnitPrepare(t, o)
			command := pubsubScoringCommand{Sequence: 2, Kind: "quiesce_shutdown", Actor: o.actor, Token: o.token,
				Local: o.local.String(), Prepare: o.prepareAck}
			ctx, cancel := context.WithTimeout(context.Background(), time.Second)
			defer cancel()
			go func() {
				defer close(quiesceDone)
				result <- o.quiesceShutdown(command, wrapped.drain, root, child, ctx, worker, cancelRoot, cancelChild)
			}()
			pubsubScoringUnitWait(t, native.resetStarted)
			if root.Err() != nil || child.Err() != nil || len(pubsubScoringUnitEvents(o, "pre_cancel_retained_resets_returned")) != 0 {
				t.Fatal("root cancelled or pre-cancel drain claimed while native Reset was held")
			}
			if readFirst {
				unblockRead()
				pubsubScoringUnitWait(t, readDone)
				if root.Err() != nil || len(pubsubScoringUnitEvents(o, "shutdown_quiesced")) != 0 {
					t.Fatal("read wake bypassed the actual held Reset RETURN")
				}
				unblockReset()
			} else {
				unblockReset()
				select {
				case <-root.Done():
				case <-ctx.Done():
					t.Fatal("retained Reset RETURN did not release context cancellation")
				}
				if len(pubsubScoringUnitEvents(o, "pre_cancel_retained_resets_returned")) != 1 ||
					len(pubsubScoringUnitEvents(o, "shutdown_quiesced")) != 0 {
					t.Fatal("reset-only phase claimed a held native Read join")
				}
				if len(pubsubScoringUnitEvents(o, "native_stream_operation")) == 0 {
					t.Fatal("native Reset RETURN was not observed")
				}
				unblockRead()
			}
			pubsubScoringUnitWait(t, quiesceDone)
			pubsubScoringUnitWait(t, readDone)
			if <-result != nil || <-readResult != native.nativeErr || root.Err() == nil || child.Err() == nil {
				t.Fatal("quiesce changed native outcome or omitted mandatory cancellation")
			}
			pre, ack := pubsubScoringUnitEvents(o, "pre_cancel_retained_resets_returned"), pubsubScoringUnitEvents(o, "shutdown_quiesced")
			resetReturns := pubsubScoringUnitEvents(o, "pre_cancel_retained_reset_return")
			resets, reads := pubsubScoringUnitEvents(o, "native_stream_operation"), pubsubScoringUnitEvents(o, "stream_io_terminal")
			if len(pre) != 1 || len(ack) != 1 || len(resets) != 1 || len(reads) != 1 ||
				pre[0]["sequence"].(int) >= ack[0]["sequence"].(int) || pre[0]["pid"] != os.Getpid() ||
				pre[0]["case_token"] != o.token || pre[0]["prepare_ack_sequence"] != command.Prepare ||
				pre[0]["pubsub_context_cancelled"] != false || pre[0]["subscriber_context_cancelled"] != false ||
				len(resetReturns) != 1 || len(pre[0]["reset_return_receipt_sequences"].([]int)) != 1 ||
				pre[0]["reset_return_receipt_sequences"].([]int)[0] != resetReturns[0]["sequence"] ||
				len(pre[0]["retained_owners"].([]map[string]any)) != 1 ||
				resetReturns[0]["stream_id"] != native.ID() || resetReturns[0]["outcome"] != "ok" {
				t.Fatal("pre-cancel receipt lacks actual Reset RETURN or its immutable live-context owner", pre, ack)
			}
			if (reads[0]["returned_order"].(uint64) < resets[0]["returned_order"].(uint64)) != readFirst {
				t.Fatal("controlled native Read/Reset return ordering was not exercised")
			}
		})
	}
}

func TestPubsubScoringQuiesceCancelsBlockedNativeOpenBeforeGlobalJoin(t *testing.T) {
	conn := pubsubScoringUnitConnection(t)
	native := &pubsubScoringResetUnitStream{pubsubScoringUnitStream: pubsubScoringUnitStream{conn: conn},
		resetStarted: make(chan struct{}), resetReturn: make(chan struct{})}
	wrapped, o := pubsubScoringUnitWrapped(t, native)
	delegate := &pubsubScoringQuiesceUnitHost{started: make(chan struct{}), finish: make(chan struct{}), waitForCancel: true}
	h := &pubsubScoringHost{Host: delegate, observer: o, drain: wrapped.drain, protocol: pubsub.GossipSubID_v10}
	root, cancelRoot := context.WithCancel(context.Background())
	child, cancelChild := context.WithCancel(root)
	openDone, worker, quiesceDone := make(chan struct{}), make(chan struct{}), make(chan struct{})
	openResult, result := make(chan error, 1), make(chan error, 1)
	var resetOnce, openOnce sync.Once
	unblockReset := func() { resetOnce.Do(func() { close(native.resetReturn) }) }
	unblockOpen := func() { openOnce.Do(func() { close(delegate.finish) }) }
	t.Cleanup(func() {
		unblockReset()
		unblockOpen()
		cancelRoot()
		cancelChild()
		pubsubScoringUnitCleanupJoin(openDone, worker, quiesceDone)
	})
	go func() { <-child.Done(); close(worker) }()
	go func() { defer close(openDone); _, err := h.NewStream(root, conn.remote, h.protocol); openResult <- err }()
	pubsubScoringUnitWait(t, delegate.started)
	pubsubScoringUnitPrepare(t, o)
	command := pubsubScoringCommand{Sequence: 2, Kind: "quiesce_shutdown", Actor: o.actor, Token: o.token,
		Local: o.local.String(), Prepare: o.prepareAck}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	go func() {
		defer close(quiesceDone)
		result <- o.quiesceShutdown(command, h.drain, root, child, ctx, worker, cancelRoot, cancelChild)
	}()
	pubsubScoringUnitWait(t, native.resetStarted)
	if root.Err() != nil || len(pubsubScoringUnitEvents(o, "pre_cancel_retained_resets_returned")) != 0 {
		t.Fatal("held retained Reset did not keep the contexts alive")
	}
	unblockReset()
	select {
	case <-root.Done():
	case <-ctx.Done():
		t.Fatal("pre-cancel phase waited for an open that needs context cancellation")
	}
	if len(pubsubScoringUnitEvents(o, "shutdown_quiesced")) != 0 {
		t.Fatal("quiesce ACK preceded the blocked native Open RETURN")
	}
	unblockOpen()
	pubsubScoringUnitWait(t, quiesceDone)
	pubsubScoringUnitWait(t, openDone)
	pre, ack := pubsubScoringUnitEvents(o, "pre_cancel_retained_resets_returned"), pubsubScoringUnitEvents(o, "shutdown_quiesced")
	if <-result != nil || <-openResult != context.Canceled || len(pre) != 1 || len(ack) != 1 ||
		pre[0]["active_stream_handlers_and_io"] != 1 || pre[0]["active_pubsub_streams"] != 0 ||
		len(pre[0]["reset_return_receipt_sequences"].([]int)) != 1 ||
		pre[0]["pubsub_context_cancelled"] != false || ack[0]["active_stream_handlers_and_io"] != 0 {
		t.Fatal("blocked open was hidden or first phase fabricated an I/O join", pre, ack)
	}
}

func TestPubsubScoringRetainPublishesProtocolBeforeShutdownCanSnapshot(t *testing.T) {
	for _, quicOwner := range []bool{false, true} {
		t.Run(fmt.Sprintf("quic_owner=%v", quicOwner), func(t *testing.T) {
			o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
			drain := newPubsubScoringDrain()
			h := &pubsubScoringHost{observer: o, drain: drain}
			native := &pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t), id: "atomic-retention"}
			var owner pubsubScoringDrainedStream = &pubsubScoringStream{Stream: native, observer: o, drain: drain}
			if quicOwner {
				owner = &pubsubQUICHostStream{Stream: native, drain: drain}
			}
			fields := map[string]any{"connection_id": native.Conn().ID(), "stream_id": native.ID(),
				"peer_id": native.Conn().RemotePeer().String(), "protocol": string(native.Protocol())}
			ctx, cancel := context.WithTimeout(context.Background(), time.Second)
			defer cancel()
			retained, snapshotted := make(chan struct{}), make(chan struct{})
			results := make(chan error, 2)
			o.mu.Lock()
			var release sync.Once
			unblock := func() { release.Do(o.mu.Unlock) }
			snapshotStarted := false
			t.Cleanup(func() {
				unblock()
				pubsubScoringUnitCleanupJoin(retained)
				if snapshotStarted {
					pubsubScoringUnitCleanupJoin(snapshotted)
				}
			})
			go func() {
				defer close(retained)
				ok, err := h.retainProtocol(owner, fields)
				if !ok && err == nil {
					err = fmt.Errorf("native owner unexpectedly rejected")
				}
				results <- err
			}()
			// Hold receipt publication, then observe the actual admission mutex.
			for drain.mu.TryLock() {
				drain.mu.Unlock()
				if ctx.Err() != nil {
					t.Fatal("retain never acquired its admission mutex")
				}
				runtime.Gosched()
			}
			snapshotStarted = true
			go func() {
				defer close(snapshotted)
				drain.mu.Lock()
				present := drain.streams[owner.ID()] == owner
				drain.mu.Unlock()
				_, err := o.retainedResetOwner(owner)
				if !present {
					err = fmt.Errorf("shutdown lost the admitted native owner")
				}
				results <- err
			}()
			select {
			case <-snapshotted:
				t.Fatal("shutdown saw retained state before protocol publication")
			default:
			}
			unblock()
			pubsubScoringUnitWait(t, retained)
			pubsubScoringUnitWait(t, snapshotted)
			if first, second := <-results, <-results; first != nil || second != nil {
				t.Fatal("retention/publication did not linearize", first, second)
			}

			// The opposite order rejects without inventing a protocol receipt.
			closed := &pubsubScoringHost{observer: newPubsubScoringObserver("victim", strings.Repeat("a", 32)),
				drain: newPubsubScoringDrain()}
			closed.drain.mu.Lock()
			closed.drain.closing = true
			closed.drain.mu.Unlock()
			if ok, err := closed.retainProtocol(owner, fields); ok || err != nil {
				t.Fatal("owner admitted after shutdown publication", ok, err)
			}
			if len(closed.observer.events) != 0 || len(closed.drain.streams) != 0 {
				t.Fatal("rejected owner acquired a protocol/retained receipt")
			}
		})
	}
}

func TestPubsubScoringQuiesceRejoinsPostCancelTerminalAndRejectedHandler(t *testing.T) {
	for _, failed := range []bool{false, true} {
		t.Run(fmt.Sprintf("failed_rejected_reset=%v", failed), func(t *testing.T) {
			native := &pubsubScoringLateTerminalStream{pubsubScoringUnitStream: pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t)},
				entered: make(chan struct{}), release: make(chan struct{})}
			wrapped, o := pubsubScoringUnitWrapped(t, native)
			if err := wrapped.Reset(); err != nil {
				t.Fatal(err)
			}
			pubsubScoringUnitPrepare(t, o)
			native.operation = "close"
			rejected := &pubsubScoringQuiesceResetStream{pubsubScoringUnitStream: pubsubScoringUnitStream{conn: native.conn, id: "late-rejected"},
				entered: make(chan struct{}), release: make(chan struct{})}
			primary := &net.OpError{Op: "reset", Net: "tcp", Err: syscall.ECONNRESET}
			if failed {
				rejected.resetErr = primary
			}
			delegate := &pubsubScoringQuiesceUnitHost{}
			h := &pubsubScoringHost{Host: delegate, observer: o, drain: wrapped.drain, protocol: pubsub.GossipSubID_v10}
			h.SetStreamHandler(h.protocol, func(network.Stream) { t.Error("closed-admission handler ran") })
			command := pubsubScoringCommand{Sequence: 2, Kind: "quiesce_shutdown", Actor: o.actor, Token: o.token,
				Local: o.local.String(), Prepare: o.prepareAck}
			root, cancelRoot := context.WithCancel(context.Background())
			child, cancelChild := context.WithCancel(root)
			terminalDone, handlerDone, worker, quiesceDone := make(chan struct{}), make(chan struct{}), make(chan struct{}), make(chan struct{})
			result := make(chan error, 1)
			var cancelOnce, terminalOnce, resetOnce sync.Once
			unblockTerminal := func() { terminalOnce.Do(func() { close(native.release) }) }
			unblockReset := func() { resetOnce.Do(func() { close(rejected.release) }) }
			t.Cleanup(func() {
				unblockTerminal()
				unblockReset()
				cancelRoot()
				cancelChild()
				pubsubScoringUnitCleanupJoin(terminalDone, handlerDone, worker, quiesceDone)
			})
			go func() { <-child.Done(); close(worker) }()
			cancelOwned := func() {
				cancelOnce.Do(func() {
					cancelRoot()
					go func() {
						defer close(terminalDone)
						if err := wrapped.Close(); err != nil {
							t.Error(err)
						}
					}()
					go func() { defer close(handlerDone); delegate.handler(rejected) }()
					<-native.entered
					<-rejected.entered
				})
			}
			ctx, cancel := context.WithTimeout(context.Background(), time.Second)
			defer cancel()
			go func() {
				defer close(quiesceDone)
				result <- o.quiesceShutdown(command, h.drain, root, child, ctx, worker, cancelOwned, cancelChild)
			}()
			pubsubScoringUnitWait(t, native.entered)
			pubsubScoringUnitWait(t, rejected.entered)
			if len(pubsubScoringUnitEvents(o, "pre_cancel_retained_resets_returned")) != 1 ||
				len(pubsubScoringUnitEvents(o, "shutdown_quiesced")) != 0 {
				t.Fatal("post-cancel native work bypassed the second drain")
			}
			unblockTerminal()
			pubsubScoringUnitWait(t, terminalDone)
			if len(pubsubScoringUnitEvents(o, "shutdown_quiesced")) != 0 {
				t.Fatal("ACK skipped held rejected Reset")
			}
			unblockReset()
			pubsubScoringUnitWait(t, quiesceDone)
			got := <-result
			if failed {
				if !errors.Is(got, primary) || o.failure != primary || len(pubsubScoringUnitEvents(o, "shutdown_quiesced")) != 0 {
					t.Fatal("late actual Reset error was hidden", got)
				}
			} else if got != nil || len(pubsubScoringUnitEvents(o, "shutdown_quiesced")) != 1 {
				t.Fatal("completed post-cancel owners did not actually join", got)
			}
		})
	}
}

func TestPubsubScoringQuiesceFirstDrainFailureStillCancelsWithoutACK(t *testing.T) {
	for _, mode := range []string{"reset_failure", "partial", "timeout", "sticky"} {
		t.Run(mode, func(t *testing.T) {
			raw := pubsubScoringUnitFrame(t, nil)
			native := &pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t), input: bytes.NewReader(raw[:len(raw)-1])}
			wrapped, o := pubsubScoringUnitWrapped(t, native)
			pubsubScoringUnitPrepare(t, o)
			primary := &net.OpError{Op: "read", Net: "tcp", Err: syscall.ECONNRESET}
			if mode == "reset_failure" {
				native.resetErr = primary
			}
			if mode == "partial" {
				if _, err := wrapped.Read(make([]byte, len(raw))); err != nil {
					t.Fatal(err)
				}
			}
			if mode == "sticky" {
				o.fail(primary)
			}
			if mode == "timeout" {
				if !wrapped.drain.begin() {
					t.Fatal("owned work was not admitted")
				}
				defer wrapped.drain.end()
			}
			command := pubsubScoringCommand{Sequence: 2, Kind: "quiesce_shutdown", Actor: o.actor, Token: o.token,
				Local: o.local.String(), Prepare: o.prepareAck}
			root, cancelRoot := context.WithCancel(context.Background())
			child, cancelChild := context.WithCancel(root)
			worker := make(chan struct{})
			go func() { <-child.Done(); close(worker) }()
			defer pubsubScoringUnitCleanupJoin(worker)
			defer cancelChild()
			defer cancelRoot()
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Millisecond)
			defer cancel()
			err := o.quiesceShutdown(command, wrapped.drain, root, child, ctx, worker, cancelRoot, cancelChild)
			preCount := 0
			if mode == "timeout" {
				preCount = 1
			}
			if err == nil || root.Err() == nil || child.Err() == nil || o.failure == nil ||
				len(pubsubScoringUnitEvents(o, "pre_cancel_retained_resets_returned")) != preCount ||
				len(pubsubScoringUnitEvents(o, "shutdown_quiesced")) != 0 {
				t.Fatal("failed first drain fabricated success or omitted cancellation", err)
			}
			if (mode == "sticky" || mode == "reset_failure") && o.failure != primary {
				t.Fatal("first native TCP failure was overwritten")
			}
			if mode == "partial" && len(pubsubScoringUnitEvents(o, "incomplete_rpc_frame")) == 0 {
				t.Fatal("partial RPC became clean during quiesce")
			}
		})
	}
}

func TestPubsubScoringQuiesceJoinsAdmittedCallbacksButKeepsNativeErrorsLive(t *testing.T) {
	o := newPubsubScoringObserver("sink", strings.Repeat("a", 32))
	o.local = pubsubScoringUnitConnection(t).local
	pubsubScoringUnitPrepare(t, o)
	command := pubsubScoringCommand{Sequence: 2, Kind: "quiesce_shutdown", Actor: o.actor, Token: o.token,
		Local: o.local.String(), Prepare: o.prepareAck}
	root, cancelRoot := context.WithCancel(context.Background())
	child, cancelChild := context.WithCancel(root)
	defer cancelChild()
	cancelRoot()
	worker := make(chan struct{})
	close(worker)
	drain := newPubsubScoringDrain()
	drain.closing = true
	entered, release, finished := make(chan struct{}), make(chan struct{}), make(chan struct{})
	var releaseOnce sync.Once
	unblock := func() { releaseOnce.Do(func() { close(release) }) }
	t.Cleanup(func() { unblock(); pubsubScoringUnitCleanupJoin(finished) })
	go func() {
		defer close(finished)
		o.callback(func() { close(entered); <-release })
	}()
	pubsubScoringUnitWait(t, entered)
	o.mu.Lock()
	o.pubsubCallbacksClosed = true
	o.mu.Unlock()
	called := false
	o.callback(func() { called = true })
	if called {
		t.Fatal("late PubSub callback passed closed admission")
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Millisecond)
	err := o.quiesceAck(command, drain, root, child, ctx, worker)
	cancel()
	unblock()
	pubsubScoringUnitWait(t, finished)
	if err == nil || len(pubsubScoringUnitEvents(o, "shutdown_quiesced")) != 0 {
		t.Fatal("ACK preceded admitted callback join")
	}
	if err := o.quiesceAck(command, drain, root, child, context.Background(), worker); err != nil {
		t.Fatal(err)
	}
	o.nativeCallback(func() { called = true })
	primary := &net.OpError{Op: "read", Net: "tcp", Err: syscall.ECONNRESET}
	o.fail(primary)
	if !called || o.failure != primary || o.admitQuiesce(command, 0) != primary {
		t.Fatal("quiesce silenced native observations or replaced sticky TCP error")
	}
}

type pubsubScoringQuiesceUnitHost struct {
	host.Host
	started, finish chan struct{}
	stream          network.Stream
	handler         network.StreamHandler
	waitForCancel   bool
}

func (h *pubsubScoringQuiesceUnitHost) NewStream(ctx context.Context, _ peer.ID, _ ...protocol.ID) (network.Stream, error) {
	close(h.started)
	if h.waitForCancel {
		<-ctx.Done()
		if h.finish != nil {
			<-h.finish
		}
		return nil, ctx.Err()
	}
	<-h.finish
	return h.stream, nil
}
func (h *pubsubScoringQuiesceUnitHost) SetStreamHandler(_ protocol.ID, handler network.StreamHandler) {
	h.handler = handler
}

type pubsubScoringQuiesceResetStream struct {
	pubsubScoringUnitStream
	entered, release         chan struct{}
	wrapEntered, wrapRelease chan struct{}
	wrapOnce                 sync.Once
}

func (s *pubsubScoringQuiesceResetStream) Conn() network.Conn {
	if s.wrapEntered != nil {
		s.wrapOnce.Do(func() { close(s.wrapEntered); <-s.wrapRelease })
	}
	return s.pubsubScoringUnitStream.Conn()
}

func (s *pubsubScoringQuiesceResetStream) Reset() error {
	close(s.entered)
	<-s.release
	return s.resetErr
}

type pubsubScoringLateTerminalStream struct {
	pubsubScoringUnitStream
	operation        string
	entered, release chan struct{}
	terminalErr      error
}

func (s *pubsubScoringLateTerminalStream) Close() error {
	if s.operation == "close" {
		close(s.entered)
		<-s.release
		return s.terminalErr
	}
	return s.pubsubScoringUnitStream.Close()
}

func (s *pubsubScoringLateTerminalStream) Reset() error {
	if s.operation == "reset" {
		close(s.entered)
		<-s.release
		return s.terminalErr
	}
	return s.pubsubScoringUnitStream.Reset()
}

func TestPubsubScoringQuiesceTracksLateTerminalAfterRegistryRelease(t *testing.T) {
	for _, operation := range []string{"close", "reset"} {
		for _, failed := range []bool{false, true} {
			t.Run(fmt.Sprintf("%s/failed=%v", operation, failed), func(t *testing.T) {
				native := &pubsubScoringLateTerminalStream{pubsubScoringUnitStream: pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t)},
					entered: make(chan struct{}), release: make(chan struct{})}
				wrapped, o := pubsubScoringUnitWrapped(t, native)
				if err := wrapped.Reset(); err != nil {
					t.Fatal(err)
				}
				if !wrapped.framingJoined() || len(wrapped.drain.streams) != 0 {
					t.Fatal("initial native disposal did not release the registry owner")
				}
				pubsubScoringUnitPrepare(t, o)
				command := pubsubScoringCommand{Sequence: 2, Kind: "quiesce_shutdown", Actor: o.actor, Token: o.token,
					Local: o.local.String(), Prepare: o.prepareAck}
				root, cancelRoot := context.WithCancel(context.Background())
				child, cancelChild := context.WithCancel(root)
				defer cancelChild()
				cancelRoot()
				wrapped.drain.closing = true
				o.pubsubCallbacksClosed = true
				native.operation = operation
				primary := errors.New("late native terminal failure")
				if failed {
					native.terminalErr = primary
				}
				finished, result := make(chan struct{}), make(chan error, 1)
				var releaseOnce sync.Once
				unblock := func() { releaseOnce.Do(func() { close(native.release) }) }
				t.Cleanup(func() { unblock(); pubsubScoringUnitCleanupJoin(finished) })
				go func() {
					defer close(finished)
					if operation == "close" {
						result <- wrapped.Close()
					} else {
						result <- wrapped.Reset()
					}
				}()
				pubsubScoringUnitWait(t, native.entered)
				worker := make(chan struct{})
				close(worker)
				ctx, cancel := context.WithTimeout(context.Background(), time.Millisecond)
				err := o.quiesceAck(command, wrapped.drain, root, child, ctx, worker)
				cancel()
				if err == nil || len(pubsubScoringUnitEvents(o, "shutdown_quiesced")) != 0 {
					t.Fatal("ACK skipped a pending terminal operation on a released wrapper")
				}
				unblock()
				pubsubScoringUnitWait(t, finished)
				got := <-result
				if failed && (got != primary || o.failure != primary) || !failed && got != nil {
					t.Fatal("native terminal result was changed or silenced", got)
				}
				ctx, cancel = context.WithTimeout(context.Background(), time.Second)
				defer cancel()
				err = o.quiesceAck(command, wrapped.drain, root, child, ctx, worker)
				if failed && err != primary || !failed && err != nil {
					t.Fatal("completed terminal did not retain its failure/join", err)
				}
			})
		}
	}
}

func TestPubsubScoringQuiesceTracksDeclinedHandlerAndInflightOpenDisposals(t *testing.T) {
	for _, incoming := range []bool{false, true} {
		for _, failed := range []bool{false, true} {
			t.Run(fmt.Sprintf("incoming=%v/failed=%v", incoming, failed), func(t *testing.T) {
				conn := pubsubScoringUnitConnection(t)
				s := &pubsubScoringQuiesceResetStream{pubsubScoringUnitStream: pubsubScoringUnitStream{conn: conn},
					entered: make(chan struct{}), release: make(chan struct{})}
				primary := &net.OpError{Op: "reset", Net: "tcp", Err: syscall.ECONNRESET}
				if failed {
					s.resetErr = primary
				}
				o := newPubsubScoringObserver("sink", strings.Repeat("a", 32))
				o.local = conn.local
				pubsubScoringUnitPrepare(t, o)
				command := pubsubScoringCommand{Sequence: 2, Kind: "quiesce_shutdown", Actor: o.actor, Token: o.token,
					Local: o.local.String(), Prepare: o.prepareAck}
				root, cancelRoot := context.WithCancel(context.Background())
				defer cancelRoot()
				child, cancelChild := context.WithCancel(root)
				defer cancelChild()
				delegate := &pubsubScoringQuiesceUnitHost{stream: s, started: make(chan struct{}), finish: make(chan struct{})}
				h := &pubsubScoringHost{Host: delegate, observer: o, drain: newPubsubScoringDrain(), protocol: pubsub.GossipSubID_v10}
				finished := make(chan struct{})
				var openOnce, resetOnce sync.Once
				unblockOpen := func() { openOnce.Do(func() { close(delegate.finish) }) }
				unblockReset := func() { resetOnce.Do(func() { close(s.release) }) }
				t.Cleanup(func() {
					unblockOpen()
					unblockReset()
					pubsubScoringUnitCleanupJoin(finished)
				})
				if !incoming {
					go func() { defer close(finished); _, _ = h.NewStream(root, conn.remote, h.protocol) }()
					pubsubScoringUnitWait(t, delegate.started)
				}
				h.drain.mu.Lock()
				h.drain.closing = true
				h.drain.mu.Unlock()
				o.mu.Lock()
				o.pubsubCallbacksClosed = true
				o.mu.Unlock()
				cancelRoot()
				if incoming {
					h.SetStreamHandler(h.protocol, func(network.Stream) { t.Error("declined handler ran") })
					go func() { defer close(finished); delegate.handler(s) }()
				} else {
					unblockOpen()
				}
				pubsubScoringUnitWait(t, s.entered)
				worker := make(chan struct{})
				close(worker)
				ctx, cancel := context.WithTimeout(context.Background(), time.Millisecond)
				err := o.quiesceAck(command, h.drain, root, child, ctx, worker)
				cancel()
				unblockReset()
				pubsubScoringUnitWait(t, finished)
				if err == nil || len(pubsubScoringUnitEvents(o, "shutdown_quiesced")) != 0 {
					t.Fatal("held rejected Reset did not prevent ACK")
				}
				receipts := pubsubScoringUnitEvents(o, "native_rejected_stream_disposal")
				if len(receipts) != 1 || receipts[0]["returned_order"].(uint64) <= receipts[0]["started_order"].(uint64) {
					t.Fatal("actual rejected Reset RETURN was not captured", receipts)
				}
				err = o.quiesceAck(command, h.drain, root, child, context.Background(), worker)
				if failed {
					if o.failure != primary || err != primary || receipts[0]["outcome"] != "error" {
						t.Fatal("native Reset failure was hidden", err)
					}
				} else if err != nil || receipts[0]["outcome"] != "ok" {
					t.Fatal("completed rejected disposal did not join", err)
				}
				if _, err := h.NewStream(root, conn.remote, h.protocol); err != context.Canceled {
					t.Fatal("new native open passed closed admission", err)
				}
			})
		}
	}
}

func TestPubsubScoringClosingBeforeCancelRejectsOnlyValidOwnersNeutrally(t *testing.T) {
	for _, incoming := range []bool{false, true} {
		for _, canceled := range []bool{false, true} {
			for _, fault := range []string{"valid", "id", "protocol", "auth", "duplicate", "capacity", "reset"} {
				t.Run(fmt.Sprintf("incoming=%v/canceled=%v/%s", incoming, canceled, fault), func(t *testing.T) {
					conn := pubsubScoringUnitConnection(t)
					s := &pubsubScoringQuiesceResetStream{pubsubScoringUnitStream: pubsubScoringUnitStream{conn: conn, id: "admission-race"},
						entered: make(chan struct{}), release: make(chan struct{}), wrapEntered: make(chan struct{}), wrapRelease: make(chan struct{})}
					primary := &net.OpError{Op: "reset", Net: "tcp", Err: syscall.ECONNRESET}
					o := newPubsubScoringObserver("sink", strings.Repeat("a", 32))
					o.local = conn.local
					pubsubScoringUnitPrepare(t, o)
					root, cancelRoot := context.WithCancel(context.Background())
					defer cancelRoot()
					child, cancelChild := context.WithCancel(root)
					defer cancelChild()
					delegate := &pubsubScoringQuiesceUnitHost{stream: s, started: make(chan struct{}), finish: make(chan struct{})}
					h := &pubsubScoringHost{Host: delegate, observer: o, drain: newPubsubScoringDrain(), protocol: pubsub.GossipSubID_v10}
					switch fault {
					case "id":
						s.missingID = true
					case "protocol":
						s.protocol = pubsub.GossipSubID_v11
					case "auth":
						conn.key = nil
					case "duplicate":
						h.drain.seen[s.ID()] = true
					case "capacity":
						for index := 0; index < pubsubScoringStreams; index++ {
							h.drain.seen[fmt.Sprintf("prior-%d", index)] = true
						}
					case "reset":
						s.resetErr = primary
					}
					finished := make(chan struct{})
					var opened network.Stream
					var openErr error
					called := false
					var openOnce, wrapOnce, resetOnce sync.Once
					unblockOpen := func() { openOnce.Do(func() { close(delegate.finish) }) }
					unblockWrap := func() { wrapOnce.Do(func() { close(s.wrapRelease) }) }
					unblockReset := func() { resetOnce.Do(func() { close(s.release) }) }
					t.Cleanup(func() { unblockOpen(); unblockWrap(); unblockReset(); pubsubScoringUnitCleanupJoin(finished) })
					if incoming {
						h.SetStreamHandler(h.protocol, func(network.Stream) { called = true })
						go func() { defer close(finished); delegate.handler(s) }()
					} else {
						go func() { defer close(finished); opened, openErr = h.NewStream(root, conn.remote, h.protocol) }()
						pubsubScoringUnitWait(t, delegate.started)
						unblockOpen()
					}
					pubsubScoringUnitWait(t, s.wrapEntered) // Admitted caller is still wrapping its actual native return.
					h.drain.mu.Lock()
					h.drain.closing = true
					h.drain.mu.Unlock()
					o.mu.Lock()
					o.pubsubCallbacksClosed = true
					o.mu.Unlock()
					if root.Err() != nil {
						t.Fatal("context canceled before admission edge")
					}
					if canceled {
						cancelRoot()
					}
					unblockWrap()
					pubsubScoringUnitWait(t, s.entered)
					ctx, cancel := context.WithTimeout(context.Background(), time.Millisecond)
					err := h.drain.stop(ctx)
					cancel()
					h.drain.mu.Lock()
					active := h.drain.active
					retained := len(h.drain.streams)
					h.drain.mu.Unlock()
					if err == nil || active != 1 || retained != 0 {
						t.Fatal("held native Reset invented a completed join", err, active, retained)
					}
					unblockReset()
					pubsubScoringUnitWait(t, finished)
					if !canceled && root.Err() != nil {
						t.Fatal("fixture hid the race by canceling its context")
					}
					if opened != nil || called || len(pubsubScoringUnitEvents(o, "protocol")) != 0 || len(pubsubScoringUnitEvents(o, "rpc")) != 0 {
						t.Fatal("closed admission exported caller/protocol/RPC ownership")
					}
					if fault == "valid" || fault == "reset" {
						if !incoming && openErr != context.Canceled {
							t.Fatal("closed admission was not explicit cancellation", openErr)
						}
					} else if o.failure == nil || !incoming && (openErr == nil || openErr == context.Canceled) {
						t.Fatal("invalid owner was suppressed by closed/canceled admission", openErr, o.failure)
					}
					if fault == "valid" && o.failure != nil {
						t.Fatal("valid closed admission became fatal", o.failure)
					}
					if fault == "reset" && o.failure != primary {
						t.Fatal("native Reset error was replaced or suppressed", o.failure)
					}
					receipts := pubsubScoringUnitEvents(o, "native_rejected_stream_disposal")
					if len(receipts) != 1 || receipts[0]["returned_order"].(uint64) <= receipts[0]["started_order"].(uint64) ||
						(fault == "reset") != (receipts[0]["outcome"] == "error") {
						t.Fatal("native Reset result/RETURN was not preserved", receipts)
					}
					cancelRoot()
					worker := make(chan struct{})
					close(worker)
					command := pubsubScoringCommand{Sequence: 2, Kind: "quiesce_shutdown", Actor: o.actor, Token: o.token,
						Local: o.local.String(), Prepare: o.prepareAck}
					err = o.quiesceAck(command, h.drain, root, child, context.Background(), worker)
					if (err == nil) != (fault == "valid") {
						t.Fatal("quiesce ACK ignored actual disposal/fatal failure", err)
					}
				})
			}
		}
	}
}

func TestPubsubScoringRetainChecksInvalidOwnersBeforeClosedAdmission(t *testing.T) {
	for _, quic := range []bool{false, true} {
		for _, fault := range []string{"valid", "id", "duplicate", "capacity"} {
			t.Run(fmt.Sprintf("quic=%v/%s", quic, fault), func(t *testing.T) {
				drain := newPubsubScoringDrain()
				drain.closing = true
				s := &pubsubScoringUnitStream{id: "late-owner", missingID: fault == "id"}
				if fault == "duplicate" {
					drain.seen[s.ID()] = true
				}
				if fault == "capacity" {
					for index := 0; index < pubsubScoringStreams; index++ {
						drain.seen[fmt.Sprintf("prior-%d", index)] = true
					}
				}
				var wrapped pubsubScoringDrainedStream = &pubsubScoringStream{Stream: s, drain: drain}
				if quic {
					wrapped = &pubsubQUICHostStream{Stream: s, drain: drain}
				}
				retained, err := drain.retain(wrapped)
				if retained || len(drain.streams) != 0 || (err == nil) != (fault == "valid") {
					t.Fatal("retention conflated closure and invalid owner", retained, err)
				}
			})
		}
	}
}

func TestPubsubScoringQuiesceRejectsUnjoinedWorkerWriteAndForeignPrepare(t *testing.T) {
	s := &pubsubScoringBlockingStream{pubsubScoringUnitStream: pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t), reset: make(chan struct{}, 1)},
		started: [2]chan struct{}{make(chan struct{}), make(chan struct{})}, finish: make(chan struct{})}
	wrapped, o := pubsubScoringUnitWrapped(t, s)
	pubsubScoringUnitPrepare(t, o)
	command := pubsubScoringCommand{Sequence: 2, Kind: "quiesce_shutdown", Actor: o.actor, Token: o.token,
		Local: o.local.String(), Prepare: o.prepareAck}
	for _, mutate := range []func(*pubsubScoringCommand){func(c *pubsubScoringCommand) { c.Prepare++ },
		func(c *pubsubScoringCommand) { c.Actor = "foreign" }, func(c *pubsubScoringCommand) { c.Sequence++ }} {
		foreign := command
		mutate(&foreign)
		if o.admitQuiesce(foreign, 0) == nil {
			t.Fatal("foreign quiesce admitted")
		}
	}
	if o.admitQuiesce(command, 1) == nil {
		t.Fatal("pending command admitted")
	}
	root, cancelRoot := context.WithCancel(context.Background())
	defer cancelRoot()
	child, cancelChild := context.WithCancel(root)
	defer cancelChild()
	writeDone := make(chan error, 1)
	writeJoined := make(chan struct{})
	var drainJoined <-chan struct{}
	var finishOnce sync.Once
	unblock := func() { finishOnce.Do(func() { close(s.finish) }) }
	t.Cleanup(func() { unblock(); pubsubScoringUnitCleanupJoin(writeJoined, drainJoined) })
	go func() { defer close(writeJoined); _, err := wrapped.Write([]byte("native input")); writeDone <- err }()
	pubsubScoringUnitWait(t, s.started[1])
	wrapped.drain.mu.Lock()
	wrapped.drain.closing = true
	wrapped.drain.mu.Unlock()
	o.mu.Lock()
	o.pubsubCallbacksClosed = true
	o.mu.Unlock()
	cancelRoot()
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	drainDone := make(chan error, 1)
	drainFinished := make(chan struct{})
	drainJoined = drainFinished
	go func() { defer close(drainFinished); drainDone <- wrapped.drain.stop(ctx) }()
	pubsubScoringUnitWait(t, s.reset)
	worker := make(chan struct{})
	if o.quiesceAck(command, wrapped.drain, root, child, ctx, worker) == nil {
		t.Fatal("unjoined subscriber admitted")
	}
	close(worker)
	short, cancelShort := context.WithTimeout(context.Background(), time.Millisecond)
	err := o.quiesceAck(command, wrapped.drain, root, child, short, worker)
	cancelShort()
	unblock()
	pubsubScoringUnitWait(t, writeJoined)
	pubsubScoringUnitWait(t, drainFinished)
	if err == nil || <-writeDone != nil || <-drainDone != nil {
		t.Fatal("inflight native write joined prematurely", err)
	}
	if err := o.quiesceAck(command, wrapped.drain, root, child, ctx, worker); err != nil {
		t.Fatal(err)
	}
	if n, err := wrapped.Write([]byte("late subscription")); n != 0 || err != context.Canceled || s.output.Len() != 0 {
		t.Fatal("native write passed closed admission", n, err)
	}
}

func TestPubsubScoringQuiesceAdmissionPreservesNativeWriteResultAndEarlierTCPFailure(t *testing.T) {
	primary := &net.OpError{Op: "write", Net: "tcp", Err: syscall.ECONNRESET}
	native := &pubsubScoringUnitStream{writeN: 2, writeErr: primary}
	drain := newPubsubScoringDrain()
	quic := &pubsubScoringQUICWriteStream{pubsubQUICHostStream: &pubsubQUICHostStream{Stream: native, drain: drain}}
	if n, err := quic.Write([]byte("bytes")); n != 2 || err != primary {
		t.Fatal("admitted native write result changed", n, err)
	}
	drain.closing = true
	if n, err := quic.Write([]byte("later")); n != 0 || err != context.Canceled || native.output.Len() != 2 {
		t.Fatal("closed QUIC PubSub write admission reached native delegate", n, err)
	}
	tcp, o := pubsubScoringUnitWrapped(t, &pubsubScoringUnitStream{conn: pubsubScoringUnitConnection(t), writeErr: primary})
	if n, err := tcp.Write([]byte("input")); n != 0 || err != primary || o.failure != primary {
		t.Fatal("original TCP error changed", n, err)
	}
	if err := o.admitQuiesce(pubsubScoringCommand{}, 0); err != primary || len(pubsubScoringUnitEvents(o, "shutdown_quiesced")) != 0 {
		t.Fatal("earlier native TCP error was cleared by quiesce", err)
	}
}

func TestPubsubScoringWholeOwnerStopCancelsRootBeforeChildWithoutLiveRemoval(t *testing.T) {
	// Bind the ordering/no-call regression to the actual deferred teardown,
	// without introducing injectable lifecycle callbacks into the fixture.
	source, err := parser.ParseFile(token.NewFileSet(), "pubsub_scoring.go", nil, 0)
	if err != nil {
		t.Fatal(err)
	}
	var teardown *ast.BlockStmt
	for _, declaration := range source.Decls {
		function, ok := declaration.(*ast.FuncDecl)
		if !ok || function.Name.Name != "runPubsubScoringLive" {
			continue
		}
		for _, statement := range function.Body.List {
			deferred, ok := statement.(*ast.DeferStmt)
			if !ok {
				continue
			}
			closure, ok := deferred.Call.Fun.(*ast.FuncLit)
			if ok {
				if teardown != nil {
					t.Fatal("ambiguous whole-owner teardown")
				}
				teardown = closure.Body
			}
		}
	}
	if teardown == nil || len(teardown.List) < 2 {
		t.Fatal("missing actual whole-owner teardown")
	}
	for index, name := range []string{"cancel", "stopSubscriber"} {
		statement, ok := teardown.List[index].(*ast.ExprStmt)
		if !ok {
			t.Fatal("context cancellation is not the first teardown action")
		}
		call, ok := statement.X.(*ast.CallExpr)
		if !ok || len(call.Args) != 0 {
			t.Fatal("unexpected context cancellation call")
		}
		callee, ok := call.Fun.(*ast.Ident)
		if !ok || callee.Name != name {
			t.Fatal("root/child context cancellation order changed")
		}
	}
	ast.Inspect(teardown, func(node ast.Node) bool {
		call, ok := node.(*ast.CallExpr)
		if !ok {
			return true
		}
		selector, ok := call.Fun.(*ast.SelectorExpr)
		if !ok {
			return true
		}
		owner, ok := selector.X.(*ast.Ident)
		if ok && ((owner.Name == "sub" && selector.Sel.Name == "Cancel") ||
			(owner.Name == "topic" && selector.Sel.Name == "Close")) {
			t.Fatal("whole-owner teardown invokes live subscription/topic removal")
		}
		return true
	})
}

func TestPubsubScoringWholeOwnerStopNativeFixtureJoinsWithoutLeave(t *testing.T) {
	directory := t.TempDir()
	args := map[string]string{"version": "1.1", "transport": "tcp", "actor": "sink", "case-token": strings.Repeat("a", 32)}
	for _, name := range []string{"ready-file", "control-file", "result-file", "stop-file", "store-dir"} {
		args[name] = filepath.Join(directory, name)
	}
	done := make(chan struct{})
	var nativeErr error
	go func() {
		defer close(done)
		nativeErr = runPubsubScoringLive(args)
	}()
	t.Cleanup(func() {
		select {
		case <-done:
			return
		default:
		}
		// A malformed control command releases the same owned cleanup on a
		// failed test; it cannot fabricate a successful Prepare or Stop.
		if err := os.WriteFile(args["control-file"], []byte("{}\n"), 0o600); err != nil {
			t.Error(err)
		}
		select {
		case <-done:
		case <-time.After(7 * time.Second):
			panic("native fixture cleanup join budget exhausted")
		}
	})
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	waitFile := func(path string, matches func(map[string]any) bool) map[string]any {
		t.Helper()
		for {
			data, err := os.ReadFile(path)
			if err == nil {
				var value map[string]any
				if err := json.Unmarshal(data, &value); err != nil {
					t.Fatal(err)
				}
				if matches(value) {
					return value
				}
			} else if !errors.Is(err, os.ErrNotExist) {
				t.Fatal(err)
			}
			select {
			case <-done:
				t.Fatal("native fixture exited before requested observation", nativeErr)
			case <-ctx.Done():
				t.Fatal("native fixture observation timed out")
			case <-time.After(time.Millisecond):
			}
		}
	}
	ready := waitFile(args["ready-file"], func(value map[string]any) bool { return value["ready"] == true })
	local, ok := ready["local_peer_id"].(string)
	if !ok || ready["subscription_created"] != true {
		t.Fatal("native subscription/identity was not created")
	}
	if _, err := peer.Decode(local); err != nil {
		t.Fatal(err)
	}
	command, err := json.Marshal(pubsubScoringCommand{Sequence: 1, Kind: "prepare_shutdown", Actor: args["actor"],
		Token: args["case-token"], Local: local})
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(args["control-file"], append(command, '\n'), 0o600); err != nil {
		t.Fatal(err)
	}
	prepared := waitFile(args["result-file"], func(value map[string]any) bool {
		events, _ := value["events"].([]any)
		for _, item := range events {
			event, _ := item.(map[string]any)
			if event["kind"] == "command_done" && event["command_kind"] == "prepare_shutdown" &&
				event["command_sequence"] == float64(1) && event["status"] == "ok" {
				return true
			}
		}
		return false
	})
	prepareRef := 0
	for _, item := range prepared["events"].([]any) {
		event := item.(map[string]any)
		if event["kind"] == "shutdown_prepared" {
			prepareRef = int(event["sequence"].(float64))
		}
	}
	if prepareRef == 0 {
		t.Fatal("missing exact Prepare ACK")
	}
	quiesce, err := json.Marshal(pubsubScoringCommand{Sequence: 2, Kind: "quiesce_shutdown", Actor: args["actor"],
		Token: args["case-token"], Local: local, Prepare: prepareRef})
	if err != nil {
		t.Fatal(err)
	}
	control := append(append(append([]byte{}, command...), '\n'), append(quiesce, '\n')...)
	if err := os.WriteFile(args["control-file"], control, 0o600); err != nil {
		t.Fatal(err)
	}
	quiesced := waitFile(args["result-file"], func(value map[string]any) bool {
		for _, item := range value["events"].([]any) {
			event := item.(map[string]any)
			if event["kind"] == "command_done" && event["command_kind"] == "quiesce_shutdown" && event["status"] == "ok" {
				return true
			}
		}
		return false
	})
	if quiesced["finalized"] != false || quiesced["joined"] != false || quiesced["error"] != nil ||
		quiesced["active_fixture_workers"] != float64(0) || quiesced["active_stream_handlers_and_io"] != float64(0) {
		t.Fatal("quiesce did not retain the live controller with joined owned work", quiesced)
	}
	acknowledgements := 0
	for _, item := range quiesced["events"].([]any) {
		event := item.(map[string]any)
		if event["kind"] == "shutdown" {
			t.Fatal("quiesce closed Host before Stop")
		}
		if event["kind"] == "shutdown_quiesced" {
			acknowledgements++
			if event["pid"] != float64(os.Getpid()) || event["prepare_ack_sequence"] != float64(prepareRef) ||
				event["pubsub_callback_admission_closed"] != true || event["native_admission_closed"] != true ||
				event["active_callbacks"] != float64(0) || event["active_pubsub_streams"] != float64(0) {
				t.Fatal("quiesce ACK lost its actual owner/drain", event)
			}
		}
	}
	if acknowledgements != 1 {
		t.Fatal("missing actual quiesce ACK")
	}
	select {
	case <-done:
		t.Fatal("controller exited before delayed Stop", nativeErr)
	case <-time.After(25 * time.Millisecond):
	}
	if err := os.WriteFile(args["stop-file"], nil, 0o600); err != nil {
		t.Fatal(err)
	}
	select {
	case <-done:
		if nativeErr != nil {
			t.Fatal(nativeErr)
		}
	case <-ctx.Done():
		t.Fatal("native whole-owner stop did not join")
	}
	result := waitFile(args["result-file"], func(value map[string]any) bool { return value["finalized"] == true })
	if result["joined"] != true || result["host_close_returned"] != true || result["error"] != nil || result["overflow"] != false ||
		result["active_fixture_workers"] != float64(0) || result["active_stream_handlers_and_io"] != float64(0) ||
		result["active_callbacks"] != float64(0) || result["local_peer_id"] != local {
		t.Fatal("native whole-owner stop lost joins/accounting", result)
	}
	joins, shutdowns := 0, 0
	for _, item := range result["events"].([]any) {
		event := item.(map[string]any)
		switch event["kind"] {
		case "join":
			joins++
		case "leave":
			t.Fatal("whole-owner stop called the live unsubscribe path")
		case "shutdown":
			shutdowns++
			if event["context_cancelled"] != true || event["joined"] != true || event["host_close_returned"] != true ||
				event["active_stream_handlers_and_io"] != float64(0) || event["active_fixture_workers"] != float64(0) {
				t.Fatal("shutdown did not record actual context/owner joins", event)
			}
		}
	}
	if joins != 1 || shutdowns != 1 {
		t.Fatal("missing actual native subscription/shutdown", joins, shutdowns)
	}
}

func TestPubsubScoringCanonicalMessageSenderAuthorAndSeqno(t *testing.T) {
	from, sender := peer.ID("unit-author"), peer.ID("unit-relay")
	topic := "unit-topic"
	m := &pubsub.Message{Message: &pubsubpb.Message{From: []byte(from), Data: []byte("unit payload"), Seqno: []byte{1, 2, 3}, Topic: &topic},
		ReceivedFrom: sender, ID: "\x00unit-native-ID\xff"}
	fields := pubsubScoringMessage(m)
	if fields["author_peer"] != from.String() || fields["propagation_peer"] != sender.String() || fields["seqno_hex"] != "010203" ||
		fields["message_id"] != hex.EncodeToString([]byte(m.ID)) || fields["message_id_observed"] != true ||
		fields["message_id_basis"] != "native_pubsub_Message.ID" {
		t.Fatal("author/sender/ID attribution changed")
	}
	o := newPubsubScoringObserver("sink", strings.Repeat("a", 32))
	o.DeliverMessage(m)
	if len(o.events) != 1 || o.events[0]["kind"] != "delivery" || o.events[0]["source"] != "go.pubsub.RawTracer.DeliverMessage" ||
		o.events[0]["author_peer"] != from.String() || o.events[0]["propagation_peer"] != sender.String() {
		t.Fatal("native delivery callback source/attribution changed")
	}
}

func TestPubsubScoringSharedSignedMessageIDPreservesRawAuthorAndSeqno(t *testing.T) {
	for _, sequence := range []uint64{0, 1, 257, ^uint64(0)} {
		seqno := make([]byte, 8)
		binary.BigEndian.PutUint64(seqno, sequence)
		p := pubsubScoringUnitConnection(t).remote
		m := &pubsubpb.Message{From: []byte(p), Seqno: seqno, Data: []byte("unit payload")}
		expected := append(append([]byte{}, []byte(p)...), seqno...)
		got := pubsubScoringMessageID(m)
		if !bytes.Equal([]byte(got), expected) || got != pubsub.DefaultMsgIdFn(m) ||
			got == p.String()+strconv.FormatUint(sequence, 10) {
			t.Fatal("shared signed ID used base58/decimal or lost leading seqno bytes")
		}
		m.Data = []byte("different unit payload")
		if pubsubScoringMessageID(m) != got {
			t.Fatal("signed author/seqno identity depends on payload")
		}
	}
}

func TestPubsubScoringMissingNativeMessageIDIsNotReconstructed(t *testing.T) {
	m := &pubsub.Message{Message: &pubsubpb.Message{From: []byte("unit-author"), Seqno: []byte{1, 2, 3}}}
	value := pubsubScoringMessage(m)
	if pubsubScoringMessageID(m.Message) == "" || value["message_id"] != "" || value["message_id_observed"] != false {
		t.Fatal("configured function synthesized absent native observation")
	}
}

func TestPubsubScoringValidatorDecisionIsNotAnAppliedNativeOutcome(t *testing.T) {
	topic := "unit-topic"
	m := &pubsub.Message{Message: &pubsubpb.Message{From: []byte(peer.ID("unit-author")), Data: []byte("unit payload"),
		Seqno: []byte{1, 2, 3}, Topic: &topic}, ReceivedFrom: peer.ID("unit-relay")}
	m.ID = pubsubScoringMessageID(m.Message)
	for _, test := range []struct {
		decision pubsub.ValidationResult
		outcome  string
		reason   string
	}{
		{pubsub.ValidationReject, "reject", pubsub.RejectValidationFailed},
		{pubsub.ValidationIgnore, "ignore", pubsub.RejectValidationIgnored},
		{pubsub.ValidationAccept, "accept", ""},
	} {
		o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
		o.validationDecision(m.ReceivedFrom, m, test.decision)
		if len(o.events) != 1 || o.events[0]["source"] != "go.pubsub.ValidatorEx" ||
			o.events[0]["phase"] != "decision" || o.events[0]["committed"] != false ||
			o.events[0]["outcome"] != test.outcome || len(o.scores) != 0 {
			t.Fatal("decision callback fabricated an applied outcome/score")
		}
		source := "go.pubsub.RawTracer.DeliverMessage"
		if test.reason == "" {
			o.DeliverMessage(m)
		} else {
			source = "go.pubsub.RawTracer.RejectMessage"
			o.RejectMessage(m, test.reason)
		}
		if len(o.events) != 2 || o.events[1]["source"] != source || o.events[1]["phase"] != "post_decision" ||
			o.events[1]["committed"] != true || o.events[1]["outcome"] != test.outcome || len(o.scores) != 0 {
			t.Fatal("missing exact native postdecision outcome")
		}
		if test.reason != "" && o.events[1]["reason"] != test.reason {
			t.Fatal("raw native rejection reason was lost")
		}
		for _, key := range []string{"propagation_peer", "author_peer", "topic", "message_id", "seqno_hex", "payload_sha256"} {
			if o.events[0][key] != o.events[1][key] {
				t.Fatalf("native decision/outcome %s identity mismatch", key)
			}
		}
	}
}

func TestPubsubScoringOtherNativeRejectionsDoNotProveValidatorCommit(t *testing.T) {
	topic := "unit-topic"
	m := &pubsub.Message{Message: &pubsubpb.Message{Topic: &topic}, ReceivedFrom: peer.ID("unit-relay")}
	for _, reason := range []string{pubsub.RejectInvalidSignature, pubsub.RejectValidationQueueFull,
		pubsub.RejectValidationThrottled, "unit unknown native rejection"} {
		o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
		o.RejectMessage(m, reason)
		if len(o.events) != 1 || o.events[0]["source"] != "go.pubsub.RawTracer.RejectMessage" ||
			o.events[0]["reason"] != reason || o.events[0]["committed"] != false ||
			o.events[0]["phase"] != "native_rejection" || o.events[0]["outcome"] != nil {
			t.Fatal("other native rejection falsely authorized an application decision")
		}
	}
}

func TestPubsubScoringResultAtomicallySerializesCanonicalKeys(t *testing.T) {
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	o.emit("unit", "go.unit", nil)
	path := filepath.Join(t.TempDir(), "result.json")
	if err := pubsubScoringAtomic(path, o.result(true, true, nil, 0)); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var result map[string]any
	if err = json.Unmarshal(data, &result); err != nil || result["schema_version"] != float64(1) || result["finalized"] != true || result["error"] != nil || result["active_fixture_workers"] != float64(0) {
		t.Fatal("incorrect finalized result", err)
	}
	if _, exists := result["returncode"]; exists {
		t.Fatal("fixture invented its future process return code")
	}
	if !strings.Contains(result["joined_scope"].(string), "fixture_workers") {
		t.Fatal("join scope missing")
	}
	info, err := os.Stat(path)
	if err != nil || info.Mode().Perm() != 0o600 {
		t.Fatal("result permissions", err)
	}
}

var _ io.ReadWriter = (*pubsubScoringUnitStream)(nil)
