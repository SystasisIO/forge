// Private Forge native worker. Configuration IPC intentionally carries secret
// request bytes; diagnostics/failure results never copy that private payload.
#[cfg(tlsn_insecure)]
compile_error!("Forge TLSNotary forbids tlsn_insecure");

mod bridge;
mod commitment;
mod protocol;
mod session;

use protocol::{Config, Failure, Result};
use std::{future::Future, os::fd::FromRawFd, time::Duration};
use tokio::{net::UnixStream, sync::mpsc, task::JoinSet};

// Coordinates operation lifetime only. Production supplies session::run; no
// bridge completion, including an empty JoinSet, can authenticate a transcript.
async fn coordinate(
    operation: impl Future<Output = Result<Vec<u8>>>,
    input: impl Future<Output = Result<()>>,
    tasks: &mut JoinSet<Result<()>>,
    timeout: Duration,
) -> Result<Vec<u8>> {
    tokio::pin!(operation, input);
    let deadline = tokio::time::sleep(timeout);
    tokio::pin!(deadline);
    let mut completed = None;
    loop {
        if tasks.is_empty() {
            if let Some(output) = completed.take() {
                return Ok(output);
            }
        }
        tokio::select! {
            result = &mut operation, if completed.is_none() => match result {
                Ok(output) => completed = Some(output),
                Err(error) => return Err(error),
            },
            result = &mut input => return Err(result.err().unwrap_or(Failure::Io)),
            _ = &mut deadline => return Err(Failure::Timeout),
            // A closed session driver can drop its streams before the protocol
            // future resumes its own join. All bridge jobs may therefore finish
            // first. join_next() on an empty set is immediately ready with None;
            // it must not compete with the remaining protocol/input/deadline.
            // Conversely, local protocol completion does not drain its duplex
            // buffers: bridge jobs must submit their final bytes and receive
            // the host's existing write/FIN acknowledgements before result128.
            result = tasks.join_next(), if !tasks.is_empty() => match result {
                Some(Ok(Ok(()))) => {},
                Some(Ok(Err(error))) => return Err(error),
                _ => return Err(Failure::Internal),
            }
        }
    }
}

async fn worker(io: UnixStream) -> Result<()> {
    let (mut reader, mut writer) = io.into_split();
    // Bootstrap itself is bounded, including hosts which never send a config.
    let frame = tokio::time::timeout(
        Duration::from_secs(10),
        protocol::read_frame(&mut reader, true),
    )
    .await
    .map_err(|_| Failure::Timeout)??;
    if frame.kind != 0 {
        return Err(Failure::Protocol);
    }
    // Offline opening checks reuse the same managed worker and bounded IPC.
    // No peer/server bridge or MPC thread pool is needed for an upstream hash.
    if frame.data.get(..4) == Some(&protocol::MAGIC_RETAINED.to_be_bytes())
        && frame.data.get(4) == Some(&3)
    {
        let status = commitment::check(&frame.data)
            .err()
            .map_or(0, |error| error as u32);
        protocol::write_frame(
            &mut writer,
            &protocol::Frame {
                kind: 128,
                data: status.to_be_bytes().to_vec(),
            },
        )
        .await?;
        return Ok(());
    }
    let config = match Config::parse(&frame.data) {
        Ok(config) => config,
        Err(error) => {
            protocol::write_frame(
                &mut writer,
                &protocol::Frame {
                    kind: 128,
                    data: (error as u32).to_be_bytes().to_vec(),
                },
            )
            .await?;
            return Ok(());
        }
    };
    drop(frame);
    // TLSNotary's default CPU parallelism also uses Rayon. Initialize both
    // process-global pools explicitly before any upstream operation: N MPZ
    // workers + N Rayon workers + this current-thread Tokio runtime.
    rayon::ThreadPoolBuilder::new()
        .num_threads(config.threads)
        .build_global()
        .map_err(|_| Failure::Internal)?;
    mpz_common::ThreadPool::builder()
        .num_threads(config.threads)
        .build_global()
        .map_err(|_| Failure::Internal)?;
    let (output, mut outgoing) = mpsc::channel::<bridge::Output>(8);
    let mut write_task = tokio::spawn(async move {
        while let Some(message) = outgoing.recv().await {
            let result = protocol::write_frame(&mut writer, &message.frame).await;
            let failed = result.is_err();
            let _ = message.written.send(result);
            if failed {
                return Err(Failure::Io);
            }
        }
        Result::<()>::Ok(())
    });
    let mut tasks = JoinSet::new();
    let (peer, mut peer_lane) = bridge::lane(1, config.peer_budget, output.clone(), &mut tasks);
    let (server, mut server_lane) = if config.prover {
        let (io, lane) = bridge::lane(
            2,
            524288 + 18432 * (config.max_sent_records + config.max_recv_records) as u64,
            output.clone(),
            &mut tasks,
        );
        (Some(io), Some(lane))
    } else {
        (None, None)
    };
    let operation = session::run(&config, peer, server);
    let input = bridge::dispatch(&mut reader, &mut peer_lane, server_lane.as_mut());
    let result = tokio::select! {
        result = coordinate(operation, input, &mut tasks, Duration::from_millis(config.deadline_ms)) => result,
        result = &mut write_task => return result.map_err(|_| Failure::Internal)?,
    };
    tasks.abort_all();
    while tasks.join_next().await.is_some() {}
    let data = result.unwrap_or_else(|error| (error as u32).to_be_bytes().to_vec());
    // Output may fail if the host has disconnected; never wait indefinitely for
    // a host which stopped draining IPC after cancellation.
    let _ = tokio::time::timeout(Duration::from_secs(1), bridge::emit(&output, 128, data)).await;
    write_task.abort();
    let _ = write_task.await;
    Ok(())
}

fn main() {
    // No logging/tracing subscriber and no argv/config files. fd0 is a private
    // connected Unix stream supplied by the owner; worker opens no network.
    std::panic::set_hook(Box::new(|_| {}));
    if std::env::args_os().count() != 1 {
        std::process::exit(1);
    }
    let result = std::panic::catch_unwind(|| {
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_io()
            .enable_time()
            .build()
            .map_err(|_| Failure::Internal)?;
        let socket = unsafe { std::os::unix::net::UnixStream::from_raw_fd(0) };
        socket.set_nonblocking(true)?;
        let result = runtime.block_on(async { worker(UnixStream::from_std(socket)?).await });
        runtime.shutdown_timeout(Duration::from_secs(1));
        result
    });
    // The process boundary owns the upstream global MPZ pool; process exit is
    // the final resource barrier. The parent must wait/reap on every outcome.
    std::process::exit(if matches!(result, Ok(Ok(()))) { 0 } else { 1 });
}

#[cfg(test)]
mod lifecycle_tests {
    use super::*;

    #[tokio::test]
    async fn local_protocol_success_waits_for_bridge_write_and_fin_acknowledgements() {
        let (acknowledge, pending) = tokio::sync::oneshot::channel();
        let mut bridges = JoinSet::new();
        bridges.spawn(async move {
            pending.await.map_err(|_| Failure::Io)?;
            Ok(())
        });
        let mut completion = tokio::spawn(async move {
            coordinate(
                async { Ok(vec![1, 2, 3]) },
                std::future::pending(),
                &mut bridges,
                Duration::from_secs(1),
            )
            .await
        });
        // The local result is immediately ready. Only the outstanding bridge
        // acknowledgement may hold completion; the old coordinator returned it.
        assert!(
            tokio::time::timeout(Duration::from_millis(10), &mut completion)
                .await
                .is_err()
        );
        acknowledge.send(()).unwrap();
        assert_eq!(completion.await.unwrap(), Ok(vec![1, 2, 3]));
    }

    #[tokio::test]
    async fn local_protocol_success_does_not_hide_a_bridge_failure() {
        let (release, pending) = tokio::sync::oneshot::channel();
        let mut bridges = JoinSet::new();
        bridges.spawn(async move {
            pending.await.map_err(|_| Failure::Internal)?;
            Err(Failure::Io)
        });
        let mut completion = tokio::spawn(async move {
            coordinate(
                async { Ok(Vec::new()) },
                std::future::pending(),
                &mut bridges,
                Duration::from_secs(1),
            )
            .await
        });
        assert!(
            tokio::time::timeout(Duration::from_millis(10), &mut completion)
                .await
                .is_err()
        );
        release.send(()).unwrap();
        assert_eq!(completion.await.unwrap(), Err(Failure::Io));
    }

    #[tokio::test]
    async fn local_protocol_success_keeps_bridge_deadline_and_input_cancellation_live() {
        let mut bridges = JoinSet::new();
        bridges.spawn(std::future::pending());
        assert_eq!(
            coordinate(
                async { Ok(Vec::new()) },
                std::future::pending(),
                &mut bridges,
                Duration::from_millis(1),
            )
            .await,
            Err(Failure::Timeout)
        );
        assert_eq!(
            coordinate(
                async { Ok(Vec::new()) },
                async { Err(Failure::Cancelled) },
                &mut bridges,
                Duration::from_secs(1),
            )
            .await,
            Err(Failure::Cancelled)
        );
    }

    #[tokio::test]
    async fn drained_bridge_workers_do_not_override_late_protocol_completion() {
        let mut bridges = JoinSet::new();
        for _ in 0..4 {
            bridges.spawn(async { Result::<()>::Ok(()) });
        }
        let mut joined = 0;
        while let Some(result) = bridges.join_next().await {
            result.unwrap().unwrap();
            joined += 1;
        }
        assert_eq!(joined, 4);
        let operation = async {
            // The session driver completed first; its owner observes completion
            // only on a later poll. An unguarded empty JoinSet fails this test
            // immediately, before the delayed terminal future can complete.
            tokio::task::yield_now().await;
            Result::Ok(Vec::new())
        };
        assert_eq!(
            coordinate(
                operation,
                std::future::pending(),
                &mut bridges,
                Duration::from_secs(1)
            )
            .await,
            Ok(Vec::new())
        );
    }

    #[tokio::test]
    async fn drained_bridge_workers_keep_input_cancellation_live() {
        let mut bridges = JoinSet::new();
        assert_eq!(
            coordinate(
                std::future::pending(),
                async { Err(Failure::Cancelled) },
                &mut bridges,
                Duration::from_secs(1),
            )
            .await,
            Err(Failure::Cancelled)
        );
    }

    #[tokio::test]
    async fn drained_bridge_workers_keep_protocol_deadline_live() {
        let mut bridges = JoinSet::new();
        assert_eq!(
            coordinate(
                std::future::pending(),
                std::future::pending(),
                &mut bridges,
                Duration::from_millis(1),
            )
            .await,
            Err(Failure::Timeout)
        );
    }
}
