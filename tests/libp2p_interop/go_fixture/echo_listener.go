package main

import (
	"bufio"
	"crypto/sha256"
	"errors"
	"fmt"
	"io"
	"path/filepath"
	"sync/atomic"
	"time"

	"github.com/libp2p/go-libp2p/core/host"
	"github.com/libp2p/go-libp2p/core/network"
)

// The opt-in fixture waits for the real request FIN before Go QUIC Close
// cancels its read side. No donor reset is translated into successful EOF.
type echoListenerCompletion struct {
	started atomic.Bool
	done    chan map[string]any
	result  map[string]any
}

func validateEchoListenerCompletion(opts options) error {
	if opts.echoListener == "" {
		if opts.echoWitness != "" {
			return fmt.Errorf("echo witness requires the orderly listener")
		}
		return nil
	}
	if opts.echoListener != "orderly-fin" || opts.command != "listen" ||
		opts.transport != "quic" || opts.scenario != "echo_large" || opts.echoBarrier != "" ||
		opts.pnetKeyFile != "" || opts.pnetControl != "" {
		return fmt.Errorf("orderly echo listener requires public QUIC echo_large listen")
	}
	// Reuse the existing distinct owned-file validation, not the dial behavior.
	files := opts
	files.command, files.echoBarrier = "dial", "stop-file"
	if err := validateEchoCompletionBarrier(files); err != nil {
		return err
	}
	if opts.echoWitness == "" {
		return fmt.Errorf("orderly listener requires a separate echo witness file")
	}
	witness, err := filepath.Abs(opts.echoWitness)
	if err != nil {
		return err
	}
	for _, path := range []string{opts.readyFile, opts.resultFile, opts.stopFile} {
		absolute, err := filepath.Abs(path)
		if err != nil {
			return err
		}
		if witness == absolute || witness+".tmp" == absolute || witness == absolute+".tmp" {
			return fmt.Errorf("echo witness collides with another owned file")
		}
	}
	return nil
}

func installEchoListenerCompletion(h host.Host) *echoListenerCompletion {
	state := &echoListenerCompletion{done: make(chan map[string]any, 1)}
	h.SetStreamHandler(echoProtocol, func(s network.Stream) {
		if !state.started.CompareAndSwap(false, true) {
			_ = s.Reset()
			return
		}
		result := map[string]any{
			"implementation": "go", "role": "listener", "scenario": "echo_large",
			"status": "ok", "final": false, "protocol": string(s.Protocol()),
			"local_peer_id":                s.Conn().LocalPeer().String(),
			"authenticated_remote_peer_id": s.Conn().RemotePeer().String(),
			"connection_local_addr":        s.Conn().LocalMultiaddr().String(),
			"connection_remote_addr":       s.Conn().RemoteMultiaddr().String(),
			"application_connection_id":    s.Conn().ID(), "application_stream_id": s.ID(),
		}
		completion := map[string]any{
			"source": "go-fixture.echo-listener-completion.v1", "mode": "orderly-fin",
			"request_eof_observed": false, "stream_close_completed": false, "host_close_completed": false,
		}
		result["echo_listener_completion"] = completion
		err := s.SetDeadline(time.Now().Add(20 * time.Second))
		var payload []byte
		reader := bufio.NewReader(s)
		if err == nil {
			payload, err = readFrame(reader)
		}
		if err == nil {
			err = writeFrame(s, payload)
		}
		if err == nil {
			err = requireEchoRequestEOF(reader)
			completion["request_eof_observed"] = err == nil
		}
		if err == nil {
			err = s.Close()
			completion["stream_close_completed"] = err == nil
		}
		if err != nil {
			err = errors.Join(err, s.Reset())
			result["status"], result["error"] = "error", err.Error()
		} else {
			result["payload_bytes"] = len(payload)
			result["payload_sha256"] = fmt.Sprintf("%x", sha256.Sum256(payload))
		}
		state.done <- result
	})
	return state
}

func requireEchoRequestEOF(reader *bufio.Reader) error {
	_, err := reader.ReadByte()
	if err == io.EOF {
		return nil
	}
	if err == nil {
		return fmt.Errorf("unexpected bytes after the framed echo request")
	}
	return fmt.Errorf("echo request did not finish with EOF: %w", err)
}

func (state *echoListenerCompletion) publish(opts options) error {
	if state.result != nil {
		return nil
	}
	select {
	case state.result = <-state.done:
		if err := writeEchoCompletionReceipt(opts.echoWitness, state.result); err != nil {
			return err
		}
		if state.result["status"] != "ok" {
			return fmt.Errorf("orderly echo listener failed: %v", state.result["error"])
		}
	default:
	}
	return nil
}

func (state *echoListenerCompletion) finish(opts options, operationErr error, closeHost func() error) error {
	closeErr := closeHost()
	if state.result == nil && state.started.Load() {
		timer := time.NewTimer(5 * time.Second)
		defer timer.Stop()
		select {
		case state.result = <-state.done:
		case <-timer.C:
			operationErr = errors.Join(operationErr, fmt.Errorf("orderly echo handler did not join"))
		}
	}
	if state.result == nil {
		state.result = map[string]any{"implementation": "go", "role": "listener", "scenario": "echo_large"}
		operationErr = errors.Join(operationErr, fmt.Errorf("missing orderly echo completion"))
	} else if state.result["status"] != "ok" {
		operationErr = errors.Join(operationErr, fmt.Errorf("echo exchange failed: %v", state.result["error"]))
	}
	err := errors.Join(operationErr, closeErr)
	state.result["final"] = true
	if completion, ok := state.result["echo_listener_completion"].(map[string]any); ok {
		completion["host_close_completed"] = closeErr == nil
	}
	if err != nil {
		state.result["status"], state.result["error"] = "error", err.Error()
	}
	return errors.Join(err, writeEchoCompletionReceipt(opts.resultFile, state.result))
}
