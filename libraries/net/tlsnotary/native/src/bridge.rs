use crate::protocol::{self, CHUNK, Failure, Frame, Result};
use std::sync::{
    Arc,
    atomic::{AtomicBool, AtomicU64, Ordering},
};
use tokio::{
    io::{AsyncReadExt, AsyncWriteExt, DuplexStream},
    sync::{mpsc, oneshot},
    task::JoinSet,
};

pub struct Output {
    pub frame: Frame,
    pub written: oneshot::Sender<Result<()>>,
}
pub type Sender = mpsc::Sender<Output>;
pub async fn emit(sender: &Sender, kind: u8, data: Vec<u8>) -> Result<()> {
    let (written, result) = oneshot::channel();
    sender
        .send(Output {
            frame: Frame { kind, data },
            written,
        })
        .await
        .map_err(|_| Failure::Io)?;
    result.await.map_err(|_| Failure::Io)?
}

pub struct Lane {
    input: mpsc::Sender<Vec<u8>>,
    acknowledgements: mpsc::Sender<()>,
    reading: Arc<AtomicBool>,
    writing: Arc<AtomicBool>,
    budget: Arc<AtomicU64>,
    maximum: u64,
    eof: bool,
}
impl Lane {
    pub fn data(&mut self, data: Vec<u8>, eof: bool) -> Result<()> {
        if self.eof
            || !self.reading.swap(false, Ordering::AcqRel)
            || data.len() > CHUNK
            || (eof != data.is_empty())
        {
            return Err(Failure::Protocol);
        }
        charge(&self.budget, self.maximum, data.len())?;
        self.eof = eof;
        self.input.try_send(data).map_err(|_| Failure::Protocol)
    }
    pub fn acknowledge(&mut self) -> Result<()> {
        if !self.writing.swap(false, Ordering::AcqRel) {
            return Err(Failure::Protocol);
        }
        self.acknowledgements
            .try_send(())
            .map_err(|_| Failure::Protocol)
    }
}
fn charge(counter: &AtomicU64, maximum: u64, count: usize) -> Result<()> {
    let previous = counter.fetch_add(count as u64, Ordering::Relaxed);
    if previous.saturating_add(count as u64) > maximum {
        Err(Failure::Limit)
    } else {
        Ok(())
    }
}

pub fn lane(
    channel: u8,
    maximum: u64,
    sender: Sender,
    tasks: &mut JoinSet<Result<()>>,
) -> (DuplexStream, Lane) {
    let (protocol, bridge) = tokio::io::duplex(CHUNK);
    let (mut upstream_output, mut upstream_input) = tokio::io::split(bridge);
    let (input_tx, mut input_rx) = mpsc::channel::<Vec<u8>>(1);
    let (ack_tx, mut ack_rx) = mpsc::channel::<()>(1);
    let reading = Arc::new(AtomicBool::new(false));
    let writing = Arc::new(AtomicBool::new(false));
    let budget = Arc::new(AtomicU64::new(0));
    let reader_pending = reading.clone();
    let read_sender = sender.clone();
    tasks.spawn(async move {
        loop {
            reader_pending.store(true, Ordering::Release);
            emit(
                &read_sender,
                channel + 16,
                (CHUNK as u32).to_be_bytes().to_vec(),
            )
            .await?;
            let data = input_rx.recv().await.ok_or(Failure::Io)?;
            if data.is_empty() {
                upstream_input.shutdown().await?;
                return Ok(());
            }
            upstream_input.write_all(&data).await?;
        }
    });
    let writer_pending = writing.clone();
    let write_budget = budget.clone();
    tasks.spawn(async move {
        let mut bytes = vec![0; CHUNK];
        loop {
            let count = upstream_output.read(&mut bytes).await?;
            charge(&write_budget, maximum, count)?;
            writer_pending.store(true, Ordering::Release);
            emit(
                &sender,
                if count == 0 { channel + 2 } else { channel },
                bytes[..count].to_vec(),
            )
            .await?;
            ack_rx.recv().await.ok_or(Failure::Io)?;
            if count == 0 {
                return Ok(());
            }
        }
    });
    (
        protocol,
        Lane {
            input: input_tx,
            acknowledgements: ack_tx,
            reading,
            writing,
            budget,
            maximum,
            eof: false,
        },
    )
}

pub async fn dispatch(
    reader: &mut (impl tokio::io::AsyncRead + Unpin),
    peer: &mut Lane,
    mut server: Option<&mut Lane>,
) -> Result<()> {
    loop {
        let frame = protocol::read_frame(reader, false).await?;
        match frame.kind {
            1 => peer.data(frame.data, false)?,
            3 if frame.data.is_empty() => peer.data(frame.data, true)?,
            6 if frame.data.is_empty() => peer.acknowledge()?,
            2 => server
                .as_mut()
                .ok_or(Failure::Protocol)?
                .data(frame.data, false)?,
            4 if frame.data.is_empty() => server
                .as_mut()
                .ok_or(Failure::Protocol)?
                .data(frame.data, true)?,
            7 if frame.data.is_empty() => {
                server.as_mut().ok_or(Failure::Protocol)?.acknowledge()?
            }
            5 if frame.data.is_empty() => return Err(Failure::Cancelled),
            _ => return Err(Failure::Protocol),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[tokio::test]
    async fn input_requires_credit_and_write_ack_requires_pending_output() {
        let (sender, _output) = mpsc::channel(8);
        let mut tasks = JoinSet::new();
        let (_io, mut peer) = lane(1, 128, sender, &mut tasks);
        assert_eq!(peer.data(vec![1], false), Err(Failure::Protocol));
        assert_eq!(peer.acknowledge(), Err(Failure::Protocol));
        tasks.abort_all();
        while tasks.join_next().await.is_some() {}
    }
    #[test]
    fn combined_wire_budget_fails_closed() {
        let counter = AtomicU64::new(0);
        assert_eq!(charge(&counter, 8, 4), Ok(()));
        assert_eq!(charge(&counter, 8, 4), Ok(()));
        assert_eq!(charge(&counter, 8, 1), Err(Failure::Limit));
    }
}
