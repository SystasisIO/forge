//! Opt-in fixture lifetime; native Kademlia and the existing task owner are unchanged.
use crate::echo_completion::{fresh, resolved, temporary, write_receipt};
use libp2p::{
    PeerId,
    core::ConnectedPoint,
    swarm::{ConnectionId, SwarmEvent},
};
use serde_json::{Value, json};
use std::{
    error::Error,
    fs,
    path::PathBuf,
    sync::{Arc, Mutex},
    time::Duration,
};

type Result<T> = std::result::Result<T, Box<dyn Error>>;

#[derive(Debug, Default)]
struct Progress {
    connection: Option<ConnectionId>,
    operation: Option<Value>,
    stopped: bool,
}

#[derive(Clone, Debug)]
pub(crate) struct Barrier {
    ready: PathBuf,
    result: PathBuf,
    stop: PathBuf,
    peer: PeerId,
    mode: String,
    progress: Arc<Mutex<Progress>>,
}

pub(crate) fn prepare(opts: &crate::Options) -> Result<Option<Barrier>> {
    if opts.dht_completion_barrier.is_empty() {
        return Ok(None);
    }
    if opts.dht_completion_barrier != "stop-file"
        || opts.command != "dial"
        || opts.transport != "quic"
        || !crate::is_dht_value_scenario(&opts.scenario)
        || !matches!(opts.payload.as_str(), "put_only" | "get_only")
        || !opts.echo_completion_barrier.is_empty()
        || !opts.pnet_key_file.as_os_str().is_empty()
        || !opts.pnet_control.is_empty()
        || !opts.relay_addr.is_empty()
        || !opts.relay_peer_id.is_empty()
    {
        return Err("DHT completion requires isolated public QUIC put_only/get_only dial".into());
    }
    let ready = resolved(&opts.ready_file)?;
    let result = resolved(&opts.result_file)?;
    let stop = resolved(&opts.stop_file)?;
    let ready_tmp = temporary(&ready);
    let result_tmp = temporary(&result);
    let paths = [&ready, &result, &stop, &ready_tmp, &result_tmp];
    for (index, path) in paths.iter().enumerate() {
        fresh(path)?;
        if paths[..index].contains(path) {
            return Err("DHT completion paths must be distinct".into());
        }
    }
    Ok(Some(Barrier {
        ready,
        result,
        stop,
        peer: opts.peer_id.parse()?,
        mode: opts.payload.clone(),
        progress: Arc::new(Mutex::new(Progress::default())),
    }))
}

impl Barrier {
    pub(crate) fn observe<T>(&self, event: &SwarmEvent<T>) -> Result<()> {
        let mut state = self.progress.lock().expect("DHT receipt mutex");
        match event {
            SwarmEvent::ConnectionEstablished {
                peer_id,
                connection_id,
                endpoint,
                ..
            } if *peer_id == self.peer => {
                if state.connection.is_some() || !matches!(endpoint, ConnectedPoint::Dialer { .. })
                {
                    return Err("DHT barrier connection replaced, duplicated or inbound".into());
                }
                state.connection = Some(*connection_id);
            }
            SwarmEvent::ConnectionClosed { connection_id, .. }
                if state.connection == Some(*connection_id) && !state.stopped =>
            {
                return Err("DHT authenticated connection closed before owned stop".into());
            }
            SwarmEvent::OutgoingConnectionError {
                peer_id: Some(peer),
                ..
            } if *peer == self.peer && !state.stopped => {
                return Err("DHT barrier actual connection failed".into());
            }
            _ => {}
        }
        Ok(())
    }

    pub(crate) fn publish(&self, receipt: Value) -> Result<()> {
        fresh(&self.stop)?;
        let mut state = self.progress.lock().expect("DHT receipt mutex");
        let id = state
            .connection
            .ok_or("DHT barrier lacks actual connection")?;
        if state.operation.is_some()
            || receipt["status"] != "ok"
            || receipt["operation"] != self.mode
            || receipt["authenticated_remote_peer_id"] != self.peer.to_string()
            || receipt["connection_id"] != id.to_string()
        {
            return Err(
                "DHT operation witness is failed, duplicated or connection/actor mismatched".into(),
            );
        }
        let mut witness = receipt.clone();
        witness["status"] = json!("operation_observed");
        witness["final"] = json!(false);
        witness["dht_completion"] = completion(false, false, false, false);
        write_receipt(&self.ready, &witness)?;
        state.operation = Some(receipt);
        Ok(())
    }

    pub(crate) async fn wait_stop(&self, deadline: tokio::time::Instant) -> Result<()> {
        loop {
            if tokio::time::Instant::now() >= deadline {
                return Err("DHT owned stop deadline expired".into());
            }
            match fs::symlink_metadata(&self.stop) {
                Ok(metadata) if metadata.file_type().is_file() => {
                    let mut state = self.progress.lock().expect("DHT receipt mutex");
                    if state.operation.is_none() {
                        return Err("DHT stop precedes operation witness".into());
                    }
                    state.stopped = true;
                    return Ok(());
                }
                Ok(_) => return Err("DHT stop must be a regular file".into()),
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
                Err(error) => return Err(error.into()),
            }
            tokio::time::sleep_until(
                deadline.min(tokio::time::Instant::now() + Duration::from_millis(10)),
            )
            .await;
        }
    }

    pub(crate) fn finalize(&self, lifecycle: Value, outcome: &mut Result<()>) -> Result<()> {
        let state = self.progress.lock().expect("DHT receipt mutex");
        let joined = lifecycle["fixture_owned_tasks_joined"] == true
            && lifecycle["errors"] == json!([])
            && lifecycle["overflow"] == false;
        if outcome.is_ok() && (!state.stopped || state.operation.is_none() || !joined) {
            *outcome = Err("DHT completion lacks operation/stop/owned join".into());
        }
        let failed = outcome.is_err();
        let mut receipt = state
            .operation
            .clone()
            .unwrap_or_else(|| json!({"implementation": "rust", "role": "dialer"}));
        receipt["status"] = json!(if failed { "error" } else { "ok" });
        receipt["final"] = json!(true);
        receipt["fixture_task_lifecycle"] = lifecycle;
        receipt["dht_completion"] = completion(true, state.stopped, joined, failed);
        if let Err(error) = outcome {
            receipt["error"] = json!(error.to_string().chars().take(512).collect::<String>());
        }
        write_receipt(&self.result, &receipt)
    }
}

fn completion(final_receipt: bool, stopped: bool, joined: bool, failed: bool) -> Value {
    json!({"source": "rust-fixture.dht-completion-barrier.v1", "mode": "stop-file",
        "phase": if failed { "failed" } else if final_receipt { "joined" } else { "await_stop" },
        "stop_observed": stopped, "owner_joined": joined})
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    struct Directory(PathBuf);
    impl Directory {
        fn new() -> Self {
            static NEXT: AtomicU64 = AtomicU64::new(0);
            let path = std::env::temp_dir().join(format!(
                "forge-rust-dht-completion-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            ));
            fs::create_dir(&path).unwrap();
            Self(path)
        }
        fn options(&self) -> crate::Options {
            let peer = libp2p::identity::Keypair::generate_ed25519()
                .public()
                .to_peer_id();
            crate::Options {
                command: "dial".into(),
                scenario: "dht_pk_put_get".into(),
                transport: "quic".into(),
                payload: "put_only".into(),
                peer_id: peer.to_string(),
                dht_completion_barrier: "stop-file".into(),
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
    fn point() -> ConnectedPoint {
        ConnectedPoint::Dialer {
            address: "/ip4/127.0.0.1/udp/41002/quic-v1".parse().unwrap(),
            role_override: libp2p::core::Endpoint::Dialer,
            port_use: libp2p::core::transport::PortUse::Reuse,
        }
    }
    fn bound(options: &crate::Options) -> Barrier {
        let barrier = prepare(options).unwrap().unwrap();
        barrier
            .observe(&SwarmEvent::<()>::ConnectionEstablished {
                peer_id: barrier.peer,
                connection_id: ConnectionId::new_unchecked(7),
                endpoint: point(),
                num_established: std::num::NonZeroU32::new(1).unwrap(),
                concurrent_dial_errors: None,
                established_in: Duration::ZERO,
            })
            .unwrap();
        barrier
    }
    fn operation(barrier: &Barrier) -> Value {
        json!({"implementation": "rust", "role": "dialer", "scenario": "dht_pk_put_get", "status": "ok",
            "operation": "put_only", "connection_id": ConnectionId::new_unchecked(7).to_string(),
            "authenticated_remote_peer_id": barrier.peer.to_string(), "local_peer_id": "actual-actor",
            "connection_remote_addr": "/ip4/127.0.0.1/udp/41002/quic-v1", "value_bytes": 36})
    }
    fn lifecycle(joined: bool) -> Value {
        json!({"fixture_owned_tasks_joined": joined, "errors": [], "overflow": false})
    }

    #[test]
    fn opt_in_preserves_legacy_and_rejects_wrong_mode_lane_alias_or_stale_path() {
        assert!(prepare(&crate::Options::default()).unwrap().is_none());
        let dir = Directory::new();
        let changes: [fn(&mut crate::Options); 4] = [
            |o| o.payload = "put_get".into(),
            |o: &mut crate::Options| o.command = "listen".into(),
            |o: &mut crate::Options| o.transport = "tcp".into(),
            |o: &mut crate::Options| o.ready_file = o.result_file.clone(),
        ];
        for change in changes {
            let mut opts = dir.options();
            change(&mut opts);
            assert!(prepare(&opts).is_err());
        }
        let opts = dir.options();
        fs::write(&opts.stop_file, []).unwrap();
        assert!(prepare(&opts).is_err());
        let argv = [
            "dial",
            "--dht-completion-barrier",
            "stop-file",
            "--dht-completion-barrier",
            "stop-file",
        ];
        assert!(crate::parse_args_from(argv.into_iter().map(str::to_owned)).is_err());
    }

    #[tokio::test]
    async fn witness_is_not_terminal_success_and_final_follows_stop_and_owned_join() {
        let dir = Directory::new();
        let opts = dir.options();
        let barrier = bound(&opts);
        barrier.publish(operation(&barrier)).unwrap();
        let witness = fs::read(&opts.ready_file).unwrap();
        assert!(!opts.result_file.exists());
        let interim: Value = serde_json::from_slice(&witness).unwrap();
        assert_eq!(interim["status"], "operation_observed");
        assert_eq!(interim["dht_completion"]["owner_joined"], false);
        fs::write(&opts.stop_file, []).unwrap();
        barrier
            .wait_stop(tokio::time::Instant::now() + Duration::from_secs(1))
            .await
            .unwrap();
        let mut outcome = Ok(());
        barrier.finalize(lifecycle(true), &mut outcome).unwrap();
        assert!(outcome.is_ok());
        let final_receipt: Value =
            serde_json::from_slice(&fs::read(&opts.result_file).unwrap()).unwrap();
        assert_eq!(final_receipt["status"], "ok");
        assert_eq!(final_receipt["dht_completion"]["owner_joined"], true);
        assert_eq!(witness, fs::read(&opts.ready_file).unwrap());
    }

    #[tokio::test]
    async fn timeout_or_connection_loss_after_witness_remains_terminal_failure() {
        for lost in [false, true] {
            let dir = Directory::new();
            let opts = dir.options();
            let barrier = bound(&opts);
            barrier.publish(operation(&barrier)).unwrap();
            let mut outcome = if lost {
                barrier.observe(&SwarmEvent::<()>::ConnectionClosed {
                    peer_id: barrier.peer,
                    connection_id: ConnectionId::new_unchecked(7),
                    endpoint: point(),
                    num_established: 0,
                    cause: None,
                })
            } else {
                barrier.wait_stop(tokio::time::Instant::now()).await
            };
            assert!(outcome.is_err());
            barrier.finalize(lifecycle(true), &mut outcome).unwrap();
            let final_receipt: Value =
                serde_json::from_slice(&fs::read(&opts.result_file).unwrap()).unwrap();
            assert_eq!(final_receipt["status"], "error");
            assert_eq!(final_receipt["dht_completion"]["stop_observed"], false);
            assert!(outcome.is_err());
        }
    }

    #[tokio::test]
    async fn wrong_actor_peer_connection_missing_stop_and_unjoined_owner_never_pass() {
        for field in ["operation", "authenticated_remote_peer_id", "connection_id"] {
            let dir = Directory::new();
            let opts = dir.options();
            let barrier = bound(&opts);
            let mut receipt = operation(&barrier);
            receipt[field] = json!("wrong");
            assert!(barrier.publish(receipt).is_err());
        }
        for joined in [false, true] {
            let dir = Directory::new();
            let opts = dir.options();
            let barrier = bound(&opts);
            barrier.publish(operation(&barrier)).unwrap();
            if !joined {
                fs::write(&opts.stop_file, []).unwrap();
                barrier
                    .wait_stop(tokio::time::Instant::now() + Duration::from_secs(1))
                    .await
                    .unwrap();
            }
            let mut outcome = Ok(());
            barrier.finalize(lifecycle(joined), &mut outcome).unwrap();
            assert!(outcome.is_err());
        }
    }
}
