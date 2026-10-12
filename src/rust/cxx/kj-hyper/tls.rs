//! rustls configurations from workerd's `TlsOptions`, and the handshakes over tokio streams.
//!
//! What OpenSSL-era options mean here: `trust_browser_cas` verifies servers with the platform's
//! own store; otherwise only `trusted_certificates` are trusted, and a trusted certificate is
//! also trusted directly (as kj's `TlsContext` adds each to its certificate store) with its
//! validity and key usage still checked. `require_client_certs` needs the accepting authorities
//! in `trusted_certificates`: the platform store verifies servers only. Cipher suites are
//! rustls' defaults; there is no cipher list.
//!
//! The cryptography is ring's, except for the ECDSA signatures ring cannot verify and kj's
//! `BoringSSL` can (a P-521 key, or SHA-512 with a P-256 or P-384 key): `BoringSSL` verifies
//! those here too, in certificate chains and in handshakes alike.

use std::fmt::Display;
use std::io;
use std::slice;
use std::sync::Arc;
use std::sync::LazyLock;

use cxx::KjError;
use cxx::KjExceptionType;
use rustls::CertificateError;
use rustls::DigitallySignedStruct;
use rustls::RootCertStore;
use rustls::SignatureScheme;
use rustls::client::WebPkiServerVerifier;
use rustls::client::danger::HandshakeSignatureValid;
use rustls::client::danger::ServerCertVerified;
use rustls::client::danger::ServerCertVerifier;
use rustls::client::verify_server_name;
use rustls::crypto::CryptoProvider;
use rustls::crypto::WebPkiSupportedAlgorithms;
use rustls::crypto::ring::default_provider;
use rustls::crypto::verify_tls12_signature;
use rustls::crypto::verify_tls13_signature;
use rustls::pki_types::AlgorithmIdentifier;
use rustls::pki_types::CertificateDer;
use rustls::pki_types::InvalidSignature;
use rustls::pki_types::PrivateKeyDer;
use rustls::pki_types::ServerName;
use rustls::pki_types::SignatureVerificationAlgorithm;
use rustls::pki_types::UnixTime;
use rustls::pki_types::alg_id;
use rustls::pki_types::pem::PemObject;
use rustls::server::WebPkiClientVerifier;
use rustls::version::TLS13;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;
use tokio_rustls::client;
use tokio_rustls::server;

use crate::Result;
use crate::ffi::Ecdsa;
use crate::ffi::ecdsa_verify;
use crate::io::io_kj_error;

/// A certificate chain and its private key, both PEM.
#[derive(Debug, Clone, Default)]
pub struct Keypair {
    pub certificate_chain: String,
    pub private_key: String,
}

/// The lowest protocol version accepted. rustls speaks TLS 1.2 and 1.3, so every floor workerd's
/// config can name below 1.2 is [`MinVersion::Tls12`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum MinVersion {
    #[default]
    Tls12,
    Tls13,
}

/// workerd's `config::TlsOptions`.
#[derive(Debug, Clone, Default)]
pub struct TlsOptions {
    pub keypair: Option<Keypair>,
    /// PEM certificates, each string holding one or more.
    pub trusted_certificates: Vec<String>,
    pub require_client_certs: bool,
    pub trust_browser_cas: bool,
    pub min_version: MinVersion,
}

fn config_error(what: impl Display) -> KjError {
    KjError::new(
        KjExceptionType::Failed,
        format!("TLS configuration: {what}"),
    )
}

/// The certificates in each PEM string; a string holding none is an error.
fn certificates(pems: &[String]) -> Result<Vec<CertificateDer<'static>>> {
    let mut all = Vec::new();
    for pem in pems {
        let certs = CertificateDer::pem_slice_iter(pem.as_bytes())
            .collect::<Result<Vec<_>, _>>()
            .map_err(config_error)?;
        if certs.is_empty() {
            return Err(config_error("no PEM certificate found"));
        }
        all.extend(certs);
    }
    Ok(all)
}

fn keypair(
    options: &TlsOptions,
) -> Result<Option<(Vec<CertificateDer<'static>>, PrivateKeyDer<'static>)>> {
    let Some(keypair) = &options.keypair else {
        return Ok(None);
    };
    let chain = certificates(slice::from_ref(&keypair.certificate_chain))?;
    let key =
        PrivateKeyDer::from_pem_slice(keypair.private_key.as_bytes()).map_err(config_error)?;
    Ok(Some((chain, key)))
}

/// `trusted_certificates` as webpki's anchors.
fn roots(trusted: &[CertificateDer<'static>]) -> RootCertStore {
    let mut roots = RootCertStore::empty();
    for cert in trusted {
        // A certificate webpki can't use as an anchor may still be trusted directly.
        let _ = roots.add(cert.clone());
    }
    roots
}

fn versions(options: &TlsOptions) -> &'static [&'static rustls::SupportedProtocolVersion] {
    static TLS13_ONLY: [&rustls::SupportedProtocolVersion; 1] = [&TLS13];
    match options.min_version {
        MinVersion::Tls13 => &TLS13_ONLY,
        MinVersion::Tls12 => rustls::DEFAULT_VERSIONS,
    }
}

/// An ECDSA signature algorithm ring cannot verify, verified by `BoringSSL`.
#[derive(Debug)]
struct BoringEcdsa {
    algorithm: Ecdsa,
    public_key: AlgorithmIdentifier,
    signature: AlgorithmIdentifier,
}

impl SignatureVerificationAlgorithm for BoringEcdsa {
    fn verify_signature(
        &self,
        public_key: &[u8],
        message: &[u8],
        signature: &[u8],
    ) -> Result<(), InvalidSignature> {
        if ecdsa_verify(self.algorithm, public_key, message, signature) {
            Ok(())
        } else {
            Err(InvalidSignature)
        }
    }

    fn public_key_alg_id(&self) -> AlgorithmIdentifier {
        self.public_key
    }

    fn signature_alg_id(&self) -> AlgorithmIdentifier {
        self.signature
    }
}

static P256_SHA512: BoringEcdsa = BoringEcdsa {
    algorithm: Ecdsa::P256_SHA512,
    public_key: alg_id::ECDSA_P256,
    signature: alg_id::ECDSA_SHA512,
};
static P384_SHA512: BoringEcdsa = BoringEcdsa {
    algorithm: Ecdsa::P384_SHA512,
    public_key: alg_id::ECDSA_P384,
    signature: alg_id::ECDSA_SHA512,
};
static P521_SHA256: BoringEcdsa = BoringEcdsa {
    algorithm: Ecdsa::P521_SHA256,
    public_key: alg_id::ECDSA_P521,
    signature: alg_id::ECDSA_SHA256,
};
static P521_SHA384: BoringEcdsa = BoringEcdsa {
    algorithm: Ecdsa::P521_SHA384,
    public_key: alg_id::ECDSA_P521,
    signature: alg_id::ECDSA_SHA384,
};
static P521_SHA512: BoringEcdsa = BoringEcdsa {
    algorithm: Ecdsa::P521_SHA512,
    public_key: alg_id::ECDSA_P521,
    signature: alg_id::ECDSA_SHA512,
};

type Verifiers = Vec<&'static dyn SignatureVerificationAlgorithm>;

/// ring's handshake signature schemes, each with the `BoringSSL` verifiers that also serve it
/// (TLS 1.2's ECDSA schemes leave the curve to the key; TLS 1.3 takes a scheme's first), and
/// `ecdsa_secp521r1_sha512`.
static SCHEMES: LazyLock<Vec<(SignatureScheme, Verifiers)>> = LazyLock::new(|| {
    let mut schemes: Vec<(SignatureScheme, Verifiers)> = default_provider()
        .signature_verification_algorithms
        .mapping
        .iter()
        .map(|&(scheme, verifiers)| {
            let mut verifiers = verifiers.to_vec();
            match scheme {
                SignatureScheme::ECDSA_NISTP256_SHA256 => verifiers.push(&P521_SHA256),
                SignatureScheme::ECDSA_NISTP384_SHA384 => verifiers.push(&P521_SHA384),
                _ => {}
            }
            (scheme, verifiers)
        })
        .collect();
    schemes.push((
        SignatureScheme::ECDSA_NISTP521_SHA512,
        vec![&P521_SHA512, &P256_SHA512, &P384_SHA512],
    ));
    schemes
});

/// ring's verifiers and `BoringSSL`'s.
static ALL: LazyLock<Verifiers> = LazyLock::new(|| {
    let mut all = default_provider()
        .signature_verification_algorithms
        .all
        .to_vec();
    all.extend::<[&'static dyn SignatureVerificationAlgorithm; 5]>([
        &P256_SHA512,
        &P384_SHA512,
        &P521_SHA256,
        &P521_SHA384,
        &P521_SHA512,
    ]);
    all
});

/// [`SCHEMES`] as rustls takes them.
static MAPPING: LazyLock<Vec<(SignatureScheme, &[&dyn SignatureVerificationAlgorithm])>> =
    LazyLock::new(|| {
        SCHEMES
            .iter()
            .map(|(scheme, verifiers)| (*scheme, verifiers.as_slice()))
            .collect()
    });

/// ring's provider, with `BoringSSL`'s ECDSA verifiers added (module docs).
fn provider() -> Arc<CryptoProvider> {
    Arc::new(CryptoProvider {
        signature_verification_algorithms: WebPkiSupportedAlgorithms {
            all: &ALL,
            mapping: &MAPPING,
        },
        ..default_provider()
    })
}

/// A client configuration: `keypair` is the client certificate, if any.
///
/// # Errors
///
/// Options rustls cannot represent (module docs), or malformed PEM.
pub fn client_config(options: &TlsOptions) -> Result<Arc<rustls::ClientConfig>> {
    let provider = provider();
    let trusted = certificates(&options.trusted_certificates)?;
    // `trust_browser_cas` verifies with the platform, as OpenSSL uses the system's store: the
    // platform's own verifier on macOS and Windows, webpki over the system's roots elsewhere.
    let verifier: Option<Arc<dyn ServerCertVerifier>> = if options.trust_browser_cas {
        Some(Arc::new(
            rustls_platform_verifier::Verifier::new_with_extra_roots(
                trusted.clone(),
                provider.clone(),
            )
            .map_err(config_error)?,
        ))
    } else {
        let roots = roots(&trusted);
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
    Ok(Arc::new(config))
}

/// A server configuration; `keypair` is required.
///
/// # Errors
///
/// As for [`client_config`], or no keypair.
pub fn server_config(options: &TlsOptions) -> Result<Arc<rustls::ServerConfig>> {
    let (chain, key) =
        keypair(options)?.ok_or_else(|| config_error("a TLS listener needs a keypair"))?;
    let provider = provider();
    let builder = rustls::ServerConfig::builder_with_provider(provider.clone())
        .with_protocol_versions(versions(options))
        .map_err(config_error)?;
    let builder = if options.require_client_certs {
        if options.trust_browser_cas {
            return Err(config_error(
                "requireClientCerts with trustBrowserCas is not supported: list the certificate \
                 authorities to accept client certificates from in trustedCertificates",
            ));
        }
        let trusted = certificates(&options.trusted_certificates)?;
        let verifier =
            WebPkiClientVerifier::builder_with_provider(Arc::new(roots(&trusted)), provider)
                .build()
                .map_err(config_error)?;
        builder.with_client_cert_verifier(verifier)
    } else {
        builder.with_no_client_auth()
    };
    let config = builder.with_single_cert(chain, key).map_err(config_error)?;
    Ok(Arc::new(config))
}

/// The configured trust, plus OpenSSL's direct trust: a server certificate byte-identical to a
/// configured trusted certificate is accepted when webpki cannot build a chain for it (it is not a
/// usable anchor, or it is a CA certificate serving as its own end entity). Such a certificate is
/// still checked as OpenSSL checks one for a TLS server ([`directly_trusted`]), and must name the
/// host.
#[derive(Debug)]
struct PinnedVerifier {
    verifier: Option<Arc<dyn ServerCertVerifier>>,
    pinned: Vec<CertificateDer<'static>>,
    provider: Arc<CryptoProvider>,
}

/// A chain-building failure direct trust may override.
fn is_untrusted_chain(error: &rustls::Error) -> bool {
    match error {
        rustls::Error::InvalidCertificate(CertificateError::UnknownIssuer) => true,
        rustls::Error::InvalidCertificate(CertificateError::Other(other)) => matches!(
            other.0.downcast_ref::<webpki::Error>(),
            Some(webpki::Error::CaUsedAsEndEntity)
        ),
        _ => false,
    }
}

/// A directly trusted certificate's own policy, as OpenSSL's `X509_PURPOSE_SSL_SERVER` check
/// applies it: within its validity period, and permitted to authenticate a TLS server by its
/// extended key usage and key usage, where it has them.
fn directly_trusted(end_entity: &CertificateDer<'_>, now: UnixTime) -> Result<(), rustls::Error> {
    use x509_cert::der::Decode;
    use x509_cert::der::oid::db::rfc5280;
    use x509_cert::ext::pkix::ExtendedKeyUsage;
    use x509_cert::ext::pkix::KeyUsage;

    let cert = x509_cert::Certificate::from_der(end_entity)
        .map_err(|_| rustls::Error::InvalidCertificate(CertificateError::BadEncoding))?;
    let tbs = &cert.tbs_certificate;
    let now = now.as_secs();
    if now < tbs.validity.not_before.to_unix_duration().as_secs() {
        return Err(CertificateError::NotValidYet.into());
    }
    if now > tbs.validity.not_after.to_unix_duration().as_secs() {
        return Err(CertificateError::Expired.into());
    }
    for extension in tbs.extensions.iter().flatten() {
        let value = extension.extn_value.as_bytes();
        let permitted = if extension.extn_id == rfc5280::ID_CE_EXT_KEY_USAGE {
            let usages = ExtendedKeyUsage::from_der(value)
                .map_err(|_| rustls::Error::InvalidCertificate(CertificateError::BadEncoding))?;
            usages.0.iter().any(|oid| {
                *oid == rfc5280::ID_KP_SERVER_AUTH || *oid == rfc5280::ANY_EXTENDED_KEY_USAGE
            })
        } else if extension.extn_id == rfc5280::ID_CE_KEY_USAGE {
            let usage = KeyUsage::from_der(value)
                .map_err(|_| rustls::Error::InvalidCertificate(CertificateError::BadEncoding))?;
            usage.digital_signature() || usage.key_encipherment() || usage.key_agreement()
        } else {
            true
        };
        if !permitted {
            return Err(CertificateError::InvalidPurpose.into());
        }
    }
    Ok(())
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
        let Some(verifier) = &self.verifier else {
            return Err(CertificateError::UnknownIssuer.into());
        };
        verifier
            .verify_server_cert(end_entity, intermediates, server_name, ocsp_response, now)
            .or_else(|error| {
                let pinned = self
                    .pinned
                    .iter()
                    .any(|c| c.as_ref() == end_entity.as_ref());
                if !pinned || !is_untrusted_chain(&error) {
                    return Err(error);
                }
                directly_trusted(end_entity, now)?;
                let parsed = rustls::server::ParsedCertificate::try_from(end_entity)?;
                verify_server_name(&parsed, server_name)?;
                Ok(ServerCertVerified::assertion())
            })
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls12_signature(
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
        verify_tls13_signature(
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

// =======================================================================================
// Handshakes

/// A failed handshake, DISCONNECTED when the peer went away during it.
fn handshake_error(e: &io::Error) -> KjError {
    let error = io_kj_error(e);
    KjError::new(
        error.exception_type(),
        format!("TLS handshake failed: {}", error.description()),
    )
}

/// The server side of a TLS connection over `io`, handshake (client authentication included)
/// complete.
///
/// A peer that later closes without `close_notify` ends reads with an
/// `UnexpectedEof`, which [`crate::io::io_kj_error`] reports as DISCONNECTED.
///
/// # Errors
///
/// A failed or refused handshake.
pub async fn accept<IO>(io: IO, config: Arc<rustls::ServerConfig>) -> Result<server::TlsStream<IO>>
where
    IO: AsyncRead + AsyncWrite + Unpin,
{
    tokio_rustls::TlsAcceptor::from(config)
        .accept(io)
        .await
        .map_err(|e| handshake_error(&e))
}

/// The client side of a TLS connection over `io` to `server_name`, handshake complete.
///
/// # Errors
///
/// An invalid server name, or a failed handshake.
pub async fn connect<IO>(
    io: IO,
    config: Arc<rustls::ClientConfig>,
    server_name: &str,
) -> Result<client::TlsStream<IO>>
where
    IO: AsyncRead + AsyncWrite + Unpin,
{
    let name = ServerName::try_from(server_name.to_owned()).map_err(config_error)?;
    tokio_rustls::TlsConnector::from(config)
        .connect(name, io)
        .await
        .map_err(|e| handshake_error(&e))
}

/// Whether the client of an accepted connection authenticated with a certificate (a
/// `kj::TlsPeerIdentity` with a certificate, for the request metadata).
pub fn has_client_certificate<IO>(stream: &server::TlsStream<IO>) -> bool {
    stream.get_ref().1.peer_certificates().is_some()
}

#[cfg(test)]
#[path = "tls-test.rs"]
mod tests;
