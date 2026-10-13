package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"time"
)

// This is opt-in harness synchronization, not a go-libp2p protocol change.
// Default echo dialers still disconnect immediately. The configured-plugin
// QUIC/TCP-TLS lanes retain the actual echo stream and host until Forge has independently
// closed its echo stream; neither endpoint ignores cancellation or invents FIN
// completion. Go QUIC stream.Close calls CancelRead, so retaining only the host
// would still race Forge's response FIN.
type echoCompletionBarrier struct {
	stopObserved bool
	stream       io.Closer
}

type echoCompletionKey struct{}

func (barrier *echoCompletionBarrier) bind(ctx context.Context, opts options) context.Context {
	if opts.echoBarrier == "" {
		return ctx
	}
	return context.WithValue(ctx, echoCompletionKey{}, barrier)
}

func retainEchoCompletionStream(ctx context.Context, stream io.Closer) bool {
	barrier, _ := ctx.Value(echoCompletionKey{}).(*echoCompletionBarrier)
	if barrier == nil || barrier.stream != nil {
		return false
	}
	barrier.stream = stream
	return true
}

func validateEchoCompletionBarrier(opts options) error {
	if opts.echoBarrier == "" {
		return nil
	}
	if opts.echoBarrier != "stop-file" || opts.command != "dial" ||
		(opts.transport != "quic" && opts.transport != "tcp-tls") || opts.scenario != "echo_large" ||
		opts.pnetKeyFile != "" || opts.pnetControl != "" {
		return fmt.Errorf("echo completion barrier requires public QUIC or TCP-TLS echo_large dial")
	}
	paths := make(map[string]bool)
	for _, path := range []string{opts.readyFile, opts.resultFile, opts.stopFile} {
		if path == "" {
			return fmt.Errorf("echo completion barrier requires separate witness, result and stop files")
		}
		absolute, err := filepath.Abs(path)
		if err != nil {
			return err
		}
		if paths[absolute] {
			return fmt.Errorf("echo completion barrier files must be distinct")
		}
		paths[absolute] = true
	}
	for _, path := range []string{opts.readyFile, opts.resultFile} {
		temporary, err := filepath.Abs(path + ".tmp")
		if err != nil {
			return err
		}
		if paths[temporary] {
			return fmt.Errorf("echo completion temporary output collides with an owned file")
		}
	}
	return nil
}

func echoCompletionReceipt(result map[string]any, status string, final, stopped, streamClosed, hostClosed bool) map[string]any {
	copy := make(map[string]any, len(result)+3)
	for key, value := range result {
		copy[key] = value
	}
	phase := "await_stop"
	if final {
		phase = "host_closed"
		if status != "ok" {
			phase = "failed"
		}
	}
	copy["status"], copy["final"] = status, final
	copy["echo_completion"] = map[string]any{
		"source": "go-fixture.echo-completion-barrier.v1", "mode": "stop-file", "phase": phase,
		"stop_observed": stopped, "stream_close_completed": streamClosed, "host_close_completed": hostClosed,
	}
	return copy
}

func writeEchoCompletionReceipt(path string, receipt map[string]any) error {
	temporary := path + ".tmp"
	if err := writeJSON(temporary, receipt); err != nil {
		return err
	}
	return os.Rename(temporary, path)
}

func (barrier *echoCompletionBarrier) wait(ctx context.Context, opts options, result map[string]any) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	if _, bounded := ctx.Deadline(); !bounded {
		return fmt.Errorf("echo completion barrier requires the owned dial deadline")
	}
	if _, err := os.Lstat(opts.stopFile); !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("echo stop file must be absent before the exchange witness")
	}
	if result["status"] != "ok" || result["echo_ok"] != true || barrier.stream == nil {
		return fmt.Errorf("echo completion barrier has no successful actual exchange")
	}
	if err := writeEchoCompletionReceipt(opts.readyFile,
		echoCompletionReceipt(result, "exchange_observed", false, false, false, false)); err != nil {
		return err
	}
	// Poll the existing StopFile predicate, not an elapsed-time success condition.
	ticker := time.NewTicker(10 * time.Millisecond)
	defer ticker.Stop()
	for {
		if err := ctx.Err(); err != nil {
			return err
		}
		info, err := os.Lstat(opts.stopFile)
		if err == nil {
			if !info.Mode().IsRegular() {
				return fmt.Errorf("echo stop file is not a regular file")
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

func (barrier *echoCompletionBarrier) finish(opts options, result map[string]any,
	operationErr error, closeHost func() error) error {
	var streamErr error
	streamClosed := false
	if barrier.stream != nil {
		streamErr = barrier.stream.Close()
		streamClosed = streamErr == nil
		barrier.stream = nil
	}
	closeErr := closeHost()
	err := errors.Join(operationErr, streamErr, closeErr)
	if !barrier.stopObserved && err == nil {
		err = fmt.Errorf("echo completion barrier ended without an observed stop")
	}
	status := "ok"
	if err != nil {
		status = "error"
	}
	if !streamClosed && err == nil {
		err = fmt.Errorf("echo completion barrier ended without checked stream close")
		status = "error"
	}
	receipt := echoCompletionReceipt(result, status, true, barrier.stopObserved, streamClosed, closeErr == nil)
	if err != nil {
		receipt["error"] = err.Error()
	}
	return errors.Join(err, writeEchoCompletionReceipt(opts.resultFile, receipt))
}
