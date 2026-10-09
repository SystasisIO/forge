//! Real pinned public transports; observations begin only at successful output.
use super::observer::{self, Evidence, NativeStack};
use super::{Config, behaviour, behaviour_with_mode, extensions::Mode, invalid, lower_hex};
use libp2p::{Swarm, Transport, gossipsub, identity, pnet::PreSharedKey};
use sha2::{Digest, Sha256};
use std::{
    error::Error,
    fs::File,
    io::{self, Read},
    str::FromStr,
    time::Duration,
};

const PNET_DOMAIN: &[u8] = b"forge.net.pnet.operational-fingerprint.v1\0";

fn private_key(text: &str, expected: &str) -> io::Result<PreSharedKey> {
    // Validate before the donor's byte-slicing parser, and never display key text.
    let lines: Vec<_> = text.lines().collect();
    if !text.is_ascii()
        || lines.len() != 3
        || lines[0] != "/key/swarm/psk/1.0.0/"
        || lines[1] != "/base16/"
        || lines[2].len() != 64
        || !lines[2].bytes().all(|b| b.is_ascii_hexdigit())
    {
        return Err(invalid("invalid private key fixture"));
    }
    let psk = PreSharedKey::from_str(text).map_err(|_| invalid("invalid private key fixture"))?;
    let material = crate::decode_hex(
        psk.to_string()
            .lines()
            .nth(2)
            .ok_or_else(|| invalid("invalid parsed key"))?,
    )
    .map_err(|_| invalid("invalid parsed key"))?;
    let mut digest = Sha256::new();
    digest.update(PNET_DOMAIN);
    digest.update(material);
    if !lower_hex(expected, 64) || format!("{:x}", digest.finalize()) != expected {
        return Err(invalid(
            "installed private key operational fingerprint mismatch",
        ));
    }
    Ok(psk)
}

pub(super) fn new_swarm(
    config: &Config,
    evidence: &Evidence,
    upgrades: &crate::upgrade_observer::Observer,
    tasks: &crate::task_owner::Owner,
) -> Result<Swarm<gossipsub::Behaviour>, Box<dyn Error>> {
    new_swarm_with_mode(config, None, evidence, upgrades, tasks)
}

pub(super) fn new_swarm_with_mode(
    config: &Config,
    mode: Option<Mode>,
    evidence: &Evidence,
    upgrades: &crate::upgrade_observer::Observer,
    tasks: &crate::task_owner::Owner,
) -> Result<Swarm<gossipsub::Behaviour>, Box<dyn Error>> {
    let key = identity::Keypair::generate_ed25519();
    let peer = key.public().to_peer_id();
    let router = match mode {
        Some(Mode::Idontwant) => behaviour_with_mode(&key, config, mode)?,
        _ => behaviour(&key, config)?,
    };
    // Facts are emitted only in the successful native output map, never from CLI.
    let (native, stack) = match config.transport.as_str() {
        "quic" => (
            crate::upgrade_observer::native_quic_transport(&key, upgrades.clone()),
            NativeStack::Quic,
        ),
        "tcp" => (
            crate::upgrade_observer::native_transport(
                &key,
                false,
                upgrades.clone().with_pubsub_yamux_errors(),
                None,
            ),
            NativeStack::NoiseYamux,
        ),
        "tcp-pnet-noise" => {
            let mut text = String::new();
            File::open(
                config
                    .key_file
                    .as_ref()
                    .ok_or_else(|| invalid("missing key file"))?,
            )?
            .take(1025)
            .read_to_string(&mut text)?;
            if text.len() > 1024 {
                return Err(invalid("private key fixture too large").into());
            }
            let psk = private_key(
                &text,
                config
                    .fingerprint
                    .as_deref()
                    .ok_or_else(|| invalid("missing fingerprint"))?,
            )?;
            evidence.lock().fingerprint = config.fingerprint.clone();
            (
                crate::upgrade_observer::native_private_transport(
                    &key,
                    false,
                    Some(psk),
                    upgrades.clone().with_pubsub_yamux_errors(),
                ),
                NativeStack::PnetNoiseYamux,
            )
        }
        _ => return Err(invalid("unsupported native transport").into()),
    };
    let transport = native.map_err(|e| io::Error::other(e.to_string()))?;
    let evidence = evidence.clone();
    let transport = transport
        .map(move |(peer, muxer), point| {
            observer::wrap(peer, muxer, point, stack, evidence.clone())
        })
        .boxed();
    Ok(Swarm::new(
        transport,
        router,
        peer,
        tasks
            .swarm_config()
            .with_idle_connection_timeout(Duration::from_secs(180)),
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn loaded_pnet_key_matches_operational_fingerprint_without_secret_errors() {
        let psk = PreSharedKey::new(std::array::from_fn(|i| i as u8));
        let text = psk.to_string();
        let expected = "7c291ef5c453de491f0a6a219ee8e3207767446da373dfbbbb91ce7d55418583";
        assert!(private_key(&text, expected).unwrap() == psk);
        assert!(private_key(&text.replace('\n', "\r\n"), expected).unwrap() == psk);
        let error = private_key(&text, &"0".repeat(64)).unwrap_err().to_string();
        assert!(!error.contains(text.lines().nth(2).unwrap()));
        assert!(private_key("non-ascii \u{0430}", expected).is_err());
        assert!(private_key(&text, &psk.fingerprint().to_string()).is_err());
    }
}
