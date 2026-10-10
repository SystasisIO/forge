package main

import (
	"bytes"
	"encoding/hex"
	"testing"
)

func TestPubsubPartialGoldenFormat(t *testing.T) {
	const token = "00112233445566778899aabbccddeeff"
	group, err := pubsubPartialGroup(token, 1)
	if err != nil || hex.EncodeToString(group) != "00112233445566778899aabbccddeeff00000001" {
		t.Fatalf("group: %x %v", group, err)
	}
	metadata, err := (pubsubPartialMetadata{0x01020304, 5, 2}).encode()
	if err != nil || hex.EncodeToString(metadata) != "01010203040502" {
		t.Fatalf("metadata: %x %v", metadata, err)
	}
	if value, err := decodePubsubPartialMetadata(metadata); err != nil || value != (pubsubPartialMetadata{0x01020304, 5, 2}) {
		t.Fatalf("metadata decode: %#v %v", value, err)
	}
	encoded, err := encodePubsubPartial(2, []byte("abc"))
	if err != nil || hex.EncodeToString(encoded) != "01020003616263" {
		t.Fatalf("part: %x %v", encoded, err)
	}
	if index, data, err := decodePubsubPartial(encoded); err != nil || index != 2 || string(data) != "abc" {
		t.Fatalf("part decode: %d %q %v", index, data, err)
	}
	var parts [][]byte
	var expected []byte
	for index := byte(0); index < pubsubPartialParts; index++ {
		data, _ := pubsubPartialData(token, index)
		part, _ := encodePubsubPartial(index, data)
		parts = append([][]byte{part}, parts...)
		expected = append(expected, data...)
	}
	if value, err := reconstructPubsubPartial(token, parts); err != nil || !bytes.Equal(value, expected) {
		t.Fatalf("reconstruct: %q %v", value, err)
	}
	corrupt := bytes.Clone(parts[2]) // Index 0; retain three distinct indices.
	corrupt[len(corrupt)-1] ^= 1
	for name, bad := range map[string][][]byte{
		"incomplete": parts[:2],
		"duplicate":  {parts[0], parts[0], parts[1]},
		"corrupt":    {parts[0], parts[1], corrupt},
	} {
		t.Run(name, func(t *testing.T) {
			if _, err := reconstructPubsubPartial(token, bad); err == nil {
				t.Fatal("accepted", name, "parts")
			}
		})
	}
}

func TestPubsubPartialRejectsMalformed(t *testing.T) {
	for _, value := range []pubsubPartialMetadata{{0, 0, 7}, {1, 8, 0}, {1, 0, 8}, {1, 1, 1}} {
		if _, err := value.encode(); err == nil {
			t.Fatalf("accepted metadata %#v", value)
		}
	}
	for _, text := range []string{"", "02000000010007", "01000000000102", "01000000010101", "01000000010800", "0100000001000700"} {
		data, _ := hex.DecodeString(text)
		if _, err := decodePubsubPartialMetadata(data); err == nil {
			t.Fatalf("accepted metadata %x", data)
		}
	}
	for _, text := range []string{"", "01000000", "0200000178", "0103000178", "0100000278"} {
		data, _ := hex.DecodeString(text)
		if _, _, err := decodePubsubPartial(data); err == nil {
			t.Fatalf("accepted part %x", data)
		}
	}
	for _, size := range []int{0, 257} {
		if _, err := encodePubsubPartial(0, make([]byte, size)); err == nil {
			t.Fatalf("accepted part size %d", size)
		}
	}
	for _, token := range []string{"", "00112233445566778899AABBCCDDEEFF", "x0112233445566778899aabbccddeeff"} {
		if _, err := pubsubPartialGroup(token, 1); err == nil {
			t.Fatalf("accepted token %q", token)
		}
	}
	if _, err := pubsubPartialGroup("00112233445566778899aabbccddeeff", 0); err == nil {
		t.Fatal("accepted zero group sequence")
	}
}
