//! Application fixture bytes, not a donor router implementation.
use std::io;

use super::{invalid, lower_hex};

pub(super) const PARTS: usize = 3;
pub(super) const MASK: u8 = (1 << PARTS) - 1;
const MAX_PART: usize = 256;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(super) struct Metadata {
    pub revision: u32,
    pub have: u8,
    pub want: u8,
}

impl Metadata {
    pub fn replace(&mut self, next: Self) -> io::Result<bool> {
        next.encode()?;
        if next.revision < self.revision || next.revision == self.revision && next != *self {
            return Err(invalid("stale or conflicting application partial metadata"));
        }
        let changed = next != *self;
        *self = next;
        Ok(changed)
    }

    pub fn encode(self) -> io::Result<[u8; 7]> {
        if self.revision == 0 || self.have > MASK || self.want > MASK || self.have & self.want != 0
        {
            return Err(invalid("invalid partial fixture metadata"));
        }
        let mut result = [0; 7];
        result[0] = 1;
        result[1..5].copy_from_slice(&self.revision.to_be_bytes());
        result[5] = self.have;
        result[6] = self.want;
        Ok(result)
    }

    pub fn decode(encoded: &[u8]) -> io::Result<Self> {
        if encoded.len() != 7 || encoded[0] != 1 {
            return Err(invalid("invalid partial fixture metadata header"));
        }
        let value = Self {
            revision: u32::from_be_bytes(
                encoded[1..5].try_into().expect("checked metadata length"),
            ),
            have: encoded[5],
            want: encoded[6],
        };
        value.encode()?;
        Ok(value)
    }
}

pub(super) fn group_id(token: &str, sequence: u32) -> io::Result<Vec<u8>> {
    if !lower_hex(token, 32) || sequence == 0 {
        return Err(invalid("invalid partial fixture group"));
    }
    let mut result = Vec::with_capacity(20);
    for offset in (0..token.len()).step_by(2) {
        result.push(
            u8::from_str_radix(&token[offset..offset + 2], 16).expect("checked canonical hex"),
        );
    }
    result.extend_from_slice(&sequence.to_be_bytes());
    Ok(result)
}

pub(super) fn part_data(token: &str, index: u8) -> io::Result<Vec<u8>> {
    if !lower_hex(token, 32) || usize::from(index) >= PARTS {
        return Err(invalid("invalid partial fixture content identity"));
    }
    Ok(format!("forge-pr12:{token}:part-{index}").into_bytes())
}

pub(super) fn encode_part(index: u8, data: &[u8]) -> io::Result<Vec<u8>> {
    if usize::from(index) >= PARTS || data.is_empty() || data.len() > MAX_PART {
        return Err(invalid("invalid partial fixture part"));
    }
    let mut result = Vec::with_capacity(4 + data.len());
    result.extend_from_slice(&[1, index]);
    result.extend_from_slice(&(data.len() as u16).to_be_bytes());
    result.extend_from_slice(data);
    Ok(result)
}

pub(super) fn decode_part(encoded: &[u8]) -> io::Result<(u8, &[u8])> {
    if !(5..=MAX_PART + 4).contains(&encoded.len())
        || encoded[0] != 1
        || usize::from(encoded[1]) >= PARTS
        || usize::from(u16::from_be_bytes([encoded[2], encoded[3]])) != encoded.len() - 4
    {
        return Err(invalid("invalid partial fixture part header"));
    }
    Ok((encoded[1], &encoded[4..]))
}

pub(super) fn reconstruct(token: &str, parts: &[Vec<u8>]) -> io::Result<Vec<u8>> {
    if !lower_hex(token, 32) || parts.len() != PARTS {
        return Err(invalid(
            "partial fixture requires three parts and a canonical token",
        ));
    }
    let mut decoded = [None; PARTS];
    for encoded in parts {
        let (index, data) = decode_part(encoded)?;
        if decoded[usize::from(index)].is_some() || data != part_data(token, index)? {
            return Err(invalid("duplicate or corrupt partial fixture part"));
        }
        decoded[usize::from(index)] = Some(data);
    }
    Ok(decoded.into_iter().flatten().flatten().copied().collect())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pubsub_scoring::observer::hex;

    const TOKEN: &str = "00112233445566778899aabbccddeeff";

    #[test]
    fn golden_format() {
        assert_eq!(
            hex(&group_id(TOKEN, 1).unwrap()),
            "00112233445566778899aabbccddeeff00000001"
        );
        let metadata = Metadata {
            revision: 0x01020304,
            have: 5,
            want: 2,
        };
        assert_eq!(hex(&metadata.encode().unwrap()), "01010203040502");
        assert_eq!(
            Metadata::decode(&metadata.encode().unwrap()).unwrap(),
            metadata
        );
        assert_eq!(hex(&encode_part(2, b"abc").unwrap()), "01020003616263");
        assert_eq!(
            decode_part(&[1, 1, 0, 3, b'a', b'b', b'c']).unwrap(),
            (1, b"abc".as_slice())
        );
        let parts = (0..PARTS as u8)
            .map(|index| encode_part(index, &part_data(TOKEN, index).unwrap()).unwrap())
            .rev()
            .collect::<Vec<_>>();
        let expected = (0..PARTS as u8)
            .flat_map(|index| part_data(TOKEN, index).unwrap())
            .collect::<Vec<_>>();
        assert_eq!(reconstruct(TOKEN, &parts).unwrap(), expected);
        assert!(reconstruct(TOKEN, &parts[..2]).is_err());
        assert!(reconstruct(TOKEN, &vec![parts[0].clone(); 3]).is_err());
        let mut corrupt = parts.clone();
        corrupt[0] = encode_part(2, b"corrupt").unwrap();
        assert!(reconstruct(TOKEN, &corrupt).is_err());
    }

    #[test]
    fn rejects_malformed_state() {
        for metadata in [
            Metadata {
                revision: 0,
                have: 0,
                want: 7,
            },
            Metadata {
                revision: 1,
                have: 8,
                want: 0,
            },
            Metadata {
                revision: 1,
                have: 0,
                want: 8,
            },
            Metadata {
                revision: 1,
                have: 1,
                want: 1,
            },
        ] {
            assert!(metadata.encode().is_err());
        }
        for data in [
            b"".as_slice(),
            &[2, 0, 0, 0, 1, 0, 7],
            &[1, 0, 0, 0, 0, 1, 2],
            &[1, 0, 0, 0, 1, 1, 1],
            &[1, 0, 0, 0, 1, 8, 0],
            &[1, 0, 0, 0, 1, 0, 7, 0],
        ] {
            assert!(Metadata::decode(data).is_err());
        }
        for data in [
            b"".as_slice(),
            &[1, 0, 0, 0],
            &[2, 0, 0, 1, b'x'],
            &[1, 3, 0, 1, b'x'],
            &[1, 0, 0, 2, b'x'],
        ] {
            assert!(decode_part(data).is_err());
        }
        assert!(encode_part(0, b"").is_err());
        assert!(encode_part(0, &[0; MAX_PART + 1]).is_err());
        assert!(group_id(TOKEN, 0).is_err());
        assert!(group_id(&TOKEN.to_uppercase(), 1).is_err());
        assert!(group_id("", 1).is_err());
    }

    #[test]
    fn revisions_replace_bitmaps_without_union_and_reject_rollback() {
        let mut metadata = Metadata {
            revision: 1,
            have: 5,
            want: 2,
        };
        assert!(!metadata.replace(metadata).unwrap());
        let newer = Metadata {
            revision: 2,
            have: 2,
            want: 5,
        };
        assert!(metadata.replace(newer).unwrap());
        assert_eq!(hex(&metadata.encode().unwrap()), "01000000020205");
        for rejected in [
            Metadata {
                revision: 1,
                have: 5,
                want: 2,
            },
            Metadata {
                revision: 2,
                have: 5,
                want: 2,
            },
            Metadata {
                revision: 3,
                have: 8,
                want: 0,
            },
        ] {
            assert!(metadata.replace(rejected).is_err());
            assert_eq!(metadata, newer);
        }
    }
}
