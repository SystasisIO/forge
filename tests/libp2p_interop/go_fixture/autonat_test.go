package main

import (
	"sync/atomic"
	"testing"
	"time"

	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/p2p/protocol/autonatv2"
)

type autoNATTestStream struct {
	network.Stream
	id    string
	reset func() error
}

func (s *autoNATTestStream) ID() string   { return s.id }
func (s *autoNATTestStream) Reset() error { return s.reset() }

func TestAutoNATHandlersJoinAndRejectLateArrival(t *testing.T) {
	g := newAutoNATHandlerGroup()
	started, release, done := make(chan struct{}), make(chan struct{}), make(chan struct{})
	defer func() {
		close(release)
		select {
		case <-done:
		case <-time.After(time.Second):
			t.Error("handler did not finish after release")
		}
		if err := g.join(time.Now().Add(time.Second)); err != nil {
			t.Error(err)
		}
	}()
	var resets atomic.Int32
	s := &autoNATTestStream{id: "one", reset: func() error {
		if !g.mu.TryLock() {
			t.Error("reset invoked under handler mutex")
		} else {
			g.mu.Unlock()
		}
		resets.Add(1)
		return nil
	}}
	go func() { defer close(done); g.wrap(func(network.Stream) { close(started); <-release })(s) }()
	select {
	case <-started:
	case <-time.After(time.Second):
		t.Fatal("handler did not start")
	}
	if err := g.closeAdmission(); err != nil {
		t.Fatal(err)
	}
	if err := g.join(time.Now()); err == nil {
		t.Fatal("join ignored running handler")
	}
	g.wrap(func(network.Stream) { t.Error("late handler admitted") })(s)
	if resets.Load() != 2 {
		t.Fatalf("reset count: %d", resets.Load())
	}
	if err := g.closeAdmission(); err != nil {
		t.Fatal(err)
	}
}

func TestAutoNATCompletedHandlersAllowLateJoin(t *testing.T) {
	g := newAutoNATHandlerGroup()
	s := &autoNATTestStream{id: "done", reset: func() error { t.Error("completed stream reset"); return nil }}
	g.wrap(func(network.Stream) {})(s)
	if err := g.closeAdmission(); err != nil {
		t.Fatal(err)
	}
	for i := 0; i != 2; i++ {
		if err := g.join(time.Now()); err != nil {
			t.Fatal(err)
		}
	}
}

func TestAutoNATTraceOverflowIsStickyAndBounded(t *testing.T) {
	trace := newAutoNATTrace()
	for i := 0; i < autoNATCompletedTraceLimit; i++ {
		trace.CompletedRequest(autonatv2.EventDialRequestCompleted{})
	}
	if err := trace.failure(); err != nil {
		t.Fatal(err)
	}
	for i := 0; i != 3; i++ {
		trace.CompletedRequest(autonatv2.EventDialRequestCompleted{})
	}
	if trace.failure() == nil {
		t.Fatal("overflow was hidden")
	}
	if len(trace.completed) != autoNATCompletedTraceLimit {
		t.Fatal("trace exceeded bound")
	}
	select {
	case <-trace.failed:
	default:
		t.Fatal("overflow did not wake owner")
	}
}
