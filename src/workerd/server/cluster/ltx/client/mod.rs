//! client — the `ReplicaClient` trait.
//!
//! Ported from litestream@v0.5.11 `replica_client.go`. The trait is the storage
//! abstraction that every backend implements.
//!
//! # I/O
//! Go's `ReplicaClient` uses `io.Reader`/`io.ReadCloser`. LTX files go
//! between the store and files on disk, so no file is held in memory.

use std::path::Path;

use async_trait::async_trait;

use crate::ltx::TXID;
use crate::ltx::error::Result;
use crate::ltx::ltx::FileInfo;

pub mod bucket;

/// Client for reading and writing LTX files on a replica backend.
///
/// Ported from the `ReplicaClient` interface (replica_client.go:19-51). Methods
/// take a compaction `level`: 0 for uploads, `SNAPSHOT_LEVEL` for snapshots.
#[async_trait(?Send)]
pub trait ReplicaClient {
    /// Returns all LTX files for `level`, sorted ascending by `min_txid`, that
    /// start at or after `seek`.
    async fn ltx_files(&self, level: i32, seek: TXID) -> Result<Vec<FileInfo>>;

    /// Writes an LTX file to the file `to`. Returns an
    /// `io::ErrorKind::NotFound` error (wrapped) if it does not exist.
    async fn open_ltx_file(
        &self,
        level: i32,
        min_txid: TXID,
        max_txid: TXID,
        to: &Path,
    ) -> Result<()>;

    /// Writes the LTX file `from` to the replica and returns its metadata.
    async fn write_ltx_file(
        &self,
        level: i32,
        min_txid: TXID,
        max_txid: TXID,
        from: &Path,
    ) -> Result<FileInfo>;
}
