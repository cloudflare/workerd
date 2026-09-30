//! `--watch`: the files the server depends on -- the config, everything it imports, and this
//! executable -- and waiting for them to change.

use std::io;
use std::path::Path;
use std::sync::Arc;
use std::time::Duration;

use cxx::KjError;
use kj_rs_io::TokioFileWatcher;

/// How long changes must stop arriving before a reload: saving several files, or a rebuild, is
/// a burst of changes.
const SETTLE_TIME: Duration = Duration::from_millis(500);

pub struct Watcher {
    files: Arc<TokioFileWatcher>,
}

impl Watcher {
    /// Starts watching `executable`, so that rebuilding the server reloads it too.
    pub fn new(executable: &Path) -> io::Result<Self> {
        let files = TokioFileWatcher::new().map_err(|error| io::Error::other(error.to_string()))?;
        let executable = std::path::absolute(executable)?;
        files
            .watch(executable.as_os_str().as_encoded_bytes())
            .map_err(|error| io::Error::other(error.to_string()))?;
        Ok(Self {
            files: Arc::new(files),
        })
    }

    /// Adds a file to the watched set: `path` is native path bytes.
    pub fn watch(&self, path: &[u8]) -> Result<(), KjError> {
        self.files.watch(path).map_err(KjError::from)
    }

    /// Waits for a watched file to change, outside any event loop: used when the config could not
    /// be compiled and there is nothing else to run until it is fixed.
    pub fn block_until_changes(&self) -> Result<(), KjError> {
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_time()
            .build()
            .map_err(|error| {
                KjError::new(
                    cxx::KjExceptionType::Failed,
                    format!("tokio runtime: {error}"),
                )
            })?;
        runtime.block_on(self.wait_for_changes())
    }

    /// Resolves once a watched file has changed and changes have settled. The watcher itself
    /// needs no runtime (kj-rs-io's is notify-driven); the settle timer needs tokio's.
    pub fn wait_for_changes(&self) -> impl Future<Output = Result<(), KjError>> + use<> {
        let files = Arc::clone(&self.files);
        async move {
            files.on_change().await.map_err(KjError::from)?;

            // Let the user know we saw the change. A carriage return rather than a newline, so
            // the next line written replaces this one.
            eprint!("Noticed configuration change, reloading shortly...\r");

            // Each further change within SETTLE_TIME restarts the wait.
            while let Ok(change) = tokio::time::timeout(SETTLE_TIME, files.on_change()).await {
                change.map_err(KjError::from)?;
            }
            Ok(())
        }
    }
}
