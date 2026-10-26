//! client::bucket — the `ReplicaClient` over the cluster's `Bucket`.
//!
//! Ported (behavior only) from litestream@v0.5.11 `s3/replica_client.go` by
//! way of celld's `celld-ltx`, keeping the invariants from the upstream
//! conformance suite:
//!
//!   * key scheme `{path}/{level:04x}/{min}-{max}.ltx`
//!     (s3/replica_client.go:629, 677, 1040-1042);
//!   * list + seek-skip on `min_txid < seek`, ascending TXID order
//!     (s3/replica_client.go:1530-1533);
//!   * `NoSuchKey` → `os.ErrNotExist` error mapping
//!     (s3/replica_client.go:647-649, 1662-1668).
//!
//! Every file is one PUT. S3 takes up to 5 GiB that way, and this port has no
//! compactor to produce oversized files, so the multipart path stays in celld.

use std::io::Read;
use std::path::Path;

use crate::Bucket;
use crate::ltx::TXID;
use crate::ltx::client::ReplicaClient;
use crate::ltx::error::Error;
use crate::ltx::error::Result;
use crate::ltx::ltx::FileInfo;
use crate::ltx::ltx::{self};
use crate::s3::format_rfc3339_nano;

/// The standard Litestream S3 metadata key for an LTX header timestamp.
const METADATA_KEY_TIMESTAMP: &str = "litestream-timestamp";

/// A replica rooted at the bucket's prefix.
#[derive(Clone)]
pub struct BucketClient {
    bucket: Bucket,
}

impl BucketClient {
    pub fn new(bucket: Bucket) -> Self {
        Self { bucket }
    }

    /// Delete a file a compaction replaced.
    pub async fn delete_ltx_file(&self, level: i32, min_txid: TXID, max_txid: TXID) -> Result<()> {
        self.bucket
            .delete(&Self::ltx_key(level, min_txid, max_txid))
            .await
            .map_err(other)
    }

    /// `{level:04x}/{min}-{max}.ltx` (s3/replica_client.go:629, 677, 1040-1042).
    fn ltx_key(level: i32, min_txid: TXID, max_txid: TXID) -> String {
        format!("{level:04x}/{}", ltx::format_filename(min_txid, max_txid))
    }
}

fn other(error: crate::Error) -> Error {
    Error::Other(error.to_string().into())
}

fn not_found(key: &str) -> Error {
    Error::Io(std::io::Error::new(
        std::io::ErrorKind::NotFound,
        format!("replica: get object {key}: not found"),
    ))
}

#[async_trait::async_trait(?Send)]
impl ReplicaClient for BucketClient {
    async fn ltx_files(&self, level: i32, seek: TXID) -> Result<Vec<FileInfo>> {
        let prefix = format!("{level:04x}/");
        let listed = self.bucket.list(&prefix).await.map_err(other)?;
        let mut infos: Vec<FileInfo> = listed
            .into_iter()
            .filter_map(|entry| {
                let name = entry.key.rsplit('/').next()?;
                let (min_txid, max_txid) = ltx::parse_filename(name).ok()?;
                (min_txid >= seek).then(|| FileInfo {
                    level,
                    min_txid,
                    max_txid,
                    ..Default::default()
                })
            })
            .collect();
        infos.sort_by_key(|info| (info.level, info.min_txid.0, info.max_txid.0));
        Ok(infos)
    }

    async fn open_ltx_file(
        &self,
        level: i32,
        min_txid: TXID,
        max_txid: TXID,
        to: &Path,
    ) -> Result<()> {
        let key = Self::ltx_key(level, min_txid, max_txid);
        match self.bucket.get_file(&key, to).await.map_err(other)? {
            true => Ok(()),
            false => Err(not_found(&key)),
        }
    }

    async fn write_ltx_file(
        &self,
        level: i32,
        min_txid: TXID,
        max_txid: TXID,
        from: &Path,
    ) -> Result<FileInfo> {
        // The LTX header timestamp goes in the standard Litestream metadata,
        // so Litestream can restore by time without reading every file.
        let mut file = std::fs::File::open(from)?;
        let mut bytes = [0; ltx::HEADER_SIZE];
        file.read_exact(&mut bytes)?;
        let header = ltx::Header::parse(&bytes)?;
        let size = file.metadata()?.len();
        let key = Self::ltx_key(level, min_txid, max_txid);
        let stamp = format_rfc3339_nano(header.timestamp);
        self.bucket
            .put_file(&key, from, &[(METADATA_KEY_TIMESTAMP, stamp)])
            .await
            .map_err(|e| Error::Other(format!("replica: upload to {key}: {e}").into()))?;
        Ok(FileInfo {
            level,
            min_txid,
            max_txid,
            size: size as i64,
            created_at: Some(
                std::time::UNIX_EPOCH
                    + std::time::Duration::from_millis(header.timestamp.max(0) as u64),
            ),
            ..Default::default()
        })
    }
}
