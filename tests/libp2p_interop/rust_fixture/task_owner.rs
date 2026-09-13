//! Owns only futures submitted by the public Swarm executor and fixture echo handler.
//! Transport-internal tasks (for example Quinn or DNS runtime work) are not covered.
use std::{
    error::Error,
    fmt,
    future::Future,
    io,
    path::Path,
    pin::Pin,
    sync::{Arc, Mutex, Weak},
    task::Poll,
};

use futures::{future::poll_fn, lock::Mutex as AsyncMutex};
use libp2p::swarm::{Config, Executor};
use serde_json::{Value, json};
use tokio::task::{AbortHandle, Id, JoinSet};

const TASK_LIMIT: usize = 256;
const ERROR_LIMIT: usize = 64;

#[derive(Clone, Debug, Default)]
pub(crate) struct Owner(Arc<Inner>);

#[derive(Debug, Default)]
struct Inner {
    state: Mutex<State>,
    join: AsyncMutex<()>,
}

#[derive(Debug, Default)]
struct State {
    // JoinSet aborts on drop. That fallback is deliberately not a join receipt.
    tasks: JoinSet<()>,
    records: Vec<Record>,
    errors: Vec<String>,
    closed: bool,
    stopping: bool,
    joined: bool,
    overflow: bool,
}

#[derive(Debug)]
struct Record {
    abort: AbortHandle,
    kind: &'static str,
    abort_requested: bool,
    terminal: Option<&'static str>,
}

#[derive(Clone, Debug)]
pub(crate) struct Spawner(Weak<Inner>);

#[derive(Clone, Debug)]
pub(crate) struct Report {
    joined: bool,
    overflow: bool,
    errors: Vec<String>,
    tasks: Vec<Value>,
}

fn bounded_error(error: impl fmt::Display) -> String {
    error.to_string().chars().take(256).collect()
}

impl State {
    fn error(&mut self, message: String) {
        if self.errors.len() == ERROR_LIMIT {
            self.overflow = true;
        } else {
            self.errors.push(message);
        }
    }

    fn terminal(&mut self, id: Id, terminal: &'static str) {
        if let Some(record) = self
            .records
            .iter_mut()
            .find(|record| record.abort.id() == id)
        {
            record.terminal = Some(terminal);
        } else {
            self.error("joined task has no owner record".into());
        }
    }

    fn report(&self) -> Report {
        Report {
            joined: self.joined,
            overflow: self.overflow,
            errors: self.errors.clone(),
            tasks: self
                .records
                .iter()
                .map(|record| {
                    json!({
                        "task_id": record.abort.id().to_string(), "kind": record.kind,
                        "abort_requested": record.abort_requested, "terminal": record.terminal,
                    })
                })
                .collect(),
        }
    }
}

impl Owner {
    pub(crate) fn spawner(&self) -> Spawner {
        Spawner(Arc::downgrade(&self.0))
    }

    pub(crate) fn swarm_config(&self) -> Config {
        // Pinned with_tokio_executor() is exactly with_executor(TokioExecutor).
        // Called only at the builder's default-config boundary, before any overrides.
        Config::with_executor(self.spawner())
    }

    pub(crate) fn spawn(
        &self,
        kind: &'static str,
        future: impl Future<Output = ()> + Send + 'static,
    ) -> io::Result<()> {
        self.spawner().spawn(kind, future)
    }

    pub(crate) fn close_admission(&self) {
        self.0
            .state
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .closed = true;
    }

    pub(crate) async fn close_and_join(&self) -> Report {
        self.close_admission();
        // Serialize drainers, not task execution. Cancellation releases this lock;
        // handles remain in the owner's JoinSet for a later caller to finish joining.
        let _join = self.0.join.lock().await;
        let aborts = {
            let mut state = self
                .0
                .state
                .lock()
                .unwrap_or_else(|error| error.into_inner());
            if state.stopping {
                Vec::new()
            } else {
                state.stopping = true;
                state
                    .records
                    .iter_mut()
                    .filter_map(|record| {
                        if record.abort.is_finished() {
                            return None;
                        }
                        record.abort_requested = true;
                        Some(record.abort.clone())
                    })
                    .collect::<Vec<_>>()
            }
        };
        // No owner mutex is held when cancellation is requested.
        for task in aborts {
            task.abort();
        }
        poll_fn(|cx| {
            let mut state = self
                .0
                .state
                .lock()
                .unwrap_or_else(|error| error.into_inner());
            loop {
                match state.tasks.poll_join_next_with_id(cx) {
                    Poll::Pending => return Poll::Pending,
                    Poll::Ready(Some(Ok((id, ())))) => state.terminal(id, "completed"),
                    Poll::Ready(Some(Err(error))) => {
                        let id = error.id();
                        if error.is_cancelled()
                            && state
                                .records
                                .iter()
                                .any(|record| record.abort.id() == id && record.abort_requested)
                        {
                            state.terminal(id, "cancelled_by_owner");
                        } else {
                            state.terminal(
                                id,
                                if error.is_panic() {
                                    "panicked"
                                } else {
                                    "unexpected_cancellation"
                                },
                            );
                            state.error(bounded_error(error));
                        }
                    }
                    Poll::Ready(None) => {
                        state.joined = true;
                        return Poll::Ready(state.report());
                    }
                }
            }
        })
        .await
    }
}

impl Spawner {
    fn spawn(
        &self,
        kind: &'static str,
        future: impl Future<Output = ()> + Send + 'static,
    ) -> io::Result<()> {
        let Some(owner) = self.0.upgrade() else {
            return Err(io::Error::new(
                io::ErrorKind::BrokenPipe,
                "fixture task owner dropped",
            ));
        };
        let mut state = owner
            .state
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        let failure = if state.closed {
            Some("fixture task admission closed")
        } else if state.records.len() == TASK_LIMIT {
            state.overflow = true;
            Some("fixture task limit exceeded")
        } else {
            None
        };
        if let Some(message) = failure {
            state.error(format!("{kind}: {message}"));
            drop(state);
            // A rejected future can own resources; destroy it outside the state mutex.
            drop(future);
            return Err(io::Error::new(io::ErrorKind::BrokenPipe, message));
        }
        let runtime = match tokio::runtime::Handle::try_current() {
            Ok(runtime) => runtime,
            Err(error) => {
                state.error(bounded_error(&error));
                drop(state);
                return Err(io::Error::other(error));
            }
        };
        let abort = state.tasks.spawn_on(future, &runtime);
        state.records.push(Record {
            abort,
            kind,
            abort_requested: false,
            terminal: None,
        });
        Ok(())
    }

    pub(crate) fn record_error(&self, operation: &str, error: impl fmt::Display) {
        if let Some(owner) = self.0.upgrade() {
            let message = format!("{operation}: {}", bounded_error(error));
            owner
                .state
                .lock()
                .unwrap_or_else(|error| error.into_inner())
                .error(message);
        }
    }
}

impl Executor for Spawner {
    fn exec(&self, future: Pin<Box<dyn Future<Output = ()> + Send>>) {
        // Executor has no error return. Admission errors remain in the owner's report.
        let _ = self.spawn("swarm_connection", future);
    }
}

impl Report {
    pub(crate) fn snapshot(&self) -> Value {
        json!({
            "scope": "public_swarm_executor_and_fixture_echo_handler",
            "shutdown_mode": "close_admission_abort_join_after_swarm_drop",
            "fixture_owned_tasks_joined": self.joined,
            "overflow": self.overflow, "errors": self.errors, "tasks": self.tasks,
        })
    }

    fn failed(&self) -> bool {
        !self.joined || self.overflow || !self.errors.is_empty()
    }

    pub(crate) fn combine(
        &self,
        primary: Result<(), Box<dyn Error>>,
    ) -> Result<(), Box<dyn Error>> {
        if !self.failed() {
            return primary;
        }
        Err(Box::new(CleanupError {
            primary: primary.err(),
            report: self.clone(),
        }))
    }

    // Finalize only after the command has dropped its Swarm and this owner has joined.
    // Existing protocol/raw evidence is retained, including on cleanup failure.
    pub(crate) fn write_result(
        &self,
        path: &Path,
        role: &str,
        scenario: &str,
        outcome: &Result<(), Box<dyn Error>>,
        observation: Option<Value>,
    ) -> io::Result<()> {
        use std::io::Read;
        let mut result = match std::fs::File::open(path) {
            Ok(file) => {
                let mut bytes = Vec::new();
                file.take(1024 * 1024 + 1).read_to_end(&mut bytes)?;
                if bytes.len() > 1024 * 1024 {
                    return Err(io::Error::other("result exceeds fixture evidence bound"));
                }
                let value: Value = serde_json::from_slice(&bytes)?;
                if !value.is_object() {
                    return Err(io::Error::other("fixture result is not an object"));
                }
                value
            }
            Err(error) if error.kind() == io::ErrorKind::NotFound => {
                if observation.is_none() && outcome.is_ok() {
                    return Ok(());
                }
                json!({"implementation": "rust", "role": role, "scenario": scenario, "status": "ok"})
            }
            Err(error) => return Err(error),
        };
        result["fixture_task_lifecycle"] = self.snapshot();
        let mut overflow = false;
        if let Some(mut evidence) = observation {
            evidence["finalized_after_swarm_drop"] = json!(true);
            evidence["fixture_owned_tasks_joined"] = json!(self.joined);
            overflow = evidence["overflow"] == true;
            result["upgrade_observation"] = evidence;
        }
        if let Err(error) = outcome {
            result["status"] = json!("error");
            result["error"] = json!(bounded_error(error));
        }
        if overflow {
            result["status"] = json!("error");
            result["upgrade_observation_error"] = json!("fixture trace overflow");
        }
        crate::write_json(path, result).map_err(|error| io::Error::other(error.to_string()))?;
        if overflow {
            return Err(io::Error::other("fixture trace overflow"));
        }
        Ok(())
    }
}

#[derive(Debug)]
struct CleanupError {
    primary: Option<Box<dyn Error>>,
    report: Report,
}

impl fmt::Display for CleanupError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        if let Some(primary) = &self.primary {
            write!(formatter, "{primary}; ")?;
        }
        write!(
            formatter,
            "fixture task cleanup: joined={}, overflow={}, errors={:?}",
            self.report.joined, self.report.overflow, self.report.errors
        )
    }
}

impl Error for CleanupError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        self.primary.as_deref()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use futures::{FutureExt, channel::oneshot, future};

    #[tokio::test]
    async fn owner_joins_unpolled_executor_tasks_and_closes_admission() {
        let owner = Owner::default();
        let spawner = owner.spawner();
        let resource = Arc::new(());
        let weak = Arc::downgrade(&resource);
        spawner.exec(Box::pin(async move {
            let _resource = resource;
            future::pending::<()>().await;
        }));
        let report = owner.close_and_join().await;
        assert!(report.joined);
        assert!(!report.failed());
        assert!(
            weak.upgrade().is_none(),
            "join must drop the actual task future"
        );
        assert_eq!(report.tasks[0]["terminal"], "cancelled_by_owner");
        assert_eq!(owner.close_and_join().await.snapshot(), report.snapshot());
        assert!(owner.spawn("late_echo", async {}).is_err());
        let rejected = owner.close_and_join().await;
        assert!(rejected.failed());
        assert_eq!(rejected.tasks.len(), 1);
    }

    #[tokio::test]
    async fn owner_cancelled_join_waiter_retains_handles_for_next_join() {
        let owner = Owner::default();
        let resource = Arc::new(());
        let weak = Arc::downgrade(&resource);
        owner
            .spawn("echo_handler", async move {
                let _resource = resource;
                future::pending::<()>().await;
            })
            .unwrap();
        assert!(owner.close_and_join().now_or_never().is_none());
        assert!(!owner.0.state.lock().unwrap().joined);
        assert!(
            weak.upgrade().is_some(),
            "abort alone is not join on the current-thread runtime"
        );
        let report = owner.close_and_join().await;
        assert!(report.joined);
        assert!(weak.upgrade().is_none());
        assert_eq!(report.tasks.len(), 1);
    }

    #[tokio::test]
    async fn owner_records_completed_panicked_and_failed_tasks_preserving_primary() {
        let owner = Owner::default();
        let errors = owner.spawner();
        let (finished, wait) = oneshot::channel();
        owner
            .spawn("echo_handler", async move {
                errors.record_error("read_frame", "primary read failure");
                errors.record_error("close_stream", "secondary close failure");
                finished.send(()).unwrap();
            })
            .unwrap();
        let (panicking, wait_panic) = oneshot::channel();
        owner
            .spawn("swarm_connection", async move {
                panicking.send(()).unwrap();
                panic!("controlled fixture panic");
            })
            .unwrap();
        wait.await.unwrap();
        wait_panic.await.unwrap();
        let report = owner.close_and_join().await;
        assert!(report.joined && report.failed());
        assert_eq!(report.errors.len(), 3);
        assert!(
            report
                .tasks
                .iter()
                .any(|task| task["terminal"] == "completed")
        );
        assert!(
            report
                .tasks
                .iter()
                .any(|task| task["terminal"] == "panicked")
        );
        let error = report
            .combine(Err(io::Error::from(io::ErrorKind::PermissionDenied).into()))
            .unwrap_err();
        assert_eq!(
            error
                .source()
                .unwrap()
                .downcast_ref::<io::Error>()
                .unwrap()
                .kind(),
            io::ErrorKind::PermissionDenied
        );
        assert!(error.to_string().contains("primary read failure"));
        assert!(error.to_string().contains("secondary close failure"));
    }

    #[tokio::test]
    async fn owner_bounds_admission_and_errors_without_detaching_tasks() {
        let owner = Owner::default();
        for _ in 0..TASK_LIMIT {
            owner.spawn("bounded_task", future::pending()).unwrap();
        }
        assert!(owner.spawn("excess_task", async {}).is_err());
        for _ in 0..=ERROR_LIMIT {
            owner.spawner().record_error("bounded", "failure");
        }
        let report = owner.close_and_join().await;
        assert!(report.joined && report.overflow);
        assert_eq!(report.tasks.len(), TASK_LIMIT);
        assert_eq!(report.errors.len(), ERROR_LIMIT);
        assert!(
            report
                .tasks
                .iter()
                .all(|task| task["terminal"] == "cancelled_by_owner")
        );
    }

    #[tokio::test]
    async fn owner_drop_is_abort_fallback_not_a_join_receipt_or_ownership_cycle() {
        struct OnDrop(Option<oneshot::Sender<()>>);
        impl Drop for OnDrop {
            fn drop(&mut self) {
                let _ = self.0.take().unwrap().send(());
            }
        }
        let owner = Owner::default();
        let spawner = owner.spawner();
        let (dropped, wait) = oneshot::channel();
        let guard = OnDrop(Some(dropped));
        owner
            .spawn("echo_handler", async move {
                let _guard = guard;
                future::pending::<()>().await;
            })
            .unwrap();
        assert!(!owner.0.state.lock().unwrap().report().joined);
        drop(owner);
        assert!(spawner.0.upgrade().is_none());
        wait.await.unwrap();
        assert!(spawner.spawn("late", async {}).is_err());
    }

    #[tokio::test]
    async fn owner_join_records_active_future_teardown_before_final_receipt() {
        struct Teardown(Spawner);
        impl Drop for Teardown {
            fn drop(&mut self) {
                self.0
                    .record_error("task_teardown", "controlled cleanup failure");
            }
        }
        let owner = Owner::default();
        let errors = owner.spawner();
        let (started, wait) = oneshot::channel();
        owner
            .spawn("echo_handler", async move {
                let _teardown = Teardown(errors);
                started.send(()).unwrap();
                future::pending::<()>().await;
            })
            .unwrap();
        wait.await.unwrap();
        assert!(owner.0.state.lock().unwrap().errors.is_empty());
        let report = owner.close_and_join().await;
        assert!(report.joined && report.failed());
        assert_eq!(
            report.errors,
            vec!["task_teardown: controlled cleanup failure"]
        );
        assert_eq!(report.tasks[0]["terminal"], "cancelled_by_owner");
    }

    #[tokio::test]
    async fn owner_failure_artifact_preserves_raw_evidence_and_narrow_join_scope() {
        struct Directory(std::path::PathBuf);
        impl Drop for Directory {
            fn drop(&mut self) {
                let _ = std::fs::remove_dir_all(&self.0);
            }
        }
        let directory = Directory(std::env::temp_dir().join(format!(
            "forge-task-owner-{}-{}",
            std::process::id(),
            libp2p::PeerId::random(),
        )));
        let path = directory.0.join("result.json");
        crate::write_json(
            &path,
            json!({"status": "ok", "raw_identify_exchange": {"raw_hex": "0102"}}),
        )
        .unwrap();
        let owner = Owner::default();
        owner
            .spawner()
            .record_error("echo_close", "controlled close error");
        let report = owner.close_and_join().await;
        let outcome = report.combine(Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "primary failure",
        )
        .into()));
        report
            .write_result(
                &path,
                "dialer",
                "identify",
                &outcome,
                Some(json!({"complete": false, "overflow": false})),
            )
            .unwrap();
        let result: Value = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
        assert_eq!(result["status"], "error");
        assert_eq!(result["raw_identify_exchange"]["raw_hex"], "0102");
        assert_eq!(
            result["fixture_task_lifecycle"]["errors"][0],
            "echo_close: controlled close error"
        );
        assert_eq!(
            result["upgrade_observation"]["fixture_owned_tasks_joined"],
            true
        );
        assert_eq!(result["upgrade_observation"]["complete"], false);
        assert!(
            result["upgrade_observation"]
                .get("all_background_tasks_joined")
                .is_none()
        );
        assert!(
            result["error"]
                .as_str()
                .unwrap()
                .contains("primary failure")
        );
    }
}
