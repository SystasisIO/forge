//! Opt-in harness synchronization, not a libp2p transport or upgrade substitute.
//! The caller retains the actual stream in its existing exchange future and services
//! the same Swarm. This component owns only immutable receipts and a stop-file wait.
use std::{
    error::Error,
    fs::{self, OpenOptions},
    io::Write,
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
    time::Duration,
};

use futures::AsyncWriteExt;
use libp2p::{PeerId, core::ConnectedPoint, multiaddr::Protocol, swarm::ConnectionId};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};

type Result<T> = std::result::Result<T, Box<dyn Error>>;

#[derive(Debug, Default)]
struct Progress {
    phase: &'static str,
    swarm_started: bool,
    connection_id: Option<ConnectionId>,
    connection: Option<Value>,
    exchange: Option<Value>,
    stop_observed: bool,
    stream_close_completed: bool,
}

#[derive(Clone, Debug)]
pub(crate) struct Barrier {
    ready: PathBuf,
    result: PathBuf,
    stop: PathBuf,
    progress: Arc<Mutex<Progress>>,
}

fn fresh(path: &Path) -> Result<()> {
    match fs::symlink_metadata(path) {
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(error.into()),
        Ok(_) => Err("echo completion requires fresh receipt/stop paths".into()),
    }
}

fn resolved(path: &Path) -> Result<PathBuf> {
    let name = path
        .file_name()
        .ok_or("echo completion requires nonempty file paths")?;
    let parent = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    Ok(fs::canonicalize(parent)?.join(name))
}

fn temporary(path: &Path) -> PathBuf {
    let mut name = path.as_os_str().to_owned();
    name.push(".tmp");
    PathBuf::from(name)
}

fn write_receipt(path: &Path, value: &Value) -> Result<()> {
    fresh(path)?;
    let temporary = temporary(path);
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&temporary)?;
    let result = (|| -> Result<()> {
        file.write_all(&serde_json::to_vec(value)?)?;
        file.flush()?;
        fresh(path)?;
        fs::rename(&temporary, path)?;
        Ok(())
    })();
    if result.is_err() {
        let _ = fs::remove_file(temporary);
    }
    result
}

pub(crate) fn prepare(opts: &crate::Options) -> Result<Option<Barrier>> {
    if opts.echo_completion_barrier.is_empty() {
        return Ok(None);
    }
    if opts.echo_completion_barrier != "stop-file"
        || opts.command != "dial"
        || opts.scenario != "echo_large"
        || opts.transport != "quic"
        || !opts.pnet_key_file.as_os_str().is_empty()
        || !opts.pnet_control.is_empty()
        || !opts.pnet_fingerprint.is_empty()
        || !opts.pnet_correlation.is_empty()
        || !opts.relay_addr.is_empty()
        || !opts.relay_peer_id.is_empty()
        || opts
            .features
            .iter()
            .any(|feature| matches!(feature.as_str(), "relay" | "dcutr"))
    {
        return Err("echo completion stop-file requires public QUIC dial echo_large".into());
    }
    let ready = resolved(&opts.ready_file)?;
    let result = resolved(&opts.result_file)?;
    let stop = resolved(&opts.stop_file)?;
    let ready_temporary = temporary(&ready);
    let result_temporary = temporary(&result);
    let paths = [&ready, &result, &stop, &ready_temporary, &result_temporary];
    for (index, path) in paths.iter().enumerate() {
        fresh(path)?;
        if paths[..index].contains(path) {
            return Err("echo completion receipt/stop paths must be distinct".into());
        }
    }
    Ok(Some(Barrier {
        ready,
        result,
        stop,
        progress: Arc::new(Mutex::new(Progress {
            phase: "connecting",
            ..Progress::default()
        })),
    }))
}

impl Barrier {
    pub(crate) fn swarm_started(&self) {
        self.progress.lock().expect("receipt mutex").swarm_started = true;
    }

    pub(crate) fn phase(&self, phase: &'static str) {
        self.progress.lock().expect("receipt mutex").phase = phase;
    }

    pub(crate) fn established(
        &self,
        local: PeerId,
        remote: PeerId,
        expected: PeerId,
        id: ConnectionId,
        point: &ConnectedPoint,
    ) -> Result<()> {
        if remote != expected {
            return Err("echo completion authenticated peer mismatch".into());
        }
        let ConnectedPoint::Dialer { address, .. } = point else {
            return Err("echo completion requires an observed outbound connection".into());
        };
        let mut protocols = address.iter();
        let numeric = matches!(protocols.next(), Some(Protocol::Ip4(_) | Protocol::Ip6(_)));
        let udp = matches!(protocols.next(), Some(Protocol::Udp(port)) if port != 0);
        let quic = matches!(protocols.next(), Some(Protocol::QuicV1));
        let tail = protocols.next();
        let peer_tail =
            tail.is_none() || matches!(tail, Some(Protocol::P2p(peer)) if peer == remote);
        if !numeric || !udp || !quic || !peer_tail || protocols.next().is_some() {
            return Err("echo completion requires an actual direct numeric QUIC endpoint".into());
        }
        let mut progress = self.progress.lock().expect("receipt mutex");
        if !progress.swarm_started || progress.connection_id.is_some() {
            return Err("echo completion connection is absent or ambiguous".into());
        }
        progress.connection_id = Some(id);
        progress.connection = Some(json!({
            "source": "rust-libp2p.SwarmEvent.ConnectionEstablished",
            "connection_id": id.to_string(), "local_peer_id": local.to_string(),
            "remote_peer_id": remote.to_string(), "direction": "outbound",
            // Dialer ConnectedPoint exposes only the actual remote address.
            "local_address": null, "remote_address": address.to_string(),
        }));
        Ok(())
    }

    pub(crate) fn disconnected(&self, id: ConnectionId) -> Result<()> {
        let progress = self.progress.lock().expect("receipt mutex");
        if progress.connection_id == Some(id) && !progress.stop_observed {
            return Err("echo completion connection closed before stop observation".into());
        }
        Ok(())
    }

    pub(crate) fn publish_exchange(&self, written: &[u8], read: &[u8]) -> Result<()> {
        if written != read || written.len() != 192 * 1024 {
            return Err("echo completion requires the complete actual echo_large response".into());
        }
        fresh(&self.stop)?;
        {
            let mut progress = self.progress.lock().expect("receipt mutex");
            if progress.connection.is_none() || progress.exchange.is_some() {
                return Err("echo completion exchange is unbound or already published".into());
            }
            progress.exchange = Some(json!({
                "source": "rust-fixture.actual-echo-io.v1",
                "write": {"bytes": written.len(), "sha256": format!("{:x}", Sha256::digest(written))},
                "read": {"bytes": read.len(), "sha256": format!("{:x}", Sha256::digest(read))},
            }));
            progress.phase = "await_stop";
        }
        write_receipt(&self.ready, &self.receipt(false, false, false, None))
    }

    pub(crate) async fn wait_stop(&self, deadline: tokio::time::Instant) -> Result<()> {
        loop {
            if tokio::time::Instant::now() >= deadline {
                return Err("echo completion stop-file deadline expired".into());
            }
            match fs::symlink_metadata(&self.stop) {
                Ok(metadata) if metadata.file_type().is_file() => {
                    let mut progress = self.progress.lock().expect("receipt mutex");
                    if progress.exchange.is_none() || progress.phase != "await_stop" {
                        return Err("echo completion stop precedes actual exchange witness".into());
                    }
                    progress.stop_observed = true;
                    progress.phase = "stream_close";
                    return Ok(());
                }
                Ok(_) => return Err("echo completion stop-file must be a regular file".into()),
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
                Err(error) => return Err(error.into()),
            }
            tokio::time::sleep_until(
                deadline.min(tokio::time::Instant::now() + Duration::from_millis(10)),
            )
            .await;
        }
    }

    pub(crate) fn stream_closed(&self) -> Result<()> {
        let mut progress = self.progress.lock().expect("receipt mutex");
        if !progress.stop_observed || progress.phase != "stream_close" {
            return Err("echo completion close lacks a stop observation".into());
        }
        progress.stream_close_completed = true;
        progress.phase = "stream_closed";
        Ok(())
    }

    pub(crate) async fn finish_stream<W: futures::AsyncWrite + Unpin>(
        &self,
        stream: &mut W,
        written: &[u8],
        read: &[u8],
        deadline: tokio::time::Instant,
    ) -> Result<()> {
        self.publish_exchange(written, read)?;
        self.wait_stop(deadline).await?;
        stream.close().await?;
        self.stream_closed()
    }

    fn receipt(
        &self,
        terminal: bool,
        success: bool,
        joined: bool,
        lifecycle: Option<Value>,
    ) -> Value {
        let progress = self.progress.lock().expect("receipt mutex");
        let mut receipt = json!({
            "implementation": "rust", "role": "dialer", "scenario": "echo_large",
            "status": if !terminal { "exchange_observed" } else if success { "ok" } else { "error" },
            "final": terminal, "protocol": "/forge/interop/relay-echo/1",
            "echo_ok": progress.exchange.is_some(),
            "payload_bytes": progress.exchange.as_ref().map(|exchange| exchange["read"]["bytes"].clone()),
            "exchange": progress.exchange, "connection": progress.connection,
            "echo_completion": {
                "source": "rust-fixture.echo-completion-barrier.v1", "mode": "stop-file",
                "phase": if !terminal { progress.phase } else if success { "joined_after_swarm_drop" } else { "failed" },
                "stop_observed": progress.stop_observed,
                "stream_close_completed": progress.stream_close_completed,
                "swarm_dropped": terminal && progress.swarm_started,
                "fixture_owned_tasks_joined": joined,
                "exchange_observations": usize::from(progress.exchange.is_some()),
                "stop_observations": usize::from(progress.stop_observed),
                "stream_close_completions": usize::from(progress.stream_close_completed),
            },
        });
        if terminal && !success {
            receipt["echo_completion"]["failure_phase"] = json!(progress.phase);
        }
        if let Some(lifecycle) = lifecycle {
            receipt["fixture_task_lifecycle"] = lifecycle;
        }
        receipt
    }

    // Called only by main, after the command-owned Swarm is dropped and Owner joins.
    pub(crate) fn finalize(&self, lifecycle: Value, outcome: &mut Result<()>) -> Result<()> {
        let joined = lifecycle["fixture_owned_tasks_joined"] == true;
        let complete = {
            let progress = self.progress.lock().expect("receipt mutex");
            progress.swarm_started
                && progress.exchange.is_some()
                && progress.stop_observed
                && progress.stream_close_completed
        };
        if outcome.is_ok()
            && (!complete
                || !joined
                || lifecycle["overflow"] != false
                || lifecycle["errors"]
                    .as_array()
                    .is_none_or(|errors| !errors.is_empty()))
        {
            *outcome =
                Err("echo completion terminal lacks completed exchange/close/owned join".into());
        }
        let mut receipt = self.receipt(true, outcome.is_ok(), joined, Some(lifecycle));
        if let Err(error) = outcome {
            receipt["error"] = json!(error.to_string().chars().take(256).collect::<String>());
        }
        write_receipt(&self.result, &receipt)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        pin::Pin,
        sync::atomic::{AtomicU64, Ordering},
        task::{Context, Poll},
    };

    struct Directory(PathBuf);

    impl Directory {
        fn new() -> Self {
            static NEXT: AtomicU64 = AtomicU64::new(0);
            let path = std::env::temp_dir().join(format!(
                "forge-rust-echo-completion-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed),
            ));
            fs::create_dir(&path).unwrap();
            Self(path)
        }

        fn options(&self) -> crate::Options {
            crate::Options {
                command: "dial".into(),
                scenario: "echo_large".into(),
                transport: "quic".into(),
                echo_completion_barrier: "stop-file".into(),
                ready_file: self.0.join("ready.json"),
                result_file: self.0.join("result.json"),
                stop_file: self.0.join("stop"),
                ..crate::Options::default()
            }
        }
    }

    impl Drop for Directory {
        fn drop(&mut self) {
            fs::remove_dir_all(&self.0).unwrap();
        }
    }

    fn peer() -> PeerId {
        libp2p::identity::Keypair::generate_ed25519()
            .public()
            .to_peer_id()
    }

    fn point(peer: PeerId) -> ConnectedPoint {
        ConnectedPoint::Dialer {
            address: "/ip4/127.0.0.1/udp/4001/quic-v1"
                .parse::<libp2p::Multiaddr>()
                .unwrap()
                .with(Protocol::P2p(peer)),
            role_override: libp2p::core::Endpoint::Dialer,
            port_use: libp2p::core::transport::PortUse::Reuse,
        }
    }

    fn bound(directory: &Directory) -> Barrier {
        let barrier = prepare(&directory.options()).unwrap().unwrap();
        let remote = peer();
        barrier.swarm_started();
        barrier
            .established(
                peer(),
                remote,
                remote,
                ConnectionId::new_unchecked(7),
                &point(remote),
            )
            .unwrap();
        barrier
    }

    fn payload() -> Vec<u8> {
        (0..192 * 1024).map(|index| (index % 251) as u8).collect()
    }

    fn lifecycle(joined: bool) -> Value {
        json!({
            "scope": "public_swarm_executor_and_fixture_echo_handler",
            "shutdown_mode": "close_admission_abort_join_after_swarm_drop",
            "fixture_owned_tasks_joined": joined, "overflow": false, "errors": [], "tasks": [],
        })
    }

    fn read_json(path: &Path) -> Value {
        serde_json::from_slice(&fs::read(path).unwrap()).unwrap()
    }

    #[test]
    fn disabled_barrier_does_not_validate_paths_or_change_legacy_arguments() {
        assert!(prepare(&crate::Options::default()).unwrap().is_none());
        for transport in ["quic", "tcp", "tcp-tls", "tcp-pnet-noise", "tcp-pnet-tls"] {
            let argv = ["dial", "--scenario", "echo_large", "--transport", transport]
                .into_iter()
                .map(str::to_owned);
            let options = crate::parse_args_from(argv).unwrap();
            assert_eq!(options.transport, transport);
            assert!(options.echo_completion.is_none());
        }
    }

    #[test]
    fn opt_in_parser_accepts_only_one_nonempty_stop_file_mode() {
        let directory = Directory::new();
        let options = directory.options();
        let argv = vec![
            "dial".into(),
            "--scenario".into(),
            "echo_large".into(),
            "--transport".into(),
            "quic".into(),
            "--echo-completion-barrier".into(),
            "stop-file".into(),
            "--ready-file".into(),
            options.ready_file.to_str().unwrap().into(),
            "--result-file".into(),
            options.result_file.to_str().unwrap().into(),
            "--stop-file".into(),
            options.stop_file.to_str().unwrap().into(),
        ];
        assert!(
            crate::parse_args_from(argv.clone().into_iter())
                .unwrap()
                .echo_completion
                .is_some()
        );
        for value in ["", "none", "stop", "STOP-FILE"] {
            let mut invalid = argv.clone();
            invalid[6] = value.into();
            assert!(crate::parse_args_from(invalid.into_iter()).is_err());
        }
        let mut duplicate = argv;
        duplicate.extend(["--echo-completion-barrier".into(), "stop-file".into()]);
        assert!(crate::parse_args_from(duplicate.into_iter()).is_err());
        assert_eq!(fs::read_dir(&directory.0).unwrap().count(), 0);
    }

    #[test]
    fn wrong_lane_private_and_relay_options_are_rejected_before_runtime() {
        let directory = Directory::new();
        let mutations: &[fn(&mut crate::Options)] = &[
            |options| options.command = "listen".into(),
            |options| options.scenario = "echo".into(),
            |options| options.transport = "tcp".into(),
            |options| options.transport = "tcp-tls".into(),
            |options| options.transport = "tcp-pnet-noise".into(),
            |options| options.pnet_key_file = "key".into(),
            |options| options.pnet_control = "wrong".into(),
            |options| options.pnet_fingerprint = "hash".into(),
            |options| options.pnet_correlation = "token".into(),
            |options| options.relay_addr = "relay".into(),
            |options| options.relay_peer_id = "peer".into(),
            |options| {
                options.features.insert("relay".into());
            },
            |options| {
                options.features.insert("dcutr".into());
            },
        ];
        for mutate in mutations {
            let mut options = directory.options();
            mutate(&mut options);
            assert!(prepare(&options).is_err());
        }
    }

    #[test]
    fn paths_must_be_present_distinct_and_include_distinct_temporary_outputs() {
        let directory = Directory::new();
        let mutations: &[fn(&mut crate::Options)] = &[
            |options| options.ready_file = PathBuf::new(),
            |options| options.result_file = PathBuf::new(),
            |options| options.stop_file = PathBuf::new(),
            |options| options.result_file = options.ready_file.clone(),
            |options| options.stop_file = options.ready_file.clone(),
            |options| options.stop_file = options.result_file.clone(),
            |options| options.stop_file = temporary(&options.ready_file),
            |options| options.ready_file = temporary(&options.result_file),
        ];
        for mutate in mutations {
            let mut options = directory.options();
            mutate(&mut options);
            assert!(prepare(&options).is_err());
        }
    }

    #[test]
    fn stale_files_and_temporary_receipts_cannot_be_reused() {
        let directory = Directory::new();
        let options = directory.options();
        for path in [
            options.ready_file.clone(),
            options.result_file.clone(),
            options.stop_file.clone(),
            temporary(&options.ready_file),
            temporary(&options.result_file),
        ] {
            fs::write(&path, b"stale").unwrap();
            assert!(prepare(&options).is_err());
            assert_eq!(fs::read(&path).unwrap(), b"stale");
            fs::remove_file(path).unwrap();
        }
    }

    #[cfg(unix)]
    #[test]
    fn parent_alias_and_broken_symlink_do_not_bypass_freshness() {
        let directory = Directory::new();
        let alias = directory.0.join("alias");
        std::os::unix::fs::symlink(&directory.0, &alias).unwrap();
        let mut options = directory.options();
        options.stop_file = alias.join("ready.json");
        assert!(prepare(&options).is_err());
        options = directory.options();
        std::os::unix::fs::symlink(directory.0.join("missing"), &options.stop_file).unwrap();
        assert!(prepare(&options).is_err());
    }

    #[test]
    fn connection_receipt_is_native_bound_and_never_invents_local_socket() {
        let directory = Directory::new();
        let barrier = bound(&directory);
        let connection = barrier.receipt(false, false, false, None)["connection"].clone();
        assert_eq!(
            connection["source"],
            "rust-libp2p.SwarmEvent.ConnectionEstablished"
        );
        assert_eq!(
            connection["connection_id"],
            ConnectionId::new_unchecked(7).to_string()
        );
        assert!(connection["local_address"].is_null());
        let address: libp2p::Multiaddr = connection["remote_address"]
            .as_str()
            .unwrap()
            .parse()
            .unwrap();
        assert_eq!(
            address.iter().last(),
            Some(Protocol::P2p(
                connection["remote_peer_id"]
                    .as_str()
                    .unwrap()
                    .parse()
                    .unwrap()
            ))
        );
        assert_eq!(connection["direction"], "outbound");
        let remote = peer();
        assert!(
            barrier
                .established(
                    peer(),
                    remote,
                    remote,
                    ConnectionId::new_unchecked(8),
                    &point(remote)
                )
                .is_err()
        );
        assert!(
            barrier
                .disconnected(ConnectionId::new_unchecked(7))
                .is_err()
        );
    }

    #[test]
    fn wrong_peer_inbound_and_non_quic_endpoints_cannot_supply_connection_evidence() {
        let directory = Directory::new();
        let barrier = prepare(&directory.options()).unwrap().unwrap();
        let remote = peer();
        let id = ConnectionId::new_unchecked(7);
        assert!(
            barrier
                .established(peer(), remote, remote, id, &point(remote))
                .is_err()
        );
        barrier.swarm_started();
        assert!(
            barrier
                .established(peer(), remote, peer(), id, &point(remote))
                .is_err()
        );
        let inbound = ConnectedPoint::Listener {
            local_addr: "/ip4/127.0.0.1/udp/4001/quic-v1".parse().unwrap(),
            send_back_addr: "/ip4/127.0.0.1/udp/4002/quic-v1".parse().unwrap(),
        };
        assert!(
            barrier
                .established(peer(), remote, remote, id, &inbound)
                .is_err()
        );
        for address in [
            "/ip4/127.0.0.1/tcp/4001",
            "/dns/example.test/udp/4001/quic-v1",
            "/ip4/127.0.0.1/udp/0/quic-v1",
            "/ip4/127.0.0.1/udp/4001/quic-v1/p2p-circuit",
        ] {
            let mut endpoint = point(remote);
            if let ConnectedPoint::Dialer {
                address: target, ..
            } = &mut endpoint
            {
                *target = address.parse().unwrap();
            }
            assert!(
                barrier
                    .established(peer(), remote, remote, id, &endpoint)
                    .is_err()
            );
        }
        assert!(barrier.receipt(false, false, false, None)["connection"].is_null());
    }

    #[test]
    fn partial_mismatched_unbound_or_pre_stopped_io_cannot_publish_witness() {
        let directory = Directory::new();
        let bytes = payload();
        let unbound = prepare(&directory.options()).unwrap().unwrap();
        assert!(unbound.publish_exchange(&bytes, &bytes).is_err());
        let barrier = bound(&directory);
        assert!(
            barrier
                .publish_exchange(&bytes[..bytes.len() - 1], &bytes[..bytes.len() - 1])
                .is_err()
        );
        let mut corrupt = bytes.clone();
        corrupt[100] ^= 1;
        assert!(barrier.publish_exchange(&bytes, &corrupt).is_err());
        fs::write(&barrier.stop, b"early").unwrap();
        assert!(barrier.publish_exchange(&bytes, &bytes).is_err());
        assert!(!barrier.ready.exists());
        assert!(!barrier.result.exists());
    }

    #[tokio::test]
    async fn interim_witness_is_immutable_and_not_terminal_success() {
        let directory = Directory::new();
        let barrier = bound(&directory);
        let bytes = payload();
        barrier.publish_exchange(&bytes, &bytes).unwrap();
        let interim = read_json(&barrier.ready);
        assert_eq!(interim["status"], "exchange_observed");
        assert_eq!(interim["final"], false);
        assert_eq!(interim["exchange"]["read"]["bytes"], bytes.len());
        assert_eq!(
            interim["exchange"]["write"]["sha256"],
            format!("{:x}", Sha256::digest(&bytes))
        );
        assert_eq!(
            interim["exchange"]["read"]["sha256"],
            format!("{:x}", Sha256::digest(&bytes))
        );
        assert_eq!(interim["echo_completion"]["phase"], "await_stop");
        assert!(barrier.disconnected(ConnectionId::new_unchecked(7)).is_err());
        for flag in [
            "stop_observed",
            "stream_close_completed",
            "swarm_dropped",
            "fixture_owned_tasks_joined",
        ] {
            assert_eq!(interim["echo_completion"][flag], false);
        }
        assert!(!barrier.result.exists());
        assert!(barrier.publish_exchange(&bytes, &bytes).is_err());
        fs::write(&barrier.stop, b"stop").unwrap();
        barrier
            .wait_stop(tokio::time::Instant::now() + Duration::from_secs(1))
            .await
            .unwrap();
        assert!(barrier.disconnected(ConnectionId::new_unchecked(7)).is_ok());
        barrier.stream_closed().unwrap();
        let mut outcome = Ok(());
        barrier.finalize(lifecycle(true), &mut outcome).unwrap();
        assert!(outcome.is_ok());
        let terminal = read_json(&barrier.result);
        assert_eq!(terminal["status"], "ok");
        assert_eq!(terminal["final"], true);
        assert_eq!(terminal["exchange"], interim["exchange"]);
        assert_eq!(terminal["connection"], interim["connection"]);
        assert_eq!(read_json(&barrier.ready), interim);
        assert_eq!(
            terminal["echo_completion"]["phase"],
            "joined_after_swarm_drop"
        );
        for flag in [
            "stop_observed",
            "stream_close_completed",
            "swarm_dropped",
            "fixture_owned_tasks_joined",
        ] {
            assert_eq!(terminal["echo_completion"][flag], true);
        }
    }

    #[tokio::test]
    async fn missing_invalid_or_expired_stop_never_closes_or_passes() {
        for invalid in [false, true] {
            let directory = Directory::new();
            let barrier = bound(&directory);
            let bytes = payload();
            barrier.publish_exchange(&bytes, &bytes).unwrap();
            if invalid {
                fs::create_dir(&barrier.stop).unwrap();
            }
            let deadline = tokio::time::Instant::now()
                + if invalid {
                    Duration::from_secs(1)
                } else {
                    Duration::from_millis(20)
                };
            assert!(barrier.wait_stop(deadline).await.is_err());
            assert!(barrier.stream_closed().is_err());
            let mut outcome = Ok(());
            barrier.finalize(lifecycle(true), &mut outcome).unwrap();
            assert!(outcome.is_err());
            let terminal = read_json(&barrier.result);
            assert_eq!(terminal["status"], "error");
            assert_eq!(terminal["echo_completion"]["failure_phase"], "await_stop");
            assert_eq!(terminal["echo_completion"]["stop_observed"], false);
            assert_eq!(terminal["echo_completion"]["stream_close_completed"], false);
        }
        let directory = Directory::new();
        let barrier = bound(&directory);
        let bytes = payload();
        barrier.publish_exchange(&bytes, &bytes).unwrap();
        fs::write(&barrier.stop, b"too late").unwrap();
        assert!(
            barrier
                .wait_stop(tokio::time::Instant::now())
                .await
                .is_err()
        );
    }

    struct CloseProbe {
        calls: Arc<AtomicU64>,
        fail: bool,
    }

    impl futures::AsyncWrite for CloseProbe {
        fn poll_write(
            self: Pin<&mut Self>,
            _: &mut Context<'_>,
            _: &[u8],
        ) -> Poll<std::io::Result<usize>> {
            panic!("completion must not duplicate application writes");
        }
        fn poll_flush(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<std::io::Result<()>> {
            panic!("completion must not substitute flush for close");
        }
        fn poll_close(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<std::io::Result<()>> {
            self.calls.fetch_add(1, Ordering::Relaxed);
            Poll::Ready(if self.fail {
                Err(std::io::ErrorKind::ConnectionReset.into())
            } else {
                Ok(())
            })
        }
    }

    #[tokio::test]
    async fn actual_close_is_after_stop_and_errors_remain_failure() {
        for fail in [false, true] {
            let directory = Directory::new();
            let barrier = bound(&directory);
            let bytes = payload();
            let calls = Arc::new(AtomicU64::new(0));
            let mut stream = CloseProbe {
                calls: calls.clone(),
                fail,
            };
            let mut finish = Box::pin(barrier.finish_stream(
                &mut stream,
                &bytes,
                &bytes,
                tokio::time::Instant::now() + Duration::from_secs(1),
            ));
            assert!(futures::poll!(finish.as_mut()).is_pending());
            assert_eq!(calls.load(Ordering::Relaxed), 0);
            assert!(barrier.ready.exists());
            assert!(!barrier.result.exists());
            fs::write(&barrier.stop, b"stop").unwrap();
            let mut outcome = finish.await;
            assert_eq!(calls.load(Ordering::Relaxed), 1);
            assert_eq!(outcome.is_err(), fail);
            barrier.finalize(lifecycle(true), &mut outcome).unwrap();
            let terminal = read_json(&barrier.result);
            assert_eq!(terminal["status"], if fail { "error" } else { "ok" });
            assert_eq!(terminal["echo_completion"]["stream_close_completed"], !fail);
            if fail {
                assert_eq!(terminal["echo_completion"]["failure_phase"], "stream_close");
            }
        }
    }

    #[tokio::test]
    async fn owned_join_failure_or_prior_error_cannot_be_repaired_by_complete_io() {
        for kind in ["missing_join", "join_error", "overflow", "earlier_error"] {
            let directory = Directory::new();
            let barrier = bound(&directory);
            let bytes = payload();
            barrier.publish_exchange(&bytes, &bytes).unwrap();
            fs::write(&barrier.stop, b"stop").unwrap();
            barrier
                .wait_stop(tokio::time::Instant::now() + Duration::from_secs(1))
                .await
                .unwrap();
            barrier.stream_closed().unwrap();
            let mut report = lifecycle(kind != "missing_join");
            if kind == "join_error" {
                report["errors"] = json!(["owned task failed"]);
            }
            if kind == "overflow" {
                report["overflow"] = json!(true);
            }
            let mut outcome: Result<()> = if kind == "earlier_error" {
                Err("earlier failure".into())
            } else {
                Ok(())
            };
            barrier.finalize(report, &mut outcome).unwrap();
            assert!(outcome.is_err());
            assert_eq!(read_json(&barrier.result)["status"], "error");
            assert_eq!(
                read_json(&barrier.result)["exchange"],
                read_json(&barrier.ready)["exchange"]
            );
        }
    }
}
