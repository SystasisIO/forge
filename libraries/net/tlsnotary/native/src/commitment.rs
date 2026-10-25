// Versioned private projection of upstream plaintext hash commitments. Hashing,
// random blinders and MPC remain exclusively owned by the pinned TLSNotary API.
use crate::protocol::{Cursor, Failure, MAGIC_RETAINED, Result, put_blob, put_u32, ranges};
use rangeset::set::RangeSet;
use serde::{
    Deserialize,
    de::value::{Error as DecodeError, SeqDeserializer},
};
use tlsn::{
    hash::{Blinder, HashAlgId, HashProvider},
    transcript::{
        Direction, Transcript, TranscriptCommitConfig, TranscriptCommitRequest,
        TranscriptCommitment, TranscriptCommitmentKind, TranscriptSecret,
        hash::{PlaintextHash, hash_plaintext},
    },
};

const MAX_COMMITMENTS: usize = 16;
const MAX_SELECTED: usize = 65536;

#[derive(Clone)]
pub struct Selection {
    direction: Direction,
    ranges: Vec<(u32, u32)>,
}

pub struct ResolvedSelection {
    direction: Direction,
    idx: RangeSet<usize>,
}

fn direction(c: &mut Cursor<'_>) -> Result<Direction> {
    let direction = match c.u8()? {
        1 => Direction::Sent,
        2 => Direction::Received,
        _ => return Err(Failure::Configuration),
    };
    if c.u8()? != HashAlgId::SHA256.as_u8() || c.bytes(2)? != [0, 0] {
        return Err(Failure::Configuration);
    }
    Ok(direction)
}

fn canonical(ranges: &[(u32, u32)]) -> Result<()> {
    if ranges.is_empty() || ranges.windows(2).any(|parts| parts[1].0 <= parts[0].1) {
        return Err(Failure::Configuration);
    }
    Ok(())
}

impl Selection {
    pub fn parse_vector(c: &mut Cursor<'_>) -> Result<Vec<Self>> {
        let count = c.u32()? as usize;
        if count > MAX_COMMITMENTS {
            return Err(Failure::Limit);
        }
        let mut selections = Vec::with_capacity(count);
        for _ in 0..count {
            let direction = direction(c)?;
            let ranges = c.ranges()?;
            canonical(&ranges)?;
            selections.push(Self { direction, ranges });
        }
        Ok(selections)
    }
}

pub fn resolve(
    selections: &[Selection],
    sent: usize,
    recv: usize,
) -> Result<Vec<ResolvedSelection>> {
    let mut resolved: Vec<ResolvedSelection> = Vec::with_capacity(selections.len());
    let mut selected = [0usize; 2];
    for selection in selections {
        let (length, total) = match selection.direction {
            Direction::Sent => (sent, &mut selected[0]),
            Direction::Received => (recv, &mut selected[1]),
        };
        let ranges = ranges(&selection.ranges, length)?;
        if ranges.iter().any(|range| range.is_empty()) {
            return Err(Failure::Configuration);
        }
        let idx = RangeSet::new_from_slice(&ranges);
        if idx.is_empty()
            || resolved
                .iter()
                .any(|value| value.direction == selection.direction && value.idx == idx)
        {
            return Err(Failure::Configuration);
        }
        for range in idx.iter() {
            *total = total.checked_add(range.len()).ok_or(Failure::Limit)?;
        }
        if *total > MAX_SELECTED {
            return Err(Failure::Limit);
        }
        resolved.push(ResolvedSelection {
            direction: selection.direction,
            idx,
        });
    }
    Ok(resolved)
}

pub fn config(
    transcript: &Transcript,
    selections: &[ResolvedSelection],
) -> Result<TranscriptCommitConfig> {
    let mut builder = TranscriptCommitConfig::builder(transcript);
    for selection in selections {
        builder
            .commit_with_kind(
                &selection.idx,
                selection.direction,
                TranscriptCommitmentKind::Hash {
                    alg: HashAlgId::SHA256,
                },
            )
            .map_err(|_| Failure::Configuration)?;
    }
    builder.build().map_err(|_| Failure::Configuration)
}

// Check before accepting the request: a peer may not introduce extra expensive
// commitments, change algorithms or select its own transcript ranges.
pub fn matches_request(
    request: Option<&TranscriptCommitRequest>,
    selections: &[ResolvedSelection],
) -> bool {
    let Some(request) = request else {
        return selections.is_empty();
    };
    let mut requested = request.iter_hash();
    for selection in selections {
        if !requested.next().is_some_and(|(direction, idx, alg)| {
            *direction == selection.direction && *idx == selection.idx && *alg == HashAlgId::SHA256
        }) {
            return false;
        }
    }
    requested.next().is_none()
}

fn authenticated<'a>(
    hash: &'a TranscriptCommitment,
    selection: &ResolvedSelection,
) -> Result<&'a PlaintextHash> {
    let TranscriptCommitment::Hash(hash) = hash else {
        return Err(Failure::Verification);
    };
    if hash.direction != selection.direction
        || hash.idx != selection.idx
        || hash.hash.alg != HashAlgId::SHA256
        || hash.hash.value.as_bytes().len() != 32
    {
        return Err(Failure::Verification);
    }
    Ok(hash)
}

fn put_selection(output: &mut Vec<u8>, hash: &PlaintextHash) {
    output.push(match hash.direction {
        Direction::Sent => 1,
        Direction::Received => 2,
    });
    output.push(hash.hash.alg.as_u8());
    output.extend_from_slice(&[0, 0]);
    put_u32(output, hash.idx.iter().count());
    for range in hash.idx.iter() {
        put_u32(output, range.start);
        put_u32(output, range.end);
    }
    put_blob(output, hash.hash.value.as_bytes());
}

pub fn append(
    output: &mut Vec<u8>,
    commitments: &[TranscriptCommitment],
    secrets: Option<(&[TranscriptSecret], &Transcript)>,
    selections: &[ResolvedSelection],
) -> Result<()> {
    if commitments.len() != selections.len() {
        return Err(Failure::Verification);
    }
    put_u32(output, commitments.len());
    for (hash, selection) in commitments.iter().zip(selections) {
        put_selection(output, authenticated(hash, selection)?);
    }
    let Some((secrets, transcript)) = secrets else {
        put_u32(output, 0);
        return Ok(());
    };
    if secrets.len() != commitments.len() {
        return Err(Failure::Verification);
    }
    put_u32(output, secrets.len());
    let provider = HashProvider::default();
    let hasher = provider
        .get(&HashAlgId::SHA256)
        .map_err(|_| Failure::Internal)?;
    for (index, ((secret, commitment), selection)) in
        secrets.iter().zip(commitments).zip(selections).enumerate()
    {
        let TranscriptSecret::Hash(secret) = secret else {
            return Err(Failure::Verification);
        };
        let hash = authenticated(commitment, selection)?;
        if secret.direction != hash.direction
            || secret.idx != hash.idx
            || secret.alg != hash.hash.alg
        {
            return Err(Failure::Verification);
        }
        let bytes = match secret.direction {
            Direction::Sent => transcript.sent(),
            Direction::Received => transcript.received(),
        };
        let mut plaintext = Vec::new();
        for range in secret.idx.iter() {
            plaintext.extend_from_slice(bytes.get(range).ok_or(Failure::Verification)?);
        }
        if hash_plaintext(hasher, &plaintext, &secret.blinder) != hash.hash {
            return Err(Failure::Verification);
        }
        put_u32(output, index);
        put_blob(output, &plaintext);
        put_blob(output, secret.blinder.as_bytes());
    }
    Ok(())
}

// Only the selected opening is checked. The caller must retain/authenticate the
// original MPC result; passing an arbitrary hash is not proof of a TLS source.
pub fn check(input: &[u8]) -> Result<()> {
    let mut c = Cursor::new(input);
    if c.u32()? != MAGIC_RETAINED || c.u8()? != 3 || c.bytes(3)? != [0, 0, 0] {
        return Err(Failure::Protocol);
    }
    let sent = c.u32()? as usize;
    let recv = c.u32()? as usize;
    if sent > MAX_SELECTED || recv > MAX_SELECTED {
        return Err(Failure::Limit);
    }
    let count = c.u32()? as usize;
    if !(1..=MAX_COMMITMENTS).contains(&count) {
        return Err(Failure::Limit);
    }
    let mut selections = Vec::with_capacity(count);
    let mut digests = Vec::with_capacity(count);
    for _ in 0..count {
        let direction = direction(&mut c)?;
        let ranges = c.ranges()?;
        canonical(&ranges)?;
        if ranges.iter().any(|(_, end)| *end == crate::protocol::END) {
            return Err(Failure::Configuration);
        }
        let digest = c.blob(32)?;
        if digest.len() != 32 {
            return Err(Failure::Configuration);
        }
        selections.push(Selection { direction, ranges });
        digests.push(digest);
    }
    let selections = resolve(&selections, sent, recv)?;
    if c.u32()? != 1 {
        return Err(Failure::Configuration);
    }
    let index = c.u32()? as usize;
    let selection = selections.get(index).ok_or(Failure::Configuration)?;
    let plaintext = c.blob(MAX_SELECTED)?;
    if plaintext.len() != selection.idx.iter().map(|range| range.len()).sum::<usize>() {
        return Err(Failure::Verification);
    }
    let blinder = c.blob(16)?;
    if blinder.len() != 16 || !c.finished() {
        return Err(Failure::Protocol);
    }
    // Blinder intentionally has no public raw constructor. Its existing upstream
    // serde implementation decodes exactly the fixed 16-byte value, without
    // depending on a Rust native layout or introducing a serialized proof format.
    let blinder = Blinder::deserialize(SeqDeserializer::<_, DecodeError>::new(std::iter::once(
        blinder,
    )))
    .map_err(|_| Failure::Protocol)?;
    let provider = HashProvider::default();
    let hasher = provider
        .get(&HashAlgId::SHA256)
        .map_err(|_| Failure::Internal)?;
    if hash_plaintext(hasher, &plaintext, &blinder)
        .value
        .as_bytes()
        != digests[index]
    {
        return Err(Failure::Verification);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use tlsn::{hash::TypedHash, transcript::hash::PlaintextHashSecret};

    fn fixture() -> (Transcript, Vec<TranscriptCommitment>, Vec<TranscriptSecret>) {
        let transcript =
            Transcript::new(b"credential stays private".to_vec(), b"AAhiddenZZ".to_vec());
        let blinder = Blinder::deserialize(SeqDeserializer::<_, DecodeError>::new(
            std::iter::once(vec![42u8; 16]),
        ))
        .unwrap();
        let idx = RangeSet::from(2..8);
        let provider = HashProvider::default();
        let hash = hash_plaintext(
            provider.get(&HashAlgId::SHA256).unwrap(),
            b"hidden",
            &blinder,
        );
        let commitments = vec![TranscriptCommitment::Hash(PlaintextHash {
            direction: Direction::Received,
            idx: idx.clone(),
            hash,
        })];
        let secrets = vec![TranscriptSecret::Hash(PlaintextHashSecret {
            direction: Direction::Received,
            idx,
            alg: HashAlgId::SHA256,
            blinder,
        })];
        (transcript, commitments, secrets)
    }

    fn selections() -> Vec<Selection> {
        vec![Selection {
            direction: Direction::Received,
            ranges: vec![(2, 8)],
        }]
    }

    fn check_input() -> Vec<u8> {
        let (transcript, commitments, secrets) = fixture();
        let (sent, recv) = transcript.len();
        let mut output = Vec::new();
        put_u32(&mut output, MAGIC_RETAINED as usize);
        output.extend_from_slice(&[3, 0, 0, 0]);
        put_u32(&mut output, sent);
        put_u32(&mut output, recv);
        append(
            &mut output,
            &commitments,
            Some((&secrets, &transcript)),
            &resolve(&selections(), sent, recv).unwrap(),
        )
        .unwrap();
        output
    }

    #[test]
    fn upstream_opening_check_rejects_tampering_and_truncated_wire() {
        let input = check_input();
        assert_eq!(check(&input), Ok(()));
        for length in 0..input.len() {
            assert!(check(&input[..length]).is_err());
        }
        // The fixture contains one range: fixed header/record fields put the
        // digest, selected plaintext and blinder at these wire positions.
        for offset in [40, 85, 94] {
            let mut invalid = input.clone();
            invalid[offset] ^= 1;
            assert_eq!(check(&invalid), Err(Failure::Verification));
        }
        let mut trailing = input;
        trailing.push(0);
        assert!(check(&trailing).is_err());
    }

    #[test]
    fn online_commitments_match_exact_order_algorithm_and_ranges() {
        let (transcript, _, _) = fixture();
        let selected = resolve(&selections(), 24, 10).unwrap();
        let request = config(&transcript, &selected).unwrap().to_request();
        assert!(matches_request(Some(&request), &selected));
        assert!(!matches_request(None, &selected));
        let changed = resolve(
            &[Selection {
                direction: Direction::Received,
                ranges: vec![(1, 7)],
            }],
            24,
            10,
        )
        .unwrap();
        assert!(!matches_request(Some(&request), &changed));
        let mut builder = TranscriptCommitConfig::builder(&transcript);
        builder
            .commit_with_kind(
                &(2..8),
                Direction::Received,
                TranscriptCommitmentKind::Hash {
                    alg: HashAlgId::BLAKE3,
                },
            )
            .unwrap();
        assert!(!matches_request(
            Some(&builder.build().unwrap().to_request()),
            &selected
        ));
    }

    #[test]
    fn verifier_retains_no_opening_and_prover_retains_only_selected_bytes() {
        let (transcript, commitments, secrets) = fixture();
        let selected = resolve(&selections(), 24, 10).unwrap();
        let mut verifier = Vec::new();
        append(&mut verifier, &commitments, None, &selected).unwrap();
        assert!(!verifier.windows(6).any(|bytes| bytes == b"hidden"));
        assert_eq!(&verifier[verifier.len() - 4..], &[0, 0, 0, 0]);
        let mut prover = Vec::new();
        append(
            &mut prover,
            &commitments,
            Some((&secrets, &transcript)),
            &selected,
        )
        .unwrap();
        assert!(prover.windows(6).any(|bytes| bytes == b"hidden"));
        assert!(
            !prover
                .windows(transcript.sent().len())
                .any(|bytes| bytes == transcript.sent())
        );
        assert!(
            !prover
                .windows(transcript.received().len())
                .any(|bytes| bytes == transcript.received())
        );
        // An authenticated-looking commitment with the wrong output hash must
        // not be combined with locally retained secret bytes.
        let wrong = vec![TranscriptCommitment::Hash(PlaintextHash {
            direction: Direction::Received,
            idx: RangeSet::from(2..8),
            hash: TypedHash {
                alg: HashAlgId::SHA256,
                value: vec![0u8; 32].try_into().unwrap(),
            },
        })];
        assert_eq!(
            append(
                &mut Vec::new(),
                &wrong,
                Some((&secrets, &transcript)),
                &selected
            ),
            Err(Failure::Verification)
        );
    }

    #[test]
    fn selection_bounds_duplicates_and_canonical_ranges_are_enforced() {
        assert!(
            resolve(
                &[Selection {
                    direction: Direction::Received,
                    ranges: vec![(10, crate::protocol::END)]
                }],
                24,
                10
            )
            .is_err()
        );
        let mut duplicate = selections();
        duplicate.extend(selections());
        assert!(resolve(&duplicate, 24, 10).is_err());
        assert!(canonical(&[(1, 2), (2, 3)]).is_err());
        let mut too_many = Vec::new();
        put_u32(&mut too_many, MAX_COMMITMENTS + 1);
        assert!(matches!(
            Selection::parse_vector(&mut Cursor::new(&too_many)),
            Err(Failure::Limit)
        ));
        let selection = Selection {
            direction: Direction::Received,
            ranges: vec![(0, 40000)],
        };
        let extra = Selection {
            direction: Direction::Received,
            ranges: vec![(1, 40001)],
        };
        assert!(matches!(
            resolve(&[selection, extra], 24, 65536),
            Err(Failure::Limit)
        ));
    }
}
