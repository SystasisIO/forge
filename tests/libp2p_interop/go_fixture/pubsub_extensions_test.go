package main

import (
	"bytes"
	"context"
	"encoding/hex"
	"fmt"
	"strings"
	"sync"
	"testing"
	"time"

	libp2p "github.com/libp2p/go-libp2p"
	pubsub "github.com/libp2p/go-libp2p-pubsub"
	"github.com/libp2p/go-libp2p-pubsub/partialmessages"
	pubsubpb "github.com/libp2p/go-libp2p-pubsub/pb"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/core/protocol"
)

// Synthetic application units do not constitute network, signature or router proof.
func TestPubsubExtensionResultVersion(t *testing.T) {
	for mode, version := range map[string]string{"idontwant": "1.2", "advertisement": "1.3", "partial": "1.3"} {
		t.Run(mode, func(t *testing.T) {
			e := pubsubExtensionsUnit(t, mode)
			result := map[string]any{}
			e.annotate(result)
			if result["version"] != version || result["extension"] != mode || result["requests_partial"] != (mode == "partial") {
				t.Fatalf("wrong configured extension version: %v", result)
			}
		})
	}
}

func pubsubExtensionsUnit(t *testing.T, mode string) *pubsubExtensions {
	t.Helper()
	return pubsubExtensionsUnitWithPending(t, mode, 0)
}

func pubsubExtensionsUnitWithPending(t *testing.T, mode string, expectedPending int) *pubsubExtensions {
	t.Helper()
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	o.local = pubsubScoringUnitConnection(t).local
	e := newPubsubExtensions(context.Background(), mode, o)
	t.Cleanup(func() { pubsubExtensionsStopUnit(t, e, expectedPending) })
	return e
}

func pubsubExtensionsStopUnit(t *testing.T, e *pubsubExtensions, expectedPending int) {
	t.Helper()
	e.cancel()
	e.mu.Lock()
	started, failure := e.started, e.failure
	if !started {
		if e.pending != expectedPending || len(e.queue) != expectedPending || e.validators != 0 || e.actions != 0 {
			t.Errorf("unstarted fixture state: pending=%d queued=%d validators=%d callbacks=%d, want pending=%d and no callbacks",
				e.pending, len(e.queue), e.validators, e.actions, expectedPending)
		}
		// These pure queue fixtures have no worker. Release their explicitly
		// expected test inputs without pretending that a native worker ran.
	drain:
		for drained := 0; drained < pubsubExtensionQueue; drained++ {
			select {
			case work, ok := <-e.queue:
				if !ok {
					break drain
				}
				e.pending--
				if work.reply != nil {
					t.Error("unstarted fixture unexpectedly owns a command waiter")
				}
			default:
				break drain
			}
		}
	}
	e.mu.Unlock()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	err := e.stop(ctx)
	e.mu.Lock()
	pending, validators, actions := e.pending, e.validators, e.actions
	closed, actionsClosed, stoppedFailure := e.closed, e.actionsClosed, e.failure
	e.mu.Unlock()
	if started {
		select {
		case <-e.done:
		default:
			panic(fmt.Sprintf("owned extension worker failed to join within cleanup budget: %v", err))
		}
	} else {
		select {
		case <-e.done:
			t.Error("unstarted fixture claimed a completed worker")
		default:
		}
	}
	if validators != 0 || actions != 0 || e.workerCount() != 0 {
		panic(fmt.Sprintf("owned extension callbacks failed to join within cleanup budget: validators=%d callbacks=%d error=%v",
			validators, actions, err))
	}
	if err != failure || stoppedFailure != failure {
		t.Errorf("extension cleanup returned %v with sticky error %v, want original %v", err, stoppedFailure, failure)
	}
	if pending != 0 || !closed || !actionsClosed {
		t.Errorf("extension cleanup incomplete: pending=%d validators=%d callbacks=%d closed=%t callbacks_closed=%t",
			pending, validators, actions, closed, actionsClosed)
	}
}

func pubsubExtensionsNativeUnit(t *testing.T, mode string) *pubsubExtensions {
	t.Helper()
	e := pubsubExtensionsUnit(t, mode)
	ctx, cancel := context.WithCancel(context.Background())
	h, err := libp2p.New(libp2p.NoListenAddrs, libp2p.DisableRelay())
	if err != nil {
		cancel()
		t.Fatal(err)
	}
	t.Cleanup(func() {
		cancel()
		pubsubExtensionsStopUnit(t, e, 0)
		if err := h.Close(); err != nil {
			t.Error(err)
		}
	})
	e.o.local = h.ID()
	id := pubsub.GossipSubID_v13
	if mode == "idontwant" {
		id = pubsub.GossipSubID_v12
	}
	opts := []pubsub.Option{pubsub.WithGossipSubProtocols([]protocol.ID{id}, pubsub.GossipSubDefaultFeatures)}
	ps, err := pubsub.NewGossipSub(ctx, h, append(opts, e.options()...)...)
	if err != nil {
		t.Fatal(err)
	}
	if err = e.start(ps); err != nil {
		t.Fatal(err)
	}
	return e
}

func pubsubExtensionsIncoming(t *testing.T, e *pubsubExtensions, p peer.ID, revision uint32, have, want byte, part *byte) pubsubExtensionWork {
	t.Helper()
	group, err := pubsubPartialGroup(e.o.token, 1)
	if err != nil {
		t.Fatal(err)
	}
	metadata, err := (pubsubPartialMetadata{revision, have, want}).encode()
	if err != nil {
		t.Fatal(err)
	}
	var body []byte
	if part != nil {
		data, _ := pubsubPartialData(e.o.token, *part)
		body, _ = encodePubsubPartial(*part, data)
	}
	states := map[peer.ID]struct{}{}
	if err := e.incoming(p, states, &pubsubpb.PartialMessagesExtension{TopicID: &e.o.topic, GroupID: group, PartsMetadata: metadata, PartialMessage: body}); err != nil {
		t.Fatal(err)
	}
	if _, ok := states[p]; !ok {
		t.Fatal("actual incoming peer not retained in native callback state")
	}
	select {
	case work := <-e.queue:
		e.mu.Lock()
		e.pending--
		e.mu.Unlock()
		return work
	default:
		t.Fatal("native callback did not queue application work")
		return pubsubExtensionWork{}
	}
}

func TestPubsubExtensionsCallbacksAreQueuedAndCopyBorrowedBytes(t *testing.T) {
	e := pubsubExtensionsUnit(t, "partial")
	p := pubsubScoringUnitConnection(t).remote
	group, _ := pubsubPartialGroup(e.o.token, 1)
	metadata, _ := (pubsubPartialMetadata{1, 7, 0}).encode()
	states := map[peer.ID]struct{}{}
	if err := e.incoming(p, states, &pubsubpb.PartialMessagesExtension{TopicID: &e.o.topic, GroupID: group, PartsMetadata: metadata}); err != nil {
		t.Fatal(err)
	}
	group[0], metadata[0] = 0, 0
	if len(e.groups) != 0 || e.ps != nil {
		t.Fatal("callback reentered publication or applied data inline")
	}
	w := <-e.queue
	e.pending--
	if w.group[0] != 0xaa || w.metadata[0] != 1 || w.body != nil {
		t.Fatal("queued work borrowed mutable callback data")
	}
	if reply, err := e.acceptIncoming(w); err != nil || !reply {
		t.Fatal("metadata should cause a queued application request", reply, err)
	}
	group, _ = pubsubPartialGroup(e.o.token, 1)
	peers := []peer.ID{p}
	e.gossip(e.o.topic, group, peers, states)
	peers[0], group[0] = "changed", 0
	gossip := <-e.queue
	e.pending--
	if gossip.peer != "" || len(gossip.peers) != 1 || gossip.peers[0] != p || gossip.group[0] != 0xaa {
		t.Fatal("native gossip recipient selection not copied exactly")
	}
	if len(pubsubScoringUnitEvents(e.o, "partial_publish_action")) != 0 {
		t.Fatal("callback fabricated a native send")
	}
}

func TestPubsubExtensionsMetadataReplacementAndReceivedOnlyReconstruction(t *testing.T) {
	e := pubsubExtensionsUnit(t, "partial")
	p := pubsubScoringUnitConnection(t).remote
	first := pubsubExtensionsIncoming(t, e, p, 1, 7, 0, nil)
	if reply, err := e.acceptIncoming(first); err != nil || !reply {
		t.Fatal(reply, err)
	}
	g := e.groups[string(first.group)]
	second := pubsubExtensionsIncoming(t, e, p, 2, 1, 6, nil)
	if _, err := e.acceptIncoming(second); err != nil || g.peers[p].have != 1 || g.peers[p].want != 6 {
		t.Fatal("new metadata was OR-ed with prior state", g.peers[p], err)
	}
	for _, meta := range []pubsubPartialMetadata{{1, 7, 0}, {2, 7, 0}} {
		w := pubsubExtensionsIncoming(t, e, p, meta.revision, meta.have, meta.want, nil)
		before := g.peers[p]
		if _, err := e.acceptIncoming(w); err == nil || g.peers[p] != before {
			t.Fatal("stale/equal conflicting metadata applied", err)
		}
	}
	if _, err := e.offer(first.group, 7); err != nil {
		t.Fatal(err)
	}
	if len(pubsubScoringUnitEvents(e.o, "partial_reconstructed")) != 0 {
		t.Fatal("local parts fabricated received reconstruction")
	}
	var refs [3]int
	for index := byte(0); index < pubsubPartialParts; index++ {
		w := pubsubExtensionsIncoming(t, e, p, 3, 7, 0, &index)
		refs[index] = w.observation
		if _, err := e.acceptIncoming(w); err != nil {
			t.Fatal(err)
		}
		if count := len(pubsubScoringUnitEvents(e.o, "partial_reconstructed")); count != map[bool]int{true: 1, false: 0}[index == 2] {
			t.Fatal("reconstruction lacks three received parts", index, count)
		}
	}
	event := pubsubScoringUnitEvents(e.o, "partial_reconstructed")[0]
	expected, _ := reconstructPubsubPartial(e.o.token, g.received[:])
	if event["payload_hex"] != hex.EncodeToString(expected) || event["part_observation_sequences"] != refs {
		t.Fatal("reconstruction not bound to exact part observations", event)
	}
	if len(pubsubScoringUnitEvents(e.o, "delivery"))+len(pubsubScoringUnitEvents(e.o, "validation")) != 0 {
		t.Fatal("partial callback fabricated full-message validation")
	}
}

func TestPubsubExtensionsMalformedApplicationBytesDoNotMutateState(t *testing.T) {
	for _, mode := range []string{"foreign_group", "empty_metadata", "missing_metadata", "bad_part", "wrong_content", "contradictory_part"} {
		t.Run(mode, func(t *testing.T) {
			e := pubsubExtensionsUnit(t, "partial")
			p := pubsubScoringUnitConnection(t).remote
			index := byte(0)
			w := pubsubExtensionsIncoming(t, e, p, 1, 7, 0, &index)
			switch mode {
			case "foreign_group":
				w.group[0] ^= 1
			case "empty_metadata":
				w.metadata = []byte{}
			case "missing_metadata":
				w.metadata = nil
			case "bad_part":
				w.body = []byte{1}
			case "wrong_content":
				w.body[len(w.body)-1] ^= 1
			case "contradictory_part":
				w.metadata, _ = (pubsubPartialMetadata{1, 0, 7}).encode()
			}
			if _, err := e.acceptIncoming(w); err == nil || len(e.groups) != 0 {
				t.Fatal("invalid application input changed state", err)
			}
			if e.o.failure != nil {
				t.Fatal("application rejection fabricated a native error")
			}
		})
	}
}

func TestPubsubExtensionsNativeOwnerOptionsAndJoinedWorker(t *testing.T) {
	for _, mode := range []string{"idontwant", "partial", "advertisement"} {
		t.Run(mode, func(t *testing.T) {
			e := pubsubExtensionsNativeUnit(t, mode)
			topic, err := e.ps.Join(e.o.topic, e.topicOptions()...)
			if err != nil {
				t.Fatal(err)
			}
			if _, err = topic.Subscribe(); err != nil {
				t.Fatal(err)
			}
			ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
			defer cancel()
			if mode == "partial" {
				for _, have := range []byte{0, 7, 7, 1} {
					if err = e.command(ctx, pubsubScoringCommand{Kind: "partial_offer", Have: have}, topic); err != nil {
						t.Fatal(err)
					}
				}
				group, _ := pubsubPartialGroup(e.o.token, 1)
				g := e.groups[string(group)]
				if g.metadata != (pubsubPartialMetadata{3, 1, 6}) {
					t.Fatal("owned revision must only advance on changes", g.metadata)
				}
			}
			if err = e.stop(ctx); err != nil || e.workerCount() != 0 || e.pending != 0 {
				t.Fatal("actual application worker not joined", err)
			}
			command := pubsubScoringCommand{Sequence: 1, Actor: e.o.actor, Token: e.o.token, Local: e.o.local.String()}
			if err = e.o.prepareShutdown(command, 0); err != nil {
				t.Fatal(err)
			}
			joined := pubsubScoringUnitEvents(e.o, "extension_stopped")
			ack := pubsubScoringUnitEvents(e.o, "shutdown_prepared")
			if len(joined) != 1 || joined[0]["worker_joined"] != true || joined[0]["sequence"].(int) >= ack[0]["sequence"].(int) {
				t.Fatal("Prepare ACK preceded application join", joined, ack)
			}
			if err = e.command(ctx, pubsubScoringCommand{Kind: "publish_extension", Payload: "later"}, topic); err == nil {
				t.Fatal("extension command admitted after join")
			}
		})
	}
}

func TestPubsubExtensionsValidationHoldReleaseAndCancellation(t *testing.T) {
	for _, cancelHold := range []bool{false, true} {
		t.Run(fmt.Sprint(cancelHold), func(t *testing.T) {
			e := pubsubExtensionsNativeUnit(t, "idontwant")
			ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
			defer cancel()
			payload := "accept:" + e.o.token + ":idontwant:" + strings.Repeat("x", 2048)
			if err := e.command(ctx, pubsubScoringCommand{Sequence: 1, Kind: "validation_hold", Payload: payload}, nil); err != nil {
				t.Fatal(err)
			}
			if err := e.command(ctx, pubsubScoringCommand{Sequence: 2, Kind: "validation_release"}, nil); err == nil {
				t.Fatal("release before actual validator observation")
			}
			p := pubsubScoringUnitConnection(t).remote
			m := &pubsub.Message{Message: &pubsubpb.Message{From: []byte(p), Seqno: make([]byte, 8), Topic: &e.o.topic, Data: []byte(payload)}, ReceivedFrom: p, ID: "unit-native-id"}
			finished := make(chan pubsub.ValidationResult, 1)
			joined := make(chan struct{})
			t.Cleanup(func() { e.cancel(); pubsubScoringUnitCleanupJoin(joined) })
			go func() { defer close(joined); finished <- e.validate(ctx, p, m) }()
			for {
				e.mu.Lock()
				observed := e.hold.observation != 0
				e.mu.Unlock()
				if observed {
					break
				}
				select {
				case <-e.changed:
				case <-ctx.Done():
					t.Fatal("held validator not observed")
				}
			}
			select {
			case <-finished:
				t.Fatal("held validation returned early")
			default:
			}
			if cancelHold {
				e.cancel()
			} else if err := e.command(ctx, pubsubScoringCommand{Sequence: 2, Kind: "validation_release"}, nil); err != nil {
				t.Fatal(err)
			}
			select {
			case result := <-finished:
				expected := pubsub.ValidationAccept
				if cancelHold {
					expected = pubsub.ValidationIgnore
				}
				if result != expected {
					t.Fatal(result)
				}
			case <-ctx.Done():
				t.Fatal("validator did not leave its owned hold")
			}
			pubsubScoringUnitCleanupJoin(joined)
			resumed := pubsubScoringUnitEvents(e.o, "validation_resumed")
			held := pubsubScoringUnitEvents(e.o, "validation_held")
			if len(resumed) != 1 || len(held) != 1 || resumed[0]["held_observation_sequence"] != held[0]["sequence"] || resumed[0]["released"] != !cancelHold {
				t.Fatal("validation release lacks actual held-message causality", resumed)
			}
			if (e.o.failure != nil) != cancelHold {
				t.Fatal("cancellation error lost or invented", e.o.failure)
			}
		})
	}
}

func TestPubsubExtensionsQueueBoundsAndClosedAdmission(t *testing.T) {
	e := pubsubExtensionsUnitWithPending(t, "partial", pubsubExtensionQueue)
	p := pubsubScoringUnitConnection(t).remote
	group, _ := pubsubPartialGroup(e.o.token, 1)
	metadata, _ := (pubsubPartialMetadata{1, 7, 0}).encode()
	states := map[peer.ID]struct{}{}
	rpc := &pubsubpb.PartialMessagesExtension{TopicID: &e.o.topic, GroupID: group, PartsMetadata: metadata}
	for i := 0; i < pubsubExtensionQueue; i++ {
		if err := e.incoming(p, states, rpc); err != nil {
			t.Fatal(err)
		}
	}
	if err := e.incoming(p, states, rpc); err == nil || e.o.failure == nil || len(e.queue) != pubsubExtensionQueue {
		t.Fatal("unbounded callback queue", err)
	}
	first := e.o.failure
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if e.stop(ctx) == nil || e.o.failure != first {
		t.Fatal("pending work/first error discarded")
	}
	before := len(e.o.events)
	if err := e.incoming(p, states, rpc); err != nil || len(e.o.events) != before || len(e.queue) != pubsubExtensionQueue {
		t.Fatal("closed callback admission restarted work", err)
	}
}

func TestPubsubExtensionsNoAutomaticReplyLoopForUnchangedMetadata(t *testing.T) {
	e := pubsubExtensionsUnit(t, "partial")
	p := pubsubScoringUnitConnection(t).remote
	w := pubsubExtensionsIncoming(t, e, p, 1, 7, 0, nil)
	if reply, err := e.acceptIncoming(w); err != nil || !reply {
		t.Fatal(reply, err)
	}
	w = pubsubExtensionsIncoming(t, e, p, 1, 7, 0, nil)
	if reply, err := e.acceptIncoming(w); err != nil || reply {
		t.Fatal("unchanged metadata induced reply loop", reply, err)
	}
	before, _ := e.groups[string(w.group)].metadata.encode()
	w = pubsubExtensionsIncoming(t, e, p, 2, 0, 7, nil)
	if reply, err := e.acceptIncoming(w); err != nil || reply {
		t.Fatal("two empty groups induced reply loop", reply, err)
	}
	after, _ := e.groups[string(w.group)].metadata.encode()
	if !bytes.Equal(before, after) {
		t.Fatal("remote metadata rewrote local ownership")
	}
}

func TestPubsubExtensionsPublicationUsesNativeRecipientsAndRequests(t *testing.T) {
	for _, kind := range []string{"eager", "gossip", "full_only", "known_want", "foreign_recipient", "stopped_iterator"} {
		t.Run(kind, func(t *testing.T) {
			e := pubsubExtensionsUnit(t, "partial")
			p := pubsubScoringUnitConnection(t).remote
			group, _ := pubsubPartialGroup(e.o.token, 1)
			g, err := e.offer(group, 7)
			if err != nil {
				t.Fatal(err)
			}
			states := map[peer.ID]struct{}{p: {}}
			var targets []peer.ID
			if kind == "gossip" || kind == "foreign_recipient" {
				targets = []peer.ID{p}
			}
			if kind == "foreign_recipient" {
				delete(states, p)
			}
			if kind == "known_want" {
				g.peers[p] = pubsubPartialMetadata{3, 3, 4}
			}
			actions, err := e.publishActions(group, g, targets, kind == "gossip", 12, 2)
			if err != nil {
				t.Fatal(err)
			}
			var received []partialmessages.PublishAction
			requests := 0
			actions(states, func(current peer.ID) bool {
				if current != p {
					t.Fatal("requested capability of a different native peer")
				}
				requests++
				return kind != "full_only"
			})(func(current peer.ID, action partialmessages.PublishAction) bool {
				if current != p {
					t.Fatal("changed native recipient")
				}
				received = append(received, action)
				return kind != "stopped_iterator"
			})
			if len(received) != 1 {
				t.Fatal("missing or duplicate application action", received)
			}
			action := received[0]
			if kind == "foreign_recipient" {
				if action.Err == nil || requests != 0 || len(pubsubScoringUnitEvents(e.o, "partial_publish_action")) != 0 {
					t.Fatal("borrowed native peer state for a foreign recipient")
				}
				return
			}
			metadata, _ := g.metadata.encode()
			if action.Err != nil || requests != 1 || !bytes.Equal(action.EncodedPartsMetadata, metadata) {
				t.Fatal("action lacks current metadata/capability", action)
			}
			if kind == "gossip" || kind == "full_only" {
				if action.EncodedPartialMessage != nil {
					t.Fatal("metadata gossip or full-only peer received a part")
				}
			} else {
				index, _, err := decodePubsubPartial(action.EncodedPartialMessage)
				expected := byte(0)
				if kind == "known_want" {
					expected = 2
				}
				if err != nil || index != expected {
					t.Fatal("wrong eagerly supplied/requested part", index, err)
				}
			}
			if len(pubsubScoringUnitEvents(e.o, "rpc")) != 0 || len(pubsubScoringUnitEvents(e.o, "partial_publish_return")) != 0 {
				t.Fatal("synthetic iterator fabricated native execution/wire receipt")
			}
		})
	}
}

func TestPubsubExtensionsAdvertisementDoesNotRequestParts(t *testing.T) {
	e := pubsubExtensionsUnit(t, "advertisement")
	p := pubsubScoringUnitConnection(t).remote
	group, _ := pubsubPartialGroup(e.o.token, 1)
	metadata, _ := (pubsubPartialMetadata{1, 7, 0}).encode()
	rpc := &pubsubpb.PartialMessagesExtension{TopicID: &e.o.topic, GroupID: group, PartsMetadata: metadata}
	states := map[peer.ID]struct{}{}
	if len(e.options()) != 1 || len(e.topicOptions()) != 0 {
		t.Fatal("advertisement mode changed the full-only topic")
	}
	if err := e.incoming(p, states, rpc); err != nil || e.o.failure != nil || len(e.queue) != 0 || len(states) != 0 {
		t.Fatal("full-only native metadata became a partial request", err)
	}
	if len(pubsubScoringUnitEvents(e.o, "partial_incoming")) != 1 || len(pubsubScoringUnitEvents(e.o, "partial_publish_action")) != 0 {
		t.Fatal("metadata observation missing or reply invented")
	}
	data, _ := pubsubPartialData(e.o.token, 0)
	rpc.PartialMessage, _ = encodePubsubPartial(0, data)
	if err := e.incoming(p, states, rpc); err == nil || e.o.failure == nil {
		t.Fatal("full-only fixture accepted an unsolicited partial body")
	}
}

func TestPubsubExtensionsPublicationSnapshotAndAdmittedCallbackJoin(t *testing.T) {
	e := pubsubExtensionsUnit(t, "partial")
	p := pubsubScoringUnitConnection(t).remote
	group, _ := pubsubPartialGroup(e.o.token, 1)
	g, err := e.offer(group, 7)
	if err != nil {
		t.Fatal(err)
	}
	actions, err := e.publishActions(group, g, nil, false, 1, 0)
	if err != nil {
		t.Fatal(err)
	}
	expected := bytes.Clone(g.parts[0])
	// A canceled PublishPartial caller may advance its worker-owned state while
	// the donor still owns the earlier callback. Its actual submission must not.
	g.parts[0][0] = 0
	g.peers[p] = pubsubPartialMetadata{1, 7, 0}
	entered := make(chan partialmessages.PublishAction, 1)
	release := make(chan struct{})
	joined := make(chan struct{})
	var once sync.Once
	unblock := func() { once.Do(func() { close(release) }) }
	t.Cleanup(func() { unblock(); pubsubScoringUnitCleanupJoin(joined) })
	go func() {
		defer close(joined)
		actions(map[peer.ID]struct{}{p: {}}, func(peer.ID) bool { return true })(func(_ peer.ID, action partialmessages.PublishAction) bool {
			entered <- action
			<-release
			return true
		})
	}()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	select {
	case action := <-entered:
		if !bytes.Equal(action.EncodedPartialMessage, expected) {
			t.Fatal("callback borrowed subsequently mutated worker state")
		}
	case <-ctx.Done():
		t.Fatal("application callback was not entered")
	}
	canceled, stop := context.WithCancel(context.Background())
	stop()
	if err := e.stop(canceled); err == nil {
		t.Fatal("application join ignored a live native-invoked callback")
	}
	e.mu.Lock()
	active, closed := e.actions, e.actionsClosed
	e.mu.Unlock()
	if active != 1 || !closed {
		t.Fatal("callback ownership/admission lost", active, closed)
	}
	unblock()
	pubsubScoringUnitCleanupJoin(joined)
	if err := e.stop(ctx); err != nil {
		t.Fatal(err)
	}
	before := len(pubsubScoringUnitEvents(e.o, "partial_publish_action"))
	actions(map[peer.ID]struct{}{p: {}}, func(peer.ID) bool { t.Fatal("capability queried after join"); return true })(func(peer.ID, partialmessages.PublishAction) bool {
		t.Fatal("callback admitted after join")
		return false
	})
	if len(pubsubScoringUnitEvents(e.o, "partial_publish_action")) != before {
		t.Fatal("late native callback fabricated application work")
	}
}
