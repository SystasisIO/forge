// Upstream API composition follows tlsn's basic MPC example at the pinned
// revision (MIT OR Apache-2.0). Cryptographic operations remain upstream-owned.
use crate::protocol::{Config, Failure, Result, put_blob, put_u32, ranges};
use futures::{AsyncReadExt, AsyncWriteExt};
use rangeset::set::RangeSet;
use std::{future::IntoFuture, ops::Range};
use tlsn::{
    Session, SessionConfig,
    config::{
        prove::ProveConfig, prover::ProverConfig, tls::TlsClientConfig,
        tls_commit::mpc::MpcTlsConfig, verifier::VerifierConfig,
    },
    connection::ServerName,
    verifier::VerifierCommitStart,
    webpki::{CertificateDer, RootCertStore},
};
use tokio::io::DuplexStream;
use tokio_util::compat::TokioAsyncReadCompatExt;

fn roots(config: &Config) -> RootCertStore {
    RootCertStore {
        roots: config.roots.iter().cloned().map(CertificateDer).collect(),
    }
}
fn response(name: &str, verified: bool, sent: usize, recv: usize) -> Vec<u8> {
    let mut output = Vec::new();
    put_u32(&mut output, 0);
    output.push(verified as u8);
    put_blob(&mut output, name.as_bytes());
    put_u32(&mut output, sent);
    put_u32(&mut output, recv);
    output
}

fn prove_config(
    transcript: &tlsn::transcript::Transcript,
    sent: &[Range<usize>],
    received: &[Range<usize>],
) -> Result<ProveConfig> {
    let mut builder = ProveConfig::builder(transcript);
    builder.server_identity();
    // An explicit empty disclosure still authenticates identity and transcript
    // lengths. Upstream otherwise leaves reveal=None when both masks are empty;
    // the verifier correctly requires an authenticated partial transcript.
    builder
        .reveal_sent(&(0..0))
        .map_err(|_| Failure::Configuration)?;
    for range in sent {
        builder
            .reveal_sent(range)
            .map_err(|_| Failure::Configuration)?;
    }
    for range in received {
        builder
            .reveal_recv(range)
            .map_err(|_| Failure::Configuration)?;
    }
    builder.build().map_err(|_| Failure::Configuration)
}

pub async fn run(
    config: &Config,
    peer: DuplexStream,
    server: Option<DuplexStream>,
) -> Result<Vec<u8>> {
    let session = Session::new_with_config(
        peer.compat(),
        SessionConfig::builder()
            .max_num_streams(64)
            .build()
            .map_err(|_| Failure::Configuration)?,
    );
    let (driver, mut handle) = session.split();
    // An owned JoinSet aborts the driver if any operation fails or is cancelled.
    let mut tasks = tokio::task::JoinSet::new();
    tasks.spawn(driver);
    let output = if config.prover {
        let prover = handle.new_prover(
            ProverConfig::builder()
                .build()
                .map_err(|_| Failure::Configuration)?,
        )?;
        let mpc = MpcTlsConfig::builder()
            .max_sent_data(config.max_sent)
            .max_recv_data(config.max_recv)
            .max_recv_data_online(if config.deferred { 0 } else { config.max_recv })
            .max_sent_records(config.max_sent_records)
            .max_recv_records_online(config.max_recv_records)
            .defer_decryption_from_start(config.deferred)
            .build()
            .map_err(|_| Failure::Configuration)?;
        let prover = prover.commit(mpc).await?;
        let tls = TlsClientConfig::builder()
            .server_name(ServerName::Dns(
                config
                    .server_name
                    .as_str()
                    .try_into()
                    .map_err(|_| Failure::Configuration)?,
            ))
            .root_store(roots(config))
            .build()
            .map_err(|_| Failure::Configuration)?;
        let (connection, prover) =
            prover.connect(tls, server.ok_or(Failure::Configuration)?.compat())?;
        let mut prover_task = tokio::task::JoinSet::new();
        prover_task.spawn(prover.into_future());
        // Write and read concurrently: bounded TLS streams must not deadlock on
        // a server which begins its response before consuming the whole request.
        let (mut read, mut write) = connection.split();
        let sending = async {
            write.write_all(&config.request).await?;
            write.close().await?;
            Result::<()>::Ok(())
        };
        let receiving = async {
            let mut bytes = [0; 16384];
            let mut received = 0usize;
            loop {
                let count = read.read(&mut bytes).await?;
                if count == 0 {
                    break;
                }
                received = received.checked_add(count).ok_or(Failure::Limit)?;
                if received > config.max_recv {
                    return Err(Failure::Limit);
                }
            }
            Result::<()>::Ok(())
        };
        futures::try_join!(sending, receiving)?;
        drop(read);
        drop(write);
        let mut prover = prover_task
            .join_next()
            .await
            .ok_or(Failure::Internal)?
            .map_err(|_| Failure::Internal)??;
        let (sent, recv) = prover.transcript().len();
        let sent_ranges = ranges(&config.reveal_sent, sent)?;
        let recv_ranges = ranges(&config.reveal_recv, recv)?;
        prover
            .prove(&prove_config(
                prover.transcript(),
                &sent_ranges,
                &recv_ranges,
            )?)
            .await?;
        prover.close().await?;
        let mut output = response(&config.server_name, false, sent, recv);
        put_u32(&mut output, 0);
        put_u32(&mut output, 0);
        output
    } else {
        let verifier = handle.new_verifier(
            VerifierConfig::builder()
                .root_store(roots(config))
                .build()
                .map_err(|_| Failure::Configuration)?,
        )?;
        let verifier = match verifier.commit().await? {
            VerifierCommitStart::Mpc(verifier) => {
                let proposed = verifier.config();
                if proposed.max_sent_data() > config.max_sent
                    || proposed.max_recv_data() > config.max_recv
                    || proposed.max_recv_data_online() > config.max_recv
                    || !proposed
                        .max_sent_records()
                        .is_some_and(|n| n <= config.max_sent_records)
                    || !proposed
                        .max_recv_records_online()
                        .is_some_and(|n| n <= config.max_recv_records)
                    || proposed.defer_decryption_from_start() != config.deferred
                {
                    verifier.reject(Some("resource limits")).await?;
                    return Err(Failure::Limit);
                }
                verifier.accept().await?.run().await?
            }
            VerifierCommitStart::Proxy(verifier) => {
                verifier.reject(Some("MPC required")).await?;
                return Err(Failure::Rejected);
            }
        };
        let verifier = verifier.verify().await?;
        if !verifier.request().server_identity() || verifier.request().reveal().is_none() {
            verifier
                .reject(Some("identity and transcript required"))
                .await?;
            return Err(Failure::Verification);
        }
        let (authenticated, verifier) =
            verifier.accept().await.map_err(|_| Failure::Verification)?;
        let Some(ServerName::Dns(name)) = authenticated.server_name else {
            return Err(Failure::Verification);
        };
        if name.as_str() != config.server_name {
            return Err(Failure::Verification);
        }
        let transcript = authenticated.transcript.ok_or(Failure::Verification)?;
        let sent = transcript.len_sent();
        let recv = transcript.len_received();
        if sent > config.max_sent || recv > config.max_recv {
            return Err(Failure::Limit);
        }
        let sent_ranges = ranges(&config.reveal_sent, sent)?;
        let recv_ranges = ranges(&config.reveal_recv, recv)?;
        if transcript.sent_authed() != &RangeSet::new_from_slice(&sent_ranges)
            || transcript.received_authed() != &RangeSet::new_from_slice(&recv_ranges)
        {
            return Err(Failure::Verification);
        }
        let sent_bytes = transcript
            .sent_unsafe(sent)
            .map_err(|_| Failure::Verification)?;
        let recv_bytes = transcript
            .received_unsafe(recv)
            .map_err(|_| Failure::Verification)?;
        let mut output = response(name.as_str(), true, sent, recv);
        for (parts, bytes) in [(&sent_ranges, &sent_bytes), (&recv_ranges, &recv_bytes)] {
            put_u32(&mut output, parts.len());
            for range in parts {
                put_u32(&mut output, range.start);
                put_blob(&mut output, &bytes[range.clone()]);
            }
        }
        verifier.close().await?;
        output
    };
    handle.close();
    tasks
        .join_next()
        .await
        .ok_or(Failure::Internal)?
        .map_err(|_| Failure::Internal)??;
    Ok(output)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn empty_disclosure_explicitly_authenticates_without_revealing_plaintext() {
        let transcript = tlsn::transcript::Transcript::new(
            b"private request".to_vec(),
            b"private response".to_vec(),
        );
        let config = prove_config(&transcript, &[], &[]).unwrap();
        assert!(config.server_identity());
        let request = config.to_request();
        let (sent, received) = request.reveal().expect("explicit empty disclosure");
        assert!(sent.is_empty());
        assert!(received.is_empty());
    }
}
