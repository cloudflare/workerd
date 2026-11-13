// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The history file, which keeps the lines typed at a terminal from one session to the next:
//! `~/.workerd_repl_history`, or the file `WORKERD_REPL_HISTORY` names. Set to the empty string,
//! `WORKERD_REPL_HISTORY` turns saving off.
//!
//! The file holds one entry per line; an entry has no line breaks, since Enter ends a line. Each
//! entry is appended as it is submitted, rather than the history being written at exit, because a
//! Ctrl+C while an input runs ends the process at once. Loading keeps the latest entries and
//! rewrites a file that holds more, in place, so that a history file that is a symbolic link stays
//! one. The file is created readable by its owner only: typed lines can hold secrets.

use std::ffi::OsString;
use std::fs;
use std::fs::File;
use std::io;
use std::io::Write;
use std::path::Path;
use std::path::PathBuf;

/// The environment variable that names the history file.
const HISTORY_VAR: &str = "WORKERD_REPL_HISTORY";

/// The history file's name in the home directory, unless `WORKERD_REPL_HISTORY` names another.
const DEFAULT_NAME: &str = ".workerd_repl_history";

/// A session's history file.
pub struct HistoryFile {
    path: PathBuf,
    /// Open for appending, from the first entry recorded.
    file: Option<File>,
}

impl HistoryFile {
    /// The history file the environment chooses; none if the session saves no history.
    pub fn from_env() -> Option<Self> {
        choose(std::env::var_os(HISTORY_VAR), std::env::var_os("HOME")).map(Self::new)
    }

    fn new(path: PathBuf) -> Self {
        Self { path, file: None }
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    /// The latest `limit` entries, oldest first, rewriting the file if it holds more. A file that
    /// does not exist holds none.
    pub fn load(&self, limit: usize) -> io::Result<Vec<String>> {
        let bytes = match fs::read(&self.path) {
            Ok(bytes) => bytes,
            Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(Vec::new()),
            Err(error) => return Err(error),
        };
        let text = String::from_utf8_lossy(&bytes);
        let mut entries: Vec<String> = text
            .lines()
            .filter(|line| !line.is_empty())
            .map(str::to_owned)
            .collect();
        if entries.len() > limit {
            entries.drain(..entries.len() - limit);
            let mut contents = entries.join("\n");
            contents.push('\n');
            open(&self.path, Mode::Truncate)?.write_all(contents.as_bytes())?;
        }
        Ok(entries)
    }

    /// Appends `entry`, creating the file if need be.
    pub fn record(&mut self, entry: &str) -> io::Result<()> {
        let file = match &mut self.file {
            Some(file) => file,
            None => self.file.insert(open(&self.path, Mode::Append)?),
        };
        // One write, so that the entries of sessions sharing the file do not interleave.
        file.write_all(format!("{entry}\n").as_bytes())
    }
}

/// The history file: `var`, the value of `WORKERD_REPL_HISTORY`, unless it is empty; else
/// `.workerd_repl_history` in `home`. None if `var` is empty, or there is neither.
fn choose(var: Option<OsString>, home: Option<OsString>) -> Option<PathBuf> {
    match var {
        Some(path) if path.is_empty() => None,
        Some(path) => Some(PathBuf::from(path)),
        None => home
            .filter(|home| !home.is_empty())
            .map(|home| Path::new(&home).join(DEFAULT_NAME)),
    }
}

#[derive(Clone, Copy)]
enum Mode {
    Append,
    Truncate,
}

/// `path` opened for writing, created readable and writable by its owner only if it does not
/// exist.
fn open(path: &Path, mode: Mode) -> io::Result<File> {
    let mut options = fs::OpenOptions::new();
    options.create(true);
    match mode {
        Mode::Append => options.append(true),
        Mode::Truncate => options.write(true).truncate(true),
    };
    #[cfg(unix)]
    std::os::unix::fs::OpenOptionsExt::mode(&mut options, 0o600);
    options.open(path)
}

#[cfg(test)]
#[path = "history-test.rs"]
mod tests;
