package main

import (
	"bufio"
	"errors"
	"path/filepath"
	"strings"
	"testing"
)

type echoErrorReader struct{ err error }

func (reader echoErrorReader) Read([]byte) (int, error) { return 0, reader.err }

func TestEchoListenerRequiresActualEOF(t *testing.T) {
	if err := requireEchoRequestEOF(bufio.NewReader(strings.NewReader(""))); err != nil {
		t.Fatal(err)
	}
	if err := requireEchoRequestEOF(bufio.NewReader(strings.NewReader("trailer"))); err == nil {
		t.Fatal("accepted bytes after the request")
	}
	for _, cause := range []error{errors.New("reset"), errors.New("timeout"), errors.New("connection closed")} {
		if err := requireEchoRequestEOF(bufio.NewReader(echoErrorReader{cause})); !errors.Is(err, cause) {
			t.Fatalf("non-EOF failure hidden: %v", err)
		}
	}
}

func TestEchoListenerStrictOptIn(t *testing.T) {
	opts := echoBarrierOptions(t)
	opts.command, opts.echoBarrier, opts.echoListener = "listen", "", "orderly-fin"
	opts.echoWitness = filepath.Join(t.TempDir(), "echo.json")
	if err := validateEchoListenerCompletion(opts); err != nil {
		t.Fatal(err)
	}
	if err := validateEchoListenerCompletion(options{}); err != nil {
		t.Fatal(err)
	}
	for _, mutate := range []func(*options){
		func(o *options) { o.command = "dial" }, func(o *options) { o.transport = "tcp" },
		func(o *options) { o.transport = "tcp-tls" }, func(o *options) { o.pnetKeyFile = "key" },
		func(o *options) { o.echoWitness = "" }, func(o *options) { o.echoWitness = o.readyFile },
		func(o *options) { o.echoWitness = o.resultFile + ".tmp" },
		func(o *options) { o.stopFile = o.echoWitness + ".tmp" },
		func(o *options) { o.echoListener = "" }, func(o *options) { o.scenario = "echo" },
	} {
		bad := opts
		mutate(&bad)
		if err := validateEchoListenerCompletion(bad); err == nil {
			t.Fatalf("accepted invalid listener options: %+v", bad)
		}
	}
}

func TestEchoListenerMissingCompletionAndCloseErrorsStayFailures(t *testing.T) {
	for _, cause := range []error{nil, errors.New("host close failed")} {
		opts := echoBarrierOptions(t)
		state := echoListenerCompletion{}
		err := state.finish(opts, nil, func() error { return cause })
		if err == nil || (cause != nil && !errors.Is(err, cause)) {
			t.Fatalf("missing completion/host error hidden: %v", err)
		}
		final := readEchoBarrierReceipt(t, opts.resultFile)
		if final["final"] != true || final["status"] != "error" {
			t.Fatalf("missing handler produced success: %+v", final)
		}
	}
}
