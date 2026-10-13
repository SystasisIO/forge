use super::*;

fn argv() -> Vec<String> {
    [
        "pubsub-live",
        "--version",
        "1.1",
        "--transport",
        "tcp",
        "--actor",
        "victim",
        "--case-token",
        "0123456789abcdef0123456789abcdef",
        "--ready-file",
        "ready.json",
        "--control-file",
        "control.jsonl",
        "--result-file",
        "result.json",
        "--stop-file",
        "stop",
        "--store-dir",
        "store",
    ]
    .into_iter()
    .map(str::to_owned)
    .collect()
}

fn config(version: &str, actor: &str) -> Config {
    let mut config = parse_args(&argv()).unwrap().unwrap().config;
    config.version = version.to_owned();
    config.actor = actor.to_owned();
    config
}

#[test]
fn cli_dispatch_and_strict_ownership_flags() {
    assert!(parse_args(&[]).unwrap().is_none());
    assert!(parse_args(&["listen".into()]).unwrap().is_none());
    assert_eq!(parse_args(&argv()).unwrap().unwrap().actor, "victim");
    for (flag, bad) in [
        ("--version", "1.4"),
        ("--transport", "tcp-tls"),
        ("--actor", "validator"),
        ("--case-token", "0123456789ABCDEF0123456789ABCDEF"),
        ("--result-file", "ready.json"),
    ] {
        let mut args = argv();
        let index = args.iter().position(|s| s == flag).unwrap();
        args[index + 1] = bad.into();
        assert!(parse_args(&args).is_err(), "{flag}");
    }
    for suffix in [
        vec!["--actor", "sink"],
        vec!["--score", "-100"],
        vec!["--mesh", "forced"],
        vec!["--actor"],
    ] {
        let mut args = argv();
        args.extend(suffix.into_iter().map(str::to_owned));
        assert!(parse_args(&args).is_err());
    }
    let mut private = argv();
    let index = private.iter().position(|s| s == "--transport").unwrap();
    private[index + 1] = "tcp-pnet-noise".into();
    assert!(parse_args(&private).is_err());
    private.extend([
        "--pnet-key-file".into(),
        "key".into(),
        "--pnet-fingerprint".into(),
        "0".repeat(64),
    ]);
    assert!(parse_args(&private).is_ok());
}

#[test]
fn extension_versions_preserve_native_protocol_kinds() {
    use libp2p::core::upgrade::UpgradeInfo;
    use libp2p::swarm::{ConnectionHandler, ConnectionId, NetworkBehaviour};

    for version in ["1.0", "1.1", "1.2", "1.3"] {
        let mut args = argv();
        args[2] = version.into();
        let cfg = parse_args(&args).unwrap().unwrap();
        assert_eq!(cfg.protocol(), format!("/meshsub/{version}.0"));
        let key = identity::Keypair::generate_ed25519();
        let mut router = behaviour(&key, &cfg).unwrap();
        let address: Multiaddr = "/ip4/127.0.0.1/tcp/1234".parse().unwrap();
        let handler = router
            .handle_established_inbound_connection(
                ConnectionId::new_unchecked(1),
                PeerId::random(),
                &address,
                &address,
            )
            .unwrap();
        let upgrade = handler.listen_protocol();
        let offered: Vec<_> = upgrade
            .upgrade()
            .protocol_info()
            .map(|protocol| {
                let protocol = protocol.left().expect("active native PubSub handler");
                (protocol.as_ref().to_owned(), format!("{:?}", protocol.kind))
            })
            .collect();
        let expected = match version {
            "1.0" => vec![("/meshsub/1.0.0", "Gossipsub")],
            "1.1" => vec![("/meshsub/1.1.0", "Gossipsubv1_1")],
            _ => vec![
                ("/meshsub/1.3.0", "Gossipsubv1_3"),
                ("/meshsub/1.2.0", "Gossipsubv1_2"),
                ("/meshsub/1.1.0", "Gossipsubv1_1"),
                ("/meshsub/1.0.0", "Gossipsub"),
            ],
        };
        assert_eq!(
            offered,
            expected
                .into_iter()
                .map(|(protocol, kind)| (protocol.to_owned(), kind.to_owned()))
                .collect::<Vec<_>>()
        );
    }
}

#[test]
fn extension_cli_modes_are_optional_and_version_exact() {
    assert!(parse_args(&argv()).unwrap().unwrap().extension.is_none());
    for version in ["1.0", "1.1", "1.2", "1.3"] {
        for mode in ["idontwant", "partial", "advertisement", "unknown"] {
            let mut args = argv();
            args[2] = version.into();
            args.extend(["--extension".into(), mode.into()]);
            let expected = mode == "idontwant" && version == "1.2"
                || matches!(mode, "partial" | "advertisement") && version == "1.3";
            assert_eq!(parse_args(&args).is_ok(), expected, "{version}/{mode}");
            if expected {
                let cfg = parse_args(&args).unwrap().unwrap();
                let actor = extensions::Actor::new(cfg.extension, &cfg).unwrap();
                let mut ready = json!({"topic": cfg.topic().hash().to_string()});
                actor.readiness(&mut ready);
                assert_eq!(ready.as_object().unwrap().len(), 4);
                assert_eq!(ready["version"], version);
                assert_eq!(ready["extension"], mode);
                assert_eq!(ready["requests_partial"], mode == "partial");
                assert_eq!(ready["topic"], format!("forge-pr11:{}", cfg.token));
            }
        }
    }
    let mut duplicate = argv();
    duplicate[2] = "1.3".into();
    duplicate.extend([
        "--extension".into(),
        "partial".into(),
        "--extension".into(),
        "advertisement".into(),
    ]);
    assert!(parse_args(&duplicate).is_err());
}

#[test]
fn extension_commands_have_exact_fields_and_byte_bounds() {
    for row in [
        json!({"sequence": 1, "kind": "publish_extension", "payload": "x"}),
        json!({"sequence": 1, "kind": "publish_extension", "payload": "x".repeat(4096)}),
        json!({"sequence": 1, "kind": "validation_hold", "payload": "accept:case:one"}),
        json!({"sequence": 1, "kind": "validation_release"}),
        json!({"sequence": 1, "kind": "partial_offer", "have": 0}),
        json!({"sequence": 1, "kind": "partial_offer", "have": 7}),
    ] {
        assert_eq!(
            Control::default()
                .ingest(format!("{row}\n").as_bytes())
                .unwrap()[0]
                .sequence(),
            1
        );
    }
    for row in [
        json!({"sequence": 1, "kind": "publish_extension", "payload": ""}),
        json!({"sequence": 1, "kind": "publish_extension", "payload": "x".repeat(4097)}),
        json!({"sequence": 1, "kind": "validation_hold", "payload": "x\u{0000}"}),
        json!({"sequence": 1, "kind": "validation_release", "observation_sequence": 2}),
        json!({"sequence": 1, "kind": "partial_offer"}),
        json!({"sequence": 1, "kind": "partial_offer", "have": -1}),
        json!({"sequence": 1, "kind": "partial_offer", "have": 1.5}),
        json!({"sequence": 1, "kind": "partial_offer", "have": 8}),
        json!({"sequence": 1, "kind": "partial_offer", "have": 1, "revision": 2}),
        json!({"sequence": 1, "kind": "partial_offer", "have": 1, "group_sequence": 2}),
        json!({"sequence": 1, "kind": "publish_large", "payload": "legacy"}),
    ] {
        assert!(
            Control::default()
                .ingest(format!("{row}\n").as_bytes())
                .is_err(),
            "{row}"
        );
    }
}

#[test]
fn extension_admission_does_not_change_legacy_actor() {
    let cfg = config("1.1", "victim");
    let evidence = Evidence::default();
    let mut actor = extensions::Actor::new(None, &cfg).unwrap();
    let mut router = behaviour(&identity::Keypair::generate_ed25519(), &cfg).unwrap();
    actor.subscribe(&mut router, &cfg).unwrap();
    let mut ready = json!({});
    actor.describe(&mut ready);
    assert_eq!(ready, json!({}));
    assert!(
        actor
            .command(
                extensions::Operation::Publish("forbidden".into()),
                1,
                &mut router,
                &cfg,
                &evidence
            )
            .is_err()
    );
    assert!(evidence.lock().events.is_empty());
}

#[test]
fn configured_signed_message_id_matches_raw_peer_and_big_endian_sequence_vector() {
    let mut peer_bytes = vec![0x00, 0x24, 0x08, 0x01, 0x12, 0x20];
    peer_bytes.extend(0u8..32);
    let peer = PeerId::from_bytes(&peer_bytes).unwrap();
    let mut message = gossipsub::Message {
        source: Some(peer),
        data: b"signed mapping vector".to_vec(),
        sequence_number: Some(0x0102030405060708),
        topic: gossipsub::IdentTopic::new("vector").hash(),
    };
    let expected = "002408011220000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f0102030405060708";
    for version in ["1.0", "1.1"] {
        let router = router_config(&config(version, "victim")).unwrap();
        let id = router.message_id(&message);
        assert_eq!(hex(&id.0), expected);
        assert_eq!(&id.0[..peer_bytes.len()], peer_bytes.as_slice());
        message.data = b"different payload, same author and sequence".to_vec();
        message.topic = gossipsub::IdentTopic::new("other-topic").hash();
        assert_eq!(router.message_id(&message), id);
    }
}

#[test]
fn idontwant_cache_and_mesh_are_mode_scoped_and_keep_the_gossip_window() {
    for (version, mode, history) in [
        ("1.0", None, 5),
        ("1.1", None, 5),
        ("1.2", None, 5),
        ("1.3", None, 5),
        ("1.2", Some(extensions::Mode::Idontwant), 64),
        ("1.3", Some(extensions::Mode::Partial), 5),
        ("1.3", Some(extensions::Mode::Advertisement), 5),
    ] {
        for role in ["victim", "offender", "replacement", "sink"] {
            let config = config(version, role);
            let router = router_config_with_mode(&config, mode).unwrap();
            let mesh = if mode == Some(extensions::Mode::Idontwant) {
                (3, 3, 4)
            } else {
                (2, 1, 4)
            };
            assert_eq!(
                (router.mesh_n(), router.mesh_n_low(), router.mesh_n_high()),
                mesh
            );
            assert_eq!(router.mesh_outbound_min(), 0);
            assert_eq!(router.retain_scores(), 1);
            assert_eq!(router.history_length(), history);
            assert_eq!(router.history_gossip(), 3);
            assert_eq!(router.heartbeat_interval(), Duration::from_millis(250));
            assert!(router.validate_messages());
        }
    }
}

#[test]
fn final_router_parameters_and_victim_only_predicates() {
    for version in ["1.0", "1.1"] {
        let cfg = config(version, "victim");
        let router = router_config(&cfg).unwrap();
        assert_eq!(
            (
                router.mesh_n(),
                router.mesh_n_low(),
                router.mesh_n_high(),
                router.retain_scores(),
                router.mesh_outbound_min()
            ),
            (2, 1, 4, 1, 0)
        );
        assert!(!router.flood_publish() && !router.do_px() && router.validate_messages());
        assert_eq!(router.prune_backoff(), Duration::from_secs(1));
        assert_eq!(router.heartbeat_interval(), Duration::from_millis(250));
        let params = score_params(cfg.topic().hash());
        assert!(params.validate().is_ok());
        assert_eq!(params.decay_interval, Duration::from_secs(1));
        assert_eq!(params.retain_score, Duration::from_secs(60));
        assert_eq!(
            (
                params.app_specific_weight,
                params.ip_colocation_factor_weight,
                params.behaviour_penalty_weight,
                params.slow_peer_weight
            ),
            (0.0, 0.0, 0.0, 0.0)
        );
        let topic = &params.topics[&cfg.topic().hash()];
        assert_eq!(
            (
                topic.topic_weight,
                topic.invalid_message_deliveries_weight,
                topic.invalid_message_deliveries_decay
            ),
            (1.0, -100.0, 0.99)
        );
        assert_eq!(
            (
                topic.time_in_mesh_weight,
                topic.first_message_deliveries_weight,
                topic.mesh_message_deliveries_weight,
                topic.mesh_failure_penalty_weight
            ),
            (0.0, 0.0, 0.0, 0.0)
        );
    }
    for actor in ["victim", "offender", "replacement", "sink"] {
        let cfg = config("1.1", actor);
        let rejected = validation(&cfg, format!("reject:{}:one", cfg.token).as_bytes());
        let ignored = validation(&cfg, format!("ignore:{}:one", cfg.token).as_bytes());
        assert!(matches!(rejected, MessageAcceptance::Reject) == (actor == "victim"));
        assert!(matches!(ignored, MessageAcceptance::Ignore) == (actor == "victim"));
        assert!(matches!(
            validation(&cfg, b"reject:foreign:one"),
            MessageAcceptance::Accept
        ));
    }
}

#[test]
fn control_reads_complete_lines_only_and_rejects_rewrites_gaps_and_limits() {
    let one = b"{\"sequence\":1,\"kind\":\"sample\",\"label\":\"before\"}\n";
    let mut control = Control::default();
    assert!(control.ingest(&one[..one.len() - 1]).unwrap().is_empty());
    assert!(control.finish().is_err());
    assert_eq!(control.ingest(one).unwrap()[0].sequence(), 1);
    assert!(control.finish().is_ok());
    assert!(control.ingest(one).unwrap().is_empty());
    assert!(control.ingest(&[]).is_err());
    assert!(
        control
            .ingest(&[one.as_slice(), one.as_slice()].concat())
            .is_err()
    );
    for row in [
        json!({"sequence": 2, "kind": "sample", "label": "gap"}),
        json!({"sequence": 1, "kind": "graft"}),
        json!({"sequence": 1, "kind": "publish", "payload": "a".repeat(PAYLOAD_LIMIT + 1)}),
        json!({"sequence": 1, "kind": "sample", "label": "x", "score": 7}),
    ] {
        let bytes = format!("{row}\n");
        assert!(Control::default().ingest(bytes.as_bytes()).is_err());
    }
    assert!(
        Control::default()
            .ingest(&vec![b'x'; LINE_LIMIT + 1])
            .is_err()
    );
    let bytes = (1..=COMMAND_LIMIT + 1)
        .map(|sequence| {
            format!(
                "{}\n",
                json!({"sequence": sequence, "kind": "sample", "label": "periodic"})
            )
        })
        .collect::<String>();
    assert!(Control::default().ingest(bytes.as_bytes()).is_err());
}

#[test]
fn snapshots_expose_native_score_hook_gap_instead_of_reconstructing_counter() {
    let cfg = config("1.1", "victim");
    let key = identity::Keypair::generate_ed25519();
    let router = behaviour(&key, &cfg).unwrap();
    let evidence = Evidence::default();
    snapshot(&router, &cfg, &BTreeSet::new(), "before", &evidence).unwrap();
    let capture = evidence.lock();
    assert_eq!(capture.events[0]["mesh_peer_ids"], json!([]));
    assert_eq!(capture.events[0]["peer_scores"], json!([]));
    drop(capture);
    let report = result(
        &cfg,
        Some(key.public().to_peer_id()),
        &evidence,
        false,
        false,
        Value::Null,
        Value::Null,
    );
    assert!(report.get("acceptance_ready").is_none());
    assert!(
        report["evidence_gaps"][0]
            .as_str()
            .unwrap()
            .contains("invalid_deliveries")
    );
}

struct NativeActor {
    swarm: Swarm<gossipsub::Behaviour>,
    config: Config,
    evidence: Evidence,
    upgrades: crate::upgrade_observer::Observer,
    tasks: crate::task_owner::Owner,
    peers: BTreeSet<PeerId>,
    pending: BTreeMap<PeerId, PendingConnect>,
    extension: Option<extensions::Actor>,
}

impl NativeActor {
    fn new(version: &str, role: &str) -> Self {
        Self::new_with_mode(version, role, None)
    }

    fn new_with_mode(version: &str, role: &str, mode: Option<extensions::Mode>) -> Self {
        let config = config(version, role);
        let evidence = Evidence::default();
        let upgrades = crate::upgrade_observer::Observer::default();
        let tasks = crate::task_owner::Owner::default();
        let swarm = match mode {
            Some(extensions::Mode::Idontwant) => {
                transport::new_swarm_with_mode(&config, mode, &evidence, &upgrades, &tasks)
            }
            _ => new_swarm(&config, &evidence, &upgrades, &tasks),
        }
        .unwrap();
        Self {
            swarm,
            config,
            evidence,
            upgrades,
            tasks,
            peers: BTreeSet::new(),
            pending: BTreeMap::new(),
            extension: None,
        }
    }
    fn event(&mut self, event: SwarmEvent<gossipsub::Event>) -> io::Result<Option<Multiaddr>> {
        let event = match event {
            SwarmEvent::Behaviour(event) if self.extension.is_some() => {
                let actor = self.extension.as_mut().unwrap();
                let remaining = actor.event(
                    event,
                    self.swarm.behaviour_mut(),
                    &self.config,
                    &self.evidence,
                )?;
                actor.drain(&self.evidence)?;
                match remaining {
                    Some(event) => SwarmEvent::Behaviour(event),
                    None => return Ok(None),
                }
            }
            event => event,
        };
        handle_event(
            event,
            &mut self.swarm,
            &self.config,
            &self.evidence,
            &self.upgrades,
            &mut self.peers,
            &mut self.pending,
        )
    }

    fn with_application(mut self, mode: extensions::Mode) -> Self {
        let actor = extensions::Actor::new(Some(mode), &self.config).unwrap();
        actor
            .subscribe(self.swarm.behaviour_mut(), &self.config)
            .unwrap();
        self.extension = Some(actor);
        self
    }

    fn extension_command(
        &mut self,
        operation: extensions::Operation,
        sequence: u64,
    ) -> io::Result<Value> {
        self.extension.as_mut().unwrap().command(
            operation,
            sequence,
            self.swarm.behaviour_mut(),
            &self.config,
            &self.evidence,
        )
    }
    fn has(&self, kind: &str, outcome: &str) -> bool {
        self.evidence
            .lock()
            .events
            .iter()
            .any(|e| e["kind"] == kind && (outcome.is_empty() || e["outcome"] == outcome))
    }

    fn native_state(&self) -> Value {
        let mesh: Vec<_> = self
            .swarm
            .behaviour()
            .mesh_peers(&self.config.topic().hash())
            .map(PeerId::to_string)
            .collect();
        let scores: Vec<_> = self
            .peers
            .iter()
            .map(|peer| {
                json!({"peer_id": peer.to_string(), "value": self.swarm.behaviour().peer_score(peer)})
            })
            .collect();
        let capture = self.evidence.lock();
        let mut event_counts = BTreeMap::<&str, usize>::new();
        for event in &capture.events {
            *event_counts
                .entry(event["kind"].as_str().unwrap_or("unknown"))
                .or_default() += 1;
        }
        json!({"version": self.config.version, "actor": self.config.actor,
            "mesh_peer_ids": mesh, "peer_scores": scores, "event_counts": event_counts,
            "capture_error": capture.error, "overflow": capture.overflow})
    }
}

async fn connect_native_pair(a: &mut NativeActor, b: &mut NativeActor) {
    a.swarm
        .listen_on("/ip4/127.0.0.1/tcp/0".parse().unwrap())
        .unwrap();
    let address = tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            let event = a.swarm.next().await.unwrap();
            if let Some(address) = a.event(event).unwrap() {
                break address;
            }
        }
    })
    .await
    .unwrap();
    b.swarm
        .dial(
            DialOpts::peer_id(*a.swarm.local_peer_id())
                .addresses(vec![address])
                .build(),
        )
        .unwrap();
    drive_until(a, b, "native pair mesh", |a, b| {
        a.swarm
            .behaviour()
            .mesh_peers(&a.config.topic().hash())
            .any(|peer| peer == b.swarm.local_peer_id())
            && b.swarm
                .behaviour()
                .mesh_peers(&b.config.topic().hash())
                .any(|peer| peer == a.swarm.local_peer_id())
    })
    .await;
}

async fn dispose_native_pair(mut a: NativeActor, mut b: NativeActor) {
    let (a_close, b_close) = futures::join!(
        close_native(&mut a.swarm, &a.evidence),
        close_native(&mut b.swarm, &b.evidence)
    );
    a_close.unwrap();
    b_close.unwrap();
    drop(a.swarm);
    drop(b.swarm);
    for (tasks, evidence) in [(a.tasks, a.evidence), (b.tasks, b.evidence)] {
        assert_eq!(
            tasks.close_and_join().await.snapshot()["fixture_owned_tasks_joined"],
            true
        );
        let capture = evidence.lock();
        assert_eq!((capture.live_muxers, capture.live_streams), (0, 0));
        assert!(!capture.overflow);
    }
}

async fn drive_until(
    a: &mut NativeActor,
    b: &mut NativeActor,
    stage: &str,
    predicate: impl Fn(&NativeActor, &NativeActor) -> bool,
) {
    let mut observation = tokio::time::interval(Duration::from_millis(25));
    observation.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    let completed = tokio::time::timeout(Duration::from_secs(10), async {
        loop {
            for actor in [&*a, &*b] {
                if let Some(extension) = &actor.extension {
                    // Metadata hooks can run while native Swarm::poll stays Pending.
                    extension.drain(&actor.evidence).unwrap();
                }
                let failed = {
                    let capture = actor.evidence.lock();
                    capture.overflow || capture.error.is_some()
                };
                assert!(
                    !failed,
                    "native capture failure at {stage}: {}",
                    actor.native_state()
                );
            }
            if predicate(a, b) {
                break;
            }
            tokio::select! {
                event = a.swarm.next() => { a.event(event.unwrap()).unwrap(); },
                event = b.swarm.next() => { b.event(event.unwrap()).unwrap(); },
                // Native heartbeat/PRUNE can change observed state without a SwarmEvent.
                _ = observation.tick() => {},
            }
        }
    })
    .await;
    assert!(
        completed.is_ok(),
        "native predicate deadline at {stage}: a={}, b={}",
        a.native_state(),
        b.native_state()
    );
}

#[tokio::test]
async fn unsuccessful_native_validation_report_cannot_emit_committed_validation_or_delivery() {
    let mut actor = NativeActor::new("1.1", "victim");
    let remote = PeerId::random();
    let event = SwarmEvent::Behaviour(gossipsub::Event::Message {
        propagation_source: remote,
        message_id: gossipsub::MessageId::new(b"not-in-native-cache"),
        message: gossipsub::Message {
            source: Some(remote),
            data: b"accept:cache-miss".to_vec(),
            sequence_number: Some(1),
            topic: actor.config.topic().hash(),
        },
    });
    assert!(
        actor
            .event(event)
            .unwrap_err()
            .to_string()
            .contains("returned false")
    );
    assert!(!actor.has("validation", "") && !actor.has("delivery", ""));
    assert!(actor.has("validation_not_committed", "accept"));
    assert_eq!(
        actor.evidence.lock().events.last().unwrap()["validation_commit"],
        false
    );
    drop(actor.swarm);
    assert_eq!(
        actor.tasks.close_and_join().await.snapshot()["fixture_owned_tasks_joined"],
        true
    );
}

#[tokio::test]
async fn real_held_signed_message_after_legacy_cache_window_fails_fatally() {
    // Intentionally use the unchanged five-slot legacy router, not IDONTWANT's override.
    let mut receiver =
        NativeActor::new("1.2", "victim").with_application(extensions::Mode::Idontwant);
    let mut publisher = NativeActor::new("1.2", "offender");
    connect_native_pair(&mut receiver, &mut publisher).await;
    let payload = format!(
        "accept:{}:real-held:{}",
        receiver.config.token,
        "x".repeat(1500)
    );
    receiver
        .extension_command(extensions::Operation::Hold(payload.clone()), 1)
        .unwrap();
    let id = publisher
        .swarm
        .behaviour_mut()
        .publish(publisher.config.topic(), payload.as_bytes())
        .unwrap();
    drive_until(
        &mut receiver,
        &mut publisher,
        "actual signed hold",
        |r, _| r.has("validation_held", ""),
    )
    .await;
    let started = Instant::now();
    drive_until(
        &mut receiver,
        &mut publisher,
        "poll beyond five heartbeats",
        |_, _| started.elapsed() >= Duration::from_secs(2),
    )
    .await;
    assert!(!receiver.has("validation", "") && !receiver.has("delivery", ""));
    let failure = receiver
        .extension_command(extensions::Operation::Release, 2)
        .unwrap_err();
    assert!(failure.to_string().contains("returned false"));
    {
        let capture = receiver.evidence.lock();
        assert!(capture.error.as_deref().unwrap().contains("returned false"));
        let failed = capture
            .events
            .iter()
            .find(|event| event["kind"] == "validation_not_committed")
            .unwrap();
        assert_eq!(failed["message_id"], hex(&id.0));
        assert_eq!(
            failed["author_peer"],
            publisher.swarm.local_peer_id().to_string()
        );
        assert_eq!(
            failed["propagation_peer"],
            publisher.swarm.local_peer_id().to_string()
        );
        assert_eq!(failed["validation_commit"], false);
    }
    assert!(!receiver.has("validation", "") && !receiver.has("delivery", ""));
    dispose_native_pair(receiver, publisher).await;
}

#[tokio::test]
async fn real_held_signed_message_releases_after_the_old_cache_window_in_idontwant_mode() {
    let mut receiver =
        NativeActor::new_with_mode("1.2", "victim", Some(extensions::Mode::Idontwant))
            .with_application(extensions::Mode::Idontwant);
    let mut publisher = NativeActor::new("1.2", "offender");
    connect_native_pair(&mut receiver, &mut publisher).await;
    let payload = format!(
        "accept:{}:real-held:{}",
        receiver.config.token,
        "x".repeat(1500)
    );
    receiver
        .extension_command(extensions::Operation::Hold(payload.clone()), 1)
        .unwrap();
    let id = publisher
        .swarm
        .behaviour_mut()
        .publish(publisher.config.topic(), payload.as_bytes())
        .unwrap();
    drive_until(
        &mut receiver,
        &mut publisher,
        "actual signed IDONTWANT hold",
        |r, _| r.has("validation_held", ""),
    )
    .await;
    let started = Instant::now();
    drive_until(
        &mut receiver,
        &mut publisher,
        "IDONTWANT cache past five heartbeats",
        |_, _| started.elapsed() >= Duration::from_secs(2),
    )
    .await;
    assert!(!receiver.has("validation", "") && !receiver.has("delivery", ""));
    let released = receiver
        .extension_command(extensions::Operation::Release, 2)
        .unwrap();
    assert_eq!(released["validation_commit"], true);
    assert_eq!(released["message_id"], hex(&id.0));
    assert_eq!(released["error"], Value::Null);
    assert!(receiver.has("validation", "accept") && receiver.has("delivery", "accept"));
    assert!(!receiver.has("validation_not_committed", ""));
    {
        let capture = receiver.evidence.lock();
        assert!(!capture.overflow && capture.error.is_none());
        let held = capture
            .events
            .iter()
            .find(|event| event["kind"] == "validation_held")
            .unwrap();
        let committed = capture
            .events
            .iter()
            .find(|event| event["kind"] == "validation")
            .unwrap();
        assert_eq!(held["message_id"], committed["message_id"]);
        assert_eq!(committed["message_id"], hex(&id.0));
        assert_eq!(
            committed["author_peer"],
            publisher.swarm.local_peer_id().to_string()
        );
        assert_eq!(
            committed["propagation_peer"],
            publisher.swarm.local_peer_id().to_string()
        );
        assert_eq!(committed["seqno_hex"], held["seqno_hex"]);
        assert_eq!(
            committed["payload_sha256"],
            format!("{:x}", Sha256::digest(payload.as_bytes()))
        );
        assert_eq!(committed["validation_commit"], true);
        assert_eq!(committed["report_message_validation_result"], true);
        assert_eq!(held["committed"], false);
        assert!(committed["sequence"].as_u64().unwrap() > held["sequence"].as_u64().unwrap());
        let requested = capture.events.iter().find(|event| event["kind"] == "validation_release_requested").unwrap();
        assert_eq!(requested["message_id"], committed["message_id"]);
        assert_eq!(requested["seqno_hex"], held["seqno_hex"]);
        assert_eq!(requested["hold_command_sequence"], 1);
        assert_eq!(requested["command_sequence"], 2);
        assert_eq!(requested["validation_commit"], false);
        assert!(held["sequence"].as_u64().unwrap() < requested["sequence"].as_u64().unwrap());
        assert!(requested["sequence"].as_u64().unwrap() < committed["sequence"].as_u64().unwrap());
    }
    receiver
        .extension
        .as_mut()
        .unwrap()
        .prepare(&receiver.evidence)
        .unwrap();
    receiver
        .extension
        .as_mut()
        .unwrap()
        .stop(
            receiver.swarm.behaviour_mut(),
            &receiver.config,
            &receiver.evidence,
        )
        .unwrap();
    dispose_native_pair(receiver, publisher).await;
}

#[tokio::test]
async fn closed_partial_application_does_not_disable_native_io_observations() {
    for prepare in [true, false] {
        let mut receiver =
            NativeActor::new("1.3", "victim").with_application(extensions::Mode::Partial);
        let mut publisher =
            NativeActor::new("1.3", "offender").with_application(extensions::Mode::Partial);
        connect_native_pair(&mut receiver, &mut publisher).await;
        receiver
            .extension_command(extensions::Operation::Offer(5), 1)
            .unwrap();
        publisher
            .extension_command(extensions::Operation::Offer(2), 1)
            .unwrap();
        drive_until(
            &mut receiver,
            &mut publisher,
            "actual retained partial hooks",
            |r, _| r.has("partial_action_hook", ""),
        )
        .await;
        {
            let actor = receiver.extension.as_mut().unwrap();
            if prepare {
                actor.prepare(&receiver.evidence).unwrap();
            } else {
                actor
                    .stop(
                        receiver.swarm.behaviour_mut(),
                        &receiver.config,
                        &receiver.evidence,
                    )
                    .unwrap();
            }
        }
        let mut before = json!({});
        receiver.extension.as_ref().unwrap().describe(&mut before);
        let io_before = receiver
            .evidence
            .lock()
            .events
            .iter()
            .filter(|event| event["kind"] == "rpc" && event["direction"] == "read")
            .count();
        publisher
            .extension_command(extensions::Operation::Offer(4), 2)
            .unwrap();
        drive_until(
            &mut receiver,
            &mut publisher,
            "native I/O after application closure",
            |r, _| {
                r.evidence
                    .lock()
                    .events
                    .iter()
                    .filter(|event| event["kind"] == "rpc" && event["direction"] == "read")
                    .count()
                    > io_before
            },
        )
        .await;
        receiver
            .extension
            .as_ref()
            .unwrap()
            .drain(&receiver.evidence)
            .unwrap();
        let mut after = json!({});
        receiver.extension.as_ref().unwrap().describe(&mut after);
        assert_eq!(after["extension_state"]["admission_closed"], true);
        assert_eq!(after["extension_state"]["pending_hooks"], 0);
        assert_eq!(
            after["extension_state"]["hook_observations"],
            before["extension_state"]["hook_observations"]
        );
        assert_eq!(after["extension_state"]["error"], Value::Null);
        dispose_native_pair(receiver, publisher).await;
    }
}

#[tokio::test]
async fn real_behaviour_signed_ignore_then_reject_scores_and_prunes_without_fixture_injection() {
    for version in ["1.0", "1.1"] {
        let mut victim = NativeActor::new(version, "victim");
        let mut offender = NativeActor::new(version, "offender");
        victim
            .swarm
            .listen_on("/ip4/127.0.0.1/tcp/0".parse().unwrap())
            .unwrap();
        let address = tokio::time::timeout(Duration::from_secs(5), async {
            loop {
                let event = victim.swarm.next().await.unwrap();
                if let Some(address) = victim.event(event).unwrap() {
                    break address;
                }
            }
        })
        .await
        .unwrap();
        offender
            .swarm
            .dial(
                DialOpts::peer_id(*victim.swarm.local_peer_id())
                    .addresses(vec![address])
                    .build(),
            )
            .unwrap();
        drive_until(&mut victim, &mut offender, "initial signed mesh", |v, o| {
            v.swarm
                .behaviour()
                .mesh_peers(&v.config.topic().hash())
                .any(|p| p == o.swarm.local_peer_id())
                && o.swarm
                    .behaviour()
                    .mesh_peers(&o.config.topic().hash())
                    .any(|p| p == v.swarm.local_peer_id())
        })
        .await;
        assert_eq!(
            victim
                .swarm
                .behaviour()
                .peer_score(offender.swarm.local_peer_id()),
            Some(0.0)
        );
        offender
            .swarm
            .behaviour_mut()
            .publish(
                offender.config.topic(),
                format!("ignore:{}:one", offender.config.token),
            )
            .unwrap();
        drive_until(&mut victim, &mut offender, "committed ignore", |v, _| {
            v.has("validation", "ignore")
        })
        .await;
        assert_eq!(
            victim
                .swarm
                .behaviour()
                .peer_score(offender.swarm.local_peer_id()),
            Some(0.0)
        );
        offender
            .swarm
            .behaviour_mut()
            .publish(
                offender.config.topic(),
                format!("reject:{}:one", offender.config.token),
            )
            .unwrap();
        drive_until(
            &mut victim,
            &mut offender,
            "native negative score",
            |v, o| {
                v.swarm
                    .behaviour()
                    .peer_score(o.swarm.local_peer_id())
                    .is_some_and(|score| score < -80.0)
            },
        )
        .await;
        assert!(victim.has("validation", "reject"));
        assert!(!victim.has("delivery", ""));
        drive_until(
            &mut victim,
            &mut offender,
            "native mesh removal and PRUNE receipt",
            |v, o| {
                !v.swarm
                    .behaviour()
                    .mesh_peers(&v.config.topic().hash())
                    .any(|p| p == o.swarm.local_peer_id())
                    && o.has("prune", "")
            },
        )
        .await;
        {
            let capture = victim.evidence.lock();
            for event in capture.events.iter().filter(|e| e["kind"] == "validation") {
                assert_eq!(event["validation_commit"], true);
            }
            assert!(
                capture
                    .events
                    .iter()
                    .filter(|e| e["kind"] == "protocol")
                    .all(|e| e["protocol"] == victim.config.protocol())
            );
            let prune = capture
                .events
                .iter()
                .find(|e| e["kind"] == "prune" && e["direction"] == "send")
                .unwrap();
            if version == "1.0" {
                assert!(prune.get("backoff").is_none() && prune.get("px").is_none());
            }
        }
        let (v_close, o_close) = futures::join!(
            close_native(&mut victim.swarm, &victim.evidence),
            close_native(&mut offender.swarm, &offender.evidence)
        );
        v_close.unwrap();
        o_close.unwrap();
        drop(victim.swarm);
        drop(offender.swarm);
        for (tasks, evidence) in [
            (victim.tasks, victim.evidence),
            (offender.tasks, offender.evidence),
        ] {
            let join = tasks.close_and_join().await.snapshot();
            assert_eq!(join["fixture_owned_tasks_joined"], true);
            let capture = evidence.lock();
            assert_eq!((capture.live_muxers, capture.live_streams), (0, 0));
            assert!(!capture.overflow);
        }
    }
}

// Explicit unit-only Swarm error models. Their use does not claim a real
// network fault; native connection disposal and owned task joins below are real.
fn close_error_event(mode: &str, peer: PeerId) -> SwarmEvent<gossipsub::Event> {
    let listener_id = libp2p::core::transport::ListenerId::next();
    match mode {
        "listener" => SwarmEvent::ListenerError {
            listener_id,
            error: io::Error::from_raw_os_error(5),
        },
        "listener_closed" => SwarmEvent::ListenerClosed {
            listener_id,
            addresses: Vec::new(),
            reason: Err(io::Error::from_raw_os_error(13)),
        },
        "incoming" => SwarmEvent::IncomingConnectionError {
            connection_id: libp2p::swarm::ConnectionId::new_unchecked(1000),
            local_addr: "/ip4/127.0.0.1/tcp/1".parse().unwrap(),
            send_back_addr: "/ip4/127.0.0.1/tcp/2".parse().unwrap(),
            peer_id: Some(peer),
            error: libp2p::swarm::ListenError::Aborted,
        },
        "outgoing" => SwarmEvent::OutgoingConnectionError {
            connection_id: libp2p::swarm::ConnectionId::new_unchecked(1001),
            peer_id: Some(peer),
            error: libp2p::swarm::DialError::Aborted,
        },
        "normal_listener_closed" => SwarmEvent::ListenerClosed {
            listener_id,
            addresses: Vec::new(),
            reason: Ok(()),
        },
        _ => panic!("unknown unit-only close event model"),
    }
}

#[test]
fn native_error_classification_preserves_listener_causes_and_normal_listener_none() {
    let peer = PeerId::random();
    for (mode, errno) in [("listener", 5), ("listener_closed", 13)] {
        let error = checked_native_event(close_error_event(mode, peer)).unwrap_err();
        assert_eq!(error.raw_os_error(), Some(errno));
    }
    for mode in ["incoming", "outgoing"] {
        let error = checked_native_event(close_error_event(mode, peer)).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::InvalidInput);
        assert!(
            error
                .to_string()
                .starts_with(&format!("native {mode} connection error:"))
        );
    }
    assert!(matches!(
        checked_native_event(close_error_event("normal_listener_closed", peer)).unwrap(),
        SwarmEvent::ListenerClosed { reason: Ok(()), .. }
    ));
}

#[tokio::test]
async fn active_outgoing_error_keeps_pending_command_error_completion() {
    let mut actor = NativeActor::new("1.1", "victim");
    let peer = PeerId::random();
    actor.pending.insert(
        peer,
        PendingConnect {
            sequence: 1,
            started: Instant::now(),
        },
    );
    let completion = actor.event(close_error_event("outgoing", peer));
    let pending_empty = actor.pending.is_empty();
    drop(actor.swarm);
    let join = actor.tasks.close_and_join().await.snapshot();
    let error = completion.unwrap_err();
    assert!(
        error
            .to_string()
            .starts_with("native outgoing connection error:")
    );
    assert!(pending_empty);
    let capture = actor.evidence.lock();
    let completions = capture
        .events
        .iter()
        .filter(|event| event["kind"] == "command_done")
        .collect::<Vec<_>>();
    assert_eq!(completions.len(), 1);
    let completed = completions[0];
    assert_eq!(
        completed["source"],
        "rust.fixture.control-native-operation-completion"
    );
    assert_eq!(completed["command_sequence"], 1);
    assert_eq!(completed["command_kind"], "connect");
    assert_eq!(completed["status"], "error");
    assert_eq!(
        completed["native_result"]["error"],
        libp2p::swarm::DialError::Aborted.to_string()
    );
    assert_eq!(join["fixture_owned_tasks_joined"], true);
    assert_eq!(join["overflow"], false);
    assert!(join["errors"].as_array().unwrap().is_empty());
}

#[tokio::test]
async fn native_close_error_after_prepare_is_sticky_and_still_joins_real_owners() {
    for mode in [
        "listener",
        "listener_closed",
        "incoming",
        "outgoing",
        "normal_listener_closed",
    ] {
        let mut victim = NativeActor::new("1.1", "victim");
        let mut offender = NativeActor::new("1.1", "offender");
        victim
            .swarm
            .listen_on("/ip4/127.0.0.1/tcp/0".parse().unwrap())
            .unwrap();
        let address = tokio::time::timeout(Duration::from_secs(5), async {
            loop {
                let event = victim.swarm.next().await.unwrap();
                if let Some(address) = victim.event(event).unwrap() {
                    break address;
                }
            }
        })
        .await
        .unwrap();
        offender
            .swarm
            .dial(
                DialOpts::peer_id(*victim.swarm.local_peer_id())
                    .addresses(vec![address])
                    .build(),
            )
            .unwrap();
        drive_until(
            &mut victim,
            &mut offender,
            "native mesh before close regression",
            |v, o| {
                v.swarm
                    .behaviour()
                    .mesh_peers(&v.config.topic().hash())
                    .any(|p| p == o.swarm.local_peer_id())
                    && o.swarm
                        .behaviour()
                        .mesh_peers(&o.config.topic().hash())
                        .any(|p| p == v.swarm.local_peer_id())
            },
        )
        .await;
        for actor in [&mut victim, &mut offender] {
            let prepare = Command::PrepareShutdown {
                sequence: 1,
                actor: actor.config.actor.clone(),
                token: actor.config.token.clone(),
                local: *actor.swarm.local_peer_id(),
            };
            command(
                prepare,
                &mut actor.swarm,
                &actor.config,
                &actor.evidence,
                &actor.peers,
                &mut actor.pending,
            )
            .unwrap();
            let capture = actor.evidence.lock();
            assert!(capture.prepared && capture.error.is_none());
            assert!(!capture.connections.is_empty());
            assert!(capture.live_muxers > 0);
        }
        let victim_peer = *victim.swarm.local_peer_id();
        let offender_peer = *offender.swarm.local_peer_id();
        let mut closing = NativeClose::begin(&mut victim.swarm, &victim.evidence);
        let peer_closing = NativeClose::begin(&mut offender.swarm, &offender.evidence);
        closing.observe(close_error_event(mode, offender_peer), &victim.evidence);
        let first = victim.evidence.lock().error.clone();
        if mode != "normal_listener_closed" {
            closing.observe(
                close_error_event("listener_closed", offender_peer),
                &victim.evidence,
            );
        }
        let after_second = victim.evidence.lock().error.clone();
        let (v_close, o_close) = futures::join!(
            closing.finish(&mut victim.swarm, &victim.evidence),
            peer_closing.finish(&mut offender.swarm, &offender.evidence)
        );
        let v_recorded = victim.evidence.lock().error.clone();
        let o_recorded = offender.evidence.lock().error.clone();
        let v_disconnected = victim.swarm.connected_peers().next().is_none();
        let o_disconnected = offender.swarm.connected_peers().next().is_none();
        drop(victim.swarm);
        drop(offender.swarm);
        let (v_join, o_join) = futures::join!(
            tokio::time::timeout(Duration::from_secs(5), victim.tasks.close_and_join()),
            tokio::time::timeout(Duration::from_secs(5), offender.tasks.close_and_join())
        );
        // Complete both actual joins before any teardown-result assertion can
        // unwind and dispose the other actor's still-live owner.
        let v_join = v_join.unwrap().snapshot();
        let o_join = o_join.unwrap().snapshot();
        if mode != "normal_listener_closed" {
            assert!(first.is_some());
            assert_eq!(after_second, first);
            let error = v_close.as_ref().unwrap_err();
            assert_eq!(Some(error.to_string()), first);
            match mode {
                "listener" => assert_eq!(error.raw_os_error(), Some(5)),
                "listener_closed" => assert_eq!(error.raw_os_error(), Some(13)),
                _ => assert_eq!(error.kind(), io::ErrorKind::InvalidInput),
            }
        } else {
            assert!(first.is_none() && after_second.is_none());
        }
        assert!(v_disconnected && o_disconnected);
        for (config, local, evidence, upgrades, close, join, injected, recorded) in [
            (
                victim.config,
                victim_peer,
                victim.evidence,
                victim.upgrades,
                v_close,
                v_join,
                first,
                v_recorded,
            ),
            (
                offender.config,
                offender_peer,
                offender.evidence,
                offender.upgrades,
                o_close,
                o_join,
                None,
                o_recorded,
            ),
        ] {
            assert_eq!(join["fixture_owned_tasks_joined"], true);
            assert_eq!(join["overflow"], false);
            assert!(join["errors"].as_array().unwrap().is_empty());
            let observed = {
                let capture = evidence.lock();
                assert_eq!((capture.live_muxers, capture.live_streams), (0, 0));
                assert!(!capture.overflow);
                assert!(
                    capture
                        .connections
                        .iter()
                        .all(|c| c.dropped && (c.closed || c.terminal))
                );
                capture.error.clone()
            };
            assert_eq!(observed, recorded);
            if let Some(first) = injected {
                assert_eq!(observed.as_ref(), Some(&first));
            } else {
                match (&close, &observed) {
                    (Err(error), Some(recorded)) => {
                        assert_eq!(&error.to_string(), recorded);
                    }
                    (Ok(()), Some(recorded)) => {
                        // Swarm close can finish while its observer retained a
                        // fatal native I/O error. This is not clean shutdown.
                        assert!(evidence.lock().events.iter().any(|event| {
                            event["kind"] == "native_io_error"
                                && event["source"] == "rust.libp2p.passive-upgraded-stream-io"
                                && event["message"] == *recorded
                        }));
                    }
                    (Ok(()), None) => {}
                    (Err(_), None) => panic!("native close failure was not recorded"),
                }
            }
            let reported = result(
                &config,
                Some(local),
                &evidence,
                true,
                true,
                join,
                upgrades.snapshot(),
            );
            assert_eq!(reported["error"], json!(observed));
            assert_eq!(
                reported["status"],
                if observed.is_some() { "error" } else { "ok" }
            );
        }
    }
}
