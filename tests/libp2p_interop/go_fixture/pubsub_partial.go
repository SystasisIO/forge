package main

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"fmt"
)

// Application-only fixture format. The native router treats these bytes as opaque.
const pubsubPartialParts = 3
const pubsubPartialMaxPart = 256
const pubsubPartialMask = byte(1<<pubsubPartialParts - 1)

type pubsubPartialMetadata struct {
	revision uint32
	have     byte
	want     byte
}

func (m pubsubPartialMetadata) encode() ([]byte, error) {
	if m.revision == 0 || m.have > pubsubPartialMask || m.want > pubsubPartialMask || m.have&m.want != 0 {
		return nil, fmt.Errorf("invalid partial fixture metadata")
	}
	out := make([]byte, 7)
	out[0] = 1
	binary.BigEndian.PutUint32(out[1:5], m.revision)
	out[5], out[6] = m.have, m.want
	return out, nil
}

func decodePubsubPartialMetadata(encoded []byte) (pubsubPartialMetadata, error) {
	if len(encoded) != 7 || encoded[0] != 1 {
		return pubsubPartialMetadata{}, fmt.Errorf("invalid partial fixture metadata header")
	}
	value := pubsubPartialMetadata{binary.BigEndian.Uint32(encoded[1:5]), encoded[5], encoded[6]}
	_, err := value.encode()
	return value, err
}

func pubsubPartialGroup(token string, sequence uint32) ([]byte, error) {
	if !coordinatedHex(token, 32) || sequence == 0 {
		return nil, fmt.Errorf("invalid partial fixture group")
	}
	prefix, err := hex.DecodeString(token)
	if err != nil {
		return nil, err
	}
	out := make([]byte, 20)
	copy(out, prefix)
	binary.BigEndian.PutUint32(out[16:], sequence)
	return out, nil
}

func pubsubPartialData(token string, index byte) ([]byte, error) {
	if !coordinatedHex(token, 32) || index >= pubsubPartialParts {
		return nil, fmt.Errorf("invalid partial fixture content identity")
	}
	return []byte(fmt.Sprintf("forge-pr12:%s:part-%d", token, index)), nil
}

func encodePubsubPartial(index byte, data []byte) ([]byte, error) {
	if index >= pubsubPartialParts || len(data) == 0 || len(data) > pubsubPartialMaxPart {
		return nil, fmt.Errorf("invalid partial fixture part")
	}
	out := make([]byte, 4+len(data))
	out[0], out[1] = 1, index
	binary.BigEndian.PutUint16(out[2:4], uint16(len(data)))
	copy(out[4:], data)
	return out, nil
}

func decodePubsubPartial(encoded []byte) (byte, []byte, error) {
	if len(encoded) < 5 || len(encoded) > pubsubPartialMaxPart+4 || encoded[0] != 1 ||
		encoded[1] >= pubsubPartialParts || int(binary.BigEndian.Uint16(encoded[2:4])) != len(encoded)-4 {
		return 0, nil, fmt.Errorf("invalid partial fixture part header")
	}
	return encoded[1], encoded[4:], nil
}

func reconstructPubsubPartial(token string, parts [][]byte) ([]byte, error) {
	if !coordinatedHex(token, 32) || len(parts) != pubsubPartialParts {
		return nil, fmt.Errorf("partial fixture requires three parts and a canonical token")
	}
	var decoded [pubsubPartialParts][]byte
	for _, encoded := range parts {
		index, data, err := decodePubsubPartial(encoded)
		if err != nil {
			return nil, err
		}
		expected, err := pubsubPartialData(token, index)
		if err != nil || decoded[index] != nil || !bytes.Equal(data, expected) {
			return nil, fmt.Errorf("duplicate or corrupt partial fixture part")
		}
		decoded[index] = data
	}
	return bytes.Join(decoded[:], nil), nil
}
