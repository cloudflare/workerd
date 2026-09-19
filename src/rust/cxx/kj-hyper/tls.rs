//! rustls configs from workerd's `TlsOptions`, and TLS over a kj stream.

use std::sync::Arc;

use kj::KjError;
use kj::KjExceptionType;
use kj_rs::KjOwn;
use rustls::DigitallySignedStruct;
use rustls::RootCertStore;
use rustls::SignatureScheme;
use rustls::client::WebPkiServerVerifier;
use rustls::client::danger::HandshakeSignatureValid;
use rustls::client::danger::ServerCertVerified;
use rustls::client::danger::ServerCertVerifier;
use rustls::pki_types::CertificateDer;
use rustls::pki_types::PrivateKeyDer;
use rustls::pki_types::ServerName;
use rustls::pki_types::UnixTime;
use rustls::pki_types::pem::PemObject;

use crate::ffi::AsyncIoStream;
use crate::ffi::TlsOptions;
use crate::io::LazyIo;
use crate::io::RustIo;
use crate::io::kj_to_tokio;

fn config_error(what: impl std::fmt::Display) -> KjError {
    KjError::new(
        KjExceptionType::Failed,
        format!("TLS configuration: {what}"),
    )
}

fn certificates(pems: &[String]) -> kj::Result<Vec<CertificateDer<'static>>> {
    pems.iter()
        .flat_map(|pem| CertificateDer::pem_slice_iter(pem.as_bytes()))
        .collect::<Result<_, _>>()
        .map_err(config_error)
}

fn keypair(
    options: &TlsOptions,
) -> kj::Result<Option<(Vec<CertificateDer<'static>>, PrivateKeyDer<'static>)>> {
    if options.certificate_chain.is_empty() {
        return Ok(None);
    }
    let chain = certificates(std::slice::from_ref(&options.certificate_chain))?;
    let key =
        PrivateKeyDer::from_pem_slice(options.private_key.as_bytes()).map_err(config_error)?;
    Ok(Some((chain, key)))
}

fn roots(options: &TlsOptions) -> kj::Result<RootCertStore> {
    let mut roots = RootCertStore::empty();
    if options.trust_system_roots {
        for cert in rustls_native_certs::load_native_certs().certs {
            let _ = roots.add(cert);
        }
    }
    for cert in certificates(&options.trusted_certificates)? {
        // A certificate webpki can't use as an anchor may still be trusted directly.
        let _ = roots.add(cert);
    }
    Ok(roots)
}

/// `minVersion`: rustls speaks TLS 1.2 and 1.3, so any floor up to 1.2 allows both.
fn versions(options: &TlsOptions) -> &'static [&'static rustls::SupportedProtocolVersion] {
    static TLS13_ONLY: [&rustls::SupportedProtocolVersion; 1] = [&rustls::version::TLS13];
    if options.min_tls13 {
        &TLS13_ONLY
    } else {
        rustls::DEFAULT_VERSIONS
    }
}

/// `cipherList` restricts TLS 1.2 to the named suites (OpenSSL names). A list naming none of
/// rustls' suites (keywords such as `HIGH:!aNULL`) leaves the defaults, all of which are ECDHE
/// with AEAD. As in OpenSSL, it has no effect on TLS 1.3.
fn provider(options: &TlsOptions) -> Arc<rustls::crypto::CryptoProvider> {
    use rustls::crypto::ring::cipher_suite as suite;
    const NAMES: [(&str, rustls::SupportedCipherSuite); 6] = [
        (
            "ECDHE-ECDSA-AES128-GCM-SHA256",
            suite::TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        ),
        (
            "ECDHE-RSA-AES128-GCM-SHA256",
            suite::TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        ),
        (
            "ECDHE-ECDSA-AES256-GCM-SHA384",
            suite::TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
        ),
        (
            "ECDHE-RSA-AES256-GCM-SHA384",
            suite::TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
        ),
        (
            "ECDHE-ECDSA-CHACHA20-POLY1305",
            suite::TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
        ),
        (
            "ECDHE-RSA-CHACHA20-POLY1305",
            suite::TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
        ),
    ];
    let mut provider = rustls::crypto::ring::default_provider();
    let named: Vec<_> = options
        .cipher_list
        .split(':')
        .filter_map(|token| NAMES.iter().find(|(name, _)| *name == token))
        .map(|(_, suite)| *suite)
        .collect();
    if !named.is_empty() {
        provider.cipher_suites.retain(|s| {
            s.version().version != rustls::ProtocolVersion::TLSv1_2 || named.contains(s)
        });
    }
    Arc::new(provider)
}

pub struct TlsClientConfig(Arc<rustls::ClientConfig>);

pub fn new_tls_client_config(options: &TlsOptions) -> kj::Result<Box<TlsClientConfig>> {
    let provider = provider(options);
    let trusted = certificates(&options.trusted_certificates)?;
    // As kj: `trustBrowserCas` trusts the system's CAs (the platform's own verifier, with the
    // configured certificates as extra roots); otherwise only the configured certificates.
    let verifier: Option<Arc<dyn ServerCertVerifier>> = if options.trust_system_roots {
        Some(Arc::new(
            rustls_platform_verifier::Verifier::new_with_extra_roots(
                trusted.clone(),
                provider.clone(),
            )
            .map_err(config_error)?,
        ))
    } else {
        let mut roots = RootCertStore::empty();
        for cert in &trusted {
            // A certificate webpki can't use as an anchor may still be trusted directly.
            let _ = roots.add(cert.clone());
        }
        if roots.is_empty() {
            None
        } else {
            Some(
                WebPkiServerVerifier::builder_with_provider(Arc::new(roots), provider.clone())
                    .build()
                    .map_err(config_error)?,
            )
        }
    };
    let verifier = PinnedVerifier {
        verifier,
        pinned: trusted,
        provider: provider.clone(),
    };
    let builder = rustls::ClientConfig::builder_with_provider(provider)
        .with_protocol_versions(versions(options))
        .map_err(config_error)?
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(verifier));
    let config = match keypair(options)? {
        Some((chain, key)) => builder
            .with_client_auth_cert(chain, key)
            .map_err(config_error)?,
        None => builder.with_no_client_auth(),
    };
    Ok(Box::new(TlsClientConfig(Arc::new(config))))
}

/// The configured trust, plus OpenSSL's direct trust: a server certificate byte-identical to a
/// configured trusted certificate is accepted (its name must still match) even when the verifier
/// refuses it, e.g. a self-signed CA certificate serving as its own end entity. With nothing
/// trusted at all, every other certificate is refused.
#[derive(Debug)]
struct PinnedVerifier {
    verifier: Option<Arc<dyn ServerCertVerifier>>,
    pinned: Vec<CertificateDer<'static>>,
    provider: Arc<rustls::crypto::CryptoProvider>,
}

impl ServerCertVerifier for PinnedVerifier {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        intermediates: &[CertificateDer<'_>],
        server_name: &ServerName<'_>,
        ocsp_response: &[u8],
        now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        let verified = self.verifier.as_ref().map_or(
            Err(rustls::Error::InvalidCertificate(
                rustls::CertificateError::UnknownIssuer,
            )),
            |v| v.verify_server_cert(end_entity, intermediates, server_name, ocsp_response, now),
        );
        verified.or_else(|error| {
            if !self
                .pinned
                .iter()
                .any(|c| c.as_ref() == end_entity.as_ref())
            {
                return Err(error);
            }
            let parsed = rustls::server::ParsedCertificate::try_from(end_entity)?;
            rustls::client::verify_server_name(&parsed, server_name)?;
            Ok(ServerCertVerified::assertion())
        })
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls12_signature(
            message,
            cert,
            dss,
            &self.provider.signature_verification_algorithms,
        )
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls13_signature(
            message,
            cert,
            dss,
            &self.provider.signature_verification_algorithms,
        )
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.provider
            .signature_verification_algorithms
            .supported_schemes()
    }
}

pub struct TlsServerConfig(Arc<rustls::ServerConfig>);

pub fn new_tls_server_config(options: &TlsOptions) -> kj::Result<Box<TlsServerConfig>> {
    let (chain, key) =
        keypair(options)?.ok_or_else(|| config_error("a TLS listener needs a keypair"))?;
    let builder = rustls::ServerConfig::builder_with_provider(provider(options))
        .with_protocol_versions(versions(options))
        .map_err(config_error)?;
    let builder = if options.require_client_certs {
        let verifier = rustls::server::WebPkiClientVerifier::builder(Arc::new(roots(options)?))
            .build()
            .map_err(config_error)?;
        builder.with_client_cert_verifier(verifier)
    } else {
        builder.with_no_client_auth()
    };
    let config = builder.with_single_cert(chain, key).map_err(config_error)?;
    Ok(Box::new(TlsServerConfig(Arc::new(config))))
}

/// `kj::SecureNetworkWrapper::wrapClient()`: the handshake completes before the stream is
/// handed back.
pub async fn wrap_tls_client(
    stream: KjOwn<AsyncIoStream>,
    config: &TlsClientConfig,
    hostname: String,
) -> kj::Result<Box<RustIo>> {
    let name = ServerName::try_from(hostname).map_err(config_error)?;
    let (io, hangup) = kj_to_tokio(stream, false);
    let tls = tokio_rustls::TlsConnector::from(config.0.clone())
        .connect(name, io)
        .await
        .map_err(|e| {
            KjError::new(
                KjExceptionType::Failed,
                format!("TLS handshake failed: {e}"),
            )
        })?;
    Ok(Box::new(RustIo::new(tls, hangup)))
}

/// `kj::SecureNetworkWrapper::wrapServer()`: the handshake runs on the connection's first I/O,
/// so a listener's accept loop never waits on a peer.
#[expect(
    clippy::unnecessary_box_returns,
    reason = "cxx passes opaque Rust types by Box"
)]
pub fn wrap_tls_server(stream: KjOwn<AsyncIoStream>, config: &TlsServerConfig) -> Box<RustIo> {
    let (io, hangup) = kj_to_tokio(stream, false);
    let accept = tokio_rustls::TlsAcceptor::from(config.0.clone()).accept(io);
    Box::new(RustIo::new(LazyIo::new(accept), hangup))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn options(min_tls13: bool, cipher_list: &str) -> TlsOptions {
        TlsOptions {
            trust_system_roots: false,
            trusted_certificates: Vec::new(),
            certificate_chain: String::new(),
            private_key: String::new(),
            require_client_certs: false,
            min_tls13,
            cipher_list: cipher_list.to_owned(),
        }
    }

    fn tls12_suites(provider: &rustls::crypto::CryptoProvider) -> Vec<rustls::CipherSuite> {
        provider
            .cipher_suites
            .iter()
            .filter(|s| s.version().version == rustls::ProtocolVersion::TLSv1_2)
            .map(rustls::SupportedCipherSuite::suite)
            .collect()
    }

    #[test]
    fn min_version_tls13_allows_only_tls13() {
        assert_eq!(versions(&options(true, "")), [&rustls::version::TLS13]);
        assert_eq!(versions(&options(false, "")), rustls::DEFAULT_VERSIONS);
    }

    #[test]
    fn cipher_list_names_the_tls12_suites() {
        let default = rustls::crypto::ring::default_provider();
        let named = provider(&options(false, "ECDHE-ECDSA-AES256-GCM-SHA384:HIGH:!aNULL"));
        assert_eq!(
            tls12_suites(&named),
            [rustls::CipherSuite::TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384]
        );
        // TLS 1.3 suites are not configured by the list.
        assert_eq!(
            named.cipher_suites.len() - 1,
            default.cipher_suites.len() - tls12_suites(&default).len()
        );
        // A list of keywords only leaves the defaults.
        assert_eq!(
            tls12_suites(&provider(&options(false, "HIGH:!aNULL"))),
            tls12_suites(&default)
        );
    }
}
