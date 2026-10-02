package main

import (
	"bytes"
	"encoding/binary"
	"testing"

	relaypb "github.com/libp2p/go-libp2p/p2p/protocol/circuitv2/pb"
	"google.golang.org/protobuf/proto"
)

func TestAutoRelayCapturedNativeHopFrame(t *testing.T) {
	message := &relaypb.HopMessage{Type: relaypb.HopMessage_STATUS.Enum(), Status: relaypb.Status_OK.Enum(),
		Reservation: &relaypb.Reservation{Expire: proto.Uint64(1700000008)}}
	payload, err := proto.Marshal(message)
	if err != nil {
		t.Fatal(err)
	}
	frame := binary.AppendUvarint(nil, uint64(len(payload)))
	frame = append(frame, payload...)
	decoded, err := autoRelayHopMessage(frame)
	if err != nil || decoded.GetStatus() != relaypb.Status_OK || decoded.GetReservation().GetExpire() != 1700000008 {
		t.Fatalf("native frame lost status/expiry: %v %v", decoded, err)
	}
	for _, bad := range [][]byte{nil, frame[:len(frame)-1], append(append([]byte{}, frame...), 0), {0x80},
		append([]byte{byte(len(payload)) | 0x80, 0}, payload...), binary.AppendUvarint(nil, 4097)} {
		if _, err := autoRelayHopMessage(bad); err == nil {
			t.Fatalf("accepted malformed/noncanonical/truncated capture %x", bad)
		}
	}
}

func TestAutoRelayCaptureSnapshotIsBoundedAndIndependent(t *testing.T) {
	stream := &autoRelayObservedStream{}
	stream.input.Write([]byte{1, 2, 3})
	stream.output.Write([]byte{4, 5})
	input, output, overflow := stream.capture()
	if overflow || !stream.finished || !bytes.Equal(input, []byte{1, 2, 3}) || !bytes.Equal(output, []byte{4, 5}) {
		t.Fatal("capture did not close/snapshot delegated observations")
	}
	stream.input.Bytes()[0] = 9
	if input[0] != 1 {
		t.Fatal("native input snapshot aliases mutable capture")
	}
	trace := &autoRelayTrace{}
	for index := 0; index < 129; index++ {
		trace.record(map[string]any{"kind": "measured"})
	}
	events, overflow := trace.snapshot()
	if len(events) != 128 || !overflow {
		t.Fatal("trace overflow was not fail-closed")
	}
}

func TestAutoRelayShutdownClosesHandlerAdmissionBeforeJoining(t *testing.T) {
	trace := &autoRelayTrace{}
	if !trace.admit() {
		t.Fatal("native observer handler admission failed before shutdown")
	}
	trace.closeAdmission()
	if trace.admit() {
		t.Fatal("native observer admitted work after shutdown")
	}
	trace.handlers.Done()
	trace.handlers.Wait()
}
