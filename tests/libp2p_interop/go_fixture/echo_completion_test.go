package main

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"testing"
	"time"
)

type echoTestCloser struct {
	close func() error
}

func (closer *echoTestCloser) Close() error {
	if closer.close != nil {
		return closer.close()
	}
	return nil
}

func echoBarrierOptions(t *testing.T) options {
	t.Helper()
	root := t.TempDir()
	return options{command: "dial", scenario: "echo_large", transport: "quic", echoBarrier: "stop-file",
		readyFile: filepath.Join(root, "witness.json"), resultFile: filepath.Join(root, "result.json"),
		stopFile: filepath.Join(root, "stop")}
}

func readEchoBarrierReceipt(t *testing.T, path string) map[string]any {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var receipt map[string]any
	if err := json.Unmarshal(data, &receipt); err != nil {
		t.Fatal(err)
	}
	return receipt
}

func TestEchoCompletionBarrierOptInOnly(t *testing.T) {
	if err := validateEchoCompletionBarrier(options{}); err != nil {
		t.Fatal(err)
	}
	opts := echoBarrierOptions(t)
	for _, transport := range []string{"quic", "tcp-tls"} {
		valid := opts
		valid.transport = transport
		if err := validateEchoCompletionBarrier(valid); err != nil {
			t.Fatal(err)
		}
	}
	for _, mutate := range []func(*options){
		func(o *options) { o.command = "listen" }, func(o *options) { o.transport = "tcp" },
		func(o *options) { o.transport = "tcp-pnet-tls" }, func(o *options) { o.transport = "tcp-pnet-noise" },
		func(o *options) { o.scenario = "echo" }, func(o *options) { o.echoBarrier = "ignore-canceled" },
		func(o *options) { o.pnetKeyFile = "key" }, func(o *options) { o.pnetControl = "missing_key" },
		func(o *options) { o.readyFile = "" }, func(o *options) { o.resultFile = o.readyFile },
		func(o *options) { o.stopFile = o.resultFile },
		func(o *options) { o.resultFile = o.readyFile + ".tmp" },
	} {
		bad := opts
		mutate(&bad)
		if err := validateEchoCompletionBarrier(bad); err == nil {
			t.Fatalf("accepted invalid barrier options: %+v", bad)
		}
	}
}

func TestEchoCompletionBarrierMissingStopTimesOutWithoutTerminalSuccess(t *testing.T) {
	opts := echoBarrierOptions(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Millisecond)
	defer cancel()
	barrier := echoCompletionBarrier{stream: &echoTestCloser{}}
	err := barrier.wait(ctx, opts, map[string]any{"status": "ok", "echo_ok": true})
	if !errors.Is(err, context.DeadlineExceeded) || barrier.stopObserved {
		t.Fatalf("missing stop did not remain a bounded failure: %v", err)
	}
	witness := readEchoBarrierReceipt(t, opts.readyFile)
	if witness["status"] != "exchange_observed" || witness["final"] != false {
		t.Fatalf("interim witness became terminal success: %+v", witness)
	}
	if _, err := os.Stat(opts.resultFile); !errors.Is(err, os.ErrNotExist) {
		t.Fatal("terminal receipt appeared before host close")
	}
	closed := false
	err = barrier.finish(opts, witness, err, func() error { closed = true; return nil })
	if !closed || !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("deadline failure or checked close lost: %v", err)
	}
	final := readEchoBarrierReceipt(t, opts.resultFile)
	if final["status"] != "error" || final["final"] != true {
		t.Fatalf("missing stop produced success: %+v", final)
	}
}

func TestEchoCompletionBarrierFinalReceiptFollowsCheckedClose(t *testing.T) {
	for _, closeErr := range []error{nil, errors.New("owned host close failed")} {
		opts := echoBarrierOptions(t)
		streamClosed := false
		barrier := echoCompletionBarrier{stopObserved: true, stream: &echoTestCloser{close: func() error {
			streamClosed = true
			return nil
		}}}
		result := map[string]any{"status": "ok", "echo_ok": true, "application_connection_id": "actual-connection"}
		err := barrier.finish(opts, result, nil, func() error {
			if !streamClosed {
				t.Fatal("host closed before the retained echo stream")
			}
			if _, err := os.Stat(opts.resultFile); !errors.Is(err, os.ErrNotExist) {
				t.Fatal("terminal receipt appeared before checked host close")
			}
			return closeErr
		})
		if !errors.Is(err, closeErr) {
			t.Fatalf("host close error lost: %v", err)
		}
		final := readEchoBarrierReceipt(t, opts.resultFile)
		completion := final["echo_completion"].(map[string]any)
		if final["final"] != true || completion["host_close_completed"] != (closeErr == nil) ||
			(final["status"] == "ok") != (closeErr == nil) || result["final"] != nil {
			t.Fatalf("checked close/final result or immutable exchange witness violated: %+v", final)
		}
	}
}

func TestEchoCompletionBarrierWaitsForStopAfterActualWitness(t *testing.T) {
	opts := echoBarrierOptions(t)
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	writer := make(chan error, 1)
	go func() {
		ticker := time.NewTicker(time.Millisecond)
		defer ticker.Stop()
		for {
			if _, err := os.Stat(opts.readyFile); err == nil {
				writer <- os.WriteFile(opts.stopFile, []byte("stop"), 0o600)
				return
			} else if !errors.Is(err, os.ErrNotExist) {
				writer <- err
				return
			}
			select {
			case <-ctx.Done():
				writer <- ctx.Err()
				return
			case <-ticker.C:
			}
		}
	}()
	closed := false
	barrier := echoCompletionBarrier{stream: &echoTestCloser{close: func() error { closed = true; return nil }}}
	result := map[string]any{"status": "ok", "echo_ok": true, "local_peer_id": "actual-peer"}
	err := barrier.wait(ctx, opts, result)
	if writerErr := <-writer; err != nil || writerErr != nil || !barrier.stopObserved {
		t.Fatalf("owned stop/wait did not complete: %v / %v", err, writerErr)
	}
	witness := readEchoBarrierReceipt(t, opts.readyFile)
	if witness["final"] != false || witness["local_peer_id"] != result["local_peer_id"] || result["final"] != nil {
		t.Fatalf("actual exchange witness mutated: %+v", witness)
	}
	if closed {
		t.Fatal("echo stream closed before the completion stop barrier")
	}
	if _, err := os.Stat(opts.resultFile); !errors.Is(err, os.ErrNotExist) {
		t.Fatal("stop observation alone published terminal success before checked close")
	}
	if err := barrier.finish(opts, result, nil, func() error { return nil }); err != nil || !closed || barrier.stream != nil {
		t.Fatalf("retained stream did not complete checked post-stop cleanup: %v", err)
	}
}

func TestEchoCompletionBarrierDefaultDoesNotRetainStream(t *testing.T) {
	ctx := context.Background()
	var barrier echoCompletionBarrier
	stream := &echoTestCloser{}
	if barrier.bind(ctx, options{}) != ctx || retainEchoCompletionStream(ctx, stream) || barrier.stream != nil {
		t.Fatal("default immediate-disconnect lane gained retained ownership")
	}
	ctx = barrier.bind(ctx, echoBarrierOptions(t))
	if !retainEchoCompletionStream(ctx, stream) || barrier.stream != stream ||
		retainEchoCompletionStream(ctx, &echoTestCloser{}) {
		t.Fatal("opt-in lane did not retain exactly the actual single echo stream")
	}
}

func TestEchoCompletionBarrierStreamCloseFailureDoesNotHideHostCleanup(t *testing.T) {
	opts := echoBarrierOptions(t)
	streamErr := errors.New("owned stream close failed")
	barrier := echoCompletionBarrier{stopObserved: true, stream: &echoTestCloser{close: func() error { return streamErr }}}
	hostClosed := false
	err := barrier.finish(opts, map[string]any{"echo_ok": true}, nil, func() error { hostClosed = true; return nil })
	if !hostClosed || !errors.Is(err, streamErr) || barrier.stream != nil {
		t.Fatalf("stream failure erased or host cleanup/owner release lost: %v", err)
	}
	final := readEchoBarrierReceipt(t, opts.resultFile)
	completion := final["echo_completion"].(map[string]any)
	if final["status"] != "error" || completion["stream_close_completed"] != false ||
		completion["host_close_completed"] != true {
		t.Fatalf("failed stream close became terminal success: %+v", final)
	}
}

func TestEchoCompletionBarrierCancellationNeverBecomesStop(t *testing.T) {
	opts := echoBarrierOptions(t)
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	cancel()
	var barrier echoCompletionBarrier
	err := barrier.wait(ctx, opts, map[string]any{"status": "ok", "echo_ok": true})
	if !errors.Is(err, context.Canceled) || barrier.stopObserved {
		t.Fatalf("cancellation relabeled successful stop: %v", err)
	}
	if err = barrier.finish(opts, nil, err, func() error { return nil }); !errors.Is(err, context.Canceled) {
		t.Fatalf("checked cleanup erased cancellation: %v", err)
	}
	if readEchoBarrierReceipt(t, opts.resultFile)["status"] != "error" {
		t.Fatal("canceled operation produced terminal success")
	}
}

func TestEchoCompletionBarrierRejectsStaleStopAndUnboundedWait(t *testing.T) {
	opts := echoBarrierOptions(t)
	result := map[string]any{"status": "ok", "echo_ok": true}
	var barrier echoCompletionBarrier
	if err := barrier.wait(context.Background(), opts, result); err == nil {
		t.Fatal("unbounded wait accepted")
	}
	if err := os.WriteFile(opts.stopFile, []byte("stale"), 0o600); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if err := barrier.wait(ctx, opts, result); err == nil || barrier.stopObserved {
		t.Fatal("stop predating the exchange witness accepted")
	}
	if _, err := os.Stat(opts.readyFile); !errors.Is(err, os.ErrNotExist) {
		t.Fatal("stale stop produced an exchange witness")
	}
}
