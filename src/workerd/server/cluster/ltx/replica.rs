//! Restore from a replica, ported from litestream@v0.5.11 `replica.go`.
//! Uploading is the owner's (`owner.rs`).
//!
//! Uploads live at L0; a compaction writes a snapshot from TXID 1 at
//! `SNAPSHOT_LEVEL` (litestream's L9). A restore is the newest snapshot and
//! the L0 files that extend it, merged the way `ltx.NewCompactor` and
//! `Decoder.DecodeDatabaseTo` do (ltx@v0.5.1).

use std::fs::File;
use std::io::BufReader;
use std::io::BufWriter;
use std::io::Write;
use std::path::Path;
use std::path::PathBuf;

use futures::StreamExt;
use futures::TryStreamExt;
use futures::stream;

use crate::ltx::TXID;
use crate::ltx::client::ReplicaClient;
use crate::ltx::error::Error;
use crate::ltx::error::Result;
use crate::ltx::ltx::{self};

/// Downloads in flight during a restore, to hide the store's round trips.
const RESTORE_DOWNLOAD_CONCURRENCY: usize = 16;

/// The level of snapshots, as litestream's `SnapshotLevel`.
pub const SNAPSHOT_LEVEL: i32 = 9;

/// The files a restore through `txid` (`TXID(0)`: the latest) reads, in
/// order: the newest snapshot, then each L0 file that extends what came
/// before. A file inside the range already reached is skipped: tiering and
/// bucket-acknowledged commits upload merged ranges concurrently, so an
/// older, narrower range can land beside a newer, wider one, and applying it
/// after would bring back old pages (lost writes on the fleet). A file that
/// extends the range carries the newest version of every page it holds. Not
/// checked for gaps.
pub async fn plan<C: ReplicaClient>(client: &C, txid: TXID) -> Result<Vec<ltx::FileInfo>> {
    let mut infos = client.ltx_files(SNAPSHOT_LEVEL, TXID(0)).await?;
    infos.extend(client.ltx_files(0, TXID(0)).await?);
    if txid != TXID(0) {
        infos.retain(|info| info.max_txid <= txid);
    }
    infos.sort_by_key(|info| (info.min_txid, std::cmp::Reverse(info.max_txid)));
    let mut reached = TXID(0);
    infos.retain(|info| {
        let extends = info.max_txid > reached;
        reached = reached.max(info.max_txid);
        extends
    });
    Ok(infos)
}

/// Restores the replica into `output_path`, which must not exist, up to
/// `txid` (`TXID(0)`: the latest). The plan (`plan`) must be contiguous
/// from TXID 1 (replica.go:1454-1526). The files are spooled to
/// `<output>.restore/` and merged a page at a time into `<output>.tmp`,
/// which is renamed into place (replica.go:657-694), on the blocking pool.
pub async fn restore<C: ReplicaClient>(client: &C, output_path: &Path, txid: TXID) -> Result<()> {
    if output_path.exists() {
        return Err(Error::Other(
            format!(
                "cannot restore, output path already exists: {}",
                output_path.display()
            )
            .into(),
        ));
    }
    let infos = plan(client, txid).await?;
    let mut reached = TXID(0);
    for info in &infos {
        if info.min_txid.0 > reached.0 + 1 {
            return Err(Error::Other(
                format!(
                    "non-contiguous ltx files: have up to {reached} but next file starts at {}",
                    info.min_txid
                )
                .into(),
            ));
        }
        reached = reached.max(info.max_txid);
    }
    if infos.is_empty() || (txid != TXID(0) && reached < txid) {
        return Err(Error::TxNotAvailable);
    }
    let spool = with_suffix(output_path, ".restore");
    std::fs::create_dir_all(&spool)?;
    let files: Vec<_> = (0..infos.len())
        .map(|i| spool.join(i.to_string()))
        .collect();
    let downloaded = stream::iter(infos.iter().zip(&files))
        .map(|(info, file)| client.open_ltx_file(info.level, info.min_txid, info.max_txid, file))
        .buffer_unordered(RESTORE_DOWNLOAD_CONCURRENCY)
        .try_collect::<()>()
        .await;
    if let Err(e) = downloaded {
        let _ = std::fs::remove_dir_all(&spool);
        return Err(e);
    }
    let output = output_path.to_owned();
    let result = blocking(move || write_image(&files, &output)).await;
    let _ = std::fs::remove_dir_all(&spool);
    result
}

/// Merges the LTX `files` (a snapshot, then what extends it) into the
/// database at `output`: pages `1..=commit`, the lock page zero-filled
/// (decoder.go:236-254). A page missing from the merge means the chain is
/// incomplete. There is no fsync: a restored image is never read after a
/// crash, since the takeover that follows one deletes it.
fn write_image(files: &[PathBuf], output: &Path) -> Result<()> {
    let readers = files
        .iter()
        .map(|f| Ok(BufReader::new(File::open(f)?)))
        .collect::<Result<Vec<_>>>()?;
    let mut merge = ltx::Merge::new(readers)?;
    let (page_size, commit) = (merge.header.page_size, merge.header.commit);
    let lock = ltx::lock_pgno(page_size);
    let zero = vec![0; page_size as usize];
    let tmp = with_suffix(output, ".tmp");
    let mut out = BufWriter::new(File::create(&tmp)?);
    let mut next = 1;
    let fill = |out: &mut BufWriter<File>, next: &mut u32, to: u32| -> Result<()> {
        for pgno in *next..to {
            if pgno != lock {
                return Err(Error::Other(format!("restore lacks page {pgno}").into()));
            }
            out.write_all(&zero)?;
        }
        *next = to.max(*next);
        Ok(())
    };
    while let Some((pgno, data)) = merge.next()? {
        fill(&mut out, &mut next, pgno)?;
        out.write_all(data)?;
        next = pgno + 1;
    }
    fill(&mut out, &mut next, commit + 1)?;
    merge.finish()?;
    out.flush()?;
    drop(out);
    Ok(std::fs::rename(&tmp, output)?)
}

fn with_suffix(path: &Path, suffix: &str) -> PathBuf {
    let mut path = path.as_os_str().to_owned();
    path.push(suffix);
    path.into()
}

async fn blocking<T: Send + 'static>(f: impl FnOnce() -> Result<T> + Send + 'static) -> Result<T> {
    tokio::task::spawn_blocking(f)
        .await
        .map_err(|e| Error::Other(format!("blocking task: {e}").into()))?
}
