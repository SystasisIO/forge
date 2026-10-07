package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"testing"
	"time"

	pubsubpb "github.com/libp2p/go-libp2p-pubsub/pb"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/core/transport"
	ma "github.com/multiformats/go-multiaddr"
	quic "github.com/quic-go/quic-go"
)

// Unit-only public-interface models. No model output is crypto/network proof,
// and these constructors are never used by pubsub-live.
type pubsubQUICUnitStream struct {
	ctx        context.Context
	cancel     context.CancelCauseFunc
	id         quic.StreamID
	input      *bytes.Reader
	output     bytes.Buffer
	readErr    error
	writeErr   error
	closeErr   error
	resetErr   error
	writeN     int
	closes     int
	resets     int
	half       int
	resetCode  network.StreamErrorCode
	deadline   time.Time
	readStart  chan struct{}
	readEnd    chan struct{}
	resetStart chan struct{}
	resetEnd   chan struct{}
}

func (s *pubsubQUICUnitStream) Context() context.Context { return s.ctx }
func (s *pubsubQUICUnitStream) StreamID() quic.StreamID  { return s.id }
func (s *pubsubQUICUnitStream) Read(p []byte) (int, error) {
	if s.readStart != nil {
		close(s.readStart)
		<-s.readEnd
	}
	if s.input == nil {
		return 0, s.readErr
	}
	n, err := s.input.Read(p)
	if s.readErr != nil {
		err = s.readErr
	}
	return n, err
}
func (s *pubsubQUICUnitStream) Write(p []byte) (int, error) {
	n := len(p)
	if s.writeN >= 0 && s.writeN < n {
		n = s.writeN
	}
	_, _ = s.output.Write(p[:n])
	return n, s.writeErr
}
func (s *pubsubQUICUnitStream) Close() error      { s.closes++; return s.closeErr }
func (s *pubsubQUICUnitStream) CloseRead() error  { s.half++; return s.closeErr }
func (s *pubsubQUICUnitStream) CloseWrite() error { s.half++; return s.closeErr }
func (s *pubsubQUICUnitStream) Reset() error {
	s.resets++
	if s.resetStart != nil {
		close(s.resetStart)
		<-s.resetEnd
	}
	if s.resetErr == nil {
		s.cancel(&quic.StreamError{StreamID: s.id})
	}
	return s.resetErr
}
func (s *pubsubQUICUnitStream) ResetWithError(code network.StreamErrorCode) error {
	s.resetCode = code
	return s.Reset()
}
func (s *pubsubQUICUnitStream) SetDeadline(value time.Time) error      { s.deadline = value; return nil }
func (s *pubsubQUICUnitStream) SetReadDeadline(value time.Time) error  { return s.SetDeadline(value) }
func (s *pubsubQUICUnitStream) SetWriteDeadline(value time.Time) error { return s.SetDeadline(value) }

type pubsubQUICUnitCapable struct {
	*pubsubScoringUnitConn
	native  *quic.Conn
	stream  network.MuxedStream
	err     error
	closed  int
	ctxSeen context.Context
	code    network.ConnErrorCode
}

func (c *pubsubQUICUnitCapable) Transport() transport.Transport { return nil }
func (c *pubsubQUICUnitCapable) Scope() network.ConnScope       { return nil }
func (c *pubsubQUICUnitCapable) IsClosed() bool                 { return c.closed > 0 }
func (c *pubsubQUICUnitCapable) As(target any) bool {
	if out, ok := target.(**quic.Conn); ok {
		*out = c.native
		return c.native != nil
	}
	return false
}
func (c *pubsubQUICUnitCapable) OpenStream(ctx context.Context) (network.MuxedStream, error) {
	c.ctxSeen = ctx
	return c.stream, c.err
}
func (c *pubsubQUICUnitCapable) AcceptStream() (network.MuxedStream, error) { return c.stream, c.err }
func (c *pubsubQUICUnitCapable) Close() error                               { c.closed++; return c.err }
func (c *pubsubQUICUnitCapable) CloseWithError(code network.ConnErrorCode) error {
	c.code = code
	return c.Close()
}

func pubsubQUICUnit(t *testing.T, direction network.Direction) (*pubsubScoringObserver, *pubsubQUICStream, *pubsubQUICUnitStream, context.CancelCauseFunc) {
	t.Helper()
	o := newPubsubScoringObserver("victim", strings.Repeat("a", 32))
	identity := pubsubScoringUnitConnection(t)
	o.local = identity.local
	q := newPubsubQUICObserver(o, &pathDialObserver{})
	o.quic = q
	connectionContext, cancelConnection := context.WithCancelCause(context.Background())
	sendContext, cancelSend := context.WithCancelCause(context.Background())
	t.Cleanup(func() { cancelSend(context.Canceled); cancelConnection(context.Canceled) })
	raw := &pubsubQUICUnitStream{ctx: sendContext, cancel: cancelSend, id: 0, writeN: -1}
	nativeIdentity := new(quic.Conn) // Identity token only; no private native state is changed.
	capable := &pubsubQUICUnitCapable{pubsubScoringUnitConn: identity, native: nativeIdentity, stream: raw}
	c := &pubsubQUICConn{CapableConn: capable, owner: q, native: nativeIdentity,
		secured: pathQUICSecured{id: "unit-native-connection"}, connectionContext: connectionContext}
	c.receipt = pubsubQUICReceipt(o, "unit_model_connection", "unit_only.not_crypto_evidence", c.fields())
	q.connections[nativeIdentity] = c
	s, ok := c.stream(raw, direction, 0).(*pubsubQUICStream)
	if !ok {
		t.Fatal("unit stream not observed")
	}
	return o, s, raw, cancelConnection
}

func pubsubQUICUnitToken(token string) []byte {
	var header [binary.MaxVarintLen64]byte
	n := binary.PutUvarint(header[:], uint64(len(token)))
	return append(append([]byte{}, header[:n]...), token...)
}

func pubsubQUICUnitRead(t *testing.T, s *pubsubQUICStream, raw *pubsubQUICUnitStream, input []byte) {
	t.Helper()
	raw.input = bytes.NewReader(input)
	got := make([]byte, len(input))
	if n, err := s.Read(got); n != len(input) || err != nil || !bytes.Equal(got, input) {
		t.Fatal("lower Read changed successful bytes/results", n, err)
	}
}

func pubsubQUICUnitSelect(t *testing.T, s *pubsubQUICStream, raw *pubsubQUICUnitStream, protocol string) {
	t.Helper()
	selection := append(pubsubQUICUnitToken("/multistream/1.0.0\n"), pubsubQUICUnitToken(protocol+"\n")...)
	if s.direction == network.DirInbound {
		pubsubQUICUnitRead(t, s, raw, selection)
		if _, err := s.Write(selection); err != nil {
			t.Fatal(err)
		}
	} else {
		if _, err := s.Write(selection); err != nil {
			t.Fatal(err)
		}
		pubsubQUICUnitRead(t, s, raw, selection)
	}
}

func pubsubQUICUnitPrepare(t *testing.T, o *pubsubScoringObserver) {
	t.Helper()
	if err := o.prepareShutdown(pubsubScoringCommand{Sequence: 1, Kind: "prepare_shutdown", Actor: o.actor,
		Token: o.token, Local: o.local.String()}, 0); err != nil {
		t.Fatal(err)
	}
}

func TestPubsubQUICLowerFragmentedLazySelectionAndExactRPC(t *testing.T) {
	for _, direction := range []network.Direction{network.DirInbound, network.DirOutbound} {
		t.Run(direction.String(), func(t *testing.T) {
			o, s, raw, _ := pubsubQUICUnit(t, direction)
			selection := append(pubsubQUICUnitToken("/multistream/1.0.0\n"), pubsubQUICUnitToken("/meshsub/1.1.0\n")...)
			rpc := pubsubScoringUnitFrame(t, nil)
			if direction == network.DirOutbound {
				if n, err := s.Write(append(append([]byte{}, selection...), rpc...)); n != len(selection)+len(rpc) || err != nil {
					t.Fatal(n, err)
				}
				for _, b := range selection {
					pubsubQUICUnitRead(t, s, raw, []byte{b})
				}
			} else {
				pubsubQUICUnitRead(t, s, raw, append(append([]byte{}, selection...), rpc...))
				for _, b := range selection {
					if _, err := s.Write([]byte{b}); err != nil {
						t.Fatal(err)
					}
				}
			}
			frames, protocols := pubsubScoringUnitEvents(o, "rpc"), pubsubScoringUnitEvents(o, "protocol")
			if o.failure != nil || len(frames) != 1 || len(protocols) != 1 {
				t.Fatal("missing native selection/RPC", o.failure, frames)
			}
			frame := frames[0]
			if _, invented := frame["stream_id"]; invented {
				t.Fatal("invented Swarm stream mapping")
			}
			if frame["native_stream_id"] != int64(0) || frame["native_connection_id"] != "unit-native-connection" ||
				frame["native_stream_receipt_sequence"] != s.receipt || frame["protocol"] != "/meshsub/1.1.0" ||
				frame["receipt"].(map[string]any)["framed_hex"] != hex.EncodeToString(rpc) {
				t.Fatal("RPC is not bound to the exact lower bytes/owner", frame)
			}
			if len(protocols[0]["negotiation_frame_sequences"].([]int)) != 4 {
				t.Fatal("selection inferred without four actual frames")
			}
		})
	}
}

func TestPubsubQUICLowerNAThenProposalAndNonPubsubBody(t *testing.T) {
	o, s, raw, _ := pubsubQUICUnit(t, network.DirOutbound)
	header := pubsubQUICUnitToken("/multistream/1.0.0\n")
	_, _ = s.Write(append(append([]byte{}, header...), pubsubQUICUnitToken("/not-supported/1.0.0\n")...))
	pubsubQUICUnitRead(t, s, raw, append(append([]byte{}, header...), pubsubQUICUnitToken("na\n")...))
	_, _ = s.Write(pubsubQUICUnitToken("/meshsub/1.0.0\n"))
	pubsubQUICUnitRead(t, s, raw, pubsubQUICUnitToken("/meshsub/1.0.0\n"))
	if o.failure != nil || s.selected != "/meshsub/1.0.0" || len(s.frames) != 6 {
		t.Fatal("valid NA continuation rejected", o.failure)
	}
	other, inbound, delegate, _ := pubsubQUICUnit(t, network.DirInbound)
	pubsubQUICUnitSelect(t, inbound, delegate, "/ipfs/id/1.0.0")
	pubsubQUICUnitRead(t, inbound, delegate, []byte{0xff, 0xff, 0xff})
	delegate.input = nil
	delegate.readErr = syscall.ECONNRESET
	if _, err := inbound.Read(make([]byte, 1)); err != syscall.ECONNRESET || other.failure != nil || len(pubsubScoringUnitEvents(other, "rpc")) != 0 {
		t.Fatal("non-PubSub native handler was reinterpreted/mutated", err, other.failure)
	}
}

func TestPubsubQUICLowerNegotiationMalformedAndMissingACK(t *testing.T) {
	for _, mode := range []string{"wrong_ack", "noncanonical", "missing_ack", "rejected_tail"} {
		t.Run(mode, func(t *testing.T) {
			o, s, raw, _ := pubsubQUICUnit(t, network.DirOutbound)
			header := pubsubQUICUnitToken("/multistream/1.0.0\n")
			proposal := append(append([]byte{}, header...), pubsubQUICUnitToken("/meshsub/1.1.0\n")...)
			if mode == "rejected_tail" {
				proposal = append(proposal, pubsubScoringUnitFrame(t, nil)...)
			}
			_, _ = s.Write(proposal)
			switch mode {
			case "wrong_ack":
				pubsubQUICUnitRead(t, s, raw, append(header, pubsubQUICUnitToken("/meshsub/1.0.0\n")...))
			case "noncanonical":
				pubsubQUICUnitRead(t, s, raw, []byte{0x93, 0x00})
			case "rejected_tail":
				pubsubQUICUnitRead(t, s, raw, append(header, pubsubQUICUnitToken("na\n")...))
			}
			_ = s.Reset()
			if !s.finalize() || o.failure == nil || len(pubsubScoringUnitEvents(o, "rpc")) != 0 {
				t.Fatal("unproven selection accepted", mode)
			}
		})
	}
}

func TestPubsubQUICPrepareCapturesActualNativeContexts(t *testing.T) {
	o, s, raw, cancelConnection := pubsubQUICUnit(t, network.DirInbound)
	pubsubQUICUnitSelect(t, s, raw, "/meshsub/1.1.0")
	streamCause := &quic.StreamError{StreamID: s.nativeID, Remote: true}
	raw.cancel(streamCause)
	pubsubQUICUnitPrepare(t, o)
	ack := pubsubScoringUnitEvents(o, "shutdown_prepared")[0]
	rows := ack["native_quic_context_snapshots"].([]map[string]any)
	if len(rows) != 1 || rows[0]["native_stream_id"] != int64(0) ||
		rows[0]["send_context"].(map[string]any)["error_code"] != uint64(0) ||
		rows[0]["send_context"].(map[string]any)["done"] != true ||
		rows[0]["connection_context"].(map[string]any)["done"] != false {
		t.Fatal("prepare snapshot fabricated live/normal contexts", rows)
	}
	send, connection, exists := o.quic.baselineFor(s)
	if !exists || send.cause != streamCause || connection.cause != nil {
		t.Fatal("snapshot not from this native owner")
	}
	connectionCause := &quic.ApplicationError{ErrorCode: 11, Remote: true}
	cancelConnection(connectionCause)
	s.connection.recordContext()
	if o.failure != connectionCause {
		t.Fatal("real nonzero connection cause was ignored after prepare")
	}
}

func TestPubsubQUICPrepareRejectsClosedConnectionAndUnclassifiedSendCause(t *testing.T) {
	for _, mode := range []string{"closed_connection_zero", "closed_connection_nonzero", "send_nonzero", "send_foreign", "send_wrapped"} {
		t.Run(mode, func(t *testing.T) {
			o, s, raw, cancelConnection := pubsubQUICUnit(t, network.DirInbound)
			pubsubQUICUnitSelect(t, s, raw, "/meshsub/1.1.0")
			var cause error
			switch mode {
			case "closed_connection_zero":
				cause = &quic.ApplicationError{}
				cancelConnection(cause)
			case "closed_connection_nonzero":
				cause = &quic.ApplicationError{ErrorCode: 5}
				cancelConnection(cause)
			case "send_nonzero":
				cause = &quic.StreamError{StreamID: s.nativeID, ErrorCode: 5}
				raw.cancel(cause)
			case "send_foreign":
				cause = &quic.StreamError{StreamID: 4}
				raw.cancel(cause)
			case "send_wrapped":
				cause = fmt.Errorf("unit wrapper: %w", &quic.StreamError{StreamID: s.nativeID})
				raw.cancel(cause)
			}
			command := pubsubScoringCommand{Sequence: 1, Kind: "prepare_shutdown", Actor: o.actor, Token: o.token, Local: o.local.String()}
			if err := o.prepareShutdown(command, 0); err != cause || o.failure != cause || o.prepared ||
				len(pubsubScoringUnitEvents(o, "shutdown_prepared")) != 0 || o.quic.prepareAck != 0 {
				t.Fatal("prepare invented an ACK over a native failure", err, o.failure)
			}
		})
	}
}

func TestPubsubQUICCloseBeforeResetUsesActualSendContextNotReadCause(t *testing.T) {
	o, s, raw, _ := pubsubQUICUnit(t, network.DirInbound)
	pubsubQUICUnitSelect(t, s, raw, "/meshsub/1.1.0")
	pubsubQUICUnitPrepare(t, o)
	cause := &quic.StreamError{StreamID: s.nativeID, Remote: true}
	raw.cancel(cause)
	opaque := errors.New("unit opaque native Close result; text is not classified")
	raw.closeErr = opaque
	if err := s.Close(); err != opaque || o.failure != nil {
		t.Fatal("lost original Close result", err, o.failure)
	}
	operation := pubsubScoringUnitEvents(o, "native_stream_operation")[0]
	if operation["outcome"] != "native_send_reset_close_pending" || operation["send_context"].(map[string]any)["remote"] != true {
		t.Fatal(operation)
	}
	if !s.finalize() || o.failure == nil {
		t.Fatal("context-only Close invented full owner disposal")
	}
	// A separate fresh model proves that a LATER successful Reset is disposal,
	// never authority for the preceding Close. No Read cause exists in this model.
	o, s, raw, _ = pubsubQUICUnit(t, network.DirInbound)
	pubsubQUICUnitSelect(t, s, raw, "/meshsub/1.1.0")
	pubsubQUICUnitPrepare(t, o)
	raw.cancel(&quic.StreamError{StreamID: s.nativeID, Remote: true})
	raw.closeErr = opaque
	_ = s.Close()
	_ = s.Reset()
	s.finalize()
	final := pubsubScoringUnitEvents(o, "native_quic_terminal_finalized")
	if o.failure != nil || len(final) != 1 || final[0]["accepted"] != true ||
		final[0]["owner_disposal_receipt_sequence"].(int) <= final[0]["operation_receipt_sequence"].(int) {
		t.Fatal("lower native send cause/disposal incorrectly bound", o.failure, final)
	}
}

func TestPubsubQUICRepeatClosePinsOwnedSuccessfulReset(t *testing.T) {
	for _, mode := range []string{"valid", "failed_reset", "foreign_stream", "nonzero", "wrapped", "joined_errno", "preprepare", "sticky", "residue"} {
		t.Run(mode, func(t *testing.T) {
			o, s, raw, _ := pubsubQUICUnit(t, network.DirInbound)
			pubsubQUICUnitSelect(t, s, raw, "/meshsub/1.1.0")
			if mode != "preprepare" {
				pubsubQUICUnitPrepare(t, o)
			}
			cause := &quic.StreamError{StreamID: s.nativeID}
			if mode == "foreign_stream" {
				cause.StreamID = 4
			}
			if mode == "nonzero" {
				cause.ErrorCode = 9
			}
			var contextCause error = cause
			if mode == "wrapped" {
				contextCause = fmt.Errorf("unit wrapper: %w", cause)
			}
			if mode == "joined_errno" {
				contextCause = errors.Join(cause, syscall.ECONNRESET)
			}
			raw.cancel(contextCause)
			if mode == "failed_reset" {
				raw.resetErr = syscall.EIO
			}
			_ = s.Reset()
			if mode == "sticky" {
				o.fail(syscall.EPIPE)
			}
			if mode == "residue" {
				pubsubQUICUnitRead(t, s, raw, []byte{0x80})
			}
			if mode == "preprepare" {
				pubsubQUICUnitPrepare(t, o)
			}
			opaque := errors.New("unit opaque Close")
			raw.closeErr = opaque
			if err := s.Close(); err != opaque {
				t.Fatal("Close error replaced")
			}
			s.finalize()
			final := pubsubScoringUnitEvents(o, "native_quic_terminal_finalized")
			valid := mode == "valid"
			if valid && (o.failure != nil || len(final) != 1 || final[0]["accepted"] != true) {
				t.Fatal(o.failure, final)
			}
			if !valid && o.failure == nil {
				t.Fatal("invalid context/reset authorized Close", mode, final)
			}
			if raw.resets != 1 || raw.closes != 1 {
				t.Fatal("observer inserted cleanup calls")
			}
		})
	}
}

func TestPubsubQUICConnectionContextRequiresSameActualTypedCause(t *testing.T) {
	for _, mode := range []string{"valid", "different_instance", "wrapped_context", "nonzero", "before_prepare", "half_close_only", "sticky"} {
		t.Run(mode, func(t *testing.T) {
			o, s, raw, cancelConnection := pubsubQUICUnit(t, network.DirInbound)
			pubsubQUICUnitSelect(t, s, raw, "/meshsub/1.1.0")
			cause := &quic.ApplicationError{Remote: true}
			if mode == "nonzero" {
				cause.ErrorCode = 5
			}
			if mode == "before_prepare" {
				cancelConnection(cause)
				command := pubsubScoringCommand{Sequence: 1, Kind: "prepare_shutdown", Actor: o.actor, Token: o.token, Local: o.local.String()}
				if o.prepareShutdown(command, 0) != cause || o.failure != cause {
					t.Fatal("active connection failure accepted")
				}
				return
			}
			pubsubQUICUnitPrepare(t, o)
			raw.input = nil
			raw.readErr = &network.ConnError{ErrorCode: network.ConnErrorCode(cause.ErrorCode), Remote: true, TransportError: cause}
			if _, err := s.Read(make([]byte, 1)); err != raw.readErr {
				t.Fatal("native connection I/O changed")
			}
			var observed error = cause
			if mode == "different_instance" {
				observed = &quic.ApplicationError{Remote: true}
			}
			if mode == "wrapped_context" {
				observed = fmt.Errorf("unit wrapper: %w", cause)
			}
			cancelConnection(observed)
			s.connection.recordContext()
			if mode == "half_close_only" {
				_ = s.CloseRead()
				_ = s.CloseWrite()
			} else {
				_ = s.Reset()
			}
			if mode == "sticky" {
				o.fail(syscall.EIO)
			}
			s.finalize()
			final := pubsubScoringUnitEvents(o, "native_quic_terminal_finalized")
			if mode == "valid" {
				if o.failure != nil || len(final) != 1 || final[0]["accepted"] != true || final[0]["same_native_connection_context_cause"] != true {
					t.Fatal(o.failure, final)
				}
			} else if o.failure == nil {
				t.Fatal("foreign/unproven connection cause accepted", mode, final)
			}
		})
	}
}

func TestPubsubQUICIORejectsWrappedForeignAndNonzeroErrorsUnchanged(t *testing.T) {
	for _, mode := range []string{"wrapped_outer", "wrapped_inner", "foreign_stream", "nonzero", "direction_mismatch", "joined_errno", "opaque_errno", "before_prepare"} {
		t.Run(mode, func(t *testing.T) {
			o, s, raw, _ := pubsubQUICUnit(t, network.DirInbound)
			pubsubQUICUnitSelect(t, s, raw, "/meshsub/1.1.0")
			if mode != "before_prepare" {
				pubsubQUICUnitPrepare(t, o)
			}
			inner := &quic.StreamError{StreamID: s.nativeID, Remote: true}
			outer := &network.StreamError{Remote: true, TransportError: inner}
			var failure error = outer
			switch mode {
			case "wrapped_outer":
				failure = fmt.Errorf("unit wrapped: %w", outer)
			case "wrapped_inner":
				outer.TransportError = fmt.Errorf("unit wrapped: %w", inner)
			case "foreign_stream":
				inner.StreamID = 4
			case "nonzero":
				inner.ErrorCode = 3
				outer.ErrorCode = 3
			case "direction_mismatch":
				inner.Remote = false
			case "joined_errno":
				failure = errors.Join(outer, syscall.ECONNRESET)
			case "opaque_errno":
				failure = syscall.ECONNRESET
			}
			raw.input, raw.readErr = nil, failure
			if n, err := s.Read(make([]byte, 1)); n != 0 || err != failure || o.failure != failure {
				t.Fatal("native fatal result was normalized", n, err, o.failure)
			}
		})
	}
}

func TestPubsubQUICBlockedIOCannotFinalizeBeforeActualNativeReturn(t *testing.T) {
	o, s, raw, _ := pubsubQUICUnit(t, network.DirInbound)
	pubsubQUICUnitSelect(t, s, raw, "/meshsub/1.1.0")
	pubsubQUICUnitPrepare(t, o)
	raw.input = nil
	raw.readStart, raw.readEnd = make(chan struct{}), make(chan struct{})
	raw.readErr = &network.StreamError{TransportError: &quic.StreamError{StreamID: s.nativeID}}
	finished := make(chan error, 1)
	joined := make(chan struct{})
	var release sync.Once
	t.Cleanup(func() {
		release.Do(func() { close(raw.readEnd) })
		<-joined
	})
	go func() {
		defer close(joined)
		_, err := s.Read(make([]byte, 1))
		finished <- err
	}()
	<-raw.readStart
	_ = s.Reset()
	if s.finalize() || len(pubsubScoringUnitEvents(o, "native_quic_terminal_finalized")) != 0 {
		t.Fatal("finalized blocked native I/O")
	}
	release.Do(func() { close(raw.readEnd) })
	if err := <-finished; err != raw.readErr {
		t.Fatal("native blocked error changed")
	}
	s.finalize()
	final := pubsubScoringUnitEvents(o, "native_quic_terminal_finalized")
	if o.failure != nil || len(final) != 1 || final[0]["accepted"] != true {
		t.Fatal("actual returned I/O/reset did not bind", o.failure, final)
	}
}

type pubsubQUICUnitDelayedContext struct {
	context.Context
	first            atomic.Bool
	entered, release chan struct{}
}

func (c *pubsubQUICUnitDelayedContext) Err() error {
	if c.first.CompareAndSwap(false, true) {
		close(c.entered)
		<-c.release
	}
	return c.Context.Err()
}

func TestPubsubQUICNativeReturnCannotBorrowLaterResetDuringReceiptPublication(t *testing.T) {
	o, s, raw, _ := pubsubQUICUnit(t, network.DirInbound)
	pubsubQUICUnitSelect(t, s, raw, "/meshsub/1.1.0")
	pubsubQUICUnitPrepare(t, o)
	delayed := &pubsubQUICUnitDelayedContext{Context: raw.ctx, entered: make(chan struct{}), release: make(chan struct{})}
	s.sendContext = delayed
	raw.input = nil
	raw.readErr = &network.StreamError{TransportError: &quic.StreamError{StreamID: s.nativeID}}
	finished := make(chan error, 1)
	joined := make(chan struct{})
	var release sync.Once
	t.Cleanup(func() { release.Do(func() { close(delayed.release) }); <-joined })
	go func() {
		defer close(joined)
		_, err := s.Read(make([]byte, 1))
		finished <- err
	}()
	<-delayed.entered // Actual delegate Read has returned; its receipt is not published.
	if err := s.Reset(); err != nil {
		t.Fatal(err)
	}
	release.Do(func() { close(delayed.release) })
	if err := <-finished; err != raw.readErr {
		t.Fatal("delegate error changed")
	}
	s.finalize()
	terminal := pubsubScoringUnitEvents(o, "stream_io_terminal")
	reset := pubsubScoringUnitEvents(o, "native_stream_operation")
	if o.failure != raw.readErr || len(terminal) != 1 || len(reset) != 1 ||
		terminal[0]["returned_order"].(uint64) >= reset[0]["started_order"].(uint64) ||
		terminal[0]["outcome"] != "error" {
		t.Fatal("later Reset became an earlier native return's authority", o.failure, terminal, reset)
	}
}

func TestPubsubQUICNativeReturnKeepsQualifyingResetBeforeLaterReset(t *testing.T) {
	for _, side := range []string{"read", "write"} {
		t.Run(side, func(t *testing.T) {
			o, s, raw, _ := pubsubQUICUnit(t, network.DirInbound)
			pubsubQUICUnitSelect(t, s, raw, "/meshsub/1.1.0")
			pubsubQUICUnitPrepare(t, o)
			if err := s.Reset(); err != nil {
				t.Fatal(err)
			}
			firstReset := pubsubScoringUnitEvents(o, "native_stream_operation")[0]
			cause, typed := context.Cause(raw.ctx).(*quic.StreamError)
			if !typed {
				t.Fatal("unit Reset did not cancel its own send context")
			}
			failure := &network.StreamError{TransportError: cause}
			raw.input, raw.readErr, raw.writeErr, raw.writeN = nil, failure, failure, 0
			delayed := &pubsubQUICUnitDelayedContext{Context: raw.ctx, entered: make(chan struct{}), release: make(chan struct{})}
			s.sendContext = delayed
			finished, joined := make(chan error, 1), make(chan struct{})
			var release sync.Once
			t.Cleanup(func() { release.Do(func() { close(delayed.release) }); <-joined })
			go func() {
				defer close(joined)
				var err error
				if side == "read" {
					_, err = s.Read(make([]byte, 1))
				} else {
					_, err = s.Write([]byte{0xff})
				}
				finished <- err
			}()
			<-delayed.entered // RETURN was sealed with Reset1 before Reset2 exists.
			if err := s.Reset(); err != nil {
				t.Fatal(err)
			}
			release.Do(func() { close(delayed.release) })
			if err := <-finished; err != failure {
				t.Fatal("delegate error changed", err)
			}
			s.finalize()
			ioEvents := pubsubScoringUnitEvents(o, "stream_io_terminal")
			resets := pubsubScoringUnitEvents(o, "native_stream_operation")
			final := pubsubScoringUnitEvents(o, "native_quic_terminal_finalized")
			if o.failure != nil || len(ioEvents) != 1 || len(resets) != 2 || len(final) != 1 ||
				final[0]["accepted"] != true || final[0]["owned_terminal_receipt_sequence"] != firstReset["sequence"] ||
				firstReset["returned_order"].(uint64) >= ioEvents[0]["returned_order"].(uint64) ||
				ioEvents[0]["returned_order"].(uint64) >= resets[1]["started_order"].(uint64) {
				t.Fatal("Reset2 replaced the sealed qualifying Reset1", o.failure, ioEvents, resets, final)
			}
		})
	}
}

func TestPubsubQUICLocalReadFallsBackToSealedSuccessfulResetAfterOpaqueClose(t *testing.T) {
	o, s, raw, _ := pubsubQUICUnit(t, network.DirInbound)
	pubsubQUICUnitSelect(t, s, raw, "/meshsub/1.1.0")
	pubsubQUICUnitPrepare(t, o)
	if err := s.Reset(); err != nil {
		t.Fatal(err)
	}
	firstReset := pubsubScoringUnitEvents(o, "native_stream_operation")[0]
	opaque := errors.New("unit-only opaque Close; no text classification")
	raw.closeErr = opaque
	if err := s.Close(); err != opaque || o.failure != nil {
		t.Fatal("opaque Close lost its actual owned send context", err, o.failure)
	}
	// The read direction has its own native cause, not the send Context's object.
	failure := &network.StreamError{TransportError: &quic.StreamError{StreamID: s.nativeID}}
	raw.input, raw.readErr = nil, failure
	delayed := &pubsubQUICUnitDelayedContext{Context: raw.ctx, entered: make(chan struct{}), release: make(chan struct{})}
	s.sendContext = delayed
	finished, joined := make(chan error, 1), make(chan struct{})
	var release sync.Once
	t.Cleanup(func() { release.Do(func() { close(delayed.release) }); <-joined })
	go func() {
		defer close(joined)
		_, err := s.Read(make([]byte, 1))
		finished <- err
	}()
	<-delayed.entered // Reset1 and nonnil Close are sealed before the actual Read RETURN.
	if err := s.Reset(); err != nil {
		t.Fatal(err)
	}
	release.Do(func() { close(delayed.release) })
	if err := <-finished; err != failure {
		t.Fatal("read error changed", err)
	}
	s.finalize()
	terminals := pubsubScoringUnitEvents(o, "stream_io_terminal")
	operations := pubsubScoringUnitEvents(o, "native_stream_operation")
	final := pubsubScoringUnitEvents(o, "native_quic_terminal_finalized")
	if o.failure != nil || len(terminals) != 1 || len(operations) != 3 || len(final) != 2 ||
		terminals[0]["outcome"] != "owned_read_terminal_pending" || terminals[0]["same_send_context_cause"] != false ||
		terminals[0]["returned_order"].(uint64) >= operations[2]["started_order"].(uint64) {
		t.Fatal("successful sealed Reset fallback was lost", o.failure, terminals, operations, final)
	}
	for _, receipt := range final {
		if receipt["accepted"] != true || receipt["owned_terminal_receipt_sequence"] != firstReset["sequence"] {
			t.Fatal("later Reset or failed Close became earlier Read authority", receipt)
		}
	}
	if raw.resets != 2 || raw.closes != 1 {
		t.Fatal("observer inserted native cleanup calls")
	}
}

func TestPubsubQUICLocalReadResetFallbackCannotClearStickyFailures(t *testing.T) {
	for _, mode := range []string{"true_close_error", "earlier_io_error", "failed_reset", "missing_reset", "partial_rpc"} {
		t.Run(mode, func(t *testing.T) {
			o, s, raw, _ := pubsubQUICUnit(t, network.DirInbound)
			pubsubQUICUnitSelect(t, s, raw, "/meshsub/1.1.0")
			pubsubQUICUnitPrepare(t, o)
			if mode == "earlier_io_error" {
				raw.input, raw.readErr = nil, syscall.EPIPE
				if _, err := s.Read(make([]byte, 1)); err != syscall.EPIPE || o.failure != syscall.EPIPE {
					t.Fatal("true I/O failure lost", err, o.failure)
				}
			}
			if mode == "partial_rpc" {
				pubsubQUICUnitRead(t, s, raw, []byte{3, 0x0a})
			}
			if mode == "failed_reset" {
				raw.resetErr = syscall.EIO
			}
			if mode != "missing_reset" {
				if err := s.Reset(); err != raw.resetErr {
					t.Fatal(err)
				}
			} else {
				raw.cancel(&quic.StreamError{StreamID: s.nativeID, Remote: true})
			}
			if mode == "failed_reset" {
				raw.cancel(&quic.StreamError{StreamID: s.nativeID, Remote: true})
			}
			raw.closeErr = errors.New("unit opaque Close")
			if mode == "true_close_error" {
				raw.closeErr = syscall.ECONNRESET
			}
			if err := s.Close(); err != raw.closeErr {
				t.Fatal("Close result changed", err)
			}
			failure := &network.StreamError{TransportError: &quic.StreamError{StreamID: s.nativeID}}
			raw.input, raw.readErr = nil, failure
			if n, err := s.Read(make([]byte, 1)); n != 0 || err != failure {
				t.Fatal("native Read result changed", n, err)
			}
			// A later successful Reset is disposal only, never fallback authority.
			raw.resetErr = nil
			_ = s.Reset()
			s.finalize()
			if o.failure == nil {
				t.Fatal("Reset fallback hid an actual failure", mode)
			}
			if mode == "true_close_error" && o.failure != syscall.ECONNRESET ||
				mode == "earlier_io_error" && o.failure != syscall.EPIPE || mode == "failed_reset" && o.failure != syscall.EIO ||
				mode == "missing_reset" && o.failure != failure {
				t.Fatal("first native failure was replaced", mode, o.failure)
			}
			for _, receipt := range pubsubScoringUnitEvents(o, "native_quic_terminal_finalized") {
				if receipt["accepted"] != false {
					t.Fatal("failed stream accepted", mode, receipt)
				}
			}
		})
	}
}

func TestPubsubQUICResetFallbackRequiresSuccessBeforeSealedReadReturn(t *testing.T) {
	o, s, raw, _ := pubsubQUICUnit(t, network.DirInbound)
	pubsubQUICUnitSelect(t, s, raw, "/meshsub/1.1.0")
	pubsubQUICUnitPrepare(t, o)
	raw.cancel(&quic.StreamError{StreamID: s.nativeID, Remote: true})
	raw.resetStart, raw.resetEnd = make(chan struct{}), make(chan struct{})
	resetDone, resetJoined := make(chan error, 1), make(chan struct{})
	var releaseReset sync.Once
	t.Cleanup(func() { releaseReset.Do(func() { close(raw.resetEnd) }); <-resetJoined })
	go func() { defer close(resetJoined); resetDone <- s.Reset() }()
	<-raw.resetStart
	raw.closeErr = errors.New("unit opaque Close while Reset remains in flight")
	if err := s.Close(); err != raw.closeErr || o.failure != nil {
		t.Fatal(err, o.failure)
	}
	delayed := &pubsubQUICUnitDelayedContext{Context: raw.ctx, entered: make(chan struct{}), release: make(chan struct{})}
	s.sendContext = delayed
	failure := &network.StreamError{TransportError: &quic.StreamError{StreamID: s.nativeID}}
	raw.input, raw.readErr = nil, failure
	readDone, readJoined := make(chan error, 1), make(chan struct{})
	var releaseRead sync.Once
	t.Cleanup(func() { releaseRead.Do(func() { close(delayed.release) }); <-readJoined })
	go func() { defer close(readJoined); _, err := s.Read(make([]byte, 1)); readDone <- err }()
	<-delayed.entered // Read RETURN sealed while Reset has not succeeded.
	releaseReset.Do(func() { close(raw.resetEnd) })
	if err := <-resetDone; err != nil {
		t.Fatal(err)
	}
	releaseRead.Do(func() { close(delayed.release) })
	if err := <-readDone; err != failure || o.failure != failure {
		t.Fatal("eventually successful Reset authorized an earlier Read", err, o.failure)
	}
	s.finalize()
	terminal := pubsubScoringUnitEvents(o, "stream_io_terminal")
	if len(terminal) != 1 || terminal[0]["outcome"] != "error" {
		t.Fatal("in-flight Reset was treated as a sealed successful fallback", terminal)
	}
}

func TestPubsubQUICUnselectedReturnsCaptureExactPrefixOwnerAndCurrentContexts(t *testing.T) {
	for _, side := range []string{"read", "write"} {
		t.Run(side, func(t *testing.T) {
			direction := network.DirInbound
			if side == "write" {
				direction = network.DirOutbound
			}
			o, s, raw, cancelConnection := pubsubQUICUnit(t, direction)
			pubsubQUICUnitPrepare(t, o)
			ack := pubsubScoringUnitEvents(o, "shutdown_prepared")[0]["sequence"]
			cause := &quic.ApplicationError{Remote: true}
			cancelConnection(cause)
			failure := &network.ConnError{Remote: true, TransportError: cause}
			prefix := append(pubsubQUICUnitToken("/multistream/1.0.0\n"), pubsubQUICUnitToken("/meshsub/1.1.0\n")...)
			lazy := []byte{3, 0x0a} // Partial lazy RPC: it has not been selected/decoded.
			prefix = append(prefix, lazy...)
			var n int
			var err error
			if side == "read" {
				raw.input, raw.readErr = bytes.NewReader(prefix), failure
				buffer := make([]byte, len(prefix)+1)
				n, err = s.Read(buffer)
				if !bytes.Equal(buffer[:n], prefix) {
					t.Fatal("Read prefix mutated")
				}
			} else {
				raw.writeN, raw.writeErr = len(prefix), failure
				n, err = s.Write(append(append([]byte{}, prefix...), 0xff))
				if !bytes.Equal(raw.output.Bytes(), prefix) {
					t.Fatal("Write prefix mutated")
				}
			}
			returns := pubsubScoringUnitEvents(o, "native_quic_negotiation_io_return")
			if n != len(prefix) || err != failure || o.failure != failure || len(returns) != 1 || s.earlyError != failure {
				t.Fatal("unselected native result lost or authorized", n, err, o.failure, returns)
			}
			receipt := returns[0]
			snapshot := receipt["negotiation_snapshot"].(map[string]any)
			captured := snapshot["successful_prefix"].(map[string]any)
			tail := snapshot[side].(map[string]any)["lazy_tail"].(map[string]any)
			if receipt["source"] != "go.quic.native_stream.io_return" || receipt["native_stream_id"] != int64(raw.id) ||
				receipt["native_connection_id"] != s.connection.secured.id || receipt["remote_peer_id"] != s.connection.RemotePeer().String() ||
				receipt["connection_receipt_sequence"] != s.connection.receipt || receipt["native_stream_receipt_sequence"] != s.receipt ||
				receipt["protocol_at_native_return"] != "" || receipt["outcome"] != "error" || receipt["error_type"] != fmt.Sprintf("%T", failure) ||
				receipt["successful_prefix_bytes"] != n || receipt["terminal_prepare_ack_sequence"] != ack ||
				receipt["prepare_baseline_present"] != true || receipt["connection_context_at_prepare"].(map[string]any)["done"] != false ||
				receipt["connection_context"].(map[string]any)["done"] != true || receipt["same_native_connection_context_cause"] != true ||
				captured["hex"] != hex.EncodeToString(prefix) || tail["hex"] != hex.EncodeToString(lazy) ||
				tail["bytes"] != len(lazy) || snapshot["capture_complete"] != true {
				t.Fatal("receipt lost native owner/prefix/independent context", receipt)
			}
			if receipt["started_order"].(uint64) >= receipt["returned_order"].(uint64) {
				t.Fatal("native return order fabricated", receipt)
			}
			if _, mapped := receipt["stream_id"]; mapped {
				t.Fatal("unselected lower stream invented a Swarm mapping")
			}
			_ = s.Reset()
			s.finalize()
			framing := pubsubScoringUnitEvents(o, "native_quic_framing_finalized")
			if len(framing) != 1 || framing[0]["negotiation_complete"] != false || framing[0]["framing_clean"] != false ||
				len(pubsubScoringUnitEvents(o, "native_quic_terminal_finalized")) != 0 || len(pubsubScoringUnitEvents(o, "rpc")) != 0 {
				t.Fatal("partial lazy RPC became accepted protocol evidence", framing)
			}
		})
	}
}

func TestPubsubQUICUnselectedOperationsCaptureExistingDisposalWithoutCompletingNegotiation(t *testing.T) {
	for _, mode := range []string{"full_close", "reset", "half_close", "failed_close", "late_stream", "before_prepare"} {
		t.Run(mode, func(t *testing.T) {
			o, s, raw, _ := pubsubQUICUnit(t, network.DirOutbound)
			if mode != "before_prepare" {
				pubsubQUICUnitPrepare(t, o)
			}
			if mode == "late_stream" {
				ctx, cancel := context.WithCancelCause(context.Background())
				t.Cleanup(func() { cancel(context.Canceled) })
				raw = &pubsubQUICUnitStream{ctx: ctx, cancel: cancel, id: 4, writeN: -1}
				s = s.connection.stream(raw, network.DirOutbound, 0).(*pubsubQUICStream)
			}
			prefix := append(pubsubQUICUnitToken("/multistream/1.0.0\n"), pubsubQUICUnitToken("/meshsub/1.1.0\n")...)
			prefix = append(prefix, []byte{5, 0x0a, 1}...)
			if n, err := s.Write(prefix); n != len(prefix) || err != nil {
				t.Fatal(n, err)
			}
			disposed := mode != "half_close" && mode != "failed_close"
			var err error
			if mode == "reset" || mode == "late_stream" {
				err = s.Reset()
			} else if mode == "half_close" {
				err = s.CloseRead()
			} else {
				if mode == "failed_close" {
					raw.closeErr = syscall.ECONNRESET
				}
				err = s.Close()
			}
			if err != raw.closeErr && mode == "failed_close" || err != nil && mode != "failed_close" {
				t.Fatal("native operation result changed", err)
			}
			operations := pubsubScoringUnitEvents(o, "native_stream_operation")
			if len(operations) != 1 {
				t.Fatal("missing actual unselected operation", operations)
			}
			operation := operations[0]
			exists := mode != "late_stream" && mode != "before_prepare"
			if operation["native_stream_id"] != int64(raw.id) || operation["native_stream_receipt_sequence"] != s.receipt ||
				operation["connection_receipt_sequence"] != s.connection.receipt || operation["observation_phase"] != "unselected_at_native_return" ||
				operation["stream_prepare_baseline_present"] != exists || operation["connection_prepare_baseline_present"] != (mode != "before_prepare") ||
				operation["send_context_at_prepare"].(map[string]any)["done"] != false ||
				operation["started_order"].(uint64) >= operation["returned_order"].(uint64) {
				t.Fatal("native ownership/baseline was invented", operation)
			}
			if mode == "failed_close" && (o.failure != raw.closeErr || operation["outcome"] != "error") {
				t.Fatal("actual unselected Close failure excused", operation, o.failure)
			}
			s.finalize()
			framing := pubsubScoringUnitEvents(o, "native_quic_framing_finalized")
			if o.failure == nil || len(framing) != 1 || framing[0]["native_owner_disposed"] != disposed ||
				framing[0]["negotiation_complete"] != false || framing[0]["framing_clean"] != false ||
				len(pubsubScoringUnitEvents(o, "native_quic_terminal_finalized")) != 0 {
				t.Fatal("disposal was substituted for actual negotiation/framing", o.failure, framing)
			}
			if disposed && framing[0]["owner_disposal_receipt_sequence"] != operation["sequence"] {
				t.Fatal("disposal lacks the same owner's actual successful native receipt", framing)
			}
			if raw.closes+raw.resets+raw.half != 1 {
				t.Fatal("observer added a native Close/Reset/poll")
			}
		})
	}
}

func TestPubsubQUICNegotiationSnapshotsRetainFragmentedBytesAndEnforceCaptureBound(t *testing.T) {
	o, s, raw, _ := pubsubQUICUnit(t, network.DirInbound)
	header := pubsubQUICUnitToken("/multistream/1.0.0\n")
	pubsubQUICUnitRead(t, s, raw, header[:2])
	first := pubsubScoringUnitEvents(o, "native_quic_negotiation_io_return")[0]
	snapshot := first["negotiation_snapshot"].(map[string]any)
	fragment := snapshot["read"].(map[string]any)["partial_frame"].(map[string]any)
	pubsubQUICUnitRead(t, s, raw, header[2:])
	if fragment["hex"] != hex.EncodeToString(header[:2]) || fragment["bytes"] != 2 ||
		snapshot["selected_protocol"] != "" || snapshot["frame_sequences"].([]int) == nil {
		t.Fatal("later parser progress mutated the indexed fragment", first)
	}
	o.mu.Lock()
	o.wireBytes = pubsubScoringWireBytes // Unit-only bounded-capture boundary, not native wire proof.
	o.mu.Unlock()
	proposal := pubsubQUICUnitToken("/meshsub/1.1.0\n")
	pubsubQUICUnitRead(t, s, raw, proposal[:1])
	returns := pubsubScoringUnitEvents(o, "native_quic_negotiation_io_return")
	last := returns[len(returns)-1]["negotiation_snapshot"].(map[string]any)
	if o.failure == nil || last["capture_complete"] != false ||
		last["read"].(map[string]any)["partial_frame"].(map[string]any)["hex"] != nil {
		t.Fatal("snapshot bound was silently truncated or normalized", o.failure, last)
	}
}

func TestPubsubQUICCloseNeverBorrowsZeroContextForPublicErrors(t *testing.T) {
	for _, mode := range []string{"errno", "stream_nonzero", "stream_zero", "application", "transport", "stream_limit", "datagram", "eof", "canceled", "wrapped", "joined"} {
		t.Run(mode, func(t *testing.T) {
			o, s, raw, _ := pubsubQUICUnit(t, network.DirInbound)
			pubsubQUICUnitSelect(t, s, raw, "/meshsub/1.1.0")
			pubsubQUICUnitPrepare(t, o)
			raw.cancel(&quic.StreamError{StreamID: s.nativeID, Remote: true})
			var failure error
			classification := "public_typed_error"
			switch mode {
			case "errno":
				failure = syscall.ECONNRESET
			case "stream_nonzero":
				failure = &quic.StreamError{StreamID: s.nativeID, ErrorCode: 7}
			case "stream_zero":
				failure = &quic.StreamError{StreamID: s.nativeID}
			case "application":
				failure = &quic.ApplicationError{ErrorCode: 7}
			case "transport":
				failure = &quic.TransportError{ErrorCode: quic.InternalError}
			case "stream_limit":
				failure = quic.StreamLimitReachedError{}
			case "datagram":
				failure = &quic.DatagramTooLargeError{MaxDatagramPayloadSize: 1}
			case "eof":
				failure, classification = io.EOF, "known_sentinel_error"
			case "canceled":
				failure, classification = context.Canceled, "known_sentinel_error"
			case "wrapped":
				failure, classification = fmt.Errorf("unit wrapper: %w", syscall.EIO), "wrapped_or_joined_error"
			case "joined":
				failure, classification = errors.Join(errors.New("unit opaque"), syscall.EIO), "wrapped_or_joined_error"
			}
			raw.closeErr = failure
			if err := s.Close(); err != failure {
				t.Fatal("known native Close error changed", err)
			}
			_ = s.Reset()
			s.finalize()
			operations := pubsubScoringUnitEvents(o, "native_stream_operation")
			if o.failure != failure || len(operations) != 2 || operations[0]["outcome"] != "error" ||
				operations[0]["native_close_error_classification"] != classification ||
				len(pubsubScoringUnitEvents(o, "native_quic_terminal_finalized")) != 0 {
				t.Fatal("zero send context authorized a different public Close error", o.failure, operations)
			}
		})
	}
}

func TestPubsubQUICPeerZeroReadRequiresLiveActualPrepareSnapshot(t *testing.T) {
	for _, mode := range []string{"live", "early_zero", "early_fin", "created_after_prepare", "sticky", "half_close_only"} {
		t.Run(mode, func(t *testing.T) {
			o, s, raw, _ := pubsubQUICUnit(t, network.DirInbound)
			pubsubQUICUnitSelect(t, s, raw, "/meshsub/1.1.0")
			cause := &quic.StreamError{StreamID: s.nativeID, Remote: true}
			if mode == "early_zero" {
				raw.cancel(cause)
			}
			if mode == "early_fin" {
				raw.cancel(context.Canceled)
			}
			pubsubQUICUnitPrepare(t, o)
			if mode == "created_after_prepare" {
				ctx, cancel := context.WithCancelCause(context.Background())
				t.Cleanup(func() { cancel(context.Canceled) })
				raw = &pubsubQUICUnitStream{ctx: ctx, cancel: cancel, id: 4, writeN: -1}
				s = s.connection.stream(raw, network.DirInbound, 0).(*pubsubQUICStream)
				pubsubQUICUnitSelect(t, s, raw, "/meshsub/1.1.0")
				cause = &quic.StreamError{StreamID: s.nativeID, Remote: true}
			}
			if mode == "sticky" {
				o.fail(syscall.EPIPE)
			}
			raw.cancel(cause)
			failure := &network.StreamError{Remote: true, TransportError: cause}
			raw.input, raw.readErr = nil, failure
			if n, err := s.Read(make([]byte, 1)); n != 0 || err != failure {
				t.Fatal(n, err)
			}
			if mode == "half_close_only" {
				_ = s.CloseRead()
				_ = s.CloseWrite()
			} else {
				_ = s.Reset()
			}
			s.finalize()
			final := pubsubScoringUnitEvents(o, "native_quic_terminal_finalized")
			if mode == "live" {
				if o.failure != nil || len(final) != 1 || final[0]["accepted"] != true {
					t.Fatal(o.failure, final)
				}
			} else if o.failure == nil {
				t.Fatal("missing/terminal prepare state authorized peer reset", mode, final)
			}
			if mode == "early_zero" || mode == "early_fin" || mode == "created_after_prepare" {
				terminal := pubsubScoringUnitEvents(o, "stream_io_terminal")
				if o.failure != failure || len(terminal) != 1 || terminal[0]["outcome"] != "error" || len(final) != 0 {
					t.Fatal("early/absent baseline was inherited as normal reset state", o.failure, terminal, final)
				}
			}
		})
	}
}

func TestPubsubQUICSuccessfulPrefixAndErrorAreNotRewritten(t *testing.T) {
	for _, side := range []string{"read", "write"} {
		t.Run(side, func(t *testing.T) {
			o, s, raw, _ := pubsubQUICUnit(t, network.DirInbound)
			pubsubQUICUnitSelect(t, s, raw, "/meshsub/1.1.0")
			frame := pubsubScoringUnitFrame(t, nil)
			failure := syscall.EIO
			var n int
			var err error
			if side == "read" {
				raw.input, raw.readErr = bytes.NewReader(frame), failure
				buffer := make([]byte, len(frame)+3)
				n, err = s.Read(buffer)
				if !bytes.Equal(buffer[:n], frame) {
					t.Fatal("successful Read prefix changed")
				}
			} else {
				raw.writeN, raw.writeErr = len(frame), failure
				n, err = s.Write(append(append([]byte{}, frame...), 0xff, 0xff, 0xff))
			}
			rows := pubsubScoringUnitEvents(o, "rpc")
			if n != len(frame) || err != failure || o.failure != failure || len(rows) != 1 ||
				rows[0]["direction"] != side || rows[0]["receipt"].(map[string]any)["framed_hex"] != hex.EncodeToString(frame) {
				t.Fatal("failed operation lost native prefix/error provenance", n, err, o.failure, rows)
			}
		})
	}
}

func TestPubsubQUICPeerZeroWriteRequiresCurrentSameSendContext(t *testing.T) {
	for _, mode := range []string{"same_context", "no_context", "different_instance", "nonzero_context"} {
		t.Run(mode, func(t *testing.T) {
			o, s, raw, _ := pubsubQUICUnit(t, network.DirOutbound)
			pubsubQUICUnitSelect(t, s, raw, "/meshsub/1.1.0")
			pubsubQUICUnitPrepare(t, o)
			cause := &quic.StreamError{StreamID: s.nativeID, Remote: true}
			var observed error = cause
			if mode == "different_instance" {
				observed = &quic.StreamError{StreamID: s.nativeID, Remote: true}
			}
			if mode == "nonzero_context" {
				observed = &quic.StreamError{StreamID: s.nativeID, ErrorCode: 8, Remote: true}
			}
			if mode != "no_context" {
				raw.cancel(observed)
			}
			raw.writeN, raw.writeErr = 0, &network.StreamError{Remote: true, TransportError: cause}
			if n, err := s.Write([]byte{0xff}); n != 0 || err != raw.writeErr {
				t.Fatal(n, err)
			}
			_ = s.Reset()
			s.finalize()
			final := pubsubScoringUnitEvents(o, "native_quic_terminal_finalized")
			if mode == "same_context" {
				if o.failure != nil || len(final) != 1 || final[0]["accepted"] != true {
					t.Fatal(o.failure, final)
				}
			} else if o.failure == nil {
				t.Fatal("foreign/previous context authorized Write", final)
			}
		})
	}
}

type pubsubQUICUnitTransport struct {
	transport.Transport
	capable transport.CapableConn
	err     error
	closed  int
	ctx     context.Context
	address ma.Multiaddr
	peer    peer.ID
}

func (t *pubsubQUICUnitTransport) Dial(ctx context.Context, address ma.Multiaddr, p peer.ID) (transport.CapableConn, error) {
	t.ctx, t.address, t.peer = ctx, address, p
	return t.capable, t.err
}
func (t *pubsubQUICUnitTransport) Close() error     { t.closed++; return t.err }
func (t *pubsubQUICUnitTransport) ListenOrder() int { return 17 }

func TestPubsubQUICPublicDelegationPreservesResultsContextDeadlinesAndAs(t *testing.T) {
	o, s, raw, _ := pubsubQUICUnit(t, network.DirOutbound)
	capable := s.connection.CapableConn.(*pubsubQUICUnitCapable)
	failure := syscall.ECONNREFUSED
	capable.err = failure
	ctx := context.WithValue(context.Background(), struct{}{}, "unit-context")
	if output, err := s.connection.OpenStream(ctx); output != raw || err != failure || capable.ctxSeen != ctx {
		t.Fatal("OpenStream output/error/context changed")
	}
	if output, err := s.connection.AcceptStream(); output != raw || err != failure {
		t.Fatal("AcceptStream output/error changed")
	}
	var native *quic.Conn
	var lower *pubsubQUICConn
	if !s.connection.As(&native) || native != capable.native || !s.connection.As(&lower) || lower != s.connection {
		t.Fatal("As native ownership changed")
	}
	deadline := time.Unix(1234, 5678)
	if err := s.SetDeadline(deadline); err != nil || raw.deadline != deadline {
		t.Fatal("deadline changed")
	}
	if err := s.SetReadDeadline(deadline); err != nil || raw.deadline != deadline {
		t.Fatal("read deadline changed")
	}
	if err := s.SetWriteDeadline(deadline); err != nil || raw.deadline != deadline {
		t.Fatal("write deadline changed")
	}
	delegate := &pubsubQUICUnitTransport{capable: capable, err: failure}
	tpt := &pubsubQUICTransport{Transport: delegate, owner: o.quic}
	address, _ := ma.NewMultiaddr("/ip4/127.0.0.1/udp/12345/quic-v1")
	if output, err := tpt.Dial(ctx, address, o.local); output != capable || err != failure ||
		delegate.ctx != ctx || !delegate.address.Equal(address) || delegate.peer != o.local {
		t.Fatal("Transport Dial changed native inputs/results")
	}
	if err := tpt.Close(); err != failure || delegate.closed != 1 || tpt.ListenOrder() != 17 {
		t.Fatal("optional native methods changed")
	}
}

// Keep net/io method signatures checked without opening sockets in these tests.
var _ io.ReadWriteCloser = (*pubsubQUICUnitStream)(nil)
var _ network.MuxedStream = (*pubsubQUICUnitStream)(nil)
var _ transport.CapableConn = (*pubsubQUICUnitCapable)(nil)

func pubsubQUICUnitSubscription(t *testing.T, topic string) []byte {
	t.Helper()
	subscribe, noPartial := true, false
	rpc := &pubsubpb.RPC{Subscriptions: []*pubsubpb.RPC_SubOpts{{Subscribe: &subscribe, Topicid: &topic,
		RequestsPartial: &noPartial, SupportsSendingPartial: &noPartial}}}
	body, err := rpc.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	var header [binary.MaxVarintLen64]byte
	n := binary.PutUvarint(header[:], uint64(len(body)))
	return append(append([]byte{}, header[:n]...), body...)
}

func pubsubQUICUnitLateCandidate(t *testing.T) (*pubsubScoringObserver, *pubsubQUICStream, *pubsubQUICUnitStream, context.CancelCauseFunc) {
	t.Helper()
	o, original, _, cancelParent := pubsubQUICUnit(t, network.DirOutbound)
	pubsubQUICUnitPrepare(t, o)
	ctx, cancel := context.WithCancelCause(context.Background())
	t.Cleanup(func() { cancel(context.Canceled) })
	raw := &pubsubQUICUnitStream{ctx: ctx, cancel: cancel, id: 4, writeN: -1}
	original.connection.CapableConn.(*pubsubQUICUnitCapable).stream = raw
	observed, err := original.connection.OpenStream(context.Background())
	s, ok := observed.(*pubsubQUICStream)
	if err != nil || !ok {
		t.Fatal("late candidate did not use the actual delegate OpenStream", err)
	}
	return o, s, raw, cancelParent
}

func pubsubQUICUnitCandidateWrite(t *testing.T, o *pubsubScoringObserver, s *pubsubQUICStream, tail []byte) {
	t.Helper()
	prefix := append(pubsubQUICUnitToken("/multistream/1.0.0\n"), pubsubQUICUnitToken("/meshsub/1.1.0\n")...)
	prefix = append(prefix, tail...)
	if n, err := s.Write(prefix); n != len(prefix) || err != nil {
		t.Fatal("candidate Write changed original native result", n, err)
	}
	if len(pubsubScoringUnitEvents(o, "rpc"))+len(pubsubScoringUnitEvents(o, "protocol")) != 0 {
		t.Fatal("candidate bytes became selected protocol/RPC authority")
	}
}

func pubsubQUICUnitCandidateError(t *testing.T, s *pubsubQUICStream, raw *pubsubQUICUnitStream,
	cancelParent context.CancelCauseFunc) *network.ConnError {
	t.Helper()
	cause := &quic.ApplicationError{Remote: true}
	cancelParent(cause)
	failure := &network.ConnError{Remote: true, TransportError: cause}
	raw.input, raw.readErr = nil, failure
	if n, err := s.Read(make([]byte, 1)); n != 0 || err != failure {
		t.Fatal("candidate error/count changed", n, err)
	}
	return failure
}

func TestPubsubQUICNegotiationAbortedCleanupRequiresActualJoinWithoutACKAuthority(t *testing.T) {
	for _, dispose := range []string{"reset", "close"} {
		t.Run(dispose, func(t *testing.T) {
			o, s, raw, cancelParent := pubsubQUICUnitLateCandidate(t)
			created := pubsubQUICEvent(o, s.receipt, "native_quic_stream", "go.quic.CapableConn.stream_return")
			if created["native_call_begin_prepare_ack_sequence"] != o.prepareAck ||
				created["native_call_begin_observation_basis"] != "published_Prepare_ACK_before_native_CapableConn_call" {
				t.Fatal("post-Prepare native OpenStream BEGIN not retained", created)
			}
			tail := pubsubQUICUnitSubscription(t, o.topic)
			pubsubQUICUnitCandidateWrite(t, o, s, tail)
			failure := pubsubQUICUnitCandidateError(t, s, raw, cancelParent)
			pending := pubsubScoringUnitEvents(o, "native_quic_negotiation_cleanup_pending")
			if o.failure != nil || len(pending) != 1 || pending[0]["selected_rpc_authority"] != false ||
				pending[0]["candidate_bytes_complete"] != true || raw.readErr != failure {
				t.Fatal("valid candidate missing or original error erased", o.failure, pending)
			}
			if dispose == "reset" {
				if err := s.Reset(); err != nil {
					t.Fatal(err)
				}
			} else if err := s.Close(); err != nil {
				t.Fatal(err)
			}
			if !s.finalize() || o.failure != nil || len(pubsubScoringUnitEvents(o, "native_quic_negotiation_cleanup_finalized")) != 0 {
				t.Fatal("cleanup accepted before actual native join", o.failure)
			}
			ctx, cancel := context.WithTimeout(context.Background(), time.Second)
			defer cancel()
			if err := o.quic.join(ctx); err != nil {
				t.Fatal(err)
			}
			final := pubsubScoringUnitEvents(o, "native_quic_negotiation_cleanup_finalized")
			framing := pubsubScoringUnitEvents(o, "native_quic_framing_finalized")
			if o.failure != nil || len(final) != 1 || len(framing) != 1 || final[0]["accepted"] != true ||
				final[0]["protocol"] != "" || final[0]["terminal_outcome"] != "negotiation_aborted_cleanup" ||
				final[0]["negotiation_complete"] != false || final[0]["framing_clean"] != false ||
				final[0]["selected_rpc_authority"] != false || final[0]["candidate_bytes_complete"] != true ||
				framing[0]["framing_clean"] != false || framing[0]["negotiation_complete"] != false ||
				len(pubsubScoringUnitEvents(o, "native_quic_terminal_finalized")) != 0 ||
				len(pubsubScoringUnitEvents(o, "protocol"))+len(pubsubScoringUnitEvents(o, "rpc")) != 0 {
				t.Fatal("cleanup was mislabeled as normal framing/negotiation", o.failure, final, framing)
			}
			proof := final[0]
			refs := []int{proof["prepare_ack_sequence"].(int), s.receipt, proof["candidate_write_receipt_sequence"].(int),
				proof["connection_context_receipt_sequence"].(int), proof["operation_receipt_sequence"].(int),
				proof["pending_receipt_sequence"].(int), proof["owner_disposal_receipt_sequence"].(int),
				proof["framing_receipt_sequence"].(int), proof["native_join_receipt_sequence"].(int), proof["sequence"].(int)}
			for index := 1; index < len(refs); index++ {
				if refs[index] <= refs[index-1] {
					t.Fatal("cleanup lacks the actual ordered receipt chain", refs)
				}
			}
			original := pubsubQUICEvent(o, proof["operation_receipt_sequence"].(int), "native_quic_negotiation_io_return", "go.quic.native_stream.io_return")
			if original["outcome"] != "error" || original["error_type"] != "*network.ConnError" ||
				original["same_native_connection_context_cause"] != true || raw.closes+raw.resets != 1 {
				t.Fatal("original error/native operation count rewritten", original)
			}
		})
	}
}

func pubsubQUICUnitCandidateHeader(t *testing.T, s *pubsubQUICStream, raw *pubsubQUICUnitStream) int {
	t.Helper()
	header := pubsubQUICUnitToken("/multistream/1.0.0\n")
	pubsubQUICUnitRead(t, s, raw, header[:1])
	pubsubQUICUnitRead(t, s, raw, header[1:])
	if len(s.frames) != 3 || s.selected != "" || s.reply != "" || len(s.negotiation[0].buffer) != 0 {
		t.Fatal("fragmented peer header became partial/selected negotiation")
	}
	return s.frames[2]
}

func TestPubsubQUICCleanupPeerHeaderPinsPostReturnDisposalWithoutReplacingFirst(t *testing.T) {
	for _, dispose := range []string{"reset", "close"} {
		t.Run(dispose, func(t *testing.T) {
			o, s, raw, cancelParent := pubsubQUICUnitLateCandidate(t)
			pubsubQUICUnitCandidateWrite(t, o, s, pubsubQUICUnitSubscription(t, o.topic))
			headerSequence := pubsubQUICUnitCandidateHeader(t, s, raw)
			cause := &quic.ApplicationError{Remote: true}
			cancelParent(cause)
			failure := &network.ConnError{Remote: true, TransportError: cause}
			raw.input, raw.readErr = nil, failure
			raw.readStart, raw.readEnd = make(chan struct{}), make(chan struct{})
			finished, joined := make(chan error, 1), make(chan struct{})
			var release sync.Once
			launched := false
			t.Cleanup(func() {
				release.Do(func() { close(raw.readEnd) })
				if launched {
					<-joined
				}
			})
			launched = true
			go func() {
				defer close(joined)
				n, err := s.Read(make([]byte, 1))
				if n != 0 {
					finished <- fmt.Errorf("unexpected native prefix count %d", n)
					return
				}
				finished <- err
			}()
			<-raw.readStart
			if err := s.Reset(); err != nil {
				t.Fatal(err)
			}
			firstDisposal := s.disposal
			if s.finalize() {
				t.Fatal("blocked native Read became finalized cleanup")
			}
			release.Do(func() { close(raw.readEnd) })
			if err := <-finished; err != failure {
				t.Fatal("original Read error changed", err)
			}
			pending := s.cleanup
			if pending == nil || o.failure != nil || pending.peerHeaderSequence != headerSequence || pending.disposalSequence != 0 {
				t.Fatal("complete header missing or prior disposal borrowed", o.failure, pending)
			}
			if dispose == "reset" {
				if err := s.Reset(); err != nil {
					t.Fatal(err)
				}
			} else if err := s.Close(); err != nil {
				t.Fatal(err)
			}
			postReturnDisposal := pending.disposalSequence
			if postReturnDisposal <= pending.sequence || s.disposal != firstDisposal {
				t.Fatal("post-RETURN disposal replaced/borrowed first disposal")
			}
			if err := s.Reset(); err != nil {
				t.Fatal(err)
			}
			if pending.disposalSequence != postReturnDisposal || s.disposal != firstDisposal {
				t.Fatal("later Reset rewrote immutable disposal pins")
			}
			if !s.finalize() || o.failure != nil || len(pubsubScoringUnitEvents(o, "native_quic_negotiation_cleanup_finalized")) != 0 {
				t.Fatal("cleanup finalized without the actual native join", o.failure)
			}
			ctx, cancel := context.WithTimeout(context.Background(), time.Second)
			defer cancel()
			if err := o.quic.join(ctx); err != nil {
				t.Fatal(err)
			}
			final := pubsubScoringUnitEvents(o, "native_quic_negotiation_cleanup_finalized")
			framing := pubsubScoringUnitEvents(o, "native_quic_framing_finalized")
			if o.failure != nil || len(final) != 1 || len(framing) != 1 || final[0]["accepted"] != true ||
				final[0]["peer_header_frame_sequence"] != headerSequence || final[0]["owner_disposal_receipt_sequence"] != postReturnDisposal ||
				final[0]["first_owner_disposal_receipt_sequence"] != firstDisposal || framing[0]["owner_disposal_receipt_sequence"] != firstDisposal ||
				framing[0]["cleanup_owner_disposal_receipt_sequence"] != postReturnDisposal ||
				final[0]["protocol"] != "" || final[0]["selected_rpc_authority"] != false ||
				final[0]["negotiation_complete"] != false || final[0]["framing_clean"] != false ||
				framing[0]["negotiation_complete"] != false || framing[0]["framing_clean"] != false ||
				len(pubsubScoringUnitEvents(o, "protocol"))+len(pubsubScoringUnitEvents(o, "rpc")) != 0 {
				t.Fatal("header-only cleanup became selected authority or lost disposal provenance", o.failure, final, framing)
			}
			original := pubsubQUICEvent(o, pending.errorSequence, "native_quic_negotiation_io_return", "go.quic.native_stream.io_return")
			prior := pubsubQUICEvent(o, firstDisposal, "native_stream_operation", "go.quic.native_stream.operation_return")
			later := pubsubQUICEvent(o, postReturnDisposal, "native_stream_operation", "go.quic.native_stream.operation_return")
			if prior["returned_order"].(uint64) >= original["returned_order"].(uint64) ||
				later["started_order"].(uint64) <= original["returned_order"].(uint64) ||
				original["outcome"] != "error" || original["error_type"] != "*network.ConnError" ||
				original["negotiation_snapshot"].(map[string]any)["parser_failed"] != false || raw.closes+raw.resets != 3 {
				t.Fatal("actual operation ordering/error/count rewritten", original, prior, later)
			}
		})
	}
}

func TestPubsubQUICCleanupRetainsDisposalCompletedBeforeReadPublication(t *testing.T) {
	for _, dispose := range []string{"reset", "close"} {
		for _, peerHeader := range []bool{false, true} {
			t.Run(fmt.Sprintf("%s/header_%t", dispose, peerHeader), func(t *testing.T) {
				o, s, raw, cancelParent := pubsubQUICUnitLateCandidate(t)
				pubsubQUICUnitCandidateWrite(t, o, s, pubsubQUICUnitSubscription(t, o.topic))
				if peerHeader {
					pubsubQUICUnitCandidateHeader(t, s, raw)
				}
				cause := &quic.ApplicationError{Remote: true}
				cancelParent(cause)
				failure := &network.ConnError{Remote: true, TransportError: cause}
				raw.input, raw.readErr = nil, failure
				if err := s.Reset(); err != nil {
					t.Fatal(err)
				}
				firstDisposal := s.disposal
				delayed := &pubsubQUICUnitDelayedContext{Context: raw.ctx, entered: make(chan struct{}), release: make(chan struct{})}
				s.sendContext = delayed
				done, joined := make(chan error, 1), make(chan struct{})
				var release sync.Once
				launched := false
				t.Cleanup(func() {
					release.Do(func() { close(delayed.release) })
					if launched {
						<-joined
					}
				})
				launched = true
				go func() {
					defer close(joined)
					n, err := s.Read(make([]byte, 1))
					if n != 0 {
						done <- fmt.Errorf("unexpected native prefix count %d", n)
						return
					}
					done <- err
				}()
				<-delayed.entered // Read nativeReturned sealed RETURN; observation publication is blocked.
				s.mu.Lock()
				sealedRead := s.cleanupRead
				unpublished := s.cleanup == nil && s.cleanupDisposal == nil
				s.mu.Unlock()
				if sealedRead == nil || !unpublished {
					t.Fatal("barrier did not isolate sealed Read RETURN from cleanup publication")
				}
				if dispose == "reset" {
					if err := s.Reset(); err != nil {
						t.Fatal(err)
					}
				} else if err := s.Close(); err != nil {
					t.Fatal(err)
				}
				disposal := s.cleanupDisposal
				if disposal == nil || disposal.sequence == 0 || disposal.readReturn != sealedRead.order ||
					disposal.started <= sealedRead.order || disposal.returned <= disposal.started ||
					s.cleanup != nil || s.disposal != firstDisposal {
					t.Fatal("actual full-disposal receipt lost before cleanup publication", disposal)
				}
				pin := disposal.sequence
				if err := s.Reset(); err != nil {
					t.Fatal(err)
				}
				if s.cleanupDisposal != disposal || disposal.sequence != pin || s.disposal != firstDisposal || s.finalize() {
					t.Fatal("later disposal replaced the immutable slot or blocked Read was finalized")
				}
				release.Do(func() { close(delayed.release) })
				if err := <-done; err != failure {
					t.Fatal("original native Read error changed", err)
				}
				pending := s.cleanup
				if pending == nil || pending.disposalSequence != pin || pending.returned != sealedRead.order ||
					pin >= pending.errorSequence || pin >= pending.sequence || o.failure != nil {
					t.Fatal("earlier publication was mistaken for earlier native completion", pending, o.failure)
				}
				ctx, cancel := context.WithTimeout(context.Background(), time.Second)
				defer cancel()
				if err := o.quic.join(ctx); err != nil {
					t.Fatal(err)
				}
				final := pubsubScoringUnitEvents(o, "native_quic_negotiation_cleanup_finalized")
				original := pubsubQUICEvent(o, pending.errorSequence, "native_quic_negotiation_io_return", "go.quic.native_stream.io_return")
				fullDisposal := pubsubQUICEvent(o, pin, "native_stream_operation", "go.quic.native_stream.operation_return")
				if o.failure != nil || len(final) != 1 || final[0]["accepted"] != true ||
					final[0]["owner_disposal_receipt_sequence"] != pin || final[0]["first_owner_disposal_receipt_sequence"] != firstDisposal ||
					original["returned_order"] != sealedRead.order || fullDisposal["started_order"] != disposal.started ||
					fullDisposal["returned_order"] != disposal.returned || original["outcome"] != "error" ||
					fullDisposal["outcome"] != "ok" || raw.readErr != failure || raw.closes+raw.resets != 3 ||
					final[0]["protocol"] != "" || final[0]["selected_rpc_authority"] != false ||
					final[0]["framing_clean"] != false || final[0]["negotiation_complete"] != false ||
					len(pubsubScoringUnitEvents(o, "protocol"))+len(pubsubScoringUnitEvents(o, "rpc")) != 0 {
					t.Fatal("causal disposal/error/cleanup-only authority changed", o.failure, final, original, fullDisposal)
				}
			})
		}
	}
}

func TestPubsubQUICCleanupRejectsDisposalBegunBeforeSealedReadReturn(t *testing.T) {
	o, s, raw, cancelParent := pubsubQUICUnitLateCandidate(t)
	pubsubQUICUnitCandidateWrite(t, o, s, pubsubQUICUnitSubscription(t, o.topic))
	pubsubQUICUnitCandidateHeader(t, s, raw)
	cause := &quic.ApplicationError{Remote: true}
	cancelParent(cause)
	failure := &network.ConnError{Remote: true, TransportError: cause}
	raw.input, raw.readErr = nil, failure
	raw.resetStart, raw.resetEnd = make(chan struct{}), make(chan struct{})
	done, joined := make(chan error, 1), make(chan struct{})
	var release sync.Once
	launched := false
	t.Cleanup(func() {
		release.Do(func() { close(raw.resetEnd) })
		if launched {
			<-joined
		}
	})
	launched = true
	go func() { defer close(joined); done <- s.Reset() }()
	<-raw.resetStart
	if n, err := s.Read(make([]byte, 1)); n != 0 || err != failure || s.cleanup == nil || o.failure != nil {
		t.Fatal("original native Read changed or candidate was not staged", n, err, o.failure)
	}
	release.Do(func() { close(raw.resetEnd) })
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	if s.cleanupDisposal != nil || s.cleanup.disposalSequence != 0 || s.disposal == 0 {
		t.Fatal("earlier native BEGIN became post-RETURN disposal")
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if err := o.quic.join(ctx); err != nil {
		t.Fatal(err)
	}
	final := pubsubScoringUnitEvents(o, "native_quic_negotiation_cleanup_finalized")
	if o.failure != failure || len(final) != 1 || final[0]["accepted"] != false || final[0]["owner_disposal_receipt_sequence"] != 0 {
		t.Fatal("inflight prior disposal excused the original error", o.failure, final)
	}
}

func TestPubsubQUICCleanupPeerHeaderRejectsPartialWrongForeignAndSelectedReplies(t *testing.T) {
	for _, mode := range []string{"partial_length", "partial_header", "partial_reply", "wrong_header", "duplicate_header", "na", "selected_ack",
		"foreign_header", "wrong_direction", "wrong_hash", "missing_raw", "incomplete_header", "read_tail"} {
		t.Run(mode, func(t *testing.T) {
			o, s, raw, cancelParent := pubsubQUICUnitLateCandidate(t)
			pubsubQUICUnitCandidateWrite(t, o, s, pubsubQUICUnitSubscription(t, o.topic))
			header := pubsubQUICUnitToken("/multistream/1.0.0\n")
			input := append([]byte{}, header...)
			switch mode {
			case "partial_length":
				input = []byte{0x80}
			case "partial_header":
				input = header[:len(header)-1]
			case "partial_reply":
				input = append(input, 0x80)
			case "wrong_header":
				input = pubsubQUICUnitToken("/multistream/2.0.0\n")
			case "duplicate_header":
				input = append(input, header...)
			case "na":
				input = append(input, pubsubQUICUnitToken("na\n")...)
			case "selected_ack":
				input = append(input, pubsubQUICUnitToken("/meshsub/1.1.0\n")...)
			}
			pubsubQUICUnitRead(t, s, raw, input)
			if mode == "foreign_header" || mode == "wrong_direction" || mode == "wrong_hash" ||
				mode == "missing_raw" || mode == "incomplete_header" {
				// Negative unit receipt models; live output is never edited.
				frame := pubsubQUICEvent(o, s.frames[2], "multistream_frame", "go.quic.native_stream.multistream")
				switch mode {
				case "foreign_header":
					frame["native_stream_id"] = int64(8)
				case "wrong_direction":
					frame["direction"] = "write"
				case "wrong_hash":
					frame["receipt"].(map[string]any)["read"].(map[string]any)["framed_sha256"] = strings.Repeat("0", 64)
				case "missing_raw":
					delete(frame["receipt"].(map[string]any), "framed_hex")
				case "incomplete_header":
					frame["receipt"].(map[string]any)["read"].(map[string]any)["complete_frames"] = false
				}
			} else if mode == "read_tail" {
				s.negotiation[0].tail = []byte{0x80} // Unit-only parser residue model.
			}
			failure := pubsubQUICUnitCandidateError(t, s, raw, cancelParent)
			if s.cleanup != nil || len(pubsubScoringUnitEvents(o, "native_quic_negotiation_cleanup_pending")) != 0 || raw.readErr != failure {
				t.Fatal("partial/foreign/replied negotiation became cleanup", mode, o.failure)
			}
			if mode == "selected_ack" {
				if s.selected != "/meshsub/1.1.0" || len(pubsubScoringUnitEvents(o, "protocol")) != 1 {
					t.Fatal("actual ACK lost its distinct selected-stream authority")
				}
			} else if o.failure == nil || len(pubsubScoringUnitEvents(o, "protocol"))+len(pubsubScoringUnitEvents(o, "rpc")) != 0 {
				t.Fatal("invalid header progress was not fatal", mode, o.failure)
			}
		})
	}
}

func TestPubsubQUICCleanupPeerHeaderRequiresOwnPostReturnDisposalAndUnchangedSnapshot(t *testing.T) {
	for _, mode := range []string{"missing", "prior_only", "foreign", "failed", "half_close", "missing_receipt", "wrong_ack", "wrong_order", "changed_read_snapshot", "sticky"} {
		t.Run(mode, func(t *testing.T) {
			o, s, raw, cancelParent := pubsubQUICUnitLateCandidate(t)
			pubsubQUICUnitCandidateWrite(t, o, s, pubsubQUICUnitSubscription(t, o.topic))
			pubsubQUICUnitCandidateHeader(t, s, raw)
			if mode == "prior_only" {
				if err := s.Reset(); err != nil {
					t.Fatal(err)
				}
			}
			failure := pubsubQUICUnitCandidateError(t, s, raw, cancelParent)
			if s.cleanup == nil || o.failure != nil {
				t.Fatal("missing header-only pending cleanup", o.failure)
			}
			switch mode {
			case "missing", "prior_only":
			case "failed":
				raw.resetErr = syscall.EIO
				if err := s.Reset(); err != syscall.EIO {
					t.Fatal("original Reset failure changed", err)
				}
			case "half_close":
				_ = s.CloseRead()
				_ = s.CloseWrite()
			default:
				if err := s.Reset(); err != nil {
					t.Fatal(err)
				}
				disposal := pubsubQUICEvent(o, s.cleanup.disposalSequence, "native_stream_operation", "go.quic.native_stream.operation_return")
				switch mode {
				case "foreign":
					disposal["native_connection_id"] = "unit-foreign"
				case "missing_receipt":
					disposal["kind"] = "unit-not-a-disposal"
				case "wrong_ack":
					disposal["prepare_ack_sequence"] = o.prepareAck + 1
				case "wrong_order":
					disposal["started_order"] = s.cleanup.returned
				case "changed_read_snapshot":
					original := pubsubQUICEvent(o, s.cleanup.errorSequence, "native_quic_negotiation_io_return", "go.quic.native_stream.io_return")
					original["negotiation_snapshot"].(map[string]any)["read"].(map[string]any)["header_seen"] = false
				case "sticky":
					o.fail(syscall.EPIPE)
				}
			}
			ctx, cancel := context.WithTimeout(context.Background(), time.Second)
			defer cancel()
			if err := o.quic.join(ctx); err != nil {
				t.Fatal(err)
			}
			final := pubsubScoringUnitEvents(o, "native_quic_negotiation_cleanup_finalized")
			if o.failure == nil || len(final) != 1 || final[0]["accepted"] != false {
				t.Fatal("missing/prior/foreign disposal or changed proof accepted", mode, o.failure, final)
			}
			if (mode == "missing" || mode == "prior_only" || mode == "half_close") && o.failure != failure {
				t.Fatal("original pending error lost", o.failure)
			}
			if mode == "failed" && o.failure != syscall.EIO {
				t.Fatal("native Reset failure not sticky", o.failure)
			}
			if mode == "sticky" && o.failure != syscall.EPIPE {
				t.Fatal("earlier sticky error cleared", o.failure)
			}
		})
	}
}

func TestPubsubQUICSubscriptionCandidateDecoderRejectsIncompleteAndNoncanonicalBytes(t *testing.T) {
	valid := pubsubQUICUnitSubscription(t, "unit-topic")
	if !pubsubQUICSubscriptionBytes(valid, "unit-topic") {
		t.Fatal("canonical subscription-only candidate rejected")
	}
	// Canonical framing alone does not make these unit-only bodies acceptable.
	for _, mode := range []string{"partial", "trailing_partial", "two_frames", "overlong_length", "over_limit", "wrong_topic",
		"unknown_root", "unknown_subscription", "duplicate_subscribe", "noncanonical_bool", "control", "publish", "invalid_utf8",
		"partial_extension", "unsubscribe", "empty"} {
		t.Run(mode, func(t *testing.T) {
			wire := append([]byte{}, valid...)
			_, header := binary.Uvarint(wire)
			body := append([]byte{}, wire[header:]...)
			topic := "unit-topic"
			switch mode {
			case "partial":
				wire = wire[:len(wire)-1]
			case "trailing_partial":
				wire = append(wire, 0x80)
			case "two_frames":
				wire = append(wire, valid...)
			case "overlong_length":
				wire = append([]byte{byte(len(body)) | 0x80, 0}, body...)
			case "over_limit":
				wire = append(pubsubQUICUnitToken(strings.Repeat("x", pubsubScoringFrame+1)), 0)
			case "wrong_topic":
				topic = "other-topic"
			case "unknown_root":
				body = append(body, 0x28, 1)
				wire = pubsubQUICUnitToken(string(body))
			case "unknown_subscription", "duplicate_subscribe", "noncanonical_bool", "invalid_utf8", "partial_extension", "unsubscribe":
				sub := append([]byte{}, body[2:]...)
				switch mode {
				case "unknown_subscription":
					sub = append(sub, 0x28, 1)
				case "duplicate_subscribe":
					sub = append(sub, 8, 1)
				case "noncanonical_bool":
					sub[1] = 2
				case "invalid_utf8":
					sub[4] = 0xff
				case "partial_extension":
					sub[len(sub)-1] = 1
				case "unsubscribe":
					sub[1] = 0
				}
				body = append([]byte{0x0a, byte(len(sub))}, sub...)
				wire = pubsubQUICUnitToken(string(body))
			case "control":
				wire = pubsubQUICUnitToken(string(append(body, 0x1a, 0)))
			case "publish":
				wire = pubsubQUICUnitToken(string(append(body, 0x12, 0)))
			case "empty":
				wire = []byte{0}
			}
			if pubsubQUICSubscriptionBytes(wire, topic) {
				t.Fatal("noncanonical/non-subscription candidate accepted", mode)
			}
		})
	}
}

func TestPubsubQUICCandidateWriteRequiresExactRawHashOwnerAndNativeFrames(t *testing.T) {
	for _, mode := range []string{"missing_raw", "wrong_hash", "wrong_count", "foreign_owner", "foreign_stream", "missing_frame",
		"wrong_frame_hash", "incomplete_frame", "wrong_lazy_tail", "partial_buffer", "wrong_proposal", "missing_begin_ack", "foreign_begin_ack"} {
		t.Run(mode, func(t *testing.T) {
			o, s, _, _ := pubsubQUICUnitLateCandidate(t)
			pubsubQUICUnitCandidateWrite(t, o, s, pubsubQUICUnitSubscription(t, o.topic))
			write := pubsubQUICEvent(o, s.candidateWrite, "native_quic_negotiation_io_return", "go.quic.native_stream.io_return")
			if write == nil || !s.subscriptionWriteLocked(write) {
				t.Fatal("missing canonical model candidate")
			}
			// Mutations are negative evidence models only, never live event output.
			snapshot := write["negotiation_snapshot"].(map[string]any)
			prefix := snapshot["successful_prefix"].(map[string]any)
			switch mode {
			case "missing_raw":
				prefix["hex"] = nil
			case "wrong_hash":
				prefix["sha256"] = strings.Repeat("0", 64)
			case "wrong_count":
				write["successful_prefix_bytes"] = 0
			case "foreign_owner":
				write["native_connection_id"] = "unit-foreign"
			case "foreign_stream":
				write["native_stream_id"] = int64(8)
			case "missing_frame":
				snapshot["frame_sequences"] = []int{s.frames[0]}
			case "wrong_frame_hash", "incomplete_frame":
				frame := pubsubQUICEvent(o, s.frames[1], "multistream_frame", "go.quic.native_stream.multistream")
				completion := frame["receipt"].(map[string]any)["write"].(map[string]any)
				if mode == "wrong_frame_hash" {
					completion["framed_sha256"] = strings.Repeat("0", 64)
				} else {
					completion["complete_frames"] = false
				}
			case "wrong_lazy_tail":
				snapshot["write"].(map[string]any)["lazy_tail"] = pubsubQUICCapturedBytes([]byte{0}, true)
			case "partial_buffer":
				s.negotiation[0].buffer = []byte{0x80}
			case "wrong_proposal":
				write["negotiation_snapshot"].(map[string]any)["proposal"] = "/meshsub/1.0.0"
			case "missing_begin_ack", "foreign_begin_ack":
				created := pubsubQUICEvent(o, s.receipt, "native_quic_stream", "go.quic.CapableConn.stream_return")
				if mode == "missing_begin_ack" {
					delete(created, "native_call_begin_prepare_ack_sequence")
				} else {
					created["native_call_begin_prepare_ack_sequence"] = o.prepareAck + 1
				}
			}
			if s.subscriptionWriteLocked(write) {
				t.Fatal("missing/foreign/noncanonical native bytes accepted", mode)
			}
		})
	}
}

func TestPubsubQUICNegotiationCleanupNeverBorrowsEarlierCauseOrExcusesOtherErrors(t *testing.T) {
	for _, mode := range []string{"nonzero", "wrapped", "wrapped_inner", "joined_errno", "different_cause", "absent_context", "remote_mismatch",
		"foreign_parent", "prior_failure", "pre_prepare_stream", "early_parent", "partial_candidate", "write_error", "opaque"} {
		t.Run(mode, func(t *testing.T) {
			o, s, raw, cancelParent := pubsubQUICUnitLateCandidate(t)
			if mode == "pre_prepare_stream" || mode == "early_parent" {
				o, s, raw, cancelParent = pubsubQUICUnit(t, network.DirOutbound)
				if mode == "early_parent" {
					cancelParent(&quic.ApplicationError{Remote: true})
					err := o.prepareShutdown(pubsubScoringCommand{Sequence: 1, Kind: "prepare_shutdown", Actor: o.actor,
						Token: o.token, Local: o.local.String()}, 0)
					if err == nil || o.prepared || len(pubsubScoringUnitEvents(o, "shutdown_prepared")) != 0 ||
						len(pubsubScoringUnitEvents(o, "native_quic_negotiation_cleanup_pending")) != 0 {
						t.Fatal("already closed parent acquired a live Prepare ACK", err)
					}
					return
				}
				pubsubQUICUnitPrepare(t, o)
			}
			tail := pubsubQUICUnitSubscription(t, o.topic)
			if mode == "partial_candidate" {
				tail = tail[:len(tail)-1]
			}
			pubsubQUICUnitCandidateWrite(t, o, s, tail)
			cause := &quic.ApplicationError{Remote: true}
			var failure error = &network.ConnError{Remote: true, TransportError: cause}
			if mode == "nonzero" {
				cause.ErrorCode = 9
			}
			if mode == "wrapped" {
				failure = fmt.Errorf("unit wrapper: %w", failure)
			}
			if mode == "wrapped_inner" {
				failure = &network.ConnError{Remote: true, TransportError: fmt.Errorf("unit wrapper: %w", cause)}
			}
			if mode == "joined_errno" {
				failure = errors.Join(failure, syscall.EIO)
			}
			if mode == "opaque" {
				failure = errors.New("unit opaque")
			}
			if mode == "remote_mismatch" {
				failure = &network.ConnError{TransportError: cause}
			}
			if mode != "absent_context" {
				if mode == "different_cause" {
					cancelParent(&quic.ApplicationError{Remote: true})
				} else {
					cancelParent(cause)
				}
			}
			if mode == "prior_failure" {
				o.fail(syscall.EPIPE)
			}
			if mode == "foreign_parent" {
				// A unit-only wrong-owner model; production never rewrites this map.
				o.quic.connections[s.connection.native] = &pubsubQUICConn{}
			}
			raw.input, raw.readErr = nil, failure
			if mode == "write_error" {
				raw.writeN, raw.writeErr = 0, failure
				if n, err := s.Write([]byte{0xff}); n != 0 || err != failure {
					t.Fatal(n, err)
				}
			} else if n, err := s.Read(make([]byte, 1)); n != 0 || err != failure {
				t.Fatal(n, err)
			}
			if o.failure == nil || len(pubsubScoringUnitEvents(o, "native_quic_negotiation_cleanup_pending")) != 0 || s.cleanup != nil {
				t.Fatal("other cause/state excused by candidate proposal", mode, o.failure)
			}
			if mode != "early_parent" && mode != "prior_failure" && o.failure != failure {
				t.Fatal("original active failure replaced", o.failure, failure)
			}
		})
	}
}

func TestPubsubQUICNegotiationCleanupDisposalAndJoinAreMandatoryAndSticky(t *testing.T) {
	for _, mode := range []string{"no_disposal", "half_close", "failed_reset", "failed_close", "inflight_reset", "late_partial",
		"foreign_disposal", "disposal_before_error", "sticky", "no_join"} {
		t.Run(mode, func(t *testing.T) {
			o, s, raw, cancelParent := pubsubQUICUnitLateCandidate(t)
			pubsubQUICUnitCandidateWrite(t, o, s, pubsubQUICUnitSubscription(t, o.topic))
			if mode == "disposal_before_error" {
				_ = s.Close()
			}
			failure := pubsubQUICUnitCandidateError(t, s, raw, cancelParent)
			if s.cleanup == nil || o.failure != nil {
				t.Fatal("missing pending candidate", o.failure)
			}
			switch mode {
			case "half_close":
				_ = s.CloseRead()
				_ = s.CloseWrite()
			case "failed_reset":
				raw.resetErr = syscall.EIO
				_ = s.Reset()
			case "failed_close":
				raw.closeErr = syscall.EIO
				_ = s.Close()
			case "no_disposal", "disposal_before_error":
			case "inflight_reset":
				raw.resetStart, raw.resetEnd = make(chan struct{}), make(chan struct{})
				resetDone, resetJoined := make(chan error, 1), make(chan struct{})
				var release sync.Once
				launched := false
				t.Cleanup(func() {
					release.Do(func() { close(raw.resetEnd) })
					if launched {
						<-resetJoined
					}
				})
				launched = true
				go func() { defer close(resetJoined); resetDone <- s.Reset() }()
				<-raw.resetStart
				if s.finalize() || len(pubsubScoringUnitEvents(o, "native_quic_negotiation_cleanup_finalized")) != 0 {
					t.Fatal("inflight native Reset became disposal/join")
				}
				release.Do(func() { close(raw.resetEnd) })
				if err := <-resetDone; err != nil {
					t.Fatal(err)
				}
				return // A completed later disposal is allowed only after actual join.
			default:
				_ = s.Reset()
				if mode == "late_partial" {
					raw.writeErr = nil
					_, _ = s.Write([]byte{0x80})
				}
				if mode == "foreign_disposal" {
					operation := pubsubQUICEvent(o, s.disposal, "native_stream_operation", "go.quic.native_stream.operation_return")
					operation["native_stream_id"] = int64(8) // Negative unit evidence only.
				}
				if mode == "sticky" {
					o.fail(syscall.EPIPE)
				}
			}
			if mode == "no_join" {
				if !s.finalize() || len(pubsubScoringUnitEvents(o, "native_quic_negotiation_cleanup_finalized")) != 0 {
					t.Fatal("framing-only observation became joined cleanup")
				}
				return
			}
			ctx, cancel := context.WithTimeout(context.Background(), time.Second)
			defer cancel()
			if err := o.quic.join(ctx); err != nil {
				t.Fatal(err)
			}
			final := pubsubScoringUnitEvents(o, "native_quic_negotiation_cleanup_finalized")
			if o.failure == nil || len(final) != 1 || final[0]["accepted"] != false {
				t.Fatal("missing/failed/foreign disposal or sticky failure accepted", mode, o.failure, final)
			}
			if mode == "sticky" && o.failure != syscall.EPIPE {
				t.Fatal("earlier sticky failure cleared", o.failure)
			}
			if (mode == "no_disposal" || mode == "half_close") && o.failure != failure {
				t.Fatal("original pending native error lost", o.failure)
			}
		})
	}
}

func TestPubsubQUICNegotiationCleanupCannotBorrowContextAfterSealedReturn(t *testing.T) {
	o, s, raw, cancelParent := pubsubQUICUnitLateCandidate(t)
	pubsubQUICUnitCandidateWrite(t, o, s, pubsubQUICUnitSubscription(t, o.topic))
	cause := &quic.ApplicationError{Remote: true}
	failure := &network.ConnError{Remote: true, TransportError: cause}
	raw.input, raw.readErr = nil, failure
	delayed := &pubsubQUICUnitDelayedContext{Context: raw.ctx, entered: make(chan struct{}), release: make(chan struct{})}
	s.sendContext = delayed
	done, joined := make(chan error, 1), make(chan struct{})
	var release sync.Once
	t.Cleanup(func() { release.Do(func() { close(delayed.release) }); <-joined })
	go func() {
		defer close(joined)
		_, err := s.Read(make([]byte, 1))
		done <- err
	}()
	<-delayed.entered // The actual Read RETURN sealed a live parent Context.
	cancelParent(cause)
	release.Do(func() { close(delayed.release) })
	if err := <-done; err != failure {
		t.Fatal("native I/O error changed", err)
	}
	if o.failure != failure || s.cleanup != nil ||
		len(pubsubScoringUnitEvents(o, "native_quic_negotiation_cleanup_pending")) != 0 {
		t.Fatal("later current Context authorized an earlier native RETURN", o.failure)
	}
	returns := pubsubScoringUnitEvents(o, "native_quic_negotiation_io_return")
	last := returns[len(returns)-1]
	if last["connection_context"].(map[string]any)["done"] != false ||
		last["same_native_connection_context_cause"] != false || last["outcome"] != "error" {
		t.Fatal("sealed native parent Context receipt was rewritten", last)
	}
}

type pubsubQUICUnitRegistrationPause struct {
	*pubsubQUICUnitStream
	once             sync.Once
	entered, release chan struct{}
}

func (s *pubsubQUICUnitRegistrationPause) Context() context.Context {
	s.once.Do(func() {
		close(s.entered)
		<-s.release
	})
	return s.pubsubQUICUnitStream.Context()
}

func TestPubsubQUICStreamBeginCannotBorrowPrepareDuringPostReturnRegistration(t *testing.T) {
	for _, method := range []string{"open", "accept"} {
		t.Run(method, func(t *testing.T) {
			o, original, _, cancelParent := pubsubQUICUnit(t, network.DirOutbound)
			ctx, cancelSend := context.WithCancelCause(context.Background())
			t.Cleanup(func() { cancelSend(context.Canceled) })
			raw := &pubsubQUICUnitStream{ctx: ctx, cancel: cancelSend, id: 4, writeN: -1}
			paused := &pubsubQUICUnitRegistrationPause{pubsubQUICUnitStream: raw,
				entered: make(chan struct{}), release: make(chan struct{})}
			original.connection.CapableConn.(*pubsubQUICUnitCapable).stream = paused
			done, joined := make(chan network.MuxedStream, 1), make(chan struct{})
			var release sync.Once
			t.Cleanup(func() { release.Do(func() { close(paused.release) }); <-joined })
			go func() {
				defer close(joined)
				var output network.MuxedStream
				var err error
				if method == "open" {
					output, err = original.connection.OpenStream(context.Background())
				} else {
					output, err = original.connection.AcceptStream()
				}
				if err != nil {
					done <- nil
					return
				}
				done <- output
			}()
			<-paused.entered // Delegate RETURN occurred; its public getter now blocks registration.
			pubsubQUICUnitPrepare(t, o)
			release.Do(func() { close(paused.release) })
			s, ok := (<-done).(*pubsubQUICStream)
			if !ok {
				t.Fatal("native delegate return was lost")
			}
			created := pubsubQUICEvent(o, s.receipt, "native_quic_stream", "go.quic.CapableConn.stream_return")
			o.quic.mu.Lock()
			_, streamInPrepare := o.quic.baseline[pubsubQUICKey{s.connection.native, s.nativeID}]
			parentBase, parentInPrepare := o.quic.connBase[s.connection.native]
			o.quic.mu.Unlock()
			if created["prepare_ack_sequence"] != o.prepareAck || s.createdAck != o.prepareAck ||
				created["native_call_begin_prepare_ack_sequence"] != 0 || s.nativeBeginAck != 0 ||
				streamInPrepare || !parentInPrepare || parentBase.done || s.latePreparedOwner() {
				t.Fatal("registration borrowed a future Prepare for the earlier native BEGIN", created)
			}
			if method == "open" {
				pubsubQUICUnitCandidateWrite(t, o, s, pubsubQUICUnitSubscription(t, o.topic))
				failure := pubsubQUICUnitCandidateError(t, s, raw, cancelParent)
				if o.failure != failure || s.candidateWrite != 0 || s.cleanup != nil ||
					len(pubsubScoringUnitEvents(o, "native_quic_negotiation_cleanup_pending")) != 0 {
					t.Fatal("complete lazy bytes excused a pre-Prepare native OpenStream", o.failure)
				}
			}
		})
	}
}
