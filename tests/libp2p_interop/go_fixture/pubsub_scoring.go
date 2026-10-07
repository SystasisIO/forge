package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"
	"unicode/utf8"

	libp2p "github.com/libp2p/go-libp2p"
	pubsub "github.com/libp2p/go-libp2p-pubsub"
	pubsubpb "github.com/libp2p/go-libp2p-pubsub/pb"
	"github.com/libp2p/go-libp2p/core/host"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/core/protocol"
	"github.com/libp2p/go-libp2p/core/transport"
	"github.com/libp2p/go-libp2p/p2p/muxer/yamux"
	"github.com/libp2p/go-libp2p/p2p/security/noise"
	"github.com/libp2p/go-libp2p/p2p/transport/tcp"
	"github.com/libp2p/go-libp2p/p2p/transport/tcpreuse"
	nativeyamux "github.com/libp2p/go-yamux/v5"
	ma "github.com/multiformats/go-multiaddr"
	manet "github.com/multiformats/go-multiaddr/net"
)

const (
	pubsubScoringCommands   = 64
	pubsubScoringLine       = 16 * 1024
	pubsubScoringPayload    = 1024
	pubsubScoringEvents     = 2048
	pubsubScoringFrame      = 16 * 1024
	pubsubScoringWireBytes  = 4 * 1024 * 1024
	pubsubScoringEventBytes = 64 * 1024
	pubsubScoringTraceBytes = 16*1024*1024 - pubsubScoringEventBytes
	pubsubScoringPeers      = 16
	pubsubScoringStreams    = 64
)

func parsePubsubScoringArgs(argv []string) (map[string]string, error) {
	required := map[string]bool{"version": true, "transport": true, "actor": true, "case-token": true,
		"ready-file": true, "control-file": true, "result-file": true, "stop-file": true, "store-dir": true}
	args := map[string]string{}
	if len(argv)%2 != 0 {
		return nil, fmt.Errorf("pubsub-live requires flag/value pairs")
	}
	for i := 0; i < len(argv); i += 2 {
		name := strings.TrimPrefix(argv[i], "--")
		if !strings.HasPrefix(argv[i], "--") || (!required[name] && name != "pnet-key-file" && name != "pnet-fingerprint") ||
			args[name] != "" || argv[i+1] == "" || strings.ContainsRune(argv[i+1], '\x00') {
			return nil, fmt.Errorf("invalid/duplicate pubsub-live flag")
		}
		args[name] = argv[i+1]
	}
	for name := range required {
		if args[name] == "" {
			return nil, fmt.Errorf("missing pubsub-live flag %s", name)
		}
	}
	if (args["version"] != "1.0" && args["version"] != "1.1") || !coordinatedHex(args["case-token"], 32) ||
		(args["actor"] != "victim" && args["actor"] != "offender" && args["actor"] != "replacement" && args["actor"] != "sink") {
		return nil, fmt.Errorf("invalid pubsub-live version/actor/token")
	}
	switch args["transport"] {
	case "tcp", "quic":
		if args["pnet-key-file"] != "" || args["pnet-fingerprint"] != "" {
			return nil, fmt.Errorf("public pubsub-live cannot accept private key flags")
		}
	case "tcp-pnet-noise":
		if args["pnet-key-file"] == "" || !coordinatedHex(args["pnet-fingerprint"], 64) {
			return nil, fmt.Errorf("private pubsub-live requires key file and fingerprint")
		}
	default:
		return nil, fmt.Errorf("unsupported pubsub-live transport")
	}
	paths := map[string]bool{}
	for _, name := range []string{"ready-file", "control-file", "result-file", "stop-file", "store-dir", "pnet-key-file"} {
		if args[name] == "" {
			continue
		}
		path, err := filepath.Abs(args[name])
		if err != nil || paths[path] {
			return nil, fmt.Errorf("pubsub-live paths must be distinct")
		}
		paths[path] = true
	}
	return args, nil
}

type pubsubScoringCommand struct {
	Sequence int    `json:"sequence"`
	Kind     string `json:"kind"`
	Peer     string `json:"peer_id,omitempty"`
	Address  string `json:"address,omitempty"`
	Payload  string `json:"payload,omitempty"`
	Label    string `json:"label,omitempty"`
	Actor    string `json:"actor,omitempty"`
	Token    string `json:"case_token,omitempty"`
	Local    string `json:"local_peer_id,omitempty"`
}

// Decode fields once: encoding/json otherwise silently accepts duplicate keys.
func decodePubsubScoringCommand(line []byte, next int) (pubsubScoringCommand, error) {
	var command pubsubScoringCommand
	if len(line) == 0 || len(line) > pubsubScoringLine || !utf8.Valid(line) || next > pubsubScoringCommands {
		return command, fmt.Errorf("pubsub control line exceeds bounds")
	}
	d := json.NewDecoder(bytes.NewReader(line))
	token, err := d.Token()
	if err != nil || token != json.Delim('{') {
		return command, fmt.Errorf("pubsub command must be an object")
	}
	fields := map[string]json.RawMessage{}
	for d.More() {
		token, err = d.Token()
		name, ok := token.(string)
		if err != nil || !ok || fields[name] != nil {
			return command, fmt.Errorf("duplicate/invalid pubsub command field")
		}
		var value json.RawMessage
		if err = d.Decode(&value); err != nil {
			return command, fmt.Errorf("invalid pubsub command field")
		}
		fields[name] = value
	}
	if token, err = d.Token(); err != nil || token != json.Delim('}') {
		return command, fmt.Errorf("invalid pubsub command object")
	}
	if _, err = d.Token(); err != io.EOF {
		return command, fmt.Errorf("trailing pubsub command data")
	}
	strict := json.NewDecoder(bytes.NewReader(line))
	strict.DisallowUnknownFields()
	if err = strict.Decode(&command); err != nil || command.Sequence != next || next < 1 {
		return command, fmt.Errorf("invalid/noncontiguous pubsub command")
	}
	allowed := map[string]bool{"sequence": true, "kind": true}
	switch command.Kind {
	case "connect":
		allowed["peer_id"], allowed["address"] = true, true
		if _, err = peer.Decode(command.Peer); err != nil || command.Address == "" {
			return command, fmt.Errorf("pubsub connect lacks peer/address")
		}
		if _, err = ma.NewMultiaddr(command.Address); err != nil {
			return command, fmt.Errorf("invalid pubsub connect address")
		}
	case "publish":
		allowed["payload"] = true
		if len(command.Payload) == 0 || len(command.Payload) > pubsubScoringPayload || strings.ContainsRune(command.Payload, '\x00') {
			return command, fmt.Errorf("pubsub publish payload exceeds bounds")
		}
	case "sample":
		allowed["label"] = true
		if len(command.Label) == 0 || len(command.Label) > 128 || strings.ContainsRune(command.Label, '\x00') || command.Label == "periodic" {
			return command, fmt.Errorf("pubsub sample label exceeds bounds")
		}
	case "prepare_shutdown":
		allowed["actor"], allowed["case_token"], allowed["local_peer_id"] = true, true, true
		if command.Actor == "" || len(command.Actor) > 64 || len(command.Token) != 32 {
			return command, fmt.Errorf("invalid prepare_shutdown actor/token")
		}
		if _, err = peer.Decode(command.Local); err != nil {
			return command, fmt.Errorf("invalid prepare_shutdown identity")
		}
	default:
		return command, fmt.Errorf("unknown pubsub command")
	}
	for name, value := range fields {
		if !allowed[name] || bytes.Equal(value, []byte("null")) {
			return command, fmt.Errorf("unexpected/null pubsub command field")
		}
	}
	return command, nil
}

type pubsubScoringControl struct {
	seen   []byte
	offset int
	next   int
}

func (c *pubsubScoringControl) read(path string) ([]pubsubScoringCommand, error) {
	f, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		if len(c.seen) != 0 {
			return nil, fmt.Errorf("pubsub control file disappeared")
		}
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	defer f.Close()
	data, err := io.ReadAll(io.LimitReader(f, pubsubScoringCommands*(pubsubScoringLine+1)+1))
	if err != nil || len(data) > pubsubScoringCommands*(pubsubScoringLine+1) || !bytes.HasPrefix(data, c.seen) {
		return nil, fmt.Errorf("pubsub control is not bounded append-only JSONL")
	}
	c.seen = data
	if c.next == 0 {
		c.next = 1
	}
	commands := []pubsubScoringCommand{}
	for {
		end := bytes.IndexByte(data[c.offset:], '\n')
		if end < 0 {
			if len(data)-c.offset > pubsubScoringLine {
				return nil, fmt.Errorf("unfinished pubsub command exceeds bound")
			}
			return commands, nil
		}
		command, err := decodePubsubScoringCommand(data[c.offset:c.offset+end], c.next)
		if err != nil {
			return nil, err
		}
		commands = append(commands, command)
		c.offset += end + 1
		c.next++
	}
}

type pubsubScoringObserver struct {
	mu                  sync.Mutex
	origin              time.Time
	local               peer.ID
	actor, token, topic string
	events              []map[string]any
	mesh                map[peer.ID]bool
	scores              []map[string]any
	connections         map[string]map[string]any
	scoreSequence       int
	inspectionStarted   atomic.Uint64
	scoreInspection     uint64
	scoreChanged        chan struct{}
	wireBytes           int
	traceBytes          int
	overflow            bool
	failure             error
	closing             bool
	prepared            bool
	prepareAck          int
	callbacks           int
	callbackChanged     chan struct{}
	quic                *pubsubQUICObserver
}

func newPubsubScoringObserver(actor, token string) *pubsubScoringObserver {
	return &pubsubScoringObserver{origin: time.Now(), actor: actor, token: token, topic: "forge-pr11:" + token,
		events: []map[string]any{}, mesh: map[peer.ID]bool{}, scores: []map[string]any{},
		connections: map[string]map[string]any{}, scoreChanged: make(chan struct{}), callbackChanged: make(chan struct{}, 1)}
}

func (o *pubsubScoringObserver) notifyScoreLocked() {
	close(o.scoreChanged)
	o.scoreChanged = make(chan struct{})
}

func (o *pubsubScoringObserver) fail(err error) {
	if err == nil {
		return
	}
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.failure == nil {
		o.failure = err
		o.notifyScoreLocked()
	}
}

func (o *pubsubScoringObserver) admitCommand() error {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.prepared {
		return fmt.Errorf("PubSub command admission closed after prepare_shutdown")
	}
	if o.failure != nil || o.overflow {
		return fmt.Errorf("PubSub sticky observation failure")
	}
	return nil
}

func (o *pubsubScoringObserver) prepareShutdown(command pubsubScoringCommand, pending int) error {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.prepared || o.failure != nil || o.overflow || pending != 0 {
		return fmt.Errorf("prepare_shutdown requires open admission, no pending commands and no errors")
	}
	if command.Actor != o.actor || command.Token != o.token || command.Local != o.local.String() {
		return fmt.Errorf("prepare_shutdown actor/token/identity mismatch")
	}
	fields := map[string]any{
		"command_sequence": command.Sequence, "actor": o.actor, "case_token": o.token,
		"local_peer_id": o.local.String(), "admission_closed": true, "pending_commands": 0}
	if o.quic != nil {
		fields["native_quic_context_snapshots"] = o.quic.prepare(len(o.events) + 1)
	}
	if o.failure != nil {
		return o.failure
	}
	o.prepared = true
	o.emitLocked("shutdown_prepared", "go.fixture.prepare_shutdown", fields)
	if o.failure == nil {
		o.prepareAck = len(o.events)
		if o.quic != nil {
			o.quic.publishedAck.Store(int64(o.prepareAck))
		}
	}
	return o.failure
}

func (o *pubsubScoringObserver) emitLocked(kind, source string, fields map[string]any) {
	if len(o.events) == pubsubScoringEvents {
		o.overflow = true
		if o.failure == nil {
			o.failure = fmt.Errorf("pubsub observation overflow")
		}
		return
	}
	value := map[string]any{"sequence": len(o.events) + 1, "mono_ns": time.Since(o.origin).Nanoseconds() + 1,
		"kind": kind, "source": source}
	for key, field := range fields {
		if key == "sequence" || key == "mono_ns" || key == "kind" || key == "source" {
			if o.failure == nil {
				o.failure = fmt.Errorf("PubSub observation attempts to replace event provenance")
			}
			return
		}
		value[key] = field
	}
	encoded, err := json.Marshal(value)
	if err != nil || len(encoded) > pubsubScoringEventBytes || o.traceBytes+len(encoded) > pubsubScoringTraceBytes {
		o.overflow = true
		if o.failure == nil {
			o.failure = fmt.Errorf("PubSub event/trace serialization exceeds bounds")
		}
		return
	}
	o.traceBytes += len(encoded)
	o.events = append(o.events, value)
}

func (o *pubsubScoringObserver) emit(kind, source string, fields map[string]any) {
	o.mu.Lock()
	defer o.mu.Unlock()
	o.emitLocked(kind, source, fields)
}

func (o *pubsubScoringObserver) callback(f func()) {
	o.mu.Lock()
	if o.closing {
		o.mu.Unlock()
		return
	}
	o.callbacks++
	o.mu.Unlock()
	defer func() {
		o.mu.Lock()
		o.callbacks--
		o.mu.Unlock()
		select {
		case o.callbackChanged <- struct{}{}:
		default:
		}
	}()
	f()
}

// The strict signed actor profile shares Forge codec::message_id's signed
// branch. Seqno stays in its original wire bytes, not decimal text.
func pubsubScoringMessageID(m *pubsubpb.Message) string {
	return string(m.GetFrom()) + string(m.GetSeqno())
}

func pubsubScoringMessage(m *pubsub.Message) map[string]any {
	digest := sha256.Sum256(m.GetData())
	return map[string]any{"propagation_peer": m.ReceivedFrom.String(), "author_peer": m.GetFrom().String(),
		"author_hex": hex.EncodeToString(m.Message.GetFrom()), "topic": m.GetTopic(),
		"payload_sha256": hex.EncodeToString(digest[:]), "payload_bytes": len(m.GetData()),
		"message_id": hex.EncodeToString([]byte(m.ID)), "message_id_observed": m.ID != "",
		"message_id_basis": "native_pubsub_Message.ID", "seqno_hex": hex.EncodeToString(m.GetSeqno())}
}

func (o *pubsubScoringObserver) message(kind, method string, m *pubsub.Message, result string) {
	o.callback(func() {
		fields := pubsubScoringMessage(m)
		if result != "" {
			fields["outcome"] = result
		}
		if method == "DeliverMessage" {
			fields["phase"], fields["committed"] = "post_decision", true
		}
		o.emit(kind, "go.pubsub.RawTracer."+method, fields)
	})
}

func (o *pubsubScoringObserver) validationDecision(p peer.ID, m *pubsub.Message, result pubsub.ValidationResult) {
	o.callback(func() {
		value := pubsubScoringMessage(m)
		value["propagation_peer"] = p.String()
		value["phase"], value["committed"] = "decision", false
		value["outcome"] = map[pubsub.ValidationResult]string{pubsub.ValidationAccept: "accept", pubsub.ValidationReject: "reject", pubsub.ValidationIgnore: "ignore"}[result]
		o.emit("validation", "go.pubsub.ValidatorEx", value)
	})
}

func (o *pubsubScoringObserver) meshEvent(kind string, p peer.ID, topic string) {
	o.callback(func() {
		o.mu.Lock()
		defer o.mu.Unlock()
		if topic == o.topic {
			if kind == "graft" {
				o.mesh[p] = true
			} else {
				delete(o.mesh, p)
			}
		}
		o.emitLocked(kind, "go.pubsub.RawTracer."+kind, map[string]any{"remote_peer_id": p.String(), "topic": topic})
	})
}

func (o *pubsubScoringObserver) OnNewOutboundStream(p peer.ID, id protocol.ID) {
	o.callback(func() {
		o.emit("peer_protocol", "go.pubsub.RawTracer.OnNewOutboundStream",
			map[string]any{"remote_peer_id": p.String(), "protocol": string(id)})
	})
}
func (o *pubsubScoringObserver) OnClosedOutboundStream(p peer.ID) {
	o.callback(func() {
		o.mu.Lock()
		defer o.mu.Unlock()
		delete(o.mesh, p)
		o.emitLocked("peer_down", "go.pubsub.RawTracer.OnClosedOutboundStream", map[string]any{"remote_peer_id": p.String()})
	})
}
func (o *pubsubScoringObserver) Join(topic string) {
	o.callback(func() { o.emit("join", "go.pubsub.RawTracer.Join", map[string]any{"topic": topic}) })
}
func (o *pubsubScoringObserver) Leave(topic string) {
	o.callback(func() {
		o.mu.Lock()
		defer o.mu.Unlock()
		if topic == o.topic {
			o.mesh = map[peer.ID]bool{}
		}
		o.emitLocked("leave", "go.pubsub.RawTracer.Leave", map[string]any{"topic": topic})
	})
}
func (o *pubsubScoringObserver) Graft(p peer.ID, topic string) { o.meshEvent("graft", p, topic) }
func (o *pubsubScoringObserver) Prune(p peer.ID, topic string) { o.meshEvent("prune", p, topic) }
func (o *pubsubScoringObserver) ValidateMessage(m *pubsub.Message) {
	o.message("validation_begin", "ValidateMessage", m, "")
}
func (o *pubsubScoringObserver) DeliverMessage(m *pubsub.Message) {
	o.message("delivery", "DeliverMessage", m, "accept")
}
func (o *pubsubScoringObserver) RejectMessage(m *pubsub.Message, reason string) {
	o.callback(func() {
		value := pubsubScoringMessage(m)
		value["reason"] = reason
		value["phase"], value["committed"], value["outcome"] = "native_rejection", false, nil
		switch reason {
		case pubsub.RejectValidationFailed:
			value["phase"], value["committed"], value["outcome"] = "post_decision", true, "reject"
		case pubsub.RejectValidationIgnored:
			value["phase"], value["committed"], value["outcome"] = "post_decision", true, "ignore"
		}
		o.emit("rejection", "go.pubsub.RawTracer.RejectMessage", value)
	})
}
func (o *pubsubScoringObserver) DuplicateMessage(m *pubsub.Message) {
	o.message("duplicate", "DuplicateMessage", m, "")
}
func (o *pubsubScoringObserver) UndeliverableMessage(m *pubsub.Message) {
	o.message("undeliverable", "UndeliverableMessage", m, "local_subscription_backpressure")
}
func (o *pubsubScoringObserver) ThrottlePeer(p peer.ID) {
	o.callback(func() {
		o.emit("throttle", "go.pubsub.RawTracer.ThrottlePeer", map[string]any{"remote_peer_id": p.String()})
	})
}

// RawTracer submission is explicitly not a network receipt. The stream wrapper
// below captures actual completed frames and their native stream/connection IDs.
func (o *pubsubScoringObserver) rpc(kind string, rpc *pubsub.RPC, p peer.ID) {
	o.callback(func() {
		data, err := rpc.Marshal()
		if err != nil {
			o.fail(fmt.Errorf("cannot inspect native PubSub RPC"))
			return
		}
		digest := sha256.Sum256(data)
		o.emit(kind, "go.pubsub.RawTracer."+kind, map[string]any{"remote_peer_id": p.String(),
			"submission_sha256": hex.EncodeToString(digest[:]), "wire_receipt": false})
	})
}
func (o *pubsubScoringObserver) RecvRPC(rpc *pubsub.RPC)            { o.rpc("rpc_receive", rpc, rpc.From()) }
func (o *pubsubScoringObserver) SendRPC(rpc *pubsub.RPC, p peer.ID) { o.rpc("rpc_send", rpc, p) }
func (o *pubsubScoringObserver) DropRPC(rpc *pubsub.RPC, p peer.ID) { o.rpc("rpc_drop", rpc, p) }

func (o *pubsubScoringObserver) inspect(scores map[peer.ID]*pubsub.PeerScoreSnapshot) {
	// The native capture time is unavailable; order actual inspector callback entry,
	// including callbacks still copying values when a sample is requested.
	inspection := o.inspectionStarted.Add(1)
	o.inspectStarted(inspection, scores)
}

func (o *pubsubScoringObserver) inspectStarted(inspection uint64, scores map[peer.ID]*pubsub.PeerScoreSnapshot) {
	o.callback(func() {
		if len(scores) > pubsubScoringPeers {
			o.fail(fmt.Errorf("native PubSub scores exceed peer bound"))
			return
		}
		values := []map[string]any{}
		for p, snapshot := range scores {
			if snapshot == nil || math.IsNaN(snapshot.Score) || math.IsInf(snapshot.Score, 0) {
				o.fail(fmt.Errorf("native PubSub inspector returned invalid score"))
				return
			}
			topics := map[string]any{}
			invalid := float64(0)
			topicPresent := false
			for topic, value := range snapshot.Topics {
				if value == nil || math.IsNaN(value.InvalidMessageDeliveries) || math.IsInf(value.InvalidMessageDeliveries, 0) {
					o.fail(fmt.Errorf("native PubSub inspector returned invalid topic counter"))
					return
				}
				if topic == o.topic {
					invalid, topicPresent = value.InvalidMessageDeliveries, true
				}
				topics[topic] = map[string]any{"invalid_message_deliveries": value.InvalidMessageDeliveries,
					"first_message_deliveries": value.FirstMessageDeliveries, "mesh_message_deliveries": value.MeshMessageDeliveries,
					"time_in_mesh_ns": value.TimeInMesh.Nanoseconds()}
			}
			values = append(values, map[string]any{"peer_id": p.String(), "value": snapshot.Score, "topics": topics,
				"invalid_deliveries": invalid, "topic_stats_present": topicPresent,
				"application_score": snapshot.AppSpecificScore, "ip_colocation_factor": snapshot.IPColocationFactor,
				"behaviour_penalty": snapshot.BehaviourPenalty})
		}
		sort.Slice(values, func(i, j int) bool { return values[i]["peer_id"].(string) < values[j]["peer_id"].(string) })
		o.mu.Lock()
		defer o.mu.Unlock()
		o.emitLocked("score", "go.pubsub.WithPeerScoreInspect", map[string]any{"peer_scores": values, "timestamp_basis": "inspection_observation"})
		if o.failure != nil {
			o.notifyScoreLocked()
			return
		}
		if inspection <= o.scoreInspection {
			return
		}
		o.scores, o.scoreSequence = values, len(o.events)
		o.scoreInspection = inspection
		o.sampleLocked("periodic")
		o.notifyScoreLocked()
	})
}

func (o *pubsubScoringObserver) sample(ctx context.Context, label string) error {
	deadline := time.Now().Add(10 * time.Second)
	timeout := time.NewTimer(time.Until(deadline))
	defer timeout.Stop()
	o.mu.Lock()
	minimum := o.inspectionStarted.Load()
	for {
		if err := ctx.Err(); err != nil {
			o.mu.Unlock()
			return fmt.Errorf("PubSub sample awaiting fresh native score inspection: %w", err)
		}
		if !time.Now().Before(deadline) {
			o.mu.Unlock()
			return fmt.Errorf("PubSub sample awaiting fresh native score inspection: %w", context.DeadlineExceeded)
		}
		if o.failure != nil {
			err := o.failure
			o.mu.Unlock()
			return err
		}
		if o.closing {
			o.mu.Unlock()
			return fmt.Errorf("PubSub sample requested while observer is closing")
		}
		if o.scoreInspection > minimum && o.scoreSequence != 0 {
			o.sampleLocked(label)
			err := o.failure
			o.mu.Unlock()
			return err
		}
		changed := o.scoreChanged
		o.mu.Unlock()
		select {
		case <-ctx.Done():
			return fmt.Errorf("PubSub sample awaiting fresh native score inspection: %w", ctx.Err())
		case <-timeout.C:
			return fmt.Errorf("PubSub sample awaiting fresh native score inspection: %w", context.DeadlineExceeded)
		case <-changed:
		}
		o.mu.Lock()
	}
}

func (o *pubsubScoringObserver) sampleLocked(label string) {
	members := []string{}
	for p := range o.mesh {
		members = append(members, p.String())
	}
	sort.Strings(members)
	o.emitLocked("snapshot", "go.pubsub.RawTracer_mesh_and_score_inspection", map[string]any{"label": label,
		"topic": o.topic, "mesh_peer_ids": members, "peer_scores": o.scores, "score_observation_sequence": o.scoreSequence,
		"mesh_basis": "derived_native_Graft_Prune_Leave_OnClosedOutboundStream_ledger", "timestamp_basis": "inspection_observation"})
}

func pubsubScoringValidator(actor, token string, data []byte) pubsub.ValidationResult {
	if actor == "victim" {
		if bytes.HasPrefix(data, []byte("reject:"+token+":")) {
			return pubsub.ValidationReject
		}
		if bytes.HasPrefix(data, []byte("ignore:"+token+":")) {
			return pubsub.ValidationIgnore
		}
	}
	return pubsub.ValidationAccept
}

func pubsubScoringParameters(topic string) (pubsub.GossipSubParams, *pubsub.PeerScoreParams, *pubsub.PeerScoreThresholds) {
	params := pubsub.DefaultGossipSubParams()
	params.D, params.Dlo, params.Dhi, params.Dscore, params.Dout = 2, 1, 4, 1, 0
	params.HeartbeatInterval = 250 * time.Millisecond
	params.PruneBackoff, params.UnsubscribeBackoff = time.Second, time.Second
	score := &pubsub.PeerScoreParams{Topics: map[string]*pubsub.TopicScoreParams{topic: {
		TopicWeight: 1, TimeInMeshQuantum: time.Second,
		InvalidMessageDeliveriesWeight: -100, InvalidMessageDeliveriesDecay: 0.99,
	}}, AppSpecificScore: func(peer.ID) float64 { return 0 }, DecayInterval: time.Second,
		DecayToZero: 0.01, RetainScore: 60 * time.Second}
	thresholds := &pubsub.PeerScoreThresholds{GossipThreshold: -10, PublishThreshold: -50,
		GraylistThreshold: -80, AcceptPXThreshold: 10, OpportunisticGraftThreshold: 20}
	return params, score, thresholds
}

func pubsubScoringConnection(c network.Conn, g *pathDialObserver) (map[string]any, error) {
	if c == nil || c.ID() == "" || c.RemotePublicKey() == nil || c.LocalPeer() == "" || c.RemotePeer() == "" ||
		c.LocalMultiaddr() == nil || c.RemoteMultiaddr() == nil ||
		(c.Stat().Direction != network.DirInbound && c.Stat().Direction != network.DirOutbound) {
		return nil, fmt.Errorf("PubSub connection lacks native authenticated owner")
	}
	id, err := peer.IDFromPublicKey(c.RemotePublicKey())
	if err != nil || id != c.RemotePeer() {
		return nil, fmt.Errorf("PubSub connection key/peer mismatch")
	}
	key, err := c.RemotePublicKey().Raw()
	if err != nil {
		return nil, fmt.Errorf("PubSub cannot inspect native remote public key")
	}
	digest := sha256.Sum256(key)
	value := endpointConnectionReceipt(c)
	value["connection_state_source"] = value["source"]
	delete(value, "source")
	value["peer_id"] = c.RemotePeer().String()
	value["authenticated"], value["remote_public_key_sha256"] = true, hex.EncodeToString(digest[:])
	value["direction"] = c.Stat().Direction.String()
	if c.ConnState().Transport == "quic-v1" {
		if g == nil {
			return nil, fmt.Errorf("PubSub QUIC connection lacks its actual secured callback owner")
		}
		secured, err := g.quicSecurity(c)
		if err != nil {
			return nil, err
		}
		value["quic_security"] = secured.receipt()
		// Canonical TLS transport classification is backed by the actual typed
		// QUIC secured owner, not a negotiated stream-security or ALPN constant.
		value["native_stream_security_protocol"] = string(c.ConnState().Security)
		value["transport"], value["security"] = "quic", "/tls/1.0.0"
		value["authentication_basis"] = "native_quic_TLS_InterceptSecured_and_RemotePublicKey"
		value["muxer_basis"] = "native_quic_connection_streams_not_multistream_muxer_selection"
	} else if c.ConnState().Transport != "tcp" || c.ConnState().Security != noise.ID || c.ConnState().StreamMultiplexer != yamux.ID {
		return nil, fmt.Errorf("PubSub connection lacks actual TCP/Noise/Yamux output")
	} else {
		value["authentication_basis"] = "native_TCP_ConnState_and_authenticated_RemotePublicKey"
	}
	return value, nil
}

// The installed PSK remains solely in the donor upgrader. Successful native
// capable output is retained without observing pre-protector key/ciphertext.
type pubsubScoringPnetUpgrader struct{ transport.Upgrader }
type pubsubScoringPnetConn struct{ transport.CapableConn }
type pubsubScoringPnetListener struct{ transport.Listener }

func (c *pubsubScoringPnetConn) As(target any) bool {
	if out, ok := target.(**pubsubScoringPnetConn); ok {
		*out = c
		return true
	}
	return c.CapableConn.As(target)
}
func (u *pubsubScoringPnetUpgrader) Upgrade(ctx context.Context, tr transport.Transport, raw manet.Conn,
	direction network.Direction, p peer.ID, scope network.ConnManagementScope) (transport.CapableConn, error) {
	c, err := u.Upgrader.Upgrade(ctx, tr, raw, direction, p, scope)
	if err != nil {
		return c, err
	}
	return &pubsubScoringPnetConn{CapableConn: c}, nil
}
func (u *pubsubScoringPnetUpgrader) UpgradeGatedMaListener(tr transport.Transport, l transport.GatedMaListener) transport.Listener {
	return &pubsubScoringPnetListener{Listener: u.Upgrader.UpgradeGatedMaListener(tr, l)}
}
func (u *pubsubScoringPnetUpgrader) UpgradeListener(tr transport.Transport, l manet.Listener) transport.Listener {
	return &pubsubScoringPnetListener{Listener: u.Upgrader.UpgradeListener(tr, l)}
}
func (l *pubsubScoringPnetListener) Accept() (transport.CapableConn, error) {
	c, err := l.Listener.Accept()
	if err != nil {
		return c, err
	}
	return &pubsubScoringPnetConn{CapableConn: c}, nil
}
func pubsubScoringPrivateTCP(u transport.Upgrader, r network.ResourceManager, m *tcpreuse.ConnMgr) (*tcp.TcpTransport, error) {
	return tcp.NewTCPTransport(&pubsubScoringPnetUpgrader{Upgrader: u}, r, m)
}

func (o *pubsubScoringObserver) connection(c network.Conn, g *pathDialObserver, fingerprint string) error {
	value, err := pubsubScoringConnection(c, g)
	if err != nil {
		return err
	}
	if o.quic != nil {
		var lower *pubsubQUICConn
		if !c.As(&lower) || lower == nil || lower.owner != o.quic || lower.receipt == 0 {
			return fmt.Errorf("PubSub QUIC connection lacks its lower authenticated observer owner")
		}
		value["lower_connection_receipt_sequence"] = lower.receipt
		value["lower_stream_binding_basis"] = "same_native_CapableConn_not_Swarm_stream_ID_mapping"
	}
	if fingerprint != "" {
		var output *pubsubScoringPnetConn
		if !c.As(&output) || output == nil || output.RemotePeer() != c.RemotePeer() ||
			output.LocalPeer() != c.LocalPeer() || output.RemotePublicKey() == nil ||
			output.RemoteMultiaddr() == nil || output.LocalMultiaddr() == nil || !output.RemotePublicKey().Equals(c.RemotePublicKey()) ||
			output.RemoteMultiaddr().String() != c.RemoteMultiaddr().String() ||
			output.LocalMultiaddr().String() != c.LocalMultiaddr().String() {
			return fmt.Errorf("private PubSub connection lacks its successful native protector/upgrader owner")
		}
		value["pnet_verified"], value["pnet_fingerprint"] = true, fingerprint
		value["pnet_basis"] = "native_installed_PSK_protector_then_successful_upgrader_capable_output"
	}
	o.mu.Lock()
	defer o.mu.Unlock()
	if previous, ok := o.connections[c.ID()]; ok {
		if previous["peer_id"] != value["peer_id"] || previous["remote_public_key_sha256"] != value["remote_public_key_sha256"] ||
			previous["security"] != value["security"] || previous["muxer"] != value["muxer"] || previous["remote_address"] != value["remote_address"] {
			return fmt.Errorf("native PubSub connection ID has conflicting owners")
		}
		return nil
	}
	if len(o.connections) == pubsubScoringPeers {
		return fmt.Errorf("PubSub observed connection/peer bound exceeded")
	}
	o.connections[c.ID()] = value
	o.emitLocked("connection", "go.network.Conn.authenticated_output", value)
	return nil
}

type pubsubScoringDrain struct {
	mu      sync.Mutex
	closing bool
	active  int
	streams map[string]pubsubScoringDrainedStream
	seen    map[string]bool
	changed chan struct{}
}

type pubsubScoringDrainedStream interface {
	ID() string
	Reset() error
	framingJoined() bool
}

func newPubsubScoringDrain() *pubsubScoringDrain {
	return &pubsubScoringDrain{streams: map[string]pubsubScoringDrainedStream{}, seen: map[string]bool{}, changed: make(chan struct{}, 1)}
}
func (d *pubsubScoringDrain) begin() bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.closing {
		return false
	}
	d.active++
	return true
}
func (d *pubsubScoringDrain) end() {
	d.mu.Lock()
	d.active--
	d.mu.Unlock()
	select {
	case d.changed <- struct{}{}:
	default:
	}
}
func (d *pubsubScoringDrain) retain(s pubsubScoringDrainedStream) bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.closing || len(d.seen) == pubsubScoringStreams || d.seen[s.ID()] {
		return false
	}
	d.seen[s.ID()] = true
	d.streams[s.ID()] = s
	return true
}
func (d *pubsubScoringDrain) release(s pubsubScoringDrainedStream) {
	d.mu.Lock()
	delete(d.streams, s.ID())
	d.mu.Unlock()
	select {
	case d.changed <- struct{}{}:
	default:
	}
}
func (d *pubsubScoringDrain) stop(ctx context.Context) error {
	d.mu.Lock()
	d.closing = true
	streams := make([]pubsubScoringDrainedStream, 0, len(d.streams))
	for _, s := range d.streams {
		streams = append(streams, s)
	}
	d.mu.Unlock()
	var failure error
	// Dispatch every native reset before waiting for any blocked I/O to finish.
	for _, s := range streams {
		failure = errors.Join(failure, s.Reset())
	}
	for {
		d.mu.Lock()
		active := d.active
		d.mu.Unlock()
		joined := active == 0
		for _, s := range streams {
			joined = s.framingJoined() && joined
		}
		if joined {
			return failure
		}
		select {
		case <-d.changed:
		case <-ctx.Done():
			return errors.Join(failure, fmt.Errorf("PubSub native handlers/I/O/framing did not drain: %w", ctx.Err()))
		}
	}
}

type pubsubScoringHost struct {
	host.Host
	ctx         context.Context
	observer    *pubsubScoringObserver
	gater       *pathDialObserver
	drain       *pubsubScoringDrain
	protocol    protocol.ID
	fingerprint string
}

func (h *pubsubScoringHost) wrap(s network.Stream) (network.Stream, error) {
	if s == nil {
		return nil, fmt.Errorf("PubSub native host returned no stream")
	}
	err := h.observer.connection(s.Conn(), h.gater, h.fingerprint)
	if err != nil {
		return nil, err
	}
	state := s.Conn().ConnState()
	if h.observer.quic != nil {
		wrapped := &pubsubQUICHostStream{Stream: s, drain: h.drain}
		if s.ID() == "" || s.Protocol() != h.protocol || !h.drain.retain(wrapped) {
			return nil, fmt.Errorf("PubSub QUIC host stream lacks selected protocol/admitted owner")
		}
		h.observer.emit("protocol", "go.network.Stream.Protocol", map[string]any{"connection_id": s.Conn().ID(),
			"stream_id": s.ID(), "peer_id": s.Conn().RemotePeer().String(), "protocol": string(s.Protocol()),
			"direction": s.Stat().Direction.String(), "observation_layer": "Swarm_selected_protocol_only",
			"selection_basis": "native_host_stream_return_not_configured_protocol_list"})
		return wrapped, nil
	}
	wrapped := &pubsubScoringStream{Stream: s, observer: h.observer, drain: h.drain,
		nativeYamux: state.Transport == "tcp" && state.Security == noise.ID && state.StreamMultiplexer == yamux.ID}
	if s.ID() == "" || s.Protocol() != h.protocol || !h.drain.retain(wrapped) {
		return nil, fmt.Errorf("PubSub stream lacks selected protocol/admitted owner")
	}
	h.observer.emit("protocol", "go.network.Stream.Protocol", map[string]any{"connection_id": s.Conn().ID(),
		"stream_id": s.ID(), "peer_id": s.Conn().RemotePeer().String(), "protocol": string(s.Protocol()),
		"direction":       s.Stat().Direction.String(),
		"selection_basis": "native_host_stream_return_not_configured_protocol_list"})
	return wrapped, nil
}
func (h *pubsubScoringHost) NewStream(ctx context.Context, p peer.ID, ids ...protocol.ID) (network.Stream, error) {
	if !h.drain.begin() {
		return nil, context.Canceled
	}
	defer h.drain.end()
	s, err := h.Host.NewStream(ctx, p, ids...)
	if err != nil {
		return nil, err
	}
	wrapped, err := h.wrap(s)
	if err != nil {
		_ = s.Reset()
		if h.ctx.Err() == nil {
			h.observer.fail(err)
		}
	}
	return wrapped, err
}
func (h *pubsubScoringHost) SetStreamHandler(id protocol.ID, handler network.StreamHandler) {
	h.Host.SetStreamHandler(id, func(s network.Stream) {
		if !h.drain.begin() {
			_ = s.Reset()
			return
		}
		defer h.drain.end()
		wrapped, err := h.wrap(s)
		if err != nil {
			_ = s.Reset()
			if h.ctx.Err() == nil {
				h.observer.fail(err)
			}
			return
		}
		handler(wrapped)
	})
}

type pubsubScoringDecoder struct {
	buffer   []byte
	failed   bool
	finished bool
}

// These bytes are the delegate's successful Read/Write prefixes, never a
// re-serialization of RawTracer RPC arguments. One receipt owns exactly one RPC.
func (d *pubsubScoringDecoder) feed(data []byte, frame func([]byte, *pubsubpb.RPC), fail func(error)) {
	if d.finished {
		if len(data) != 0 {
			d.failed = true
			fail(fmt.Errorf("native PubSub bytes after framing termination"))
		}
		return
	}
	if d.failed {
		return
	}
	for len(data) > 0 {
		capacity := pubsubScoringFrame + binary.MaxVarintLen64 - len(d.buffer)
		if capacity == 0 {
			d.failed = true
			fail(fmt.Errorf("PubSub frame capture overflow"))
			return
		}
		n := len(data)
		if n > capacity {
			n = capacity
		}
		d.buffer = append(d.buffer, data[:n]...)
		data = data[n:]
		for len(d.buffer) > 0 {
			size, header := binary.Uvarint(d.buffer)
			if header == 0 {
				break
			}
			var canonical [binary.MaxVarintLen64]byte
			if header < 0 || size > pubsubScoringFrame || header != binary.PutUvarint(canonical[:], size) || !bytes.Equal(d.buffer[:header], canonical[:header]) {
				d.failed = true
				fail(fmt.Errorf("malformed/over-limit native PubSub frame"))
				return
			}
			if uint64(len(d.buffer)-header) < size {
				break
			}
			end := header + int(size)
			raw := append([]byte{}, d.buffer[:end]...)
			rpc := &pubsubpb.RPC{}
			if err := rpc.Unmarshal(raw[header:]); err != nil {
				d.failed = true
				fail(fmt.Errorf("native PubSub frame is not an RPC"))
				return
			}
			frame(raw, rpc)
			d.buffer = append(d.buffer[:0], d.buffer[end:]...)
		}
	}
}

type pubsubScoringStream struct {
	network.Stream
	observer        *pubsubScoringObserver
	drain           *pubsubScoringDrain
	mu              sync.Mutex
	ioGate          [2]chan struct{}
	activeIO        [2]int
	ending          [2]string
	finalized       [2]bool
	finalizing      int
	nativeClosing   int
	disposed        bool
	disposalReceipt int
	nativeYamux     bool
	operationOrder  uint64
	reset           *pubsubScoringResetAttempt
	readTerminal    *pubsubScoringReadTerminalAttempt
	residue         [2]int
	pendingTerminal []pubsubScoringTerminalReceipt
	read, write     pubsubScoringDecoder
}

type pubsubScoringOperation struct {
	name          string
	started       uint64
	prepared      bool
	prepareAck    int
	resetReceipt  int
	resetReturned uint64
	resetStarted  uint64
	reset         *pubsubScoringResetAttempt
	readTerminal  *pubsubScoringReadTerminalAttempt
}

type pubsubScoringResetAttempt struct {
	started, returned uint64
	prepared          bool
	prepareAck        int
	completed         bool
	sequence          int
	err               error
}

type pubsubScoringOperationReturn struct {
	order uint64
	err   error
}

type pubsubScoringReadTerminalAttempt struct {
	started, returned uint64
	prepared          bool
	prepareAck        int
	completed         bool
	sequence          int
	err               error
}

type pubsubScoringTerminalReceipt struct {
	sequence int
	fields   map[string]any
	err      error
	kind     string
	reset    *pubsubScoringResetAttempt
	terminal *pubsubScoringReadTerminalAttempt
	returned uint64
}

func (s *pubsubScoringStream) beginOperation(name string, closing bool) pubsubScoringOperation {
	s.observer.mu.Lock()
	prepared, prepareAck := s.observer.prepared, s.observer.prepareAck
	s.observer.mu.Unlock()
	s.mu.Lock()
	defer s.mu.Unlock()
	s.operationOrder++
	op := pubsubScoringOperation{name: name, started: s.operationOrder, prepared: prepared, prepareAck: prepareAck}
	if name == "stream_close" && s.reset != nil && s.reset.completed && s.reset.err == nil {
		op.reset = s.reset
		op.resetReceipt, op.resetReturned, op.resetStarted = s.reset.sequence, s.reset.returned, s.reset.started
	}
	if name == "stream_reset" {
		s.reset = &pubsubScoringResetAttempt{started: op.started, prepared: prepared, prepareAck: prepareAck}
		op.reset = s.reset
	} else if name == "stream_reset_with_error" {
		s.reset = nil
	}
	switch name {
	case "stream_close", "stream_close_read", "stream_reset":
		s.readTerminal = &pubsubScoringReadTerminalAttempt{started: op.started, prepared: prepared, prepareAck: prepareAck}
		op.readTerminal = s.readTerminal
	case "stream_reset_with_error":
		s.readTerminal = nil // This slice does not infer a read-terminal state from an explicit reset code.
	}
	if closing {
		s.nativeClosing++
	}
	return op
}

func (s *pubsubScoringStream) returnedOperation(op pubsubScoringOperation, err error) pubsubScoringOperationReturn {
	s.mu.Lock()
	s.operationOrder++
	result := pubsubScoringOperationReturn{order: s.operationOrder, err: err}
	if op.name == "stream_reset" {
		// Native return is causal evidence even while its receipt is unpublished.
		op.reset.returned, op.reset.err, op.reset.completed = result.order, result.err, true
	}
	if op.readTerminal != nil {
		op.readTerminal.returned, op.readTerminal.err, op.readTerminal.completed = result.order, result.err, true
	}
	s.mu.Unlock()
	if op.name == "stream_reset" && err != nil {
		s.observer.fail(err)
	}
	return result
}

func pubsubScoringYamuxResetPair(err error) bool {
	joined, ok := err.(interface{ Unwrap() []error })
	if !ok {
		return false
	}
	causes := joined.Unwrap()
	return len(causes) == 2 && causes[0] == network.ErrReset && causes[1] == nativeyamux.ErrStreamReset
}

func (s *pubsubScoringStream) returnedIOOperation(op pubsubScoringOperation, err error) (uint64, *pubsubScoringResetAttempt, *pubsubScoringReadTerminalAttempt, map[string]any) {
	outer, typed := err.(*network.StreamError)
	direct, peerZero := false, false
	if typed && outer.ErrorCode == 0 {
		inner, ok := outer.TransportError.(*nativeyamux.StreamError)
		if ok && inner.ErrorCode == 0 {
			direct = !outer.Remote && !inner.Remote
			peerZero = outer.Remote && inner.Remote
		}
	}
	readPair := op.name == "stream_read" && pubsubScoringYamuxResetPair(err)
	returnPrepareAck := 0
	if (peerZero || readPair) && s.nativeYamux {
		s.observer.mu.Lock()
		if s.observer.prepared && s.observer.failure == nil && !s.observer.overflow {
			returnPrepareAck = s.observer.prepareAck
		}
		s.observer.mu.Unlock()
	}
	s.mu.Lock()
	s.operationOrder++
	returned := s.operationOrder
	var reset *pubsubScoringResetAttempt
	attempt := s.reset
	if direct && s.nativeYamux && attempt != nil && attempt.prepared && attempt.prepareAck != 0 && attempt.started < returned &&
		(!attempt.completed || attempt.err == nil) {
		reset = attempt
	}
	var terminal *pubsubScoringReadTerminalAttempt
	var terminalSequence int
	var terminalReturned uint64
	current := s.readTerminal
	if readPair && returnPrepareAck != 0 && current != nil && current.prepared && current.prepareAck == returnPrepareAck &&
		current.completed && current.err == nil && current.returned < returned {
		terminal = current
		terminalSequence, terminalReturned = current.sequence, current.returned
	}
	s.mu.Unlock()
	var fields map[string]any
	if err != nil {
		fields = s.operationFields(op, returned, err)
		if reset != nil {
			// notifyWaiting precedes Reset's return; pin this attempt at I/O return.
			fields["outcome"], fields["reset_started_order"] = "owned_reset_pending", reset.started
		} else if peerZero && returnPrepareAck != 0 {
			// Observe peer cancellation, not its reason or any remote cleanup cause.
			fields["outcome"] = "peer_zero_reset_pending"
			fields["terminal_prepare_ack_sequence"], fields["peer_reset_reason"] = returnPrepareAck, "unknown"
		} else if terminal != nil {
			// The sentinel pair has no remote/code authority. Record owned terminal state, not its physical cause.
			fields["outcome"], fields["typed_cause"] = "owned_read_terminal_pending", "yamux_reset_sentinel_pair"
			fields["terminal_state_cause"] = "unknown"
			fields["terminal_started_order"], fields["terminal_returned_order"] = terminal.started, terminalReturned
			fields["terminal_receipt_sequence"] = terminalSequence
		} else if fields["outcome"] != "eof" {
			s.observer.fail(err)
		}
	}
	return returned, reset, terminal, fields
}

func (s *pubsubScoringStream) operationFields(op pubsubScoringOperation, returned uint64, err error) map[string]any {
	fields := map[string]any{"connection_id": "", "remote_peer_id": "", "stream_id": s.ID(),
		"protocol": string(s.Protocol()), "operation": op.name, "started_order": op.started,
		"returned_order": returned, "prepared": op.prepared, "native_yamux": s.nativeYamux,
		"prepare_ack_sequence": op.prepareAck, "reset_started_order": op.resetStarted,
		"reset_receipt_sequence": op.resetReceipt, "reset_returned_order": op.resetReturned,
		"requested_reset_code": nil, "outcome": "ok", "typed_cause": "none", "error": nil,
		"error_type": nil, "error_code": nil, "remote": nil,
		"transport_error_type": nil, "transport_error_code": nil, "transport_error_remote": nil}
	if c := s.Conn(); c != nil {
		fields["connection_id"], fields["remote_peer_id"] = c.ID(), c.RemotePeer().String()
	}
	if err == nil {
		return fields
	}
	fields["outcome"], fields["typed_cause"] = "error", "opaque"
	fields["error"], fields["error_type"] = pubsubScoringDiagnostic(err), fmt.Sprintf("%T", err)
	// Only the direct outer type can describe an error eligible for repeat Close.
	// errors.As/Is could conceal a joined TCP/transport failure behind ErrReset.
	switch cause := err.(type) {
	case *nativeyamux.StreamError:
		fields["typed_cause"], fields["error_code"], fields["remote"] = "yamux_stream_error", cause.ErrorCode, cause.Remote
	case *network.StreamError:
		fields["typed_cause"], fields["error_code"], fields["remote"] = "libp2p_stream_error", uint32(cause.ErrorCode), cause.Remote
		if cause.TransportError != nil {
			fields["transport_error_type"] = fmt.Sprintf("%T", cause.TransportError)
		}
		if inner, ok := cause.TransportError.(*nativeyamux.StreamError); ok {
			fields["transport_error_code"], fields["transport_error_remote"] = inner.ErrorCode, inner.Remote
		}
	}
	if err == io.EOF && op.name == "stream_read" {
		fields["outcome"], fields["typed_cause"] = "eof", "io_eof"
	}
	return fields
}

func (s *pubsubScoringStream) operationReceipt(kind, source string, fields map[string]any) int {
	s.observer.mu.Lock()
	defer s.observer.mu.Unlock()
	previous := len(s.observer.events)
	s.observer.emitLocked(kind, source, fields)
	if len(s.observer.events) == previous {
		return 0
	}
	return previous + 1
}

func (s *pubsubScoringStream) queueTerminal(receipt pubsubScoringTerminalReceipt) {
	s.mu.Lock()
	bounded := receipt.sequence != 0 && len(s.pendingTerminal) < pubsubScoringEvents
	if bounded {
		s.pendingTerminal = append(s.pendingTerminal, receipt)
	}
	s.mu.Unlock()
	if !bounded {
		s.observer.fail(fmt.Errorf("PubSub causal terminal receipt capture overflow"))
	}
}

func (s *pubsubScoringStream) beginIO(side int) {
	s.mu.Lock()
	if s.ioGate[side] == nil {
		s.ioGate[side] = make(chan struct{}, 1)
		s.ioGate[side] <- struct{}{}
	}
	gate := s.ioGate[side]
	s.activeIO[side]++
	s.mu.Unlock()
	// Preserve prefix order without holding a mutex across native I/O.
	// Native close/reset never waits for this per-direction token.
	<-gate
}

func (s *pubsubScoringStream) endIO(side int) {
	s.mu.Lock()
	s.activeIO[side]--
	gate := s.ioGate[side]
	s.mu.Unlock()
	gate <- struct{}{}
	s.finalizeFraming()
}

func (s *pubsubScoringStream) markEnd(side int, operation string) {
	s.mu.Lock()
	if s.ending[side] == "" {
		s.ending[side] = operation
	}
	s.mu.Unlock()
	s.finalizeFraming()
}

func (s *pubsubScoringStream) finalizeFraming() {
	var pending [2]int
	var operations [2]string
	claims := 0
	s.mu.Lock()
	for side, decoder := range []*pubsubScoringDecoder{&s.read, &s.write} {
		if s.ending[side] != "" && s.activeIO[side] == 0 && !s.finalized[side] {
			pending[side], operations[side] = len(decoder.buffer), s.ending[side]
			s.residue[side] = pending[side]
			decoder.finished, decoder.buffer = true, nil
			s.finalized[side] = true
			claims++
		}
	}
	s.finalizing += claims
	s.mu.Unlock()
	for side, count := range pending {
		if count == 0 {
			continue
		}
		direction := []string{"read", "write"}[side]
		fields := map[string]any{"connection_id": s.Conn().ID(), "stream_id": s.ID(),
			"remote_peer_id": s.Conn().RemotePeer().String(), "protocol": string(s.Protocol()),
			"direction": direction, "operation": operations[side], "pending_frame_bytes": count,
			"typed_cause": "incomplete_rpc_frame"}
		s.observer.mu.Lock()
		if s.observer.failure == nil {
			s.observer.failure = fmt.Errorf("incomplete native PubSub %s RPC at %s: %d captured bytes", direction, operations[side], count)
			s.observer.notifyScoreLocked()
		}
		fields["prepared"] = s.observer.prepared
		s.observer.emitLocked("incomplete_rpc_frame", "go.pubsub.native_stream.finalize", fields)
		s.observer.mu.Unlock()
	}
	s.mu.Lock()
	s.finalizing -= claims
	var receipts []pubsubScoringTerminalReceipt
	clean := false
	if s.directionsJoinedLocked() {
		receipts, s.pendingTerminal = s.pendingTerminal, nil
		s.finalizing += len(receipts)
		clean = s.residue[0] == 0 && s.residue[1] == 0 && !s.read.failed && !s.write.failed
	}
	residue, disposed, disposalReceipt := s.residue, s.disposed, s.disposalReceipt
	s.mu.Unlock()
	for _, receipt := range receipts {
		fields := make(map[string]any, len(receipt.fields)+8)
		for key, value := range receipt.fields {
			fields[key] = value
		}
		accepted := clean && disposed && disposalReceipt != 0
		if receipt.fields["outcome"] == "peer_zero_reset_pending" || receipt.terminal != nil {
			s.observer.mu.Lock()
			accepted = accepted && s.observer.prepared && s.observer.failure == nil && !s.observer.overflow
			if receipt.terminal == nil {
				accepted = accepted && receipt.fields["terminal_prepare_ack_sequence"] == s.observer.prepareAck
			} else {
				terminal := receipt.terminal
				accepted = accepted && terminal.completed && terminal.err == nil && terminal.sequence != 0 &&
					terminal.prepared && terminal.prepareAck == s.observer.prepareAck && terminal.returned < receipt.returned
				fields["observed_terminal_receipt_sequence"] = terminal.sequence
			}
			s.observer.mu.Unlock()
		} else {
			reset := receipt.reset
			accepted = accepted && reset.completed && reset.err == nil && reset.sequence != 0
			if receipt.kind == "native_stream_io_finalized" {
				accepted = accepted && reset.prepared && reset.prepareAck != 0 && reset.started < receipt.returned
			}
			fields["causal_reset_receipt_sequence"], fields["causal_reset_returned_order"] = reset.sequence, reset.returned
		}
		fields["operation_receipt_sequence"], fields["accepted"] = receipt.sequence, accepted
		fields["native_owner_disposed"] = disposed
		fields["owner_disposal_receipt_sequence"] = disposalReceipt
		fields["io_joined"], fields["read_finalized"], fields["write_finalized"] = true, true, true
		fields["pending_read_frame_bytes"], fields["pending_write_frame_bytes"] = residue[0], residue[1]
		fields["framing_clean"] = clean
		if !accepted {
			s.observer.fail(receipt.err)
		}
		s.operationReceipt(receipt.kind, "go.pubsub.native_stream.native_operation", fields)
	}
	s.mu.Lock()
	s.finalizing -= len(receipts)
	release := s.joinedLocked() && len(s.pendingTerminal) == 0
	s.mu.Unlock()
	if release {
		s.drain.release(s)
	}
}

func (s *pubsubScoringStream) directionsJoinedLocked() bool {
	return s.nativeClosing == 0 && s.finalizing == 0 && s.activeIO[0] == 0 && s.activeIO[1] == 0 && s.finalized[0] && s.finalized[1]
}

func (s *pubsubScoringStream) joinedLocked() bool {
	return s.disposed && s.directionsJoinedLocked()
}

func (s *pubsubScoringStream) framingJoined() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.joinedLocked() && len(s.pendingTerminal) == 0
}

func (s *pubsubScoringStream) endClose(op pubsubScoringOperation, side int, result pubsubScoringOperationReturn, code any) {
	returned, err := result.order, result.err
	fields := s.operationFields(op, returned, err)
	fields["requested_reset_code"] = code
	cause, typed := err.(*nativeyamux.StreamError)
	candidate := typed && cause.ErrorCode == 0 && op.name == "stream_close" && op.prepared && s.nativeYamux &&
		op.reset != nil && op.resetStarted > 0 && op.resetReturned > 0 && op.resetReturned < op.started
	if candidate {
		fields["outcome"] = "repeat_close_pending"
	} else if err != nil {
		s.observer.fail(err)
	}
	sequence := s.operationReceipt("native_stream_operation", "go.pubsub.native_stream.native_operation", fields)
	if candidate {
		s.queueTerminal(pubsubScoringTerminalReceipt{sequence: sequence, fields: fields, err: err,
			kind: "native_stream_close_finalized", reset: op.reset, returned: returned})
	}
	s.mu.Lock()
	s.nativeClosing--
	if side < 0 && err == nil {
		s.disposed = true
		if s.disposalReceipt == 0 {
			s.disposalReceipt = sequence
		}
	}
	if op.name == "stream_reset" {
		op.reset.sequence = sequence
	}
	if op.readTerminal != nil {
		op.readTerminal.sequence = sequence
	}
	for index := range s.ending {
		if (side < 0 || side == index) && s.ending[index] == "" {
			s.ending[index] = op.name
		}
	}
	s.mu.Unlock()
	s.finalizeFraming()
}

func (s *pubsubScoringStream) frame(direction string, framed []byte, rpc *pubsubpb.RPC) {
	digest := sha256.Sum256(framed)
	fields := map[string]any{"connection_id": s.Conn().ID(), "stream_id": s.ID(),
		"peer_id": s.Conn().RemotePeer().String(), "protocol": string(s.Protocol()), "direction": direction,
		"receipt": map[string]any{"framed_hex": hex.EncodeToString(framed), direction: map[string]any{"framed_bytes": len(framed),
			"framed_sha256": hex.EncodeToString(digest[:]), "frames": 1, "complete_frames": true, "invalid_or_over_limit": false}}}
	// Decoded summaries are supplemental; acceptance must parse framed_hex.
	prunes, grafts := []map[string]any{}, []map[string]any{}
	if ctl := rpc.GetControl(); ctl != nil {
		for _, p := range ctl.GetPrune() {
			prunes = append(prunes, map[string]any{"topic": p.GetTopicID(), "backoff_present": p.Backoff != nil,
				"backoff": p.GetBackoff(), "px_entries": len(p.GetPeers())})
		}
		for _, g := range ctl.GetGraft() {
			grafts = append(grafts, map[string]any{"topic": g.GetTopicID()})
		}
	}
	fields["prunes"], fields["grafts"] = prunes, grafts
	s.observer.mu.Lock()
	defer s.observer.mu.Unlock()
	if s.observer.wireBytes+len(framed) > pubsubScoringWireBytes {
		s.observer.overflow = true
		if s.observer.failure == nil {
			s.observer.failure = fmt.Errorf("PubSub wire capture byte limit")
		}
		return
	}
	s.observer.wireBytes += len(framed)
	s.observer.emitLocked("rpc", "go.pubsub.native_stream."+direction, fields)
}
func (s *pubsubScoringStream) Read(p []byte) (int, error) {
	if !s.drain.begin() {
		return 0, context.Canceled
	}
	defer s.drain.end()
	s.beginIO(0)
	defer s.endIO(0)
	op := s.beginOperation("stream_read", false)
	n, err := s.Stream.Read(p)
	returned, reset, terminal, fields := s.returnedIOOperation(op, err)
	s.read.feed(p[:n], func(frame []byte, rpc *pubsubpb.RPC) { s.frame("read", frame, rpc) }, s.observer.fail)
	if err != nil {
		s.terminal(reset, terminal, fields, returned, "read", n, err, len(s.read.buffer))
		s.markEnd(0, "stream_read")
	}
	return n, err
}
func (s *pubsubScoringStream) Write(p []byte) (int, error) {
	if !s.drain.begin() {
		return 0, context.Canceled
	}
	defer s.drain.end()
	s.beginIO(1)
	defer s.endIO(1)
	op := s.beginOperation("stream_write", false)
	n, err := s.Stream.Write(p)
	returned, reset, terminal, fields := s.returnedIOOperation(op, err)
	s.write.feed(p[:n], func(frame []byte, rpc *pubsubpb.RPC) { s.frame("write", frame, rpc) }, s.observer.fail)
	if err != nil {
		s.terminal(reset, terminal, fields, returned, "write", n, err, len(s.write.buffer))
		s.markEnd(1, "stream_write")
	}
	return n, err
}
func (s *pubsubScoringStream) terminal(reset *pubsubScoringResetAttempt, terminal *pubsubScoringReadTerminalAttempt, fields map[string]any, returned uint64, direction string, n int, err error, pending int) {
	fields["direction"], fields["successful_prefix_bytes"], fields["pending_frame_bytes"] = direction, n, pending
	sequence := s.operationReceipt("stream_io_terminal", "go.network.Stream."+direction, fields)
	if reset != nil || terminal != nil || fields["outcome"] == "peer_zero_reset_pending" {
		s.queueTerminal(pubsubScoringTerminalReceipt{sequence: sequence, fields: fields, err: err,
			kind: "native_stream_io_finalized", reset: reset, terminal: terminal, returned: returned})
	}
}
func (s *pubsubScoringStream) Close() error {
	op := s.beginOperation("stream_close", true)
	err := s.Stream.Close()
	s.endClose(op, -1, s.returnedOperation(op, err), nil)
	return err
}
func (s *pubsubScoringStream) CloseRead() error {
	op := s.beginOperation("stream_close_read", true)
	err := s.Stream.CloseRead()
	s.endClose(op, 0, s.returnedOperation(op, err), nil)
	return err
}
func (s *pubsubScoringStream) CloseWrite() error {
	op := s.beginOperation("stream_close_write", true)
	err := s.Stream.CloseWrite()
	s.endClose(op, 1, s.returnedOperation(op, err), nil)
	return err
}
func (s *pubsubScoringStream) Reset() error {
	op := s.beginOperation("stream_reset", true)
	err := s.Stream.Reset()
	s.endClose(op, -1, s.returnedOperation(op, err), nil)
	return err
}
func (s *pubsubScoringStream) ResetWithError(code network.StreamErrorCode) error {
	op := s.beginOperation("stream_reset_with_error", true)
	err := s.Stream.ResetWithError(code)
	s.endClose(op, -1, s.returnedOperation(op, err), uint32(code))
	return err
}

func pubsubScoringDiagnostic(err error) string {
	message := []rune(err.Error())
	if len(message) > 512 {
		message = message[:512]
	}
	return string(message)
}

func pubsubScoringAtomic(path string, value any) error {
	data, err := json.Marshal(value)
	if err != nil {
		return err
	}
	f, err := os.CreateTemp(filepath.Dir(path), ".pubsub-result-")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	if err = f.Chmod(0o600); err == nil {
		_, err = f.Write(append(data, '\n'))
	}
	err = errors.Join(err, f.Close())
	if err != nil {
		return err
	}
	return os.Rename(f.Name(), path)
}

func (o *pubsubScoringObserver) result(finalized, joined bool, failure error, workers int) map[string]any {
	o.mu.Lock()
	defer o.mu.Unlock()
	failure = errors.Join(failure, o.failure)
	var message any
	if failure != nil {
		message = failure.Error()
	}
	return map[string]any{"schema_version": 1, "implementation": "go", "actor": o.actor, "case_token": o.token,
		"local_peer_id": o.local.String(), "pid": os.Getpid(), "finalized": finalized, "joined": joined, "overflow": o.overflow,
		"error": message, "events": append([]map[string]any{}, o.events...), "active_callbacks": o.callbacks,
		"active_fixture_workers": workers, "joined_scope": "fixture_workers_native_host_close_stream_handlers_IO_and_admitted_observer_callbacks",
		"limitations": []string{"pinned_go_pubsub_has_no_public_router_goroutine_join", "score_inspection_is_asynchronous_not_mutation_time",
			"outbound_host_stream_return_can_use_lazy_negotiation_use_paired_receiver_wire_receipt"}}
}

func runPubsubScoringLive(args map[string]string) (failure error) {
	o := newPubsubScoringObserver(args["actor"], args["case-token"])
	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()
	subscriberCtx, stopSubscriber := context.WithCancel(ctx)
	defer stopSubscriber()
	var h host.Host
	var sub *pubsub.Subscription
	var topic *pubsub.Topic
	var observed *pubsubScoringHost
	var notifier *network.NotifyBundle
	workerDone := make(chan struct{})
	workerStarted := false
	defer func() {
		stopSubscriber()
		if sub != nil {
			sub.Cancel()
		}
		// Topic.Close requires the donor process loop to still be running.
		if topic != nil && ctx.Err() == nil {
			failure = errors.Join(failure, topic.Close())
		}
		cancel()
		joinCtx, stopJoin := context.WithTimeout(context.Background(), 5*time.Second)
		defer stopJoin()
		joined := true
		hostClosed := h == nil
		if observed != nil {
			if err := observed.drain.stop(joinCtx); err != nil {
				joined = false
				failure = errors.Join(failure, err)
			}
		}
		if h != nil {
			if err := h.Close(); err != nil {
				joined = false
				failure = errors.Join(failure, err)
			} else {
				hostClosed = true
			}
			if notifier != nil {
				h.Network().StopNotify(notifier)
			}
		}
		if o.quic != nil {
			if err := o.quic.join(joinCtx); err != nil {
				joined = false
				failure = errors.Join(failure, err)
			}
		}
		if workerStarted {
			select {
			case <-workerDone:
			case <-joinCtx.Done():
				joined = false
				failure = errors.Join(failure, fmt.Errorf("PubSub subscriber did not join"))
			}
		}
		o.mu.Lock()
		o.closing = true
		o.notifyScoreLocked()
		o.mu.Unlock()
		for {
			o.mu.Lock()
			active := o.callbacks
			o.mu.Unlock()
			if active == 0 {
				break
			}
			select {
			case <-o.callbackChanged:
			case <-joinCtx.Done():
				joined = false
				failure = errors.Join(failure, fmt.Errorf("PubSub callbacks did not join"))
			}
			if joinCtx.Err() != nil {
				break
			}
		}
		activeWorkers := 0
		if workerStarted {
			select {
			case <-workerDone:
			default:
				activeWorkers = 1
			}
		}
		activeStreams := 0
		if observed != nil {
			observed.drain.mu.Lock()
			activeStreams = observed.drain.active
			observed.drain.mu.Unlock()
		}
		o.emit("shutdown", "go.fixture.owned_context_cancel_and_drain", map[string]any{"context_cancelled": ctx.Err() != nil,
			"joined": joined, "host_close_returned": hostClosed, "active_stream_handlers_and_io": activeStreams,
			"active_fixture_workers": activeWorkers})
		result := o.result(true, joined, failure, activeWorkers)
		result["host_close_returned"], result["active_stream_handlers_and_io"] = hostClosed, activeStreams
		if err := pubsubScoringAtomic(args["result-file"], result); err != nil {
			failure = errors.Join(failure, err)
		}
		o.mu.Lock()
		failure = errors.Join(failure, o.failure)
		o.mu.Unlock()
	}()
	if err := os.MkdirAll(args["store-dir"], 0o700); err != nil {
		return err
	}
	gater := &pathDialObserver{}
	opts := []libp2p.Option{libp2p.NoTransports, libp2p.DisableRelay(), libp2p.ConnectionGater(gater)}
	if args["transport"] == "quic" {
		o.quic = newPubsubQUICObserver(o, gater)
		opts = append(opts, libp2p.Transport(o.quic.constructor()), libp2p.ListenAddrStrings("/ip4/127.0.0.1/udp/0/quic-v1"))
	} else {
		tcpOption := libp2p.Transport(tcp.NewTCPTransport)
		if args["transport"] == "tcp-pnet-noise" {
			psk, err := loadPnetKey(args["pnet-key-file"])
			if err != nil {
				return fmt.Errorf("cannot load private PubSub key")
			}
			fingerprint := sha256.New()
			_, _ = fingerprint.Write([]byte("forge.net.pnet.operational-fingerprint.v1\x00"))
			_, _ = fingerprint.Write(psk)
			if hex.EncodeToString(fingerprint.Sum(nil)) != args["pnet-fingerprint"] {
				return fmt.Errorf("private PubSub key fingerprint mismatch")
			}
			opts = append(opts, libp2p.PrivateNetwork(psk))
			tcpOption = libp2p.Transport(pubsubScoringPrivateTCP)
		}
		opts = append(opts, tcpOption, libp2p.Security(noise.ID, noise.New),
			libp2p.Muxer(yamux.ID, yamux.DefaultTransport), libp2p.ListenAddrStrings("/ip4/127.0.0.1/tcp/0"))
	}
	var err error
	h, err = libp2p.New(opts...)
	if err != nil {
		return err
	}
	o.local = h.ID()
	id := pubsub.GossipSubID_v11
	if args["version"] == "1.0" {
		id = pubsub.GossipSubID_v10
	}
	observed = &pubsubScoringHost{Host: h, ctx: ctx, observer: o, gater: gater, drain: newPubsubScoringDrain(), protocol: id,
		fingerprint: args["pnet-fingerprint"]}
	notifier = &network.NotifyBundle{ConnectedF: func(_ network.Network, c network.Conn) {
		o.callback(func() {
			if err := o.connection(c, gater, args["pnet-fingerprint"]); err != nil {
				o.fail(err)
			}
		})
	}, DisconnectedF: func(_ network.Network, c network.Conn) {
		o.callback(func() {
			o.emit("connection_closed", "go.network.NotifyBundle.Disconnected", map[string]any{"connection_id": c.ID(), "remote_peer_id": c.RemotePeer().String()})
		})
	}}
	h.Network().Notify(notifier)
	params, score, thresholds := pubsubScoringParameters(o.topic)
	ps, err := pubsub.NewGossipSub(ctx, observed, pubsub.WithGossipSubProtocols([]protocol.ID{id}, pubsub.GossipSubDefaultFeatures),
		pubsub.WithGossipSubParams(params), pubsub.WithFloodPublish(false), pubsub.WithPeerExchange(false),
		pubsub.WithMessageSignaturePolicy(pubsub.StrictSign), pubsub.WithMaxMessageSize(pubsubScoringFrame),
		pubsub.WithMessageIdFn(pubsubScoringMessageID),
		pubsub.WithPeerScore(score, thresholds), pubsub.WithRawTracer(o),
		pubsub.WithPeerScoreInspect(pubsub.ExtendedPeerScoreInspectFn(o.inspect), 250*time.Millisecond))
	if err != nil {
		return err
	}
	validator := pubsub.ValidatorEx(func(_ context.Context, p peer.ID, m *pubsub.Message) pubsub.ValidationResult {
		result := pubsubScoringValidator(o.actor, o.token, m.Data)
		o.validationDecision(p, m, result)
		return result
	})
	if err = ps.RegisterTopicValidator(o.topic, validator, pubsub.WithValidatorInline(true)); err != nil {
		return err
	}
	topic, err = ps.Join(o.topic)
	if err != nil {
		return err
	}
	sub, err = topic.Subscribe(pubsub.WithBufferSize(64))
	if err != nil {
		return err
	}
	workerStarted = true
	go func() {
		defer close(workerDone)
		for {
			m, err := sub.Next(subscriberCtx)
			if err != nil {
				if subscriberCtx.Err() == nil {
					o.fail(fmt.Errorf("PubSub subscriber failed: %w", err))
				}
				return
			}
			o.callback(func() { o.emit("subscription_delivery", "go.pubsub.Subscription.Next", pubsubScoringMessage(m)) })
		}
	}()
	addresses := []string{}
	for _, addr := range h.Addrs() {
		addresses = append(addresses, addr.String())
	}
	sort.Strings(addresses)
	if len(addresses) == 0 {
		return fmt.Errorf("PubSub host has no actual listener")
	}
	if err = pubsubScoringAtomic(args["ready-file"], map[string]any{"schema_version": 1, "implementation": "go", "actor": o.actor,
		"case_token": o.token, "local_peer_id": h.ID().String(), "peer_id": h.ID().String(), "listen_addrs": addresses,
		"topic": o.topic, "ready": true, "subscription_created": true}); err != nil {
		return err
	}
	control := pubsubScoringControl{}
	sampleLabels := map[string]bool{}
	poll := time.NewTicker(10 * time.Millisecond)
	defer poll.Stop()
	writeResult := time.NewTicker(100 * time.Millisecond)
	defer writeResult.Stop()
	for {
		select {
		case <-ctx.Done():
			return fmt.Errorf("PubSub actor deadline expired: %w", ctx.Err())
		case <-writeResult.C:
			if err = pubsubScoringAtomic(args["result-file"], o.result(false, false, nil, 1)); err != nil {
				return err
			}
		case <-poll.C:
			o.mu.Lock()
			err = o.failure
			o.mu.Unlock()
			if err != nil {
				return err
			}
			commands, err := control.read(args["control-file"])
			if err != nil {
				return err
			}
			o.mu.Lock()
			prepared := o.prepared
			o.mu.Unlock()
			if prepared && control.offset != len(control.seen) {
				return fmt.Errorf("PubSub command admission closed after prepare_shutdown")
			}
			if _, stopErr := os.Stat(args["stop-file"]); stopErr == nil {
				if control.offset != len(control.seen) || len(commands) != 0 {
					return fmt.Errorf("PubSub stop with incomplete/unexecuted command")
				}
				o.mu.Lock()
				prepared = o.prepared
				o.mu.Unlock()
				if !prepared {
					return fmt.Errorf("PubSub stop before prepare_shutdown acknowledgement")
				}
				return nil
			} else if !errors.Is(stopErr, os.ErrNotExist) {
				return stopErr
			}
			for index, command := range commands {
				if err = o.admitCommand(); err != nil {
					return err
				}
				commandCtx, stopCommand := context.WithTimeout(ctx, 10*time.Second)
				switch command.Kind {
				case "connect":
					p, _ := peer.Decode(command.Peer)
					if p == h.ID() {
						stopCommand()
						return fmt.Errorf("PubSub actor cannot connect to itself")
					}
					addr, _ := ma.NewMultiaddr(command.Address)
					if suffix, suffixErr := addr.ValueForProtocol(ma.P_P2P); suffixErr == nil && suffix != p.String() {
						stopCommand()
						return fmt.Errorf("PubSub connect address/peer mismatch")
					}
					err = h.Connect(commandCtx, peer.AddrInfo{ID: p, Addrs: []ma.Multiaddr{addr}})
				case "publish":
					err = topic.Publish(commandCtx, []byte(command.Payload))
					digest := sha256.Sum256([]byte(command.Payload))
					var outcome any
					if err != nil {
						outcome = err.Error()
					}
					o.emit("publish", "go.pubsub.Topic.Publish", map[string]any{"command_sequence": command.Sequence,
						"topic": o.topic, "payload_sha256": hex.EncodeToString(digest[:]), "payload_bytes": len(command.Payload), "error": outcome})
				case "sample":
					if sampleLabels[command.Label] {
						stopCommand()
						return fmt.Errorf("duplicate PubSub sample label")
					}
					sampleLabels[command.Label] = true
					err = o.sample(commandCtx, command.Label)
				case "prepare_shutdown":
					if control.offset != len(control.seen) {
						stopCommand()
						return fmt.Errorf("prepare_shutdown with incomplete control command")
					}
					err = o.prepareShutdown(command, len(commands)-index-1)
				}
				stopCommand()
				if err != nil {
					return err
				}
				o.emit("command_done", "go.fixture.append_only_control", map[string]any{"command_sequence": command.Sequence, "command_kind": command.Kind, "status": "ok"})
			}
		}
	}
}

var _ pubsub.RawTracer = (*pubsubScoringObserver)(nil)
var _ host.Host = (*pubsubScoringHost)(nil)
var _ network.Stream = (*pubsubScoringStream)(nil)
