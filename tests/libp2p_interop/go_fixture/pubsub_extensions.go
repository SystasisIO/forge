package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"iter"
	"log/slog"
	"sort"
	"strings"
	"sync"
	"time"

	pubsub "github.com/libp2p/go-libp2p-pubsub"
	"github.com/libp2p/go-libp2p-pubsub/partialmessages"
	pubsubpb "github.com/libp2p/go-libp2p-pubsub/pb"
	"github.com/libp2p/go-libp2p/core/peer"
)

const (
	pubsubExtensionPayload = 4096
	pubsubExtensionQueue   = 32
	pubsubExtensionGroups  = 1
	pubsubExtensionInputs  = 64
	pubsubExtensionHold    = 10 * time.Second
)

func validatePubsubExtension(mode, version string) error {
	switch mode {
	case "":
		return nil
	case "idontwant":
		if version == "1.2" {
			return nil
		}
	case "partial", "advertisement":
		if version == "1.3" {
			return nil
		}
	}
	return fmt.Errorf("unsupported PubSub extension mode/version")
}

func pubsubExtensionCommandFields(c pubsubScoringCommand, mode string) (map[string]bool, error) {
	allowed := map[string]bool{"sequence": true, "kind": true}
	switch c.Kind {
	case "publish_extension", "validation_hold":
		allowed["payload"] = true
		if (mode != "idontwant" && mode != "partial" && mode != "advertisement") ||
			c.Kind == "validation_hold" && mode != "idontwant" || len(c.Payload) == 0 || len(c.Payload) > pubsubExtensionPayload || strings.ContainsRune(c.Payload, '\x00') {
			return nil, fmt.Errorf("extension command requires bounded payload and matching mode")
		}
	case "validation_release":
		if mode != "idontwant" {
			return nil, fmt.Errorf("validation release requires IDONTWANT mode")
		}
	case "partial_offer":
		allowed["have"] = true
		if mode != "partial" || c.Have > pubsubPartialMask {
			return nil, fmt.Errorf("invalid partial offer")
		}
	default:
		return nil, fmt.Errorf("unknown pubsub command")
	}
	return allowed, nil
}

func requirePubsubExtensionFields(fields map[string]json.RawMessage, allowed map[string]bool) error {
	for name := range allowed {
		if fields[name] == nil {
			return fmt.Errorf("missing extension command field %s", name)
		}
	}
	return nil
}

type pubsubExtensionWork struct {
	kind        string
	observation int
	peer        peer.ID
	group       []byte
	body        []byte
	metadata    []byte
	peers       []peer.ID
	command     pubsubScoringCommand
	reply       chan error
}

type pubsubExtensionGroup struct {
	local         bool
	metadata      pubsubPartialMetadata
	parts         [pubsubPartialParts][]byte
	received      [pubsubPartialParts][]byte
	receivedAt    [pubsubPartialParts]int
	peers         map[peer.ID]pubsubPartialMetadata
	reconstructed bool
}

type pubsubExtensionValidation struct {
	payload     string
	command     int
	observation int
	released    bool
	done        chan struct{}
}

// Owns only bounded application state and one explicitly joined worker. Native
// callbacks copy observations into its queue, never reenter PubSub's event loop.
type pubsubExtensions struct {
	mode          string
	o             *pubsubScoringObserver
	ctx           context.Context
	cancel        context.CancelFunc
	queue         chan pubsubExtensionWork
	done          chan struct{}
	changed       chan struct{}
	mu            sync.Mutex
	closed        bool
	started       bool
	pending       int
	validators    int
	actions       int
	actionsClosed bool
	failure       error
	hold          *pubsubExtensionValidation
	ps            *pubsub.PubSub
	groups        map[string]*pubsubExtensionGroup // Worker-owned, never donor peerState maps.
	inputs        int
}

func newPubsubExtensions(parent context.Context, mode string, o *pubsubScoringObserver) *pubsubExtensions {
	if mode == "" {
		return nil
	}
	ctx, cancel := context.WithCancel(parent)
	return &pubsubExtensions{mode: mode, o: o, ctx: ctx, cancel: cancel,
		queue: make(chan pubsubExtensionWork, pubsubExtensionQueue), done: make(chan struct{}), changed: make(chan struct{}, 1),
		groups: make(map[string]*pubsubExtensionGroup)}
}

func (e *pubsubExtensions) emit(kind, source string, fields map[string]any) int {
	e.o.mu.Lock()
	defer e.o.mu.Unlock()
	fields["extension_mode"], fields["topic"] = e.mode, e.o.topic
	before := len(e.o.events)
	e.o.emitLocked(kind, source, fields)
	if len(e.o.events) != before+1 {
		return 0
	}
	return before + 1
}

func (e *pubsubExtensions) failLocked(err error) {
	if err != nil && e.failure == nil {
		e.failure = err
		e.o.fail(err)
	}
}

func (e *pubsubExtensions) notifyLocked() {
	select {
	case e.changed <- struct{}{}:
	default:
	}
}

func (e *pubsubExtensions) enqueueLocked(work pubsubExtensionWork) error {
	if e.closed || e.ctx.Err() != nil {
		return fmt.Errorf("extension application admission closed")
	}
	select {
	case e.queue <- work:
		e.pending++
		return nil
	default:
		err := fmt.Errorf("extension application queue limit")
		e.failLocked(err)
		return err
	}
}

func (e *pubsubExtensions) options() []pubsub.Option {
	if e == nil || e.mode == "idontwant" {
		return nil
	}
	return []pubsub.Option{pubsub.WithPartialMessagesExtension(&partialmessages.PartialMessagesExtension[struct{}]{
		Logger:        slog.New(slog.NewTextHandler(io.Discard, nil)),
		OnIncomingRPC: e.incoming, OnEmitGossip: e.gossip,
		PeerInitiatedGroupLimitPerTopic:        pubsubExtensionGroups,
		PeerInitiatedGroupLimitPerTopicPerPeer: pubsubExtensionGroups,
		GroupTTLByHeatbeat:                     6,
	})}
}

func (e *pubsubExtensions) topicOptions() []pubsub.TopicOpt {
	if e != nil && e.mode == "partial" {
		return []pubsub.TopicOpt{pubsub.RequestPartialMessages()}
	}
	return nil // Advertisement mode enables the extension, not partial topic delivery.
}

func (e *pubsubExtensions) incoming(from peer.ID, states map[peer.ID]struct{}, rpc *pubsubpb.PartialMessagesExtension) error {
	e.mu.Lock()
	defer e.mu.Unlock()
	if e.closed {
		return nil // No application callback/work is admitted beyond our joined boundary.
	}
	if rpc == nil || from == "" || rpc.GetTopicID() != e.o.topic || len(rpc.GroupID) != 20 || len(rpc.PartialMessage) > pubsubPartialMaxPart+4 || len(rpc.PartsMetadata) > 7 {
		err := fmt.Errorf("partial callback shape exceeds fixture bounds")
		e.failLocked(err)
		return err
	}
	sequence := e.emit("partial_incoming", "go.pubsub.partialmessages.OnIncomingRPC", map[string]any{
		"peer_id": from.String(), "group_id_hex": hex.EncodeToString(rpc.GroupID),
		"body_present": rpc.PartialMessage != nil, "body_hex": hex.EncodeToString(rpc.PartialMessage),
		"metadata_present": rpc.PartsMetadata != nil, "metadata_hex": hex.EncodeToString(rpc.PartsMetadata),
		"authority": "native_application_callback_not_validation_or_delivery"})
	if sequence == 0 {
		return fmt.Errorf("partial callback observation not retained")
	}
	if e.mode != "partial" {
		if rpc.PartialMessage != nil {
			err := fmt.Errorf("partial body received on full-only fixture topic")
			e.failLocked(err)
			return err
		}
		return nil // Native metadata remains observable, without opting into parts/replies.
	}
	if _, exists := states[from]; !exists && len(states) >= pubsubScoringPeers {
		err := fmt.Errorf("native partial peer state exceeds fixture bound")
		e.failLocked(err)
		return err
	}
	states[from] = struct{}{} // Only this actual callback sender, within the donor-owned callback.
	return e.enqueueLocked(pubsubExtensionWork{kind: "incoming", observation: sequence, peer: from,
		group: bytes.Clone(rpc.GroupID), body: bytes.Clone(rpc.PartialMessage), metadata: bytes.Clone(rpc.PartsMetadata)})
}

func (e *pubsubExtensions) gossip(topic string, group []byte, peers []peer.ID, _ map[peer.ID]struct{}) {
	e.mu.Lock()
	defer e.mu.Unlock()
	if e.closed {
		return
	}
	if e.mode != "partial" || topic != e.o.topic || len(group) != 20 || len(peers) > pubsubScoringPeers {
		e.failLocked(fmt.Errorf("invalid/bounded native partial gossip callback"))
		return
	}
	names := make([]string, len(peers))
	seen := make(map[peer.ID]bool)
	for i, p := range peers {
		if p == "" || seen[p] {
			e.failLocked(fmt.Errorf("invalid/duplicate native partial gossip peer"))
			return
		}
		seen[p], names[i] = true, p.String()
	}
	sequence := e.emit("partial_gossip", "go.pubsub.partialmessages.OnEmitGossip", map[string]any{
		"group_id_hex": hex.EncodeToString(group), "peer_ids": names, "authority": "native_gossip_recipient_callback_not_wire_send"})
	if sequence == 0 {
		e.failLocked(fmt.Errorf("partial gossip observation not retained"))
		return
	}
	_ = e.enqueueLocked(pubsubExtensionWork{kind: "gossip", observation: sequence, group: bytes.Clone(group), peers: append([]peer.ID(nil), peers...)})
}

func (e *pubsubExtensions) start(ps *pubsub.PubSub) error {
	if e == nil {
		return nil
	}
	e.mu.Lock()
	if ps == nil || e.started || e.closed {
		e.mu.Unlock()
		return fmt.Errorf("extension worker requires one live native PubSub owner")
	}
	e.ps, e.started = ps, true
	e.mu.Unlock()
	go func() {
		defer close(e.done)
		for work := range e.queue {
			var err error
			if e.ctx.Err() != nil {
				err = e.ctx.Err()
			} else {
				err = e.process(work)
			}
			e.mu.Lock()
			e.pending--
			e.failLocked(err)
			e.notifyLocked()
			e.mu.Unlock()
			if work.reply != nil {
				work.reply <- err
			}
		}
	}()
	return nil
}

func (e *pubsubExtensions) workerCount() int {
	if e == nil {
		return 0
	}
	e.mu.Lock()
	defer e.mu.Unlock()
	if !e.started {
		return 0
	}
	select {
	case <-e.done:
		return 0
	default:
		return 1
	}
}

func (e *pubsubExtensions) annotate(result map[string]any) {
	if e == nil {
		return
	}
	e.mu.Lock()
	defer e.mu.Unlock()
	result["extension"], result["requests_partial"] = e.mode, e.mode == "partial"
	result["version"] = "1.3"
	if e.mode == "idontwant" {
		result["version"] = "1.2"
	}
	result["extension_admission_closed"], result["active_extension_validators"], result["pending_extension_work"] = e.closed, e.validators, e.pending
	result["active_extension_publish_callbacks"] = e.actions
}

func (e *pubsubExtensions) stop(ctx context.Context) error {
	if e == nil {
		return nil
	}
	e.mu.Lock()
	first := !e.closed
	if first {
		e.closed = true
		close(e.queue)
		if e.hold != nil && !e.hold.released {
			e.failLocked(fmt.Errorf("extension shutdown with unreleased validation hold"))
			e.cancel()
		}
	}
	started := e.started
	e.mu.Unlock()
	if started {
		select {
		case <-e.done:
		case <-ctx.Done():
			return fmt.Errorf("extension worker did not join: %w", ctx.Err())
		}
	}
	e.mu.Lock()
	e.actionsClosed = true
	e.mu.Unlock()
	for {
		e.mu.Lock()
		active, pending, failure := e.validators, e.pending, e.failure
		if active == 0 && e.actions == 0 {
			if first {
				e.emit("extension_stopped", "go.fixture.extensions.owned_worker_join", map[string]any{
					"admission_closed": true, "worker_started": started, "worker_joined": started,
					"pending_work": pending, "active_validators": 0, "active_publish_callbacks": 0,
					"scope": "fixture_application_worker_and_admitted_callbacks_not_router_goroutines"})
			}
			e.mu.Unlock()
			e.cancel()
			if pending != 0 {
				return errors.Join(failure, fmt.Errorf("extension work not joined"))
			}
			return failure
		}
		e.mu.Unlock()
		select {
		case <-e.changed:
		case <-ctx.Done():
			return fmt.Errorf("extension application callbacks did not join: %w", ctx.Err())
		}
	}
}

func (e *pubsubExtensions) validate(ctx context.Context, p peer.ID, m *pubsub.Message) pubsub.ValidationResult {
	e.mu.Lock()
	if e.closed {
		e.mu.Unlock()
		return pubsub.ValidationIgnore
	}
	e.validators++
	hold := e.hold
	matched := hold != nil && hold.payload == string(m.Data) && !hold.released
	if matched {
		if hold.observation != 0 {
			e.failLocked(fmt.Errorf("duplicate admission of held payload"))
			e.validators--
			e.notifyLocked()
			e.mu.Unlock()
			return pubsub.ValidationIgnore
		}
		fields := pubsubScoringMessage(m)
		fields["propagation_peer"], fields["command_sequence"], fields["committed"] = p.String(), hold.command, false
		hold.observation = e.emit("validation_held", "go.fixture.extensions.ValidatorEx", fields)
		e.notifyLocked()
	}
	e.mu.Unlock()
	result := pubsubScoringValidator(e.o.actor, e.o.token, m.Data)
	if matched {
		timer := time.NewTimer(pubsubExtensionHold)
		defer timer.Stop()
		var err error
		select {
		case <-hold.done:
		case <-e.ctx.Done():
			err = e.ctx.Err()
		case <-ctx.Done():
			err = ctx.Err()
		case <-timer.C:
			err = fmt.Errorf("application validation hold expired")
		}
		e.mu.Lock()
		fields := pubsubScoringMessage(m)
		fields["held_observation_sequence"], fields["committed"] = hold.observation, false
		fields["released"] = err == nil && hold.released
		if err != nil {
			fields["error"] = err.Error()
			e.failLocked(err)
			result = pubsub.ValidationIgnore
		}
		e.emit("validation_resumed", "go.fixture.extensions.ValidatorEx", fields)
		e.mu.Unlock()
	}
	e.o.validationDecision(p, m, result)
	e.mu.Lock()
	e.validators--
	e.notifyLocked()
	e.mu.Unlock()
	return result
}

func (e *pubsubExtensions) command(ctx context.Context, c pubsubScoringCommand, topic *pubsub.Topic) error {
	if e == nil {
		return fmt.Errorf("extension command without extension mode")
	}
	if _, err := pubsubExtensionCommandFields(c, e.mode); err != nil {
		return err
	}
	if c.Kind == "validation_hold" {
		if !strings.HasPrefix(c.Payload, "accept:"+e.o.token+":") {
			return fmt.Errorf("hold payload is not the designated case-scoped message")
		}
	}
	e.mu.Lock()
	if e.closed || e.failure != nil || !e.started {
		e.mu.Unlock()
		return fmt.Errorf("extension command admission closed/failed")
	}
	switch c.Kind {
	case "validation_hold":
		if e.hold != nil {
			e.mu.Unlock()
			return fmt.Errorf("only one designated validation hold per actor")
		}
		e.hold = &pubsubExtensionValidation{payload: c.Payload, command: c.Sequence, done: make(chan struct{})}
		digest := sha256.Sum256([]byte(c.Payload))
		e.emit("validation_hold_armed", "go.fixture.extensions.command", map[string]any{
			"command_sequence": c.Sequence, "payload_sha256": hex.EncodeToString(digest[:]), "payload_bytes": len(c.Payload)})
		e.mu.Unlock()
		return nil
	case "validation_release":
		if e.hold == nil || e.hold.observation == 0 || e.hold.released {
			e.mu.Unlock()
			return fmt.Errorf("validation release does not bind a currently held native validator")
		}
		e.hold.released = true
		e.emit("validation_release", "go.fixture.extensions.command", map[string]any{
			"command_sequence": c.Sequence, "held_observation_sequence": e.hold.observation})
		close(e.hold.done)
		e.mu.Unlock()
		return nil
	case "publish_extension":
		e.mu.Unlock()
		if topic == nil {
			return fmt.Errorf("extension publish lacks native joined topic")
		}
		err := topic.Publish(ctx, []byte(c.Payload))
		digest := sha256.Sum256([]byte(c.Payload))
		fields := map[string]any{"command_sequence": c.Sequence, "payload_sha256": hex.EncodeToString(digest[:]), "payload_bytes": len(c.Payload), "error": nil}
		if err != nil {
			fields["error"] = err.Error()
		}
		fields["topic"] = e.o.topic
		e.o.emit("publish", "go.pubsub.Topic.Publish", fields)
		return err
	default:
		reply := make(chan error, 1)
		err := e.enqueueLocked(pubsubExtensionWork{kind: "command", command: c, reply: reply})
		e.mu.Unlock()
		if err != nil {
			return err
		}
		select {
		case err := <-reply:
			return err
		case <-ctx.Done():
			return ctx.Err()
		}
	}
}

func (e *pubsubExtensions) process(w pubsubExtensionWork) error {
	switch w.kind {
	case "incoming":
		reply, err := e.acceptIncoming(w)
		if err != nil {
			e.emit("partial_rejected", "go.fixture.extensions.application", map[string]any{
				"observation_sequence": w.observation, "peer_id": w.peer.String(), "group_id_hex": hex.EncodeToString(w.group),
				"error": err.Error(), "scope": "application_only_no_router_validation_result"})
			return nil
		}
		if reply {
			return e.publish(w.group, e.groups[string(w.group)], []peer.ID{w.peer}, false, w.observation, 0)
		}
		return nil
	case "gossip":
		g := e.groups[string(w.group)]
		if g == nil || !g.local {
			return nil // Receiving/answering remote groups never opts them into application gossip.
		}
		return e.publish(w.group, g, w.peers, true, w.observation, 0)
	case "command":
		c := w.command
		if c.Kind == "partial_offer" {
			group, _ := pubsubPartialGroup(e.o.token, 1)
			g, err := e.offer(group, c.Have)
			if err != nil {
				return err
			}
			return e.publish(group, g, nil, false, 0, c.Sequence)
		}
		return fmt.Errorf("unknown extension application command")
	default:
		return fmt.Errorf("unknown extension application work")
	}
}

func (e *pubsubExtensions) offer(group []byte, have byte) (*pubsubExtensionGroup, error) {
	key := string(group)
	g := e.groups[key]
	if g == nil {
		if len(e.groups) >= pubsubExtensionGroups {
			return nil, fmt.Errorf("partial group limit")
		}
		g = &pubsubExtensionGroup{peers: make(map[peer.ID]pubsubPartialMetadata)}
	}
	if have > pubsubPartialMask || g.metadata.revision == ^uint32(0) {
		return nil, fmt.Errorf("invalid/exhausted partial offer revision")
	}
	revision := g.metadata.revision
	if revision == 0 || have != g.metadata.have {
		revision++
	}
	for index := byte(0); index < pubsubPartialParts; index++ {
		if have&(1<<index) != 0 {
			data, _ := pubsubPartialData(e.o.token, index)
			g.parts[index], _ = encodePubsubPartial(index, data)
		} else {
			g.parts[index] = nil
		}
	}
	g.local, g.metadata = true, pubsubPartialMetadata{revision, have, pubsubPartialMask &^ have}
	e.groups[key] = g
	e.emit("partial_offer", "go.fixture.extensions.application", map[string]any{
		"group_id_hex": hex.EncodeToString(group), "revision": revision, "have": have, "want": g.metadata.want,
		"group_basis": "case_token_and_sequence_not_content_hash"})
	return g, nil
}

func (e *pubsubExtensions) acceptIncoming(w pubsubExtensionWork) (bool, error) {
	expectedGroup, _ := pubsubPartialGroup(e.o.token, 1)
	if !bytes.Equal(w.group, expectedGroup) {
		return false, fmt.Errorf("foreign partial group")
	}
	if e.inputs >= pubsubExtensionInputs {
		return false, fmt.Errorf("partial incoming observation limit")
	}
	e.inputs++
	key := string(w.group)
	g := e.groups[key]
	if g == nil {
		if len(e.groups) >= pubsubExtensionGroups {
			return false, fmt.Errorf("partial group limit")
		}
		g = &pubsubExtensionGroup{metadata: pubsubPartialMetadata{1, 0, pubsubPartialMask}, peers: make(map[peer.ID]pubsubPartialMetadata)}
	}
	previous, exists := g.peers[w.peer]
	metadata := previous
	if w.metadata != nil {
		var err error
		metadata, err = decodePubsubPartialMetadata(w.metadata)
		if err != nil || exists && (metadata.revision < previous.revision || metadata.revision == previous.revision && metadata != previous) {
			return false, fmt.Errorf("invalid/stale/conflicting partial metadata")
		}
	} else if !exists {
		return false, fmt.Errorf("new partial peer lacks metadata")
	}
	if !exists && len(g.peers) >= pubsubScoringPeers {
		return false, fmt.Errorf("partial peer state limit")
	}
	var part []byte
	var index byte
	if w.body != nil {
		var data []byte
		var err error
		index, data, err = decodePubsubPartial(w.body)
		if err != nil {
			return false, err
		}
		expected, _ := pubsubPartialData(e.o.token, index)
		if !bytes.Equal(data, expected) || metadata.have&(1<<index) == 0 {
			return false, fmt.Errorf("partial part disagrees with checked content/metadata")
		}
		part = bytes.Clone(w.body)
		if g.parts[index] == nil && g.metadata.revision == ^uint32(0) {
			return false, fmt.Errorf("partial local revision exhausted")
		}
	}
	g.peers[w.peer] = metadata // Replacement, never an OR of old and new bitmaps.
	newPart := part != nil && g.parts[index] == nil
	if part != nil && g.received[index] == nil {
		g.received[index], g.receivedAt[index] = part, w.observation
	}
	if newPart {
		g.parts[index] = part
		g.metadata.revision++
		g.metadata.have |= 1 << index
		g.metadata.want = pubsubPartialMask &^ g.metadata.have
	}
	e.groups[key] = g
	encoded, _ := g.metadata.encode()
	e.emit("partial_applied", "go.fixture.extensions.application", map[string]any{
		"observation_sequence": w.observation, "peer_id": w.peer.String(), "group_id_hex": hex.EncodeToString(w.group),
		"metadata_hex": hex.EncodeToString(encoded), "body_applied": part != nil,
		"authority": "application_checked_bytes_not_signed_message_validation"})
	if g.received[0] != nil && g.received[1] != nil && g.received[2] != nil && !g.reconstructed {
		value, err := reconstructPubsubPartial(e.o.token, g.received[:])
		if err != nil {
			return false, err
		}
		g.reconstructed = true
		digest := sha256.Sum256(value)
		e.emit("partial_reconstructed", "go.fixture.extensions.application", map[string]any{
			"observation_sequence": w.observation, "group_id_hex": hex.EncodeToString(w.group),
			"part_observation_sequences": g.receivedAt,
			"payload_hex":                hex.EncodeToString(value), "payload_sha256": hex.EncodeToString(digest[:]), "payload_bytes": len(value),
			"authority": "application_reconstruction_not_router_delivery_or_signature"})
	}
	changed := !exists || previous != metadata || newPart
	return changed && (newPart || g.metadata.have&metadata.want != 0 || g.metadata.want&metadata.have != 0), nil
}

func (e *pubsubExtensions) publish(group []byte, g *pubsubExtensionGroup, targets []peer.ID, gossipOnly bool, observation, command int) error {
	if e.ps == nil {
		return fmt.Errorf("partial publication lacks native PubSub owner")
	}
	actions, err := e.publishActions(group, g, targets, gossipOnly, observation, command)
	if err != nil {
		return err
	}
	err = pubsub.PublishPartial(e.ps, e.o.topic, group, actions)
	fields := map[string]any{"group_id_hex": hex.EncodeToString(group), "observation_sequence": observation, "command_sequence": command, "error": nil}
	if err != nil {
		fields["error"] = err.Error()
	}
	e.emit("partial_publish_return", "go.pubsub.PublishPartial", fields)
	return err
}

func (e *pubsubExtensions) publishActions(group []byte, g *pubsubExtensionGroup, targets []peer.ID, gossipOnly bool, observation, command int) (partialmessages.PublishActionsFn[struct{}], error) {
	metadata, err := g.metadata.encode()
	if err != nil {
		return nil, err
	}
	// PublishPartial may return on context cancellation before its native callback.
	// Keep callback data independent of the worker's subsequent state transitions.
	group = bytes.Clone(group)
	if targets != nil {
		targets = append([]peer.ID{}, targets...)
	}
	parts := g.parts
	for i := range parts {
		parts[i] = bytes.Clone(parts[i])
	}
	knownPeers := make(map[peer.ID]pubsubPartialMetadata, len(g.peers))
	for p, state := range g.peers {
		knownPeers[p] = state
	}
	return func(states map[peer.ID]struct{}, requests func(peer.ID) bool) iter.Seq2[peer.ID, partialmessages.PublishAction] {
		return func(yield func(peer.ID, partialmessages.PublishAction) bool) {
			e.mu.Lock()
			if e.actionsClosed {
				e.mu.Unlock()
				return
			}
			e.actions++
			e.mu.Unlock()
			defer func() {
				e.mu.Lock()
				e.actions--
				e.notifyLocked()
				e.mu.Unlock()
			}()
			peers := append([]peer.ID(nil), targets...)
			if targets == nil {
				for p := range states {
					peers = append(peers, p)
				}
			}
			if len(peers) > pubsubScoringPeers {
				yield("", partialmessages.PublishAction{Err: fmt.Errorf("partial publish peer bound")})
				return
			}
			sort.Slice(peers, func(i, j int) bool { return peers[i].String() < peers[j].String() })
			for _, p := range peers {
				if _, exists := states[p]; !exists {
					if !yield(p, partialmessages.PublishAction{Err: fmt.Errorf("partial recipient no longer in native group state")}) {
						return
					}
					continue
				}
				action := partialmessages.PublishAction{EncodedPartsMetadata: bytes.Clone(metadata)}
				requested := requests(p)
				if !gossipOnly && requested {
					want := pubsubPartialMask
					if known, exists := knownPeers[p]; exists {
						want = known.want
					}
					for index := byte(0); index < pubsubPartialParts; index++ {
						if want&(1<<index) != 0 && parts[index] != nil {
							action.EncodedPartialMessage = bytes.Clone(parts[index])
							break // One bounded part per request; newer metadata requests the next.
						}
					}
				}
				e.emit("partial_publish_action", "go.pubsub.PublishPartial.actions", map[string]any{
					"peer_id": p.String(), "group_id_hex": hex.EncodeToString(group), "observation_sequence": observation,
					"command_sequence": command, "requests_partial": requested, "gossip_metadata_only": gossipOnly,
					"body_present": action.EncodedPartialMessage != nil, "body_hex": hex.EncodeToString(action.EncodedPartialMessage),
					"metadata_hex": hex.EncodeToString(metadata), "authority": "application_submission_not_wire_write"})
				if !yield(p, action) {
					return
				}
			}
		}
	}, nil
}
