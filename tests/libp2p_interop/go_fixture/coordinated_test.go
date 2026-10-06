package main

import (
	"context"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/p2p/muxer/yamux"
	ma "github.com/multiformats/go-multiaddr"
)

func coordinatedTestArgs() []string {
	return []string{"--scenario", coordinatedScenario, "--transport", "tcp", "--coord-role", "initiator",
		"--case-token", strings.Repeat("a", 32), "--bind-ip", "127.0.0.1", "--ready-file", "ready",
		"--result-file", "result", "--stop-file", "stop", "--control-file", "control", "--plan-file", "plan"}
}

func TestCoordinatedCLIRequiresExplicitNativeOrPrivateContract(t *testing.T) {
	args, err := parseCoordinatedArgs(coordinatedTestArgs())
	if err != nil || args["timeout-ms"] != "20000" {
		t.Fatal("shared native CLI/default budget rejected", err)
	}
	private := coordinatedTestArgs()
	private[1], private[3] = coordinatedScenario+"_private_pnet", "tcp-pnet-noise"
	if _, err := parseCoordinatedArgs(private); err == nil {
		t.Fatal("private profile accepted without PSK provenance")
	}
	private = append(private, "--pnet-key-file", "key", "--pnet-fingerprint", strings.Repeat("b", 64))
	if _, err := parseCoordinatedArgs(private); err != nil {
		t.Fatal("shared private CLI rejected", err)
	}
	for _, bad := range [][]string{
		append(coordinatedTestArgs(), "--transport", "tcp"),
		append(coordinatedTestArgs(), "--unknown", "value"),
		append(coordinatedTestArgs(), "--timeout-ms", "0"),
		append(coordinatedTestArgs(), "--pnet-key-file", "key"),
		append(coordinatedTestArgs(), "--pnet-fingerprint", strings.Repeat("b", 64)),
		append(coordinatedTestArgs(), "--incomplete"),
	} {
		if _, err := parseCoordinatedArgs(bad); err == nil {
			t.Fatal("invalid coordinated flags accepted", bad)
		}
	}
	for _, bad := range []struct {
		index int
		value string
	}{{5, "source"}, {7, "not-a-token"}, {9, "0.0.0.0"}} {
		argv := coordinatedTestArgs()
		argv[bad.index] = bad.value
		if _, err := parseCoordinatedArgs(argv); err == nil {
			t.Fatal("invalid actor role/token/source accepted")
		}
	}
}

func TestCoordinatedControlIsBoundedAndTokenScoped(t *testing.T) {
	root := t.TempDir()
	control := filepath.Join(root, "control")
	args, _ := parseCoordinatedArgs(coordinatedTestArgs())
	args["control-file"], args["stop-file"] = control, filepath.Join(root, "stop")
	put := func(value string) {
		t.Helper()
		if err := os.WriteFile(control, []byte(value), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	put("case-token=" + args["case-token"] + "\nsequence=1\naction=start\n")
	if err := coordinatedGate(args, "start", 1); err != nil {
		t.Fatal(err)
	}
	put("case-token=" + strings.Repeat("b", 32) + "\nsequence=1\naction=start\n")
	if err := coordinatedGate(args, "start", 1); err == nil {
		t.Fatal("foreign case gate admitted")
	}
	put("case-token=" + args["case-token"] + "\nsequence=2\naction=probe\n")
	if err := coordinatedGate(args, "start", 1); err == nil {
		t.Fatal("probe admitted before start")
	}
	put("action=start\naction=start\n")
	if _, err := coordinatedFields(control); err == nil {
		t.Fatal("duplicate control field admitted")
	}
	put(strings.Repeat("a", 8193))
	if _, err := coordinatedFields(control); err == nil {
		t.Fatal("oversized control admitted")
	}
}

func TestCoordinatedDialerRejectsOrdinaryOrWrongRoleContextBeforeSocket(t *testing.T) {
	trace := &coordinatedTrace{source: &net.TCPAddr{IP: net.ParseIP("127.0.0.1"), Port: 4100},
		remote: ma.StringCast("/ip4/127.0.0.1/tcp/4200"), token: strings.Repeat("a", 32), client: true}
	dialer := &coordinatedDialer{trace: trace}
	for _, ctx := range []context.Context{
		context.Background(), network.WithSimultaneousConnect(context.Background(), false, trace.token),
		network.WithSimultaneousConnect(context.Background(), true, "foreign"),
	} {
		if c, err := dialer.DialContext(ctx, "tcp4", "127.0.0.1:4200"); err == nil || c != nil {
			t.Fatal("ordinary/wrong-role native socket admitted")
		}
	}
	ctx := network.WithSimultaneousConnect(context.Background(), true, trace.token)
	if c, err := dialer.DialContext(ctx, "tcp4", "127.0.0.1:4201"); err == nil || c != nil {
		t.Fatal("another remote socket admitted")
	}
	if len(trace.dials) != 0 {
		t.Fatal("rejected context manufactured a native dial receipt")
	}
}

// Synthetic adapters test receipt rejection only; they are not live acceptance.
type coordinatedReceiptConn struct {
	network.Conn
	outgoing bool
	roles    coordinatedRoles
	state    network.ConnectionState
}

func (c *coordinatedReceiptConn) As(target any) bool {
	if out, ok := target.(**coordinatedCapableConn); ok {
		if !c.outgoing {
			return false
		}
		*out = &coordinatedCapableConn{outgoing: true}
		return true
	}
	if out, ok := target.(**coordinatedMuxedConn); ok {
		*out = &coordinatedMuxedConn{roles: c.roles}
		return true
	}
	return false
}
func (c *coordinatedReceiptConn) ConnState() network.ConnectionState { return c.state }
func (*coordinatedReceiptConn) ID() string                           { return "synthetic-owner" }
func (*coordinatedReceiptConn) LocalPeer() peer.ID                   { return peer.ID("local") }
func (*coordinatedReceiptConn) RemotePeer() peer.ID                  { return peer.ID("remote") }
func (*coordinatedReceiptConn) LocalMultiaddr() ma.Multiaddr {
	return ma.StringCast("/ip4/127.0.0.1/tcp/4100")
}
func (*coordinatedReceiptConn) RemoteMultiaddr() ma.Multiaddr {
	return ma.StringCast("/ip4/127.0.0.1/tcp/4200")
}
func (*coordinatedReceiptConn) IsClosed() bool { return false }
func (*coordinatedReceiptConn) Stat() network.ConnStats {
	return network.ConnStats{Stats: network.Stats{Direction: network.DirOutbound}}
}

func TestCoordinatedInitiatorCannotRelabelAcceptedCollisionOrConfiguredRole(t *testing.T) {
	trace := &coordinatedTrace{source: &net.TCPAddr{IP: net.ParseIP("127.0.0.1"), Port: 4100},
		remote: ma.StringCast("/ip4/127.0.0.1/tcp/4200"), expected: peer.ID("remote"), client: true}
	c := &coordinatedReceiptConn{outgoing: true, roles: coordinatedRoles{Security: "initiator", Yamux: "initiator"},
		state: network.ConnectionState{Transport: "tcp", Security: "/noise", StreamMultiplexer: yamux.ID}}
	if _, err := coordinatedWinner(c, trace, true); err != nil {
		t.Fatal("complete adapter receipt rejected", err)
	}
	c.outgoing = false
	if _, err := coordinatedWinner(c, trace, true); err == nil {
		t.Fatal("accepted collision relabeled as outgoing source")
	}
	c.outgoing, c.roles.Security = true, "responder"
	if _, err := coordinatedWinner(c, trace, true); err == nil {
		t.Fatal("requested role replaced contradictory actual security role")
	}
	c.roles.Security, c.roles.Yamux = "initiator", ""
	if _, err := coordinatedWinner(c, trace, true); err == nil {
		t.Fatal("configured Yamux replaced absent role observation")
	}
	c.roles = coordinatedRoles{Security: "responder", Yamux: "responder"}
	trace.client = false
	if proof, err := coordinatedWinner(c, trace, false); err != nil || proof["physical_direction"] != "outbound" {
		t.Fatal("outgoing socket/security responder composition was collapsed", proof, err)
	}
}
