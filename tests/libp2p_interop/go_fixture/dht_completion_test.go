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

func dhtCompletionOptions(t *testing.T) options {
	t.Helper()
	root := t.TempDir()
	return options{command: "dial", scenario: "dht_pk_put_get", transport: "quic", payload: "put_only",
		peerID: "server-peer", dhtBarrier: "stop-file", readyFile: filepath.Join(root, "ready.json"),
		resultFile: filepath.Join(root, "result.json"), stopFile: filepath.Join(root, "stop")}
}

func dhtOperation(opts options) map[string]any {
	return map[string]any{"implementation": "go", "role": "dialer", "scenario": opts.scenario,
		"status": "ok", "operation": opts.payload, "local_peer_id": "actor-peer",
		"authenticated_remote_peer_id": opts.peerID, "connection_id": "native-connection", "value_bytes": 36}
}

func dhtRead(t *testing.T, path string) map[string]any {
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

func TestDHTCompletionOptInAndIsolatedModes(t *testing.T) {
	if err := validateDHTCompletionBarrier(options{}); err != nil {
		t.Fatal(err)
	}
	for _, mutate := range []func(*options){
		func(o *options) { o.command = "listen" }, func(o *options) { o.payload = "put_get" },
		func(o *options) { o.transport = "tcp" }, func(o *options) { o.scenario = "echo_large" },
		func(o *options) { o.readyFile = o.resultFile }, func(o *options) { o.stopFile = o.readyFile + ".tmp" },
		func(o *options) { o.pnetKeyFile = "key" }, func(o *options) { o.readyFile = "" },
	} {
		opts := dhtCompletionOptions(t)
		mutate(&opts)
		if validateDHTCompletionBarrier(opts) == nil {
			t.Fatal("invalid opt-in accepted")
		}
	}
	for _, mode := range []string{"put_only", "get_only"} {
		opts := dhtCompletionOptions(t)
		opts.payload = mode
		if err := validateDHTCompletionBarrier(opts); err != nil {
			t.Fatal(err)
		}
	}
	previous := os.Args
	defer func() { os.Args = previous }()
	os.Args = []string{"fixture", "dial", "--dht-completion-barrier", "stop-file", "--dht-completion-barrier", "stop-file"}
	if _, err := parseArgs(); err == nil {
		t.Fatal("duplicate barrier accepted")
	}
}

func TestDHTCompletionWitnessThenStopThenCheckedHostClose(t *testing.T) {
	opts := dhtCompletionOptions(t)
	result := dhtOperation(opts)
	barrier := dhtCompletionBarrier{}
	if err := barrier.publish(opts, result); err != nil {
		t.Fatal(err)
	}
	witness, _ := os.ReadFile(opts.readyFile)
	if dhtRead(t, opts.readyFile)["status"] != "operation_observed" {
		t.Fatal("witness claimed final success")
	}
	if _, err := os.Stat(opts.resultFile); !errors.Is(err, os.ErrNotExist) {
		t.Fatal("terminal receipt exists before stop")
	}
	if err := os.WriteFile(opts.stopFile, nil, 0o644); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if err := barrier.waitStop(ctx, opts, func() bool { return true }); err != nil {
		t.Fatal(err)
	}
	closed := 0
	if err := barrier.finish(opts, result, nil, func() error { closed++; return nil }); err != nil {
		t.Fatal(err)
	}
	final := dhtRead(t, opts.resultFile)
	if closed != 1 || final["status"] != "ok" || final["final"] != true ||
		final["dht_completion"].(map[string]any)["owner_joined"] != true {
		t.Fatal("missing joined final result")
	}
	unchanged, _ := os.ReadFile(opts.readyFile)
	if string(witness) != string(unchanged) {
		t.Fatal("immutable witness changed during cleanup")
	}
}

func TestDHTCompletionHoldErrorsRemainTerminalFailures(t *testing.T) {
	for _, mode := range []string{"timeout", "connection-loss", "invalid-stop", "close-failure"} {
		t.Run(mode, func(t *testing.T) {
			opts := dhtCompletionOptions(t)
			barrier := dhtCompletionBarrier{}
			result := dhtOperation(opts)
			if err := barrier.publish(opts, result); err != nil {
				t.Fatal(err)
			}
			ctx, cancel := context.WithTimeout(context.Background(), time.Second)
			defer cancel()
			if mode == "timeout" {
				cancel()
				ctx, cancel = context.WithDeadline(context.Background(), time.Now().Add(-time.Second))
				defer cancel()
			}
			if mode == "invalid-stop" {
				if err := os.Mkdir(opts.stopFile, 0o755); err != nil {
					t.Fatal(err)
				}
			}
			if mode == "close-failure" {
				if err := os.WriteFile(opts.stopFile, nil, 0o644); err != nil {
					t.Fatal(err)
				}
			}
			err := barrier.waitStop(ctx, opts, func() bool { return mode != "connection-loss" })
			if mode != "close-failure" && err == nil {
				t.Fatal("hold failure accepted")
			}
			closeErr := errors.New("native host close failed")
			closed := 0
			terminal := barrier.finish(opts, result, err, func() error {
				closed++
				if mode == "close-failure" {
					return closeErr
				}
				return nil
			})
			if terminal == nil || closed != 1 || dhtRead(t, opts.resultFile)["status"] != "error" {
				t.Fatal("interim success hid terminal failure")
			}
			if err != nil && !errors.Is(terminal, err) {
				t.Fatal("original hold cause lost")
			}
		})
	}
}

func TestDHTCompletionRejectsStaleStopWrongActorAndUnboundWitness(t *testing.T) {
	for _, mode := range []string{"stale-stop", "wrong-actor", "wrong-peer", "missing-id"} {
		opts := dhtCompletionOptions(t)
		result := dhtOperation(opts)
		switch mode {
		case "stale-stop":
			if err := os.WriteFile(opts.stopFile, nil, 0o644); err != nil {
				t.Fatal(err)
			}
		case "wrong-actor":
			result["operation"] = "get_only"
		case "wrong-peer":
			result["authenticated_remote_peer_id"] = "foreign-peer"
		case "missing-id":
			delete(result, "connection_id")
		}
		barrier := dhtCompletionBarrier{}
		if barrier.publish(opts, result) == nil {
			t.Fatal("invalid witness accepted: " + mode)
		}
	}
}
