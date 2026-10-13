package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"time"
)

// Fixture lifetime only: native Kademlia has already completed its sole operation.
type dhtCompletionBarrier struct {
	stopObserved bool
	published    bool
}

func validateDHTCompletionBarrier(opts options) error {
	if opts.dhtBarrier == "" {
		return nil
	}
	if opts.dhtBarrier != "stop-file" || opts.command != "dial" || opts.transport != "quic" ||
		!isDHTValueScenario(opts.scenario) || (opts.payload != "put_only" && opts.payload != "get_only") ||
		opts.echoBarrier != "" || opts.echoListener != "" || opts.pnetKeyFile != "" || opts.pnetControl != "" ||
		opts.relayAddr != "" || opts.relayPeerID != "" {
		return fmt.Errorf("DHT completion requires isolated public QUIC put_only/get_only dial")
	}
	paths := make(map[string]bool)
	for _, path := range []string{opts.readyFile, opts.resultFile, opts.stopFile,
		opts.readyFile + ".tmp", opts.resultFile + ".tmp"} {
		if path == "" {
			return fmt.Errorf("DHT completion requires witness, result and stop paths")
		}
		parent, err := filepath.EvalSymlinks(filepath.Dir(path))
		if err != nil {
			return err
		}
		absolute, err := filepath.Abs(filepath.Join(parent, filepath.Base(path)))
		if err != nil {
			return err
		}
		if paths[absolute] {
			return fmt.Errorf("DHT completion paths must be distinct")
		}
		paths[absolute] = true
		if _, err := os.Lstat(path); !errors.Is(err, os.ErrNotExist) {
			return fmt.Errorf("DHT completion paths must be fresh")
		}
	}
	return nil
}

func dhtCompletionReceipt(result map[string]any, final, stopped, joined bool, operationErr error) map[string]any {
	copy := make(map[string]any, len(result)+3)
	for key, value := range result {
		copy[key] = value
	}
	status, phase := "operation_observed", "await_stop"
	if final {
		status, phase = "ok", "joined"
	}
	if operationErr != nil {
		status, phase = "error", "failed"
		copy["error"] = operationErr.Error()
	}
	copy["status"], copy["final"] = status, final
	copy["dht_completion"] = map[string]any{
		"source": "go-fixture.dht-completion-barrier.v1", "mode": "stop-file", "phase": phase,
		"stop_observed": stopped, "owner_joined": joined,
	}
	return copy
}

func (barrier *dhtCompletionBarrier) publish(opts options, result map[string]any) error {
	id, bound := result["connection_id"].(string)
	local, localBound := result["local_peer_id"].(string)
	if barrier.published || result["status"] != "ok" || result["operation"] != opts.payload || !bound || id == "" ||
		!localBound || local == "" || local == opts.peerID || result["authenticated_remote_peer_id"] != opts.peerID {
		return fmt.Errorf("DHT completion has no successful bound isolated operation")
	}
	for _, path := range []string{opts.readyFile, opts.stopFile} {
		if _, err := os.Lstat(path); !errors.Is(err, os.ErrNotExist) {
			return fmt.Errorf("DHT witness/stop must be absent before operation publication")
		}
	}
	if err := writeEchoCompletionReceipt(opts.readyFile, dhtCompletionReceipt(result, false, false, false, nil)); err != nil {
		return err
	}
	barrier.published = true
	return nil
}

func (barrier *dhtCompletionBarrier) waitStop(ctx context.Context, opts options, live func() bool) error {
	if _, bounded := ctx.Deadline(); !bounded {
		return fmt.Errorf("DHT completion requires the existing dial deadline")
	}
	if !barrier.published {
		return fmt.Errorf("DHT stop lacks an operation witness")
	}
	ticker := time.NewTicker(10 * time.Millisecond)
	defer ticker.Stop()
	for {
		if err := ctx.Err(); err != nil {
			return err
		}
		if !live() {
			return fmt.Errorf("DHT authenticated connection closed before owned stop")
		}
		info, err := os.Lstat(opts.stopFile)
		if err == nil {
			if !info.Mode().IsRegular() {
				return fmt.Errorf("DHT stop must be a regular file")
			}
			if err := ctx.Err(); err != nil {
				return err
			}
			barrier.stopObserved = true
			return nil
		}
		if !errors.Is(err, os.ErrNotExist) {
			return err
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-ticker.C:
		}
	}
}

func (barrier *dhtCompletionBarrier) wait(ctx context.Context, opts options, result map[string]any, live func() bool) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	if !live() {
		return fmt.Errorf("DHT operation lost its actual connection before publication")
	}
	if err := barrier.publish(opts, result); err != nil {
		return err
	}
	return barrier.waitStop(ctx, opts, live)
}

func (barrier *dhtCompletionBarrier) finish(opts options, result map[string]any, operationErr error, closeHost func() error) error {
	closeErr := closeHost()
	err := errors.Join(operationErr, closeErr)
	if err == nil && (!barrier.stopObserved || result["status"] != "ok") {
		err = fmt.Errorf("DHT completion ended without successful operation and owned stop")
	}
	return errors.Join(err, writeEchoCompletionReceipt(opts.resultFile,
		dhtCompletionReceipt(result, true, barrier.stopObserved, closeErr == nil, err)))
}
