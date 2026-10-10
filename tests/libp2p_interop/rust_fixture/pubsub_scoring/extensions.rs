//! Bounded application state over the pinned donor's public extension APIs.
use std::{
    collections::{BTreeMap, VecDeque},
    io,
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

use libp2p::{
    PeerId,
    gossipsub::{
        self, MessageAcceptance,
        partial_messages::{Metadata as NativeMetadata, Partial, PartialAction, PartialError},
    },
};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};

use super::observer::{Evidence, hex};
use super::{
    BEHAVIOUR_SOURCE, CONNECTION_LIMIT, Config, EVENT_LIMIT, invalid, partial, validation,
};

pub(super) const PAYLOAD_LIMIT: usize = 4096;
const HOOK_LIMIT: usize = 64;
const HOLD_LIMIT: Duration = Duration::from_secs(10);
const APP_SOURCE: &str = "rust.fixture.extensions.application";

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum Mode {
    Idontwant,
    Partial,
    Advertisement,
}

impl Mode {
    pub(super) fn parse(mode: &str, version: &str) -> io::Result<Self> {
        match (mode, version) {
            ("idontwant", "1.2") => Ok(Self::Idontwant),
            ("partial", "1.3") => Ok(Self::Partial),
            ("advertisement", "1.3") => Ok(Self::Advertisement),
            _ => Err(invalid(
                "extension requires idontwant/1.2 or partial|advertisement/1.3",
            )),
        }
    }

    fn name(self) -> &'static str {
        match self {
            Self::Idontwant => "idontwant",
            Self::Partial => "partial",
            Self::Advertisement => "advertisement",
        }
    }
}

#[derive(Debug)]
pub(super) enum Operation {
    Publish(String),
    Hold(String),
    Release,
    Offer(u8),
}

impl Operation {
    pub(super) fn kind(&self) -> &'static str {
        match self {
            Self::Publish(_) => "publish_extension",
            Self::Hold(_) => "validation_hold",
            Self::Release => "validation_release",
            Self::Offer(_) => "partial_offer",
        }
    }

    pub(super) fn parse(kind: &str, row: &Value) -> io::Result<(Self, &'static [&'static str])> {
        match kind {
            "publish_extension" | "validation_hold" => {
                let payload = row["payload"]
                    .as_str()
                    .filter(|p| (1..=PAYLOAD_LIMIT).contains(&p.len()) && !p.contains('\0'))
                    .ok_or_else(|| {
                        invalid("extension payload must contain 1..4096 non-NUL bytes")
                    })?;
                Ok((
                    if kind == "validation_hold" {
                        Self::Hold(payload.into())
                    } else {
                        Self::Publish(payload.into())
                    },
                    &["sequence", "kind", "payload"],
                ))
            }
            "validation_release" => Ok((Self::Release, &["sequence", "kind"])),
            "partial_offer" => {
                let have = row["have"]
                    .as_u64()
                    .filter(|n| *n <= u64::from(partial::MASK))
                    .ok_or_else(|| invalid("partial_offer have must be an integer in 0..7"))?;
                Ok((Self::Offer(have as u8), &["sequence", "kind", "have"]))
            }
            _ => Err(invalid("unknown control command")),
        }
    }
}

fn message_detail(
    config: &Config,
    peer: PeerId,
    id: &gossipsub::MessageId,
    message: &gossipsub::Message,
    limit: usize,
) -> io::Result<Value> {
    if message.topic != config.topic().hash() || message.data.len() > limit {
        return Err(invalid("native message outside actor topic/payload bounds"));
    }
    let author = message
        .source
        .ok_or_else(|| invalid("strict native message has no author"))?;
    let sequence = message
        .sequence_number
        .ok_or_else(|| invalid("strict native message has no author seqno"))?;
    Ok(
        json!({"propagation_peer": peer.to_string(), "author_peer": author.to_string(),
        "topic": message.topic.to_string(), "message_id": hex(&id.0), "seqno_hex": hex(&sequence.to_be_bytes()),
        "payload_sha256": format!("{:x}", Sha256::digest(&message.data))}),
    )
}

fn commit_message(
    router: &mut gossipsub::Behaviour,
    config: &Config,
    evidence: &Evidence,
    peer: PeerId,
    id: gossipsub::MessageId,
    message: gossipsub::Message,
    limit: usize,
    decision: Option<MessageAcceptance>,
) -> io::Result<()> {
    let mut detail = message_detail(config, peer, &id, &message, limit)?;
    let acceptance = decision.unwrap_or_else(|| validation(config, &message.data));
    let outcome = match &acceptance {
        MessageAcceptance::Accept => "accept",
        MessageAcceptance::Reject => "reject",
        MessageAcceptance::Ignore => "ignore",
    };
    let committed = router.report_message_validation_result(&id, &peer, acceptance);
    detail["outcome"] = json!(outcome);
    detail["report_message_validation_result"] = json!(committed);
    detail["validation_commit"] = json!(committed);
    if !committed {
        evidence.emit("validation_not_committed", BEHAVIOUR_SOURCE, detail);
        evidence
            .lock()
            .fail("native report_message_validation_result returned false");
        return Err(invalid(
            "native report_message_validation_result returned false",
        ));
    }
    evidence.emit("validation", BEHAVIOUR_SOURCE, detail.clone());
    if outcome == "accept" {
        evidence.emit("delivery", BEHAVIOUR_SOURCE, detail);
    }
    Ok(())
}

struct HeldMessage {
    peer: PeerId,
    id: gossipsub::MessageId,
    message: gossipsub::Message,
}
struct Hold {
    payload: String,
    sequence: u64,
    deadline: Instant,
    message: Option<HeldMessage>,
}

#[derive(Clone, Debug)]
struct ReceivedPart {
    body: Vec<u8>,
    peer: PeerId,
    incoming_sequence: u64,
}

#[derive(Clone, Debug)]
struct Group {
    metadata: partial::Metadata,
    local_parts: [Option<Vec<u8>>; partial::PARTS],
    received_parts: [Option<ReceivedPart>; partial::PARTS],
    reconstructed: bool,
}

impl Default for Group {
    fn default() -> Self {
        Self {
            metadata: partial::Metadata {
                revision: 1,
                have: 0,
                want: partial::MASK,
            },
            local_parts: std::array::from_fn(|_| None),
            received_parts: std::array::from_fn(|_| None),
            reconstructed: false,
        }
    }
}

impl Group {
    fn available_part(&self, index: usize) -> Option<Vec<u8>> {
        self.local_parts[index].clone().or_else(|| {
            self.received_parts[index]
                .as_ref()
                .map(|part| part.body.clone())
        })
    }

    fn received_mask(&self) -> u8 {
        self.received_parts
            .iter()
            .enumerate()
            .fold(0, |mask, (index, part)| {
                mask | if part.is_some() { 1 << index } else { 0 }
            })
    }
}

#[derive(Debug)]
struct Hook {
    kind: &'static str,
    source: &'static str,
    detail: Value,
}

#[derive(Debug, Default)]
struct Application {
    closed: bool,
    group: Option<Group>,
    peers: BTreeMap<PeerId, partial::Metadata>,
    hooks: VecDeque<Hook>,
    hook_count: usize,
    error: Option<String>,
}

impl Application {
    fn record(&mut self, kind: &'static str, source: &'static str, detail: Value) {
        if self.closed {
            return;
        }
        if self.hooks.len() >= HOOK_LIMIT || self.hook_count >= EVENT_LIMIT {
            self.error
                .get_or_insert_with(|| "partial application hook observation overflow".into());
            return;
        }
        self.hook_count += 1;
        self.hooks.push_back(Hook {
            kind,
            source,
            detail,
        });
    }

    fn peer_metadata(&mut self, peer: PeerId, next: partial::Metadata) -> io::Result<()> {
        if let Some(previous) = self.peers.get_mut(&peer) {
            previous.replace(next)?;
        } else {
            if self.peers.len() >= CONNECTION_LIMIT {
                return Err(invalid("partial application peer bound"));
            }
            self.peers.insert(peer, next);
        }
        Ok(())
    }
}

#[derive(Clone, Debug, Default)]
struct Shared(Arc<Mutex<Application>>);

impl Shared {
    fn lock(&self) -> std::sync::MutexGuard<'_, Application> {
        self.0.lock().unwrap_or_else(|e| e.into_inner())
    }
}

// This object is a copy of a particular view, not the live application group.
// A peer is retained only when the public Partial callback supplied that owner.
#[derive(Debug)]
struct MetadataView {
    encoded: [u8; 7],
    shared: Shared,
    peer: Option<PeerId>,
    view: &'static str,
    group: Vec<u8>,
}

impl MetadataView {
    fn new(
        metadata: partial::Metadata,
        shared: Shared,
        peer: Option<PeerId>,
        view: &'static str,
        group: Vec<u8>,
    ) -> Self {
        Self {
            encoded: metadata.encode().expect("checked application metadata"),
            shared,
            peer,
            view,
            group,
        }
    }
}

impl NativeMetadata for MetadataView {
    fn as_slice(&self) -> &[u8] {
        &self.encoded
    }

    fn update(&mut self, data: &[u8]) -> Result<bool, PartialError> {
        // Admission, mutation and hook enqueue share the lifecycle lock.
        let mut state = self.shared.lock();
        if state.closed {
            return Ok(false);
        }
        let outcome = (|| {
            let mut previous = partial::Metadata::decode(&self.encoded)?;
            let next = partial::Metadata::decode(data)?;
            let changed = previous.replace(next)?;
            if let Some(peer) = self.peer {
                state.peer_metadata(peer, next)?;
            }
            self.encoded = next.encode()?;
            Ok::<_, io::Error>(changed)
        })();
        state.record("partial_metadata_hook", "rust.libp2p.gossipsub.Metadata.update", json!({
            "peer_id": self.peer.map(|peer| peer.to_string()), "view": self.view,
            "group_id_hex": hex(&self.group), "metadata_hex": hex(&data[..data.len().min(7)]), "metadata_bytes": data.len(),
            "changed": outcome.as_ref().ok(), "error": outcome.as_ref().err().map(ToString::to_string),
            "authority": "application_metadata_replacement_not_generic_router_validation"}));
        outcome.map_err(|_| PartialError::ValidationFailed)
    }

    fn update_from_data(&mut self, data: &[u8]) -> Result<(), PartialError> {
        let mut state = self.shared.lock();
        if state.closed {
            return Ok(());
        }
        state.record("partial_data_hook", "rust.libp2p.gossipsub.Metadata.update_from_data", json!({
            "peer_id": self.peer.map(|peer| peer.to_string()), "view": self.view, "group_id_hex": hex(&self.group),
            "body_hex": hex(&data[..data.len().min(260)]), "body_bytes": data.len(),
            "authority": "native_hook_observation_no_predicted_part_ownership"}));
        if data.len() > 260 {
            return Err(PartialError::OutOfRange);
        }
        Ok(()) // Only Event::Partial may apply received bytes to our group.
    }
}

struct FixturePartial {
    group: Vec<u8>,
    local: Group,
    shared: Shared,
}

impl Partial for FixturePartial {
    fn group_id(&self) -> Vec<u8> {
        self.group.clone()
    }

    fn metadata(&self) -> Box<dyn NativeMetadata> {
        Box::new(MetadataView::new(
            self.local.metadata,
            self.shared.clone(),
            None,
            "local_advertisement",
            self.group.clone(),
        ))
    }

    fn partial_action_from_metadata(
        &self,
        peer: PeerId,
        data: Option<&[u8]>,
    ) -> Result<PartialAction, PartialError> {
        let mut state = self.shared.lock();
        if state.closed {
            // Closing application admission is not invalid peer input.
            return Ok(PartialAction {
                need: false,
                send: None,
            });
        }
        let outcome = (|| {
            let remote = data.map(partial::Metadata::decode).transpose()?;
            if let Some(remote) = remote {
                state.peer_metadata(peer, remote)?;
            }
            // Unknown state gets metadata first. Never send undeclared known data.
            let send = remote.and_then(|remote| {
                (0..partial::PARTS).find_map(|index| {
                    if remote.want & self.local.metadata.have & (1 << index) == 0 {
                        return None;
                    }
                    self.local.available_part(index).map(|body| {
                        (
                            body,
                            Box::new(MetadataView::new(
                                remote,
                                self.shared.clone(),
                                Some(peer),
                                "remote_peer_metadata",
                                self.group.clone(),
                            )) as Box<dyn NativeMetadata>,
                        )
                    })
                })
            });
            Ok::<_, io::Error>(PartialAction {
                // The donor uses need to emit Event::Partial. Availability from
                // a local offer must not suppress independently received bytes.
                need: self.local.received_mask() != partial::MASK,
                send,
            })
        })();
        state.record("partial_action_hook", "rust.libp2p.gossipsub.Partial.partial_action_from_metadata", json!({
            "peer_id": peer.to_string(), "group_id_hex": hex(&self.group), "metadata_present": data.is_some(),
            "metadata_hex": data.map(|bytes| hex(&bytes[..bytes.len().min(7)])), "metadata_bytes": data.map(<[u8]>::len),
            "local_metadata_hex": hex(&self.local.metadata.encode().expect("checked local metadata")),
            "need": outcome.as_ref().ok().map(|action| action.need),
            "body_hex": outcome.as_ref().ok().and_then(|action| action.send.as_ref().map(|(body, _)| hex(body))),
            "error": outcome.as_ref().err().map(ToString::to_string),
            "authority": "native_application_action_not_wire_write_or_signed_delivery"}));
        outcome.map_err(|_| PartialError::ValidationFailed)
    }
}

pub(super) struct Actor {
    mode: Option<Mode>,
    token: String,
    group: Vec<u8>,
    shared: Shared,
    hold: Option<Hold>,
    hold_used: bool,
    prepared: bool,
    stopped: bool,
}

impl Actor {
    pub(super) fn new(mode: Option<Mode>, config: &Config) -> io::Result<Self> {
        Ok(Self {
            mode,
            token: config.token.clone(),
            group: partial::group_id(&config.token, 1)?,
            shared: Shared::default(),
            hold: None,
            hold_used: false,
            prepared: false,
            stopped: false,
        })
    }

    pub(super) fn subscribe(
        &self,
        router: &mut gossipsub::Behaviour,
        config: &Config,
    ) -> io::Result<()> {
        if self.mode == Some(Mode::Partial) {
            // No listener/peer exists yet, so replace only the local subscription.
            if !router.unsubscribe(&config.topic()) {
                return Err(invalid("initial full subscription missing"));
            }
            if !router
                .subscribe_partial(&config.topic(), true)
                .map_err(|e| invalid(e.to_string()))?
            {
                return Err(invalid("native partial subscription not created"));
            }
        }
        Ok(())
    }

    pub(super) fn readiness(&self, result: &mut Value) {
        if let Some(mode) = self.mode {
            result["extension"] = json!(mode.name());
            result["requests_partial"] = json!(mode == Mode::Partial);
            result["version"] = json!(if mode == Mode::Idontwant { "1.2" } else { "1.3" });
        }
    }

    pub(super) fn describe(&self, result: &mut Value) {
        self.readiness(result);
        if self.mode.is_some() {
            let state = self.shared.lock();
            result["extension_state"] = json!({"pending_hooks": state.hooks.len(), "hook_observations": state.hook_count,
                "error": state.error, "validation_hold_pending": self.hold.is_some(), "admission_closed": state.closed,
                "application_stopped": self.stopped, "scope": "inline_fixture_application_not_native_router_or_transport_join"});
        }
    }

    pub(super) fn drain(&self, evidence: &Evidence) -> io::Result<()> {
        if self.mode.is_none() {
            return Ok(());
        }
        // Detach observations before entering Evidence or making any router call.
        let (hooks, error) = {
            let mut state = self.shared.lock();
            (
                state.hooks.drain(..).collect::<Vec<_>>(),
                state.error.clone(),
            )
        };
        for hook in hooks {
            evidence.emit(hook.kind, hook.source, hook.detail);
        }
        if let Some(error) = error {
            evidence.lock().overflow = true;
            evidence.lock().fail(&error);
            return Err(invalid(error));
        }
        if let Some(error) = evidence.lock().error.clone() {
            return Err(invalid(error));
        }
        Ok(())
    }

    pub(super) async fn close_native(
        &self,
        swarm: &mut libp2p::Swarm<gossipsub::Behaviour>,
        evidence: &Evidence,
    ) -> io::Result<()> {
        let close = super::close_native(swarm, evidence);
        tokio::pin!(close);
        let mut tick = tokio::time::interval(Duration::from_millis(25));
        tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        loop {
            tokio::select! {
                result = &mut close => return result,
                _ = tick.tick() => {
                    // Keep polling the original close future even if capture fails.
                    if let Err(error) = self.drain(evidence) { evidence.lock().fail(error); }
                }
            }
        }
    }

    pub(super) fn command(
        &mut self,
        operation: Operation,
        sequence: u64,
        router: &mut gossipsub::Behaviour,
        config: &Config,
        evidence: &Evidence,
    ) -> io::Result<Value> {
        if self.mode.is_none() || self.prepared || self.stopped || self.shared.lock().closed {
            return Err(invalid("extension command admission closed or mode absent"));
        }
        self.expire(router, config, evidence)?;
        match operation {
            Operation::Publish(payload) => {
                let published = router.publish(config.topic(), payload.as_bytes());
                let detail = match &published {
                    Ok(id) => json!({"message_id": hex(&id.0), "outcome": "published"}),
                    Err(e) => json!({"outcome": "error", "error": e.to_string()}),
                };
                let mut event = detail.clone();
                event["command_sequence"] = json!(sequence);
                event["payload_sha256"] =
                    json!(format!("{:x}", Sha256::digest(payload.as_bytes())));
                event["topic"] = json!(config.topic().hash().to_string());
                evidence.emit("publish", BEHAVIOUR_SOURCE, event);
                published.map_err(|e| invalid(e.to_string()))?;
                Ok(detail)
            }
            Operation::Hold(payload) => {
                if self.mode != Some(Mode::Idontwant)
                    || self.hold_used
                    || !payload.starts_with(&format!("accept:{}:", self.token))
                {
                    return Err(invalid(
                        "one IDONTWANT hold requires exact accept:<case-token>: payload",
                    ));
                }
                let detail = json!({"command_sequence": sequence, "payload_sha256": format!("{:x}", Sha256::digest(payload.as_bytes())),
                    "payload_bytes": payload.len(), "maximum_lifetime_ms": HOLD_LIMIT.as_millis(), "committed": false});
                self.hold = Some(Hold {
                    payload,
                    sequence,
                    deadline: Instant::now() + HOLD_LIMIT,
                    message: None,
                });
                self.hold_used = true;
                evidence.emit("validation_hold_armed", APP_SOURCE, detail.clone());
                Ok(detail)
            }
            Operation::Release => {
                if self.mode != Some(Mode::Idontwant)
                    || self
                        .hold
                        .as_ref()
                        .and_then(|hold| hold.message.as_ref())
                        .is_none()
                {
                    return Err(invalid(
                        "validation_release requires an actually held native message",
                    ));
                }
                self.resolve_hold(router, config, evidence, Some(sequence), None)
            }
            Operation::Offer(have) => {
                if self.mode != Some(Mode::Partial) {
                    return Err(invalid("partial_offer requires partial mode"));
                }
                self.offer(have)?;
                let detail = self.local_detail();
                evidence.emit("partial_offer", APP_SOURCE, detail.clone());
                self.publish_partial(router, config, evidence)?;
                Ok(detail)
            }
        }
    }

    fn offer(&self, have: u8) -> io::Result<()> {
        if have > partial::MASK {
            return Err(invalid("partial offer mask outside 0..7"));
        }
        let mut state = self.shared.lock();
        if state.closed {
            return Err(invalid("partial application admission closed"));
        }
        let mut group = state.group.clone().unwrap_or_default();
        let available = have | group.received_mask();
        if state.group.is_some() && group.metadata.have != available {
            group.metadata.revision = group
                .metadata
                .revision
                .checked_add(1)
                .ok_or_else(|| invalid("partial local revision exhausted"))?;
        }
        group.metadata.have = available;
        group.metadata.want = partial::MASK ^ available;
        for index in 0..partial::PARTS {
            group.local_parts[index] = if have & (1 << index) != 0 {
                Some(partial::encode_part(
                    index as u8,
                    &partial::part_data(&self.token, index as u8)?,
                )?)
            } else {
                None
            };
        }
        state.group = Some(group);
        Ok(())
    }

    fn local_detail(&self) -> Value {
        let state = self.shared.lock();
        let group = state.group.as_ref().expect("initialized application group");
        json!({"group_id_hex": hex(&self.group), "revision": group.metadata.revision, "have": group.metadata.have, "want": group.metadata.want,
            "received_have": group.received_mask(),
            "metadata_hex": hex(&group.metadata.encode().expect("checked metadata")), "group_basis": "case_token_and_fixed_sequence_one_not_content_hash"})
    }

    fn publish_partial(
        &self,
        router: &mut gossipsub::Behaviour,
        config: &Config,
        evidence: &Evidence,
    ) -> io::Result<()> {
        let local = {
            let state = self.shared.lock();
            if state.closed {
                return Ok(());
            }
            state
                .group
                .clone()
                .ok_or_else(|| invalid("partial publication has no actual group state"))?
        };
        let published = router.publish_partial(
            config.topic().hash(),
            FixturePartial {
                group: self.group.clone(),
                local,
                shared: self.shared.clone(),
            },
        );
        evidence.emit("partial_publish_return", "rust.libp2p.gossipsub.Behaviour.publish_partial", json!({
            "group_id_hex": hex(&self.group), "error": published.as_ref().err().map(ToString::to_string),
            "authority": "native_public_API_return_not_wire_write", "off_mesh_basis": "only_native_same_group_state_recipients"}));
        published.map_err(|error| invalid(error.to_string()))
    }

    pub(super) fn event(
        &mut self,
        event: gossipsub::Event,
        router: &mut gossipsub::Behaviour,
        config: &Config,
        evidence: &Evidence,
    ) -> io::Result<Option<gossipsub::Event>> {
        self.expire(router, config, evidence)?;
        match event {
            gossipsub::Event::Message {
                propagation_source,
                message_id,
                message,
            } => {
                let mut detail = message_detail(
                    config,
                    propagation_source,
                    &message_id,
                    &message,
                    PAYLOAD_LIMIT,
                )?;
                if let Some(hold) = &mut self.hold {
                    if message.data == hold.payload.as_bytes() {
                        if hold.message.is_some() {
                            return Err(invalid(
                                "multiple native messages match the single validation hold",
                            ));
                        }
                        detail["command_sequence"] = json!(hold.sequence);
                        detail["committed"] = json!(false);
                        evidence.emit("validation_held", APP_SOURCE, detail);
                        hold.message = Some(HeldMessage {
                            peer: propagation_source,
                            id: message_id,
                            message,
                        });
                        return Ok(None);
                    }
                }
                commit_message(
                    router,
                    config,
                    evidence,
                    propagation_source,
                    message_id,
                    message,
                    PAYLOAD_LIMIT,
                    None,
                )?;
            }
            gossipsub::Event::Partial {
                topic_hash,
                peer_id,
                group_id,
                message,
                metadata,
            } => {
                let incoming_sequence = {
                    let mut capture = evidence.lock();
                    let previous = capture.events.len();
                    capture.emit("partial_incoming", "rust.libp2p.gossipsub.Event.Partial", json!({"peer_id": peer_id.to_string(),
                    "topic": topic_hash.to_string(), "group_id_hex": hex(&group_id[..group_id.len().min(20)]), "group_bytes": group_id.len(),
                    "body_present": message.is_some(), "body_hex": message.as_ref().map(|bytes| hex(&bytes[..bytes.len().min(260)])),
                    "body_bytes": message.as_ref().map(Vec::len), "metadata_present": metadata.is_some(),
                    "metadata_hex": metadata.as_ref().map(|bytes| hex(&bytes[..bytes.len().min(7)])),
                    "metadata_bytes": metadata.as_ref().map(Vec::len), "authority": "native_application_event_not_signed_validation_or_delivery"}));
                    if capture.events.len() != previous + 1
                        || capture.overflow
                        || capture.error.is_some()
                    {
                        return Err(invalid("partial incoming observation capture failed"));
                    }
                    capture.events[previous]["sequence"]
                        .as_u64()
                        .expect("captured event sequence")
                };
                if self.shared.lock().closed {
                    evidence.emit("partial_application_closed", APP_SOURCE, json!({"peer_id": peer_id.to_string(), "group_id_hex": hex(&self.group), "admitted": false}));
                } else if self.mode != Some(Mode::Partial)
                    || topic_hash != config.topic().hash()
                    || group_id != self.group
                {
                    evidence.emit("partial_rejected", APP_SOURCE, json!({"peer_id": peer_id.to_string(),
                        "group_id_hex": hex(&group_id[..group_id.len().min(20)]), "error": "wrong mode/topic/group",
                        "scope": "application_only_no_report_invalid_partial_or_full_validation"}));
                } else {
                    match self.apply(peer_id, message.as_deref(), metadata.as_deref(), incoming_sequence, evidence) {
                        Ok(true) => { self.publish_partial(router, config, evidence)?; }
                        Ok(false) => {}
                        Err(error) => evidence.emit("partial_rejected", APP_SOURCE, json!({"peer_id": peer_id.to_string(), "group_id_hex": hex(&self.group),
                            "error": error.to_string(), "scope": "application_only_no_report_invalid_partial_or_full_validation"})),
                    }
                }
            }
            event => return Ok(Some(event)),
        }
        Ok(None)
    }

    fn apply(
        &self,
        peer: PeerId,
        body: Option<&[u8]>,
        metadata: Option<&[u8]>,
        incoming_sequence: u64,
        evidence: &Evidence,
    ) -> io::Result<bool> {
        let (changed, reconstructed) = {
            let mut state = self.shared.lock();
            if state.closed {
                return Ok(false);
            }
            let remote = metadata
                .map(partial::Metadata::decode)
                .transpose()?
                .or_else(|| state.peers.get(&peer).copied())
                .ok_or_else(|| invalid("new partial peer lacks metadata"))?;
            // Check everything before changing either the peer ledger or local parts.
            if let Some(previous) = state.peers.get(&peer) {
                let mut previous = *previous;
                previous.replace(remote)?;
            }
            let mut group = state.group.clone().unwrap_or_default();
            let mut changed = state.group.is_none();
            if let Some(body) = body {
                let (index, data) = partial::decode_part(body)?;
                if data != partial::part_data(&self.token, index)?
                    || remote.have & (1 << index) == 0
                {
                    return Err(invalid(
                        "partial bytes disagree with expected content or remote metadata",
                    ));
                }
                if incoming_sequence == 0 {
                    return Err(invalid("received part lacks incoming event reference"));
                }
                let slot = &mut group.received_parts[usize::from(index)];
                if slot.is_none() {
                    if group.metadata.have & (1 << index) == 0 {
                        group.metadata.revision = group
                            .metadata
                            .revision
                            .checked_add(1)
                            .ok_or_else(|| invalid("partial local revision exhausted"))?;
                        group.metadata.have |= 1 << index;
                        group.metadata.want = partial::MASK ^ group.metadata.have;
                    }
                    *slot = Some(ReceivedPart {
                        body: body.to_vec(),
                        peer,
                        incoming_sequence,
                    });
                    changed = true;
                } else if slot.as_ref().map(|part| part.body.as_slice()) != Some(body) {
                    return Err(invalid("conflicting actual part bytes"));
                }
            }
            let reconstructed = if group.received_mask() == partial::MASK && !group.reconstructed {
                let received = group
                    .received_parts
                    .iter()
                    .cloned()
                    .collect::<Option<Vec<_>>>()
                    .ok_or_else(|| invalid("claimed parts missing actual bytes"))?;
                let parts = received
                    .iter()
                    .map(|part| part.body.clone())
                    .collect::<Vec<_>>();
                let bytes = partial::reconstruct(&self.token, &parts)?;
                group.reconstructed = true;
                Some((bytes, parts, received))
            } else {
                None
            };
            state.peer_metadata(peer, remote)?;
            state.group = Some(group);
            (changed, reconstructed)
        };
        let mut detail = self.local_detail();
        detail["peer_id"] = json!(peer.to_string());
        detail["body_applied"] = json!(body.is_some());
        detail["incoming_sequence"] = json!(incoming_sequence);
        detail["authority"] = json!("application_checked_bytes_not_signed_message_validation");
        evidence.emit("partial_applied", APP_SOURCE, detail);
        if let Some((bytes, parts, received)) = reconstructed {
            evidence.emit("partial_reconstructed", APP_SOURCE, json!({"group_id_hex": hex(&self.group),
                "parts_hex": parts.iter().map(|part| hex(part)).collect::<Vec<_>>(),
                "part_incoming_sequences": received.iter().map(|part| part.incoming_sequence).collect::<Vec<_>>(),
                "part_peer_ids": received.iter().map(|part| part.peer.to_string()).collect::<Vec<_>>(), "payload_hex": hex(&bytes),
                "payload_sha256": format!("{:x}", Sha256::digest(&bytes)), "payload_bytes": bytes.len(),
                "authority": "application_reconstruction_not_router_delivery_or_signature"}));
        }
        Ok(changed)
    }

    fn resolve_hold(
        &mut self,
        router: &mut gossipsub::Behaviour,
        config: &Config,
        evidence: &Evidence,
        release: Option<u64>,
        cancellation: Option<&str>,
    ) -> io::Result<Value> {
        let hold = self
            .hold
            .take()
            .ok_or_else(|| invalid("no validation hold to resolve"))?;
        let mut detail = json!({"hold_command_sequence": hold.sequence, "command_sequence": release,
            "payload_sha256": format!("{:x}", Sha256::digest(hold.payload.as_bytes())), "cancellation": cancellation, "validation_commit": false});
        let outcome = if let Some(held) = hold.message {
            let identity =
                message_detail(config, held.peer, &held.id, &held.message, PAYLOAD_LIMIT)?;
            for (key, value) in identity.as_object().expect("message detail object") {
                detail[key] = value.clone();
            }
            if release.is_some() && cancellation.is_none() {
                evidence.emit("validation_release_requested", APP_SOURCE, detail.clone());
            }
            commit_message(
                router,
                config,
                evidence,
                held.peer,
                held.id,
                held.message,
                PAYLOAD_LIMIT,
                cancellation.map(|_| MessageAcceptance::Ignore),
            )
        } else {
            Ok(())
        };
        detail["validation_commit"] = json!(outcome.is_ok() && detail.get("message_id").is_some());
        detail["error"] = json!(outcome.as_ref().err().map(ToString::to_string));
        evidence.emit(
            if cancellation.is_some() {
                "validation_cancelled"
            } else {
                "validation_released"
            },
            APP_SOURCE,
            detail.clone(),
        );
        outcome?;
        Ok(detail)
    }

    pub(super) fn expire(
        &mut self,
        router: &mut gossipsub::Behaviour,
        config: &Config,
        evidence: &Evidence,
    ) -> io::Result<()> {
        if self
            .hold
            .as_ref()
            .is_some_and(|hold| Instant::now() >= hold.deadline)
        {
            let cancellation =
                self.resolve_hold(router, config, evidence, None, Some("ten_second_deadline"));
            evidence.lock().fail("application validation hold expired");
            cancellation?;
            return Err(invalid("application validation hold expired"));
        }
        Ok(())
    }

    pub(super) fn prepare(&mut self, evidence: &Evidence) -> io::Result<()> {
        // Every retained callback finishes its mutation/enqueue before this lock
        // can close admission. Drain after closing, never before it.
        self.shared.lock().closed = true;
        self.drain(evidence)?;
        if self.hold.is_some() {
            return Err(invalid(
                "prepare_shutdown with pending application validation hold",
            ));
        }
        self.prepared = true;
        Ok(())
    }

    pub(super) fn stop(
        &mut self,
        router: &mut gossipsub::Behaviour,
        config: &Config,
        evidence: &Evidence,
    ) -> io::Result<()> {
        if self.mode.is_none() {
            return Ok(());
        }
        self.shared.lock().closed = true;
        self.stopped = true;
        let cancelled = if self.hold.is_some() {
            let result =
                self.resolve_hold(router, config, evidence, None, Some("application_shutdown"));
            evidence
                .lock()
                .fail("extension shutdown with unreleased validation hold");
            result.map(|_| ())
        } else {
            Ok(())
        };
        let drained = self.drain(evidence);
        evidence.emit("extension_stopped", APP_SOURCE, json!({"admission_closed": true, "pending_validations": 0,
            "pending_hooks": self.shared.lock().hooks.len(), "drain_error": drained.as_ref().err().map(ToString::to_string),
            "cancellation_error": cancelled.as_ref().err().map(ToString::to_string), "scope": "inline_application_only_not_native_close_or_transport_join"}));
        cancelled?;
        drained
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const TOKEN: &str = "00112233445566778899aabbccddeeff";

    fn config() -> Config {
        Config {
            version: "1.3".into(),
            transport: "tcp".into(),
            actor: "victim".into(),
            token: TOKEN.into(),
            ready: "ready".into(),
            control: "control".into(),
            result: "result".into(),
            stop: "stop".into(),
            store: "store".into(),
            key_file: None,
            fingerprint: None,
        }
    }

    fn fixture(have: u8) -> FixturePartial {
        let mut local = Group::default();
        local.metadata.have = have;
        local.metadata.want = partial::MASK ^ have;
        for index in 0..partial::PARTS {
            if have & (1 << index) != 0 {
                local.local_parts[index] = Some(
                    partial::encode_part(
                        index as u8,
                        &partial::part_data(TOKEN, index as u8).unwrap(),
                    )
                    .unwrap(),
                );
            }
        }
        FixturePartial {
            group: partial::group_id(TOKEN, 1).unwrap(),
            local,
            shared: Shared::default(),
        }
    }

    // Synthetic application input, explicitly not a native callback or wire receipt.
    fn apply_input(
        actor: &Actor,
        peer: PeerId,
        body: Option<&[u8]>,
        metadata: Option<&[u8]>,
        evidence: &Evidence,
    ) -> io::Result<bool> {
        let sequence = {
            let mut capture = evidence.lock();
            capture.emit(
                "partial_incoming",
                "rust.fixture.tests.synthetic",
                json!({
                "peer_id": peer.to_string(), "group_id_hex": hex(&actor.group),
                "body_hex": body.map(hex), "metadata_hex": metadata.map(hex),
                "authority": "synthetic_application_regression_not_interop_proof"}),
            );
            capture.events.last().unwrap()["sequence"].as_u64().unwrap()
        };
        actor.apply(peer, body, metadata, sequence, evidence)
    }

    #[test]
    fn native_partial_action_uses_only_owned_parts_and_actual_peer_metadata() {
        let fixture = fixture(5);
        let peer = PeerId::random();
        let unknown = fixture.partial_action_from_metadata(peer, None).unwrap();
        assert!(unknown.need && unknown.send.is_none());
        let remote = partial::Metadata {
            revision: 1,
            have: 2,
            want: 5,
        }
        .encode()
        .unwrap();
        let action = fixture
            .partial_action_from_metadata(peer, Some(&remote))
            .unwrap();
        let (body, metadata) = action.send.unwrap();
        assert_eq!(
            body,
            partial::encode_part(0, &partial::part_data(TOKEN, 0).unwrap()).unwrap()
        );
        assert_eq!(metadata.as_slice(), remote); // No invented acknowledgement of the sent part.
        assert!(fixture.shared.0.try_lock().is_ok());
        let latest = partial::Metadata {
            revision: 2,
            have: 4,
            want: 3,
        }
        .encode()
        .unwrap();
        let action = fixture
            .partial_action_from_metadata(peer, Some(&latest))
            .unwrap();
        assert_eq!(partial::decode_part(&action.send.unwrap().0).unwrap().0, 0);
        assert_eq!(fixture.shared.lock().peers[&peer].have, 4); // Not 2|4.
        assert!(
            fixture
                .partial_action_from_metadata(peer, Some(&remote))
                .is_err()
        );
        let conflict = partial::Metadata {
            revision: 2,
            have: 1,
            want: 6,
        }
        .encode()
        .unwrap();
        assert!(
            fixture
                .partial_action_from_metadata(peer, Some(&conflict))
                .is_err()
        );
        let observation = fixture.shared.lock().hooks.pop_front().unwrap();
        assert_eq!(
            observation.source,
            "rust.libp2p.gossipsub.Partial.partial_action_from_metadata"
        );
        assert_eq!(observation.detail["peer_id"], peer.to_string());
        assert_eq!(
            observation.detail["group_id_hex"],
            "00112233445566778899aabbccddeeff00000001"
        );
    }

    #[test]
    fn native_metadata_hook_replaces_and_retains_only_known_source_ownership() {
        let fixture = fixture(5);
        let mut metadata = fixture.metadata();
        let next = partial::Metadata {
            revision: 2,
            have: 2,
            want: 5,
        }
        .encode()
        .unwrap();
        assert!(metadata.update(&next).unwrap());
        assert_eq!(hex(metadata.as_slice()), "01000000020205");
        assert!(!metadata.update(&next).unwrap());
        assert!(metadata.update(&[1, 0, 0, 0, 1, 5, 2]).is_err());
        assert!(metadata.update(&[1, 0, 0, 0, 2, 5, 2]).is_err());
        assert_eq!(metadata.as_slice(), next);
        assert!(fixture.shared.lock().peers.is_empty());
        assert!(
            fixture
                .shared
                .lock()
                .hooks
                .iter()
                .all(|hook| hook.detail["peer_id"].is_null())
        );
        let peer = PeerId::random();
        let mut metadata = MetadataView::new(
            partial::Metadata::decode(&next).unwrap(),
            fixture.shared.clone(),
            Some(peer),
            "remote_peer_metadata",
            fixture.group.clone(),
        );
        let newest = partial::Metadata {
            revision: 3,
            have: 4,
            want: 3,
        }
        .encode()
        .unwrap();
        assert!(metadata.update(&newest).unwrap());
        assert_eq!(fixture.shared.lock().peers[&peer].have, 4);
        let owned = fixture.shared.lock().hooks.pop_back().unwrap();
        assert_eq!(owned.detail["peer_id"], peer.to_string());
        let mut actor = Actor::new(Some(Mode::Partial), &config()).unwrap();
        actor.shared = fixture.shared.clone();
        let evidence = Evidence::default();
        actor.drain(&evidence).unwrap();
        assert!(
            evidence
                .lock()
                .events
                .iter()
                .all(|event| event["kind"] == "partial_metadata_hook")
        );
        assert!(actor.shared.lock().hooks.is_empty());
        assert!(
            !evidence
                .lock()
                .events
                .iter()
                .any(|event| event["kind"] == "partial_incoming")
        );
    }

    #[test]
    fn actual_parts_reconstruct_in_index_order_without_router_delivery_claim() {
        let config = config();
        let actor = Actor::new(Some(Mode::Partial), &config).unwrap();
        let evidence = Evidence::default();
        let peer = PeerId::random();
        for (revision, index) in [2, 0, 1].into_iter().enumerate() {
            let metadata = partial::Metadata {
                revision: revision as u32 + 1,
                have: 1 << index,
                want: partial::MASK ^ (1 << index),
            }
            .encode()
            .unwrap();
            let body =
                partial::encode_part(index, &partial::part_data(TOKEN, index).unwrap()).unwrap();
            assert!(apply_input(&actor, peer, Some(&body), Some(&metadata), &evidence).unwrap());
        }
        let expected: Vec<_> = (0..3)
            .flat_map(|index| partial::part_data(TOKEN, index).unwrap())
            .collect();
        let capture = evidence.lock();
        let reconstructed = capture
            .events
            .iter()
            .filter(|event| event["kind"] == "partial_reconstructed")
            .collect::<Vec<_>>();
        assert_eq!(reconstructed.len(), 1);
        assert_eq!(reconstructed[0]["payload_hex"], hex(&expected));
        assert_eq!(reconstructed[0]["parts_hex"].as_array().unwrap().len(), 3);
        assert!(
            !capture
                .events
                .iter()
                .any(|event| event["kind"] == "delivery" || event["kind"] == "validation")
        );
        drop(capture);
        assert_eq!(actor.shared.lock().group.as_ref().unwrap().metadata.have, 7);
        assert_eq!(actor.shared.lock().peers[&peer].have, 2);
    }

    #[test]
    fn offered_bytes_cannot_substitute_for_received_part_or_its_event_reference() {
        let actor = Actor::new(Some(Mode::Partial), &config()).unwrap();
        let evidence = Evidence::default();
        let peer = PeerId::random();
        actor.offer(1).unwrap(); // Same state transition as partial_offer, no router/wire claim.
        for (revision, index) in [(1, 1), (2, 2)] {
            let metadata = partial::Metadata {
                revision,
                have: 7,
                want: 0,
            }
            .encode()
            .unwrap();
            let body =
                partial::encode_part(index, &partial::part_data(TOKEN, index).unwrap()).unwrap();
            assert!(apply_input(&actor, peer, Some(&body), Some(&metadata), &evidence).unwrap());
        }
        {
            let state = actor.shared.lock();
            let group = state.group.as_ref().unwrap();
            assert_eq!(group.metadata.have, 7);
            assert_eq!(group.received_mask(), 6);
            assert!(group.received_parts[0].is_none() && group.local_parts[0].is_some());
            assert!(!group.reconstructed);
        }
        assert!(
            !evidence
                .lock()
                .events
                .iter()
                .any(|event| event["kind"] == "partial_reconstructed")
        );
        let retained = FixturePartial {
            group: actor.group.clone(),
            local: actor.shared.lock().group.clone().unwrap(),
            shared: actor.shared.clone(),
        };
        assert_eq!(retained.local.metadata.want, 0);
        assert!(
            retained
                .partial_action_from_metadata(peer, None)
                .unwrap()
                .need
        );
        let metadata = partial::Metadata {
            revision: 3,
            have: 7,
            want: 0,
        }
        .encode()
        .unwrap();
        let body = partial::encode_part(0, &partial::part_data(TOKEN, 0).unwrap()).unwrap();
        assert!(apply_input(&actor, peer, Some(&body), Some(&metadata), &evidence).unwrap());
        assert!(!apply_input(&actor, peer, Some(&body), Some(&metadata), &evidence).unwrap());
        actor.offer(7).unwrap(); // Offers must not erase received provenance or completion.
        let capture = evidence.lock();
        let completed = capture
            .events
            .iter()
            .filter(|event| event["kind"] == "partial_reconstructed")
            .collect::<Vec<_>>();
        assert_eq!(completed.len(), 1);
        for index in 0..partial::PARTS {
            let sequence = completed[0]["part_incoming_sequences"][index]
                .as_u64()
                .unwrap();
            let input = &capture.events[sequence as usize - 1];
            assert_eq!(input["kind"], "partial_incoming");
            assert_eq!(input["body_hex"], completed[0]["parts_hex"][index]);
            assert_eq!(input["peer_id"], completed[0]["part_peer_ids"][index]);
        }
        let parts = completed[0]["parts_hex"]
            .as_array()
            .unwrap()
            .iter()
            .map(|value| crate::decode_hex(value.as_str().unwrap()).unwrap())
            .collect::<Vec<_>>();
        // These encoded bytes, not a boolean, are available to independent Python.
        assert_eq!(
            completed[0]["payload_hex"],
            hex(&partial::reconstruct(TOKEN, &parts).unwrap())
        );
    }

    #[test]
    fn retained_hooks_are_neutral_after_prepare_or_stop_and_keep_prior_errors() {
        for prepare in [true, false] {
            let fixture = fixture(5);
            let mut actor = Actor::new(Some(Mode::Partial), &config()).unwrap();
            actor.shared = fixture.shared.clone();
            let evidence = Evidence::default();
            let peer = PeerId::random();
            let remote = partial::Metadata {
                revision: 1,
                have: 2,
                want: 5,
            };
            let mut metadata = MetadataView::new(
                remote,
                fixture.shared.clone(),
                Some(peer),
                "remote_peer_metadata",
                fixture.group.clone(),
            );
            assert!(
                fixture
                    .partial_action_from_metadata(peer, Some(&remote.encode().unwrap()))
                    .unwrap()
                    .send
                    .is_some()
            );
            let mut router =
                super::super::behaviour(&libp2p::identity::Keypair::generate_ed25519(), &config())
                    .unwrap();
            if prepare {
                actor.prepare(&evidence).unwrap();
            } else {
                actor.stop(&mut router, &config(), &evidence).unwrap();
            }
            let observations = fixture.shared.lock().hook_count;
            let before = metadata.as_slice().to_vec();
            // Even malformed late input must not turn local closure into a peer penalty.
            assert!(!metadata.update(&[0; 500]).unwrap());
            metadata.update_from_data(&[0; 500]).unwrap();
            let action = fixture
                .partial_action_from_metadata(peer, Some(&[0; 500]))
                .unwrap();
            assert!(!action.need && action.send.is_none());
            assert_eq!(metadata.as_slice(), before);
            {
                let mut state = fixture.shared.lock();
                assert!(state.closed && state.hooks.is_empty());
                assert_eq!(state.hook_count, observations);
                assert_eq!(state.peers[&peer], remote);
                state.error = Some("previous application error".into());
            }
            assert!(!metadata.update(&remote.encode().unwrap()).unwrap());
            assert!(actor.drain(&evidence).is_err());
            assert_eq!(
                evidence.lock().error.as_deref(),
                Some("previous application error")
            );
        }
    }

    #[test]
    fn closing_retained_hooks_cannot_hide_an_already_recorded_error() {
        let fixture = fixture(5);
        let mut actor = Actor::new(Some(Mode::Partial), &config()).unwrap();
        actor.shared = fixture.shared.clone();
        fixture.shared.lock().error = Some("pre-close application error".into());
        let evidence = Evidence::default();
        assert!(actor.prepare(&evidence).is_err());
        assert!(fixture.shared.lock().closed && !actor.prepared);
        let action = fixture
            .partial_action_from_metadata(PeerId::random(), Some(&[0; 500]))
            .unwrap();
        assert!(!action.need && action.send.is_none());
        let mut metadata = fixture.metadata();
        assert!(!metadata.update(&[0; 500]).unwrap());
        assert!(actor.drain(&evidence).is_err());
        assert_eq!(
            evidence.lock().error.as_deref(),
            Some("pre-close application error")
        );
    }

    #[test]
    fn invalid_partial_bytes_and_metadata_cannot_mutate_local_parts() {
        let config = config();
        let actor = Actor::new(Some(Mode::Partial), &config).unwrap();
        let evidence = Evidence::default();
        let peer = PeerId::random();
        let metadata = partial::Metadata {
            revision: 2,
            have: 1,
            want: 6,
        }
        .encode()
        .unwrap();
        let corrupt = partial::encode_part(0, b"not expected").unwrap();
        assert!(apply_input(&actor, peer, Some(&corrupt), Some(&metadata), &evidence).is_err());
        assert!(actor.shared.lock().group.is_none());
        let good = partial::encode_part(0, &partial::part_data(TOKEN, 0).unwrap()).unwrap();
        apply_input(&actor, peer, Some(&good), Some(&metadata), &evidence).unwrap();
        for metadata in [
            partial::Metadata {
                revision: 1,
                have: 1,
                want: 6,
            },
            partial::Metadata {
                revision: 2,
                have: 2,
                want: 5,
            },
            partial::Metadata {
                revision: 3,
                have: 2,
                want: 5,
            },
        ] {
            assert!(
                apply_input(
                    &actor,
                    peer,
                    Some(&good),
                    Some(&metadata.encode().unwrap()),
                    &evidence
                )
                .is_err()
            );
            assert_eq!(actor.shared.lock().group.as_ref().unwrap().metadata.have, 1);
        }
        assert!(apply_input(&actor, PeerId::random(), None, None, &evidence).is_err());
    }

    #[test]
    fn hook_overflow_is_sticky_and_drained_into_final_evidence() {
        let config = config();
        let actor = Actor::new(Some(Mode::Partial), &config).unwrap();
        for _ in 0..=HOOK_LIMIT {
            actor.shared.lock().record(
                "partial_metadata_hook",
                "rust.fixture.tests.synthetic",
                json!({}),
            );
        }
        assert_eq!(actor.shared.lock().hooks.len(), HOOK_LIMIT);
        let evidence = Evidence::default();
        assert!(actor.drain(&evidence).is_err());
        assert!(actor.shared.lock().hooks.is_empty());
        assert!(evidence.lock().overflow);
        assert!(evidence.lock().error.is_some());
        assert!(actor.drain(&evidence).is_err());
        let mut final_result = json!({});
        actor.describe(&mut final_result);
        assert!(final_result["extension_state"]["error"].is_string());
    }

    #[test]
    fn hold_defers_but_cannot_fabricate_native_release_or_cancellation_success() {
        let mut config = config();
        config.version = "1.2".into();
        let mut router =
            super::super::behaviour(&libp2p::identity::Keypair::generate_ed25519(), &config)
                .unwrap();
        let evidence = Evidence::default();
        let mut actor = Actor::new(Some(Mode::Idontwant), &config).unwrap();
        assert!(
            actor
                .command(
                    Operation::Hold("accept:foreign:one".into()),
                    1,
                    &mut router,
                    &config,
                    &evidence
                )
                .is_err()
        );
        let payload = format!("accept:{TOKEN}:idontwant:{}", "x".repeat(1500));
        actor
            .command(
                Operation::Hold(payload.clone()),
                1,
                &mut router,
                &config,
                &evidence,
            )
            .unwrap();
        assert!(
            actor
                .command(Operation::Release, 2, &mut router, &config, &evidence)
                .is_err()
        );
        let peer = PeerId::random();
        let message = gossipsub::Message {
            source: Some(peer),
            data: payload.into_bytes(),
            sequence_number: Some(1),
            topic: config.topic().hash(),
        };
        // An application event alone cannot put this message into the native cache.
        actor
            .event(
                gossipsub::Event::Message {
                    propagation_source: peer,
                    message_id: super::super::signed_message_id(&message),
                    message,
                },
                &mut router,
                &config,
                &evidence,
            )
            .unwrap();
        assert!(
            evidence
                .lock()
                .events
                .iter()
                .any(|event| event["kind"] == "validation_held")
        );
        assert!(
            !evidence
                .lock()
                .events
                .iter()
                .any(|event| event["kind"] == "validation" || event["kind"] == "delivery")
        );
        assert!(actor.prepare(&evidence).is_err());
        assert!(actor.stop(&mut router, &config, &evidence).is_err());
        assert!(actor.hold.is_none());
        assert!(
            evidence
                .lock()
                .events
                .iter()
                .any(|event| event["kind"] == "validation_not_committed")
        );
        assert!(
            evidence
                .lock()
                .events
                .iter()
                .any(|event| event["kind"] == "validation_cancelled"
                    && event["validation_commit"] == false)
        );
        assert!(evidence.lock().error.is_some());
    }

    #[test]
    fn one_hold_deadline_cancels_armed_state_without_blocking_router() {
        let config = config();
        let mut router =
            super::super::behaviour(&libp2p::identity::Keypair::generate_ed25519(), &config)
                .unwrap();
        let evidence = Evidence::default();
        let mut actor = Actor::new(Some(Mode::Idontwant), &config).unwrap();
        let payload = format!("accept:{TOKEN}:one");
        actor
            .command(
                Operation::Hold(payload.clone()),
                1,
                &mut router,
                &config,
                &evidence,
            )
            .unwrap();
        assert!(
            actor
                .command(Operation::Hold(payload), 2, &mut router, &config, &evidence)
                .is_err()
        );
        actor.hold.as_mut().unwrap().deadline = Instant::now();
        assert!(actor.expire(&mut router, &config, &evidence).is_err());
        assert!(actor.hold.is_none());
        assert!(
            evidence
                .lock()
                .events
                .iter()
                .any(|event| event["kind"] == "validation_cancelled"
                    && event["cancellation"] == "ten_second_deadline")
        );
    }
}
