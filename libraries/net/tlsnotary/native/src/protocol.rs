use std::ops::Range;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

pub const MAGIC: u32 = 0x46544e31;
pub const MAX_FRAME: usize = 65536;
pub const MAX_RECORD: usize = 2 * 1024 * 1024;
pub const CHUNK: usize = 16384;
pub const END: u32 = u32::MAX;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum Failure {
    Configuration = 1,
    Io = 2,
    Rejected = 3,
    Internal = 4,
    Cancelled = 5,
    Timeout = 6,
    Limit = 7,
    Verification = 8,
    Protocol = 9,
}
pub type Result<T> = std::result::Result<T, Failure>;

impl From<std::io::Error> for Failure {
    fn from(_: std::io::Error) -> Self {
        Self::Io
    }
}
impl From<tlsn::Error> for Failure {
    fn from(error: tlsn::Error) -> Self {
        if error.is_config() {
            Self::Configuration
        } else if error.is_io() {
            Self::Io
        } else if error.is_user() {
            Self::Rejected
        } else {
            Self::Internal
        }
    }
}

pub struct Frame {
    pub kind: u8,
    pub data: Vec<u8>,
}

pub async fn read_frame(reader: &mut (impl AsyncRead + Unpin), bootstrap: bool) -> Result<Frame> {
    let kind = reader.read_u8().await?;
    let length = reader.read_u32().await? as usize;
    let limit = if bootstrap && kind == 0 {
        MAX_RECORD
    } else {
        MAX_FRAME
    };
    if length > limit {
        return Err(Failure::Limit);
    }
    let mut data = vec![0; length];
    reader.read_exact(&mut data).await?;
    Ok(Frame { kind, data })
}

pub async fn write_frame(writer: &mut (impl AsyncWrite + Unpin), frame: &Frame) -> Result<()> {
    let limit = if frame.kind == 128 {
        MAX_RECORD
    } else {
        MAX_FRAME
    };
    if frame.data.len() > limit {
        return Err(Failure::Limit);
    }
    writer.write_u8(frame.kind).await?;
    writer.write_u32(frame.data.len() as u32).await?;
    writer.write_all(&frame.data).await?;
    writer.flush().await?;
    Ok(())
}

pub struct Cursor<'a> {
    data: &'a [u8],
    position: usize,
}
impl<'a> Cursor<'a> {
    pub fn new(data: &'a [u8]) -> Self {
        Self { data, position: 0 }
    }
    pub fn bytes(&mut self, count: usize) -> Result<&'a [u8]> {
        let end = self.position.checked_add(count).ok_or(Failure::Protocol)?;
        let value = self.data.get(self.position..end).ok_or(Failure::Protocol)?;
        self.position = end;
        Ok(value)
    }
    pub fn u8(&mut self) -> Result<u8> {
        Ok(self.bytes(1)?[0])
    }
    pub fn u32(&mut self) -> Result<u32> {
        Ok(u32::from_be_bytes(self.bytes(4)?.try_into().unwrap()))
    }
    pub fn u64(&mut self) -> Result<u64> {
        Ok(u64::from_be_bytes(self.bytes(8)?.try_into().unwrap()))
    }
    pub fn blob(&mut self, maximum: usize) -> Result<Vec<u8>> {
        let count = self.u32()? as usize;
        if count > maximum {
            return Err(Failure::Limit);
        }
        Ok(self.bytes(count)?.to_vec())
    }
    fn ranges(&mut self) -> Result<Vec<(u32, u32)>> {
        let count = self.u32()? as usize;
        if count > 1024 {
            return Err(Failure::Limit);
        }
        let mut ranges = Vec::with_capacity(count);
        let mut previous_end = 0;
        for _ in 0..count {
            let begin = self.u32()?;
            let end = self.u32()?;
            if begin >= end || begin < previous_end || begin > 65536 || (end != END && end > 65536)
            {
                return Err(Failure::Configuration);
            }
            ranges.push((begin, end));
            previous_end = end;
        }
        Ok(ranges)
    }
}

pub struct Config {
    pub prover: bool,
    pub deferred: bool,
    pub deadline_ms: u64,
    pub max_sent: usize,
    pub max_recv: usize,
    pub max_sent_records: usize,
    pub max_recv_records: usize,
    pub threads: usize,
    pub peer_budget: u64,
    pub server_name: String,
    pub roots: Vec<Vec<u8>>,
    pub request: Vec<u8>,
    pub reveal_sent: Vec<(u32, u32)>,
    pub reveal_recv: Vec<(u32, u32)>,
}
impl Config {
    pub fn parse(data: &[u8]) -> Result<Self> {
        let mut c = Cursor::new(data);
        if c.u32()? != MAGIC {
            return Err(Failure::Protocol);
        }
        let prover = match c.u8()? {
            1 => true,
            2 => false,
            _ => return Err(Failure::Configuration),
        };
        let flags = c.u8()?;
        if flags > 1 || c.bytes(2)? != [0, 0] {
            return Err(Failure::Configuration);
        }
        let deadline_ms = c.u64()?;
        let max_sent = c.u32()? as usize;
        let max_recv = c.u32()? as usize;
        let max_sent_records = c.u32()? as usize;
        let max_recv_records = c.u32()? as usize;
        let threads = c.u32()? as usize;
        let peer_budget = c.u64()?;
        if !(1..=600000).contains(&deadline_ms)
            || !(1..=65536).contains(&max_sent)
            || !(1..=65536).contains(&max_recv)
            || !(1..=256).contains(&max_sent_records)
            || !(1..=256).contains(&max_recv_records)
            || !(1..=8).contains(&threads)
            || !(1..=1073741824).contains(&peer_budget)
        {
            return Err(Failure::Configuration);
        }
        let server_name = String::from_utf8(c.blob(253)?).map_err(|_| Failure::Configuration)?;
        let _: tlsn::connection::DnsName = server_name
            .as_str()
            .try_into()
            .map_err(|_| Failure::Configuration)?;
        let root_count = c.u32()? as usize;
        if !(1..=64).contains(&root_count) {
            return Err(Failure::Configuration);
        }
        let mut roots = Vec::with_capacity(root_count);
        let mut roots_bytes = 0;
        for _ in 0..root_count {
            let root = c.blob(65536)?;
            roots_bytes += root.len();
            if root.is_empty() || roots_bytes > 1048576 {
                return Err(Failure::Configuration);
            }
            roots.push(root);
        }
        let request = c.blob(max_sent)?;
        if (!prover && !request.is_empty()) || (prover && request.is_empty()) {
            return Err(Failure::Configuration);
        }
        let reveal_sent = c.ranges()?;
        let reveal_recv = c.ranges()?;
        if c.position != data.len() {
            return Err(Failure::Protocol);
        }
        Ok(Self {
            prover,
            deferred: flags == 1,
            deadline_ms,
            max_sent,
            max_recv,
            max_sent_records,
            max_recv_records,
            threads,
            peer_budget,
            server_name,
            roots,
            request,
            reveal_sent,
            reveal_recv,
        })
    }
}

pub fn ranges(values: &[(u32, u32)], actual: usize) -> Result<Vec<Range<usize>>> {
    values
        .iter()
        .map(|&(begin, end)| {
            let end = if end == END { actual } else { end as usize };
            if begin as usize > end || end > actual {
                return Err(Failure::Verification);
            }
            Ok(begin as usize..end)
        })
        .collect()
}
pub fn put_u32(out: &mut Vec<u8>, value: usize) {
    out.extend_from_slice(&(value as u32).to_be_bytes());
}
pub fn put_blob(out: &mut Vec<u8>, value: &[u8]) {
    put_u32(out, value.len());
    out.extend_from_slice(value);
}

#[cfg(test)]
mod tests {
    use super::*;
    fn valid() -> Vec<u8> {
        let mut value = Vec::new();
        put_u32(&mut value, MAGIC as usize);
        value.extend_from_slice(&[1, 0, 0, 0]);
        value.extend_from_slice(&10000u64.to_be_bytes());
        for limit in [1024, 2048, 8, 8, 2] {
            put_u32(&mut value, limit);
        }
        value.extend_from_slice(&134217728u64.to_be_bytes());
        put_blob(&mut value, b"fixture.forge.test");
        put_u32(&mut value, 1);
        put_blob(&mut value, b"dummy DER parse is upstream-owned");
        put_blob(&mut value, b"request");
        put_u32(&mut value, 1);
        put_u32(&mut value, 0);
        put_u32(&mut value, END as usize);
        put_u32(&mut value, 0);
        value
    }
    #[test]
    fn rejects_truncated_and_wrong_version() {
        for n in 0..64 {
            assert!(Config::parse(&vec![0; n]).is_err());
        }
    }
    #[test]
    fn range_bounds_are_checked() {
        assert_eq!(ranges(&[(1, END)], 5).unwrap(), [1..5]);
        assert_eq!(ranges(&[(7, END)], 5), Err(Failure::Verification));
        assert_eq!(ranges(&[(1, 9)], 5), Err(Failure::Verification));
    }
    #[test]
    fn upstream_categories_are_not_parsed_as_text() {
        assert_eq!(
            Failure::from(tlsn::Error::from(std::io::Error::other("sensitive"))),
            Failure::Io
        );
    }
    #[test]
    fn configured_limits_and_trailing_bytes_are_checked() {
        let input = valid();
        assert_eq!(Config::parse(&input).unwrap().threads, 2);
        for offset in [16, 20, 24, 28, 32] {
            let mut invalid = input.clone();
            invalid[offset..offset + 4].fill(0);
            assert!(Config::parse(&invalid).is_err());
        }
        let mut too_many_threads = input.clone();
        too_many_threads[32..36].copy_from_slice(&9u32.to_be_bytes());
        assert!(Config::parse(&too_many_threads).is_err());
        let mut trailing = input.clone();
        trailing.push(0);
        assert!(Config::parse(&trailing).is_err());
        for length in 0..input.len() {
            assert!(Config::parse(&input[..length]).is_err());
        }
    }
    #[tokio::test]
    async fn allocation_length_is_checked_before_payload_read() {
        let (mut input, mut output) = tokio::io::duplex(16);
        input.write_all(&[1, 0, 1, 0, 1]).await.unwrap();
        assert!(matches!(
            read_frame(&mut output, false).await,
            Err(Failure::Limit)
        ));
        let (mut input, mut output) = tokio::io::duplex(16);
        input.write_all(&[0, 0, 32, 0, 1]).await.unwrap();
        assert!(matches!(
            read_frame(&mut output, true).await,
            Err(Failure::Limit)
        ));
    }
}
