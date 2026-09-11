//! Client-side and server-side TLS for the hyper HTTP layer, mapping workerd's
//! `config::TlsOptions` (the subsets the hyper-backed paths use) onto rustls.
//!
//! # Mapping from `config::TlsOptions` / `kj::TlsContext::Options`
//!
//! - `trustBrowserCas` (kj `useSystemTrustStore`): platform trust. When no explicit
//!   `trustedCertificates` are configured, verification is delegated to the operating
//!   system's verifier via `rustls-platform-verifier` — matching what native binaries do, and
//!   handling locally-installed corporate CAs whose key types rustls' `ring` provider cannot
//!   verify. When explicit `trustedCertificates` are ALSO configured, the platform store is
//!   loaded via `rustls-native-certs` and merged with the explicit anchors into one webpki
//!   store (cached per process; unparseable store entries are skipped, like OpenSSL).
//! - `trustedCertificates`: PEM certificates added as extra trust anchors. Additionally, any
//!   certificate listed here is accepted when the server presents *exactly* that certificate
//!   (byte-identical DER) for the expected server name, even if it is not a valid CA — this
//!   mirrors OpenSSL, where placing a self-signed end-entity certificate in the trust store
//!   makes it directly trusted, a pattern workerd configs use for local development. (One
//!   divergence: expiry is not re-checked on this exact-match path.)
//! - `keypair`: client-certificate identity, presented when the server requests one. PEM chain
//!   plus PKCS#8/PKCS#1/SEC1 private key; encrypted (passphrase-protected) keys are not
//!   supported (`kj::TlsKeypair` passphrase support is likewise absent in workerd's config).
//! - `minVersion`: rustls implements TLS 1.2 and 1.3 only. `goodDefault` and `tls1Dot2` enable
//!   1.2+1.3 (kj's good default is also 1.2); `tls1Dot3` enables 1.3 only; `ssl3`, `tls1Dot0`
//!   and `tls1Dot1` are **rejected with a config error** — rustls cannot speak them, and
//!   silently raising the floor to 1.2 would misrepresent the configuration.
//! - `cipherList` (OpenSSL cipher-list syntax): best-effort mapping — the string is split on
//!   ':' and each token matched against the OpenSSL and IANA names of the suites rustls
//!   supports. Like OpenSSL's `SSL_CTX_set_cipher_list`, the list governs TLS <= 1.2 only. If
//!   no token maps to a supported TLS 1.2 suite (and TLS 1.2 is enabled), the config is
//!   rejected with an error listing the supported names — though OpenSSL would accept some
//!   lists (e.g. CBC-suite-only) that this mapping rejects.
//!
//! # Error surface
//!
//! Certificate verification failures are rendered as kj does —
//! `"TLS peer's certificate is not trusted; reason = <reason>"` — with the common OpenSSL
//! reason strings ("unable to get local issuer certificate", "certificate has expired",
//! "hostname mismatch") reproduced for the corresponding rustls errors so callers matching on
//! kj's texts keep working. Other handshake failures use rustls' own message text (kj would
//! show OpenSSL's) with a "TLS handshake failed" prefix.

use std::sync::Arc;
use std::sync::OnceLock;

use cxx::KjError;
use cxx::KjExceptionType;
use kj::Result;
use rustls::RootCertStore;
use rustls::SupportedProtocolVersion;
use rustls::client::WebPkiServerVerifier;
use rustls::client::danger::HandshakeSignatureValid;
use rustls::client::danger::ServerCertVerified;
use rustls::client::danger::ServerCertVerifier;
use rustls::crypto::CryptoProvider;
use rustls::pki_types::CertificateDer;
use rustls::pki_types::ServerName;
use rustls::pki_types::UnixTime;

use crate::ffi;

/// A fully-built rustls client configuration, shareable across per-host clients (the Arc is
/// cloned into each `HyperClient`). Created once per configured service by
/// `new_hyper_tls_client_config()`.
pub struct HyperTlsClientConfig {
    config: Arc<rustls::ClientConfig>,
}

impl HyperTlsClientConfig {
    pub fn new(options: &ffi::TlsClientOptions) -> Result<Self> {
        let versions = protocol_versions(options.min_version)?;
        let tls12_enabled = versions.contains(&&rustls::version::TLS12);
        let provider = Arc::new(crypto_provider(&options.cipher_list, tls12_enabled)?);

        // --- Trust anchors (webpki path only: explicit trustedCertificates, optionally merged
        // with the native store; the no-explicit-trust default below delegates to the platform
        // verifier instead and never builds a root store).
        let mut roots = RootCertStore::empty();
        if options.trust_system_roots && !options.trusted_certificates.is_empty() {
            for cert in system_roots() {
                // Skip anchors the webpki parser rejects; OpenSSL is similarly lenient about
                // odd store contents.
                let _ = roots.add(cert.clone());
            }
        }
        let mut pinned: Vec<CertificateDer<'static>> = Vec::new();
        for pem in &options.trusted_certificates {
            let mut any = false;
            for cert in rustls_pemfile::certs(&mut pem.as_bytes()) {
                let cert = cert.map_err(|e| {
                    config_error(format!("invalid PEM in trustedCertificates: {e}"))
                })?;
                any = true;
                // Both a trust anchor for chain building and a directly-trusted (pinned)
                // certificate; see the module docs. Anchor parse failures (e.g. a v1
                // certificate webpki cannot model as a CA) still leave the exact-match path.
                let _ = roots.add(cert.clone());
                pinned.push(cert);
            }
            if !any {
                // Matches the spirit of kj::TlsCertificate's "invalid certificate" PEM error.
                return Err(config_error(
                    "invalid certificate in trustedCertificates: no PEM certificate found"
                        .to_owned(),
                ));
            }
        }

        let verifier: Arc<dyn ServerCertVerifier> = if options.trust_system_roots
            && options.trusted_certificates.is_empty()
        {
            // DEFAULT trust path (trustBrowserCas, no explicit trustedCertificates — e.g. the
            // implicit "internet" service): delegate verification to the operating system,
            // like kj/OpenSSL's system trust store and every native binary/browser. See the
            // module docs for why this is not a webpki RootCertStore built from the native
            // certs (short version: corporate TLS-interception CAs with key types ring cannot
            // verify, such as ECDSA P-521).
            Arc::new(
                rustls_platform_verifier::Verifier::new(provider.clone()).map_err(|e| {
                    config_error(format!(
                        "failed to initialize the platform certificate verifier: {e}"
                    ))
                })?,
            )
        } else {
            let webpki = if roots.is_empty() {
                // WebPkiServerVerifier cannot be built with an empty root store; verification
                // must still run (and fail with UnknownIssuer), like kj/OpenSSL with an empty
                // trust store.
                None
            } else {
                Some(
                    WebPkiServerVerifier::builder_with_provider(Arc::new(roots), provider.clone())
                        .build()
                        .map_err(|e| config_error(format!("invalid trusted certificates: {e}")))?,
                )
            };
            Arc::new(KjParityVerifier {
                webpki,
                pinned,
                provider: provider.clone(),
            })
        };

        let builder = rustls::ClientConfig::builder_with_provider(provider)
            .with_protocol_versions(versions)
            .map_err(|e| config_error(format!("unsupported TLS configuration: {e}")))?
            .dangerous()
            .with_custom_certificate_verifier(verifier);

        let config = if options.certificate_chain.is_empty() {
            builder.with_no_client_auth()
        } else {
            let chain: Vec<CertificateDer<'static>> =
                rustls_pemfile::certs(&mut options.certificate_chain.as_bytes())
                    .collect::<std::result::Result<_, _>>()
                    .map_err(|e| {
                        config_error(format!("invalid PEM in keypair.certificateChain: {e}"))
                    })?;
            if chain.is_empty() {
                return Err(config_error(
                    "keypair.certificateChain contains no PEM certificate".to_owned(),
                ));
            }
            let key = rustls_pemfile::private_key(&mut options.private_key.as_bytes())
                .map_err(|e| config_error(format!("invalid PEM in keypair.privateKey: {e}")))?
                .ok_or_else(|| {
                    config_error(
                        "keypair.privateKey contains no PEM private key (note: encrypted keys \
                         are not supported by the hyper (rustls) HTTP client)"
                            .to_owned(),
                    )
                })?;
            builder
                .with_client_auth_cert(chain, key)
                .map_err(|e| config_error(format!("invalid keypair: {e}")))?
        };

        Ok(Self {
            config: Arc::new(config),
        })
    }

    /// A connector sharing this configuration.
    #[must_use]
    pub(crate) fn connector(&self) -> tokio_rustls::TlsConnector {
        tokio_rustls::TlsConnector::from(self.config.clone())
    }

    /// The shared rustls client configuration, for building a synchronous
    /// `rustls::ClientConnection` (the raw-socket STARTTLS path in `client_tls.rs`).
    #[must_use]
    pub(crate) fn client_config(&self) -> Arc<rustls::ClientConfig> {
        self.config.clone()
    }
}

/// The parameters a `HyperClient` needs to dial TLS: the shared connector plus the name the
/// server's certificate must be valid for (kj's `expectedServerHostname`), which is also sent
/// as SNI.
pub struct TlsParams {
    pub connector: tokio_rustls::TlsConnector,
    pub server_name: ServerName<'static>,
}

impl TlsParams {
    pub fn new(config: &HyperTlsClientConfig, expected_server_hostname: &str) -> Result<Self> {
        let server_name =
            ServerName::try_from(expected_server_hostname.to_owned()).map_err(|_| {
                // kj surfaces an equivalent failure from X509_VERIFY_PARAM_set1_host().
                config_error(format!(
                    "invalid TLS server name \"{expected_server_hostname}\""
                ))
            })?;
        Ok(Self {
            connector: config.connector(),
            server_name,
        })
    }
}

fn config_error(message: String) -> KjError {
    KjError::new(KjExceptionType::Failed, message)
}

// =======================================================================================
// Server-side TLS (inbound https sockets under the rust I/O backend)

/// A fully-built rustls *server* configuration for inbound TLS sockets, created once per
/// configured socket by `new_hyper_tls_server_config()` and shared by every accepted
/// connection.
///
/// Mapping notes beyond the module docs (which cover the shared knobs):
/// - `keypair` is required (rustls cannot serve TLS without one; kj would accept the config
///   and then fail every handshake at runtime — the C++ side turns this into a config error).
/// - `requireClientCerts` (kj `verifyClients`): clients must present a certificate chaining to
///   the configured trust anchors (`trustedCertificates` + optionally the system store),
///   mirroring OpenSSL's `SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT`. The
///   directly-trusted (byte-identical) fallback is not applied to client certificates.
pub struct HyperTlsServerConfig {
    config: Arc<rustls::ServerConfig>,
}

impl HyperTlsServerConfig {
    pub fn new(options: &ffi::TlsServerOptions) -> Result<Self> {
        let versions = protocol_versions(options.min_version)?;
        let tls12_enabled = versions.contains(&&rustls::version::TLS12);
        let provider = Arc::new(crypto_provider(&options.cipher_list, tls12_enabled)?);

        let client_verifier: Option<Arc<dyn rustls::server::danger::ClientCertVerifier>> =
            if options.require_client_certs {
                let mut roots = RootCertStore::empty();
                if options.trust_system_roots {
                    for cert in system_roots() {
                        let _ = roots.add(cert.clone());
                    }
                }
                for pem in &options.trusted_certificates {
                    for cert in rustls_pemfile::certs(&mut pem.as_bytes()) {
                        let cert = cert.map_err(|e| {
                            config_error(format!("invalid PEM in trustedCertificates: {e}"))
                        })?;
                        let _ = roots.add(cert);
                    }
                }
                Some(
                    rustls::server::WebPkiClientVerifier::builder_with_provider(
                        Arc::new(roots),
                        provider.clone(),
                    )
                    .build()
                    .map_err(|e| {
                        config_error(format!(
                            "requireClientCerts needs at least one usable trusted certificate: \
                             {e}"
                        ))
                    })?,
                )
            } else {
                None
            };

        let chain: Vec<CertificateDer<'static>> =
            rustls_pemfile::certs(&mut options.certificate_chain.as_bytes())
                .collect::<std::result::Result<_, _>>()
                .map_err(|e| {
                    config_error(format!("invalid PEM in keypair.certificateChain: {e}"))
                })?;
        if chain.is_empty() {
            return Err(config_error(
                "keypair.certificateChain contains no PEM certificate".to_owned(),
            ));
        }
        let key = rustls_pemfile::private_key(&mut options.private_key.as_bytes())
            .map_err(|e| config_error(format!("invalid PEM in keypair.privateKey: {e}")))?
            .ok_or_else(|| {
                config_error(
                    "keypair.privateKey contains no PEM private key (note: encrypted keys are \
                     not supported by the hyper (rustls) HTTP server)"
                        .to_owned(),
                )
            })?;

        let builder = rustls::ServerConfig::builder_with_provider(provider)
            .with_protocol_versions(versions)
            .map_err(|e| config_error(format!("unsupported TLS configuration: {e}")))?;
        let config = match client_verifier {
            Some(verifier) => builder.with_client_cert_verifier(verifier),
            None => builder.with_no_client_auth(),
        }
        .with_single_cert(chain, key)
        .map_err(|e| config_error(format!("invalid keypair: {e}")))?;

        Ok(Self {
            config: Arc::new(config),
        })
    }

    /// An acceptor sharing this configuration.
    #[must_use]
    pub fn acceptor(&self) -> tokio_rustls::TlsAcceptor {
        tokio_rustls::TlsAcceptor::from(self.config.clone())
    }

    /// The shared rustls server config, for the synchronous server-side connection state
    /// machine (client_tls.rs `new_rustls_server_conn`).
    pub(crate) fn server_config(&self) -> Arc<rustls::ServerConfig> {
        self.config.clone()
    }
}

// =======================================================================================
// minVersion / cipherList policy

fn protocol_versions(
    min_version: ffi::TlsMinVersion,
) -> Result<&'static [&'static SupportedProtocolVersion]> {
    static BOTH: [&SupportedProtocolVersion; 2] =
        [&rustls::version::TLS13, &rustls::version::TLS12];
    static ONLY13: [&SupportedProtocolVersion; 1] = [&rustls::version::TLS13];
    match min_version {
        // kj's "good default" minimum is also TLS 1.2.
        ffi::TlsMinVersion::GOOD_DEFAULT | ffi::TlsMinVersion::TLS1_2 => Ok(&BOTH),
        ffi::TlsMinVersion::TLS1_3 => Ok(&ONLY13),
        ffi::TlsMinVersion::SSL3 | ffi::TlsMinVersion::TLS1_0 | ffi::TlsMinVersion::TLS1_1 => {
            let name = match min_version {
                ffi::TlsMinVersion::SSL3 => "ssl3",
                ffi::TlsMinVersion::TLS1_0 => "tls1Dot0",
                _ => "tls1Dot1",
            };
            Err(config_error(format!(
                "TlsOptions.minVersion = {name} is not supported by the hyper (rustls) HTTP \
                 client: rustls implements only TLS 1.2 and TLS 1.3. Use minVersion = \
                 goodDefault/tls1Dot2/tls1Dot3."
            )))
        }
        _ => Err(config_error(
            "unknown TlsOptions.minVersion value".to_owned(),
        )),
    }
}

/// The cipher suites rustls' ring provider supports, with their OpenSSL and IANA names.
/// (rustls supports no CBC or non-ECDHE suites, by design.)
const SUITE_NAMES: &[(&str, &str, rustls::SupportedCipherSuite)] = {
    use rustls::crypto::ring::cipher_suite as cs;
    &[
        // TLS 1.2
        (
            "ECDHE-ECDSA-AES128-GCM-SHA256",
            "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256",
            cs::TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        ),
        (
            "ECDHE-ECDSA-AES256-GCM-SHA384",
            "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384",
            cs::TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
        ),
        (
            "ECDHE-ECDSA-CHACHA20-POLY1305",
            "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256",
            cs::TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
        ),
        (
            "ECDHE-RSA-AES128-GCM-SHA256",
            "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256",
            cs::TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        ),
        (
            "ECDHE-RSA-AES256-GCM-SHA384",
            "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384",
            cs::TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
        ),
        (
            "ECDHE-RSA-CHACHA20-POLY1305",
            "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256",
            cs::TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
        ),
        // TLS 1.3 (OpenSSL's cipher-list syntax doesn't govern 1.3, but accept the names)
        (
            "TLS_AES_128_GCM_SHA256",
            "TLS13_AES_128_GCM_SHA256",
            cs::TLS13_AES_128_GCM_SHA256,
        ),
        (
            "TLS_AES_256_GCM_SHA384",
            "TLS13_AES_256_GCM_SHA384",
            cs::TLS13_AES_256_GCM_SHA384,
        ),
        (
            "TLS_CHACHA20_POLY1305_SHA256",
            "TLS13_CHACHA20_POLY1305_SHA256",
            cs::TLS13_CHACHA20_POLY1305_SHA256,
        ),
    ]
};

/// Build the crypto provider, applying the best-effort cipherList mapping (see module docs).
fn crypto_provider(cipher_list: &str, tls12_enabled: bool) -> Result<CryptoProvider> {
    let mut provider = rustls::crypto::ring::default_provider();
    if cipher_list.is_empty() {
        return Ok(provider);
    }

    // Like SSL_CTX_set_cipher_list, this only governs TLS <= 1.2 suites.
    let mut tls12_suites: Vec<rustls::SupportedCipherSuite> = Vec::new();
    for token in cipher_list.split([':', ',', ' ']) {
        if token.is_empty() {
            continue;
        }
        if let Some((_, _, suite)) = SUITE_NAMES.iter().find(|(openssl, iana, _)| {
            token.eq_ignore_ascii_case(openssl) || token.eq_ignore_ascii_case(iana)
        }) && suite.version().version == rustls::ProtocolVersion::TLSv1_2
            && !tls12_suites.contains(suite)
        {
            tls12_suites.push(*suite);
        }
        // TLS 1.3 names are accepted but have no effect, as in OpenSSL. Unknown tokens (CBC
        // suites, keywords like HIGH, !EXPORT, ...) are skipped; what matters is whether
        // anything usable remains, checked below.
    }

    if tls12_enabled {
        if tls12_suites.is_empty() {
            let supported = SUITE_NAMES
                .iter()
                .filter(|(_, _, s)| s.version().version == rustls::ProtocolVersion::TLSv1_2)
                .map(|(openssl, _, _)| *openssl)
                .collect::<Vec<_>>()
                .join(", ");
            // OpenSSL fails SSL_CTX_set_cipher_list ("no cipher match") when nothing usable
            // remains; kj turns that into a config-time exception too.
            return Err(config_error(format!(
                "TlsOptions.cipherList matches no TLS 1.2 cipher supported by the hyper \
                 (rustls) HTTP client. Supported ciphers: {supported}. TLS 1.3 suites are \
                 always enabled (OpenSSL cipher lists do not govern TLS 1.3 either). Omit \
                 cipherList to use the defaults."
            )));
        }
        let mut suites = tls12_suites;
        suites.extend(
            provider
                .cipher_suites
                .iter()
                .filter(|s| s.version().version == rustls::ProtocolVersion::TLSv1_3)
                .copied(),
        );
        provider.cipher_suites = suites;
    }
    // TLS 1.3-only configs keep the default (1.3) suites: the cipher list has nothing to
    // govern, matching OpenSSL.
    Ok(provider)
}

// =======================================================================================
// System trust store

fn system_roots() -> &'static Vec<CertificateDer<'static>> {
    static ROOTS: OnceLock<Vec<CertificateDer<'static>>> = OnceLock::new();
    ROOTS.get_or_init(|| {
        // Best-effort, like OpenSSL's default verify paths (and kj's Windows store import):
        // per-certificate load errors are skipped. An empty result simply means verification
        // against system CAs will fail, which is observable per-connection.
        rustls_native_certs::load_native_certs().certs
    })
}

// =======================================================================================
// Certificate verification with kj-parity errors and OpenSSL-style direct trust

/// Wraps `WebPkiServerVerifier` to add the "directly-trusted certificate" behavior of
/// OpenSSL trust stores: a server presenting a certificate byte-identical to a configured
/// trusted certificate is accepted (for the expected name) even if that certificate is not a
/// valid CA — the self-signed-cert-in-`trustedCertificates` development pattern.
#[derive(Debug)]
struct KjParityVerifier {
    /// None when the trust store is empty (webpki refuses to build); verification then only
    /// has the pinned path, and otherwise fails with `UnknownIssuer` like OpenSSL with an
    /// empty store.
    webpki: Option<Arc<WebPkiServerVerifier>>,
    pinned: Vec<CertificateDer<'static>>,
    provider: Arc<CryptoProvider>,
}

impl ServerCertVerifier for KjParityVerifier {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        intermediates: &[CertificateDer<'_>],
        server_name: &ServerName<'_>,
        ocsp_response: &[u8],
        now: UnixTime,
    ) -> std::result::Result<ServerCertVerified, rustls::Error> {
        let webpki_result = match &self.webpki {
            Some(webpki) => webpki.verify_server_cert(
                end_entity,
                intermediates,
                server_name,
                ocsp_response,
                now,
            ),
            None => Err(rustls::Error::InvalidCertificate(
                rustls::CertificateError::UnknownIssuer,
            )),
        };
        match webpki_result {
            Ok(verified) => Ok(verified),
            Err(e) => {
                // Exact-match path: only for certificates the config listed verbatim, and the
                // name must still match.
                if self
                    .pinned
                    .iter()
                    .any(|c| c.as_ref() == end_entity.as_ref())
                {
                    let parsed = rustls::server::ParsedCertificate::try_from(end_entity)?;
                    rustls::client::verify_server_name(&parsed, server_name)?;
                    return Ok(ServerCertVerified::assertion());
                }
                Err(e)
            }
        }
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &rustls::DigitallySignedStruct,
    ) -> std::result::Result<HandshakeSignatureValid, rustls::Error> {
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
        dss: &rustls::DigitallySignedStruct,
    ) -> std::result::Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls13_signature(
            message,
            cert,
            dss,
            &self.provider.signature_verification_algorithms,
        )
    }

    fn supported_verify_schemes(&self) -> Vec<rustls::SignatureScheme> {
        self.provider
            .signature_verification_algorithms
            .supported_schemes()
    }
}

// =======================================================================================
// Error rendering

/// Render a TLS connect/handshake failure as a `kj::Exception`, matching kj's error surface
/// for the certificate-verification cases (see module docs).
pub fn kj_error_for_tls(e: &std::io::Error) -> KjError {
    // tokio-rustls surfaces handshake failures as io::Error wrapping rustls::Error.
    if let Some(inner) = e.get_ref()
        && let Some(tls_error) = inner.downcast_ref::<rustls::Error>()
    {
        return kj_error_for_rustls_error(tls_error);
    }
    // Plain I/O failure during the handshake (peer reset etc.).
    crate::client::kj_error_for_io("TLS handshake", e)
}

/// Render a `rustls::Error` as a `kj::Exception`, matching kj's error surface for the
/// certificate-verification cases (see the module docs). Shared by the tokio-rustls path
/// (`kj_error_for_tls`) and the synchronous raw-socket STARTTLS path (`client_tls.rs`).
pub fn kj_error_for_rustls_error(tls_error: &rustls::Error) -> KjError {
    match tls_error {
        rustls::Error::InvalidCertificate(cert_error) => {
            // kj: KJ_FAIL_REQUIRE("TLS peer's certificate is not trusted", reason) with
            // OpenSSL's X509_verify_cert_error_string() as the reason.
            let reason = match cert_error {
                rustls::CertificateError::UnknownIssuer => {
                    "unable to get local issuer certificate".to_owned()
                }
                rustls::CertificateError::Expired
                | rustls::CertificateError::ExpiredContext { .. } => {
                    "certificate has expired".to_owned()
                }
                rustls::CertificateError::NotValidYet
                | rustls::CertificateError::NotValidYetContext { .. } => {
                    "certificate is not yet valid".to_owned()
                }
                rustls::CertificateError::NotValidForName
                | rustls::CertificateError::NotValidForNameContext { .. } => {
                    "hostname mismatch".to_owned()
                }
                rustls::CertificateError::Revoked => "certificate revoked".to_owned(),
                other => format!("{other:?}"),
            };
            KjError::new(
                KjExceptionType::Failed,
                format!("TLS peer's certificate is not trusted; reason = {reason}"),
            )
        }
        _ => KjError::new(
            KjExceptionType::Failed,
            format!("TLS handshake failed: {tls_error}"),
        ),
    }
}
