//! The LTX error type, after Litestream's sentinel errors (litestream.go:32-37).

use thiserror::Error;

pub type Result<T> = std::result::Result<T, Error>;

#[derive(Debug, Error)]
pub enum Error {
    /// The replica's rolling checksum does not match what was expected.
    #[error("invalid replica, checksum mismatch")]
    ChecksumMismatch,
    /// An LTX file's contents are corrupt (bad magic, truncated, bad CRC).
    #[error("ltx file corrupted")]
    LTXCorrupted,
    /// No backup chain reaches the requested transaction.
    #[error("transaction not available")]
    TxNotAvailable,
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
    #[error("{0}")]
    Other(#[from] Box<dyn std::error::Error + Send + Sync>),
}
