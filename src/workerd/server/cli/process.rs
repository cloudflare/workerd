//! The running process as the C++ driver sees it: our own executable (and the config `workerd
//! compile` appended to it), the files `--watch` watches, and the listen sockets inherited through
//! `--socket-fd`.

use std::fs::File;
use std::io;
use std::io::BufWriter;
use std::io::Read;
use std::io::Seek;
use std::io::SeekFrom;
use std::io::Write;
use std::path::Path;
use std::path::PathBuf;

use cxx::KjError;

use crate::config::stdout_file;
use crate::config::words_from_bytes;
use crate::socket_fd::InheritedSocket;
use crate::watch::Watcher;

/// Identifies a binary that has a config compiled in. The layout of such a binary is:
///
/// - Binary executable data (a copy of the workerd binary).
/// - Padding to an 8-byte boundary.
/// - Cap'n-Proto-encoded config: an encoded message (segment table, then segments).
/// - 8-byte size of the config, counted in 8-byte words.
/// - This 16-byte magic number.
///
/// Words are written in native byte order.
const COMPILED_MAGIC_SUFFIX: [u64; 2] = [0xa69e_da94_d3cc_02b5, 0xa3d9_77fd_bf54_7d7f];

const WORD_BYTES: u64 = 8;

/// The config size word plus the magic number.
const TRAILER_BYTES: u64 = WORD_BYTES * 3;

/// Our own executable.
pub struct Executable {
    path: PathBuf,
}

impl Executable {
    /// Finds the executable this process was started from.
    pub fn find() -> Option<Self> {
        let path = std::env::current_exe().ok()?;
        path.is_file().then_some(Self { path })
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    /// Reads the config compiled into this executable, as an encoded message, if there is one.
    pub fn read_embedded_config(&self) -> io::Result<Option<Vec<u64>>> {
        read_embedded_config(&File::open(&self.path)?)
    }
}

pub struct Process {
    executable: Option<Executable>,
    watcher: Option<Watcher>,
    /// Kept open, as inherited, so that a `--watch` re-exec passes them on unchanged.
    inherited_sockets: Vec<InheritedSocket>,
}

impl Process {
    pub const fn new(
        executable: Option<Executable>,
        watcher: Option<Watcher>,
        inherited_sockets: Vec<InheritedSocket>,
    ) -> Self {
        Self {
            executable,
            watcher,
            inherited_sockets,
        }
    }

    pub const fn is_watching(&self) -> bool {
        self.watcher.is_some()
    }

    pub const fn watcher(&self) -> Option<&Watcher> {
        self.watcher.as_ref()
    }

    /// Adds a file the config depends on to the watched set (native path bytes); a no-op without
    /// `--watch`.
    pub fn watch_file(&self, path: &[u8]) -> Result<(), KjError> {
        self.watcher
            .as_ref()
            .map_or(Ok(()), |watcher| watcher.watch(path))
    }

    /// `--watch`'s reload: re-executes the server with the same arguments, waiting for the
    /// executable to reappear if it is missing (it is being rebuilt).
    pub fn reload(&self) -> ! {
        // Extra spaces fully overwrite the line written earlier with a CR but no LF:
        //     "Noticed configuration change, reloading shortly...\r"
        eprintln!("Reloading due to config change...                                      ");
        let mut missing_binary = false;
        loop {
            if let Err(error) = self.exec_self() {
                eprintln!("failed to re-execute the server: {error}");
                std::process::exit(1);
            }
            if !missing_binary {
                eprint!("The server executable is missing! Waiting for it to reappear...\r");
                missing_binary = true;
            }
            std::thread::sleep(std::time::Duration::from_secs(1));
        }
    }

    /// Replaces the process with a fresh run of the executable and the original arguments. The
    /// inherited sockets are passed on: they are held here exactly as they arrived. Returns only
    /// if the executable is currently missing.
    #[cfg(unix)]
    fn exec_self(&self) -> io::Result<()> {
        use std::os::unix::process::CommandExt;
        use std::process::Command;

        let executable = self.executable.as_ref().ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::NotFound,
                "cannot re-execute: the program's own executable was not found at startup",
            )
        })?;
        // The sockets are kept for exactly this moment; nothing else reads them here.
        let _ = &self.inherited_sockets;

        let mut args = std::env::args_os();
        let mut command = Command::new(&executable.path);
        if let Some(arg0) = args.next() {
            command.arg0(arg0);
        }
        let error = command.args(args).exec();
        if error.kind() == io::ErrorKind::NotFound {
            Ok(())
        } else {
            Err(error)
        }
    }

    #[cfg(windows)]
    #[expect(
        clippy::unused_self,
        reason = "same signature as the unix implementation"
    )]
    fn exec_self(&self) -> io::Result<()> {
        Err(io::Error::new(
            io::ErrorKind::Unsupported,
            "watching is not yet implemented on Windows",
        ))
    }

    /// Writes a copy of the executable with `config` (an encoded message) compiled in to stdout.
    pub fn write_compiled_binary(&self, config: &[u64]) -> io::Result<()> {
        let executable = self.executable.as_ref().ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::NotFound,
                "the program's own executable was not found at startup",
            )
        })?;

        let stdout = stdout_file()?;
        // Grab the inode info before we write anything.
        let before = stdout.metadata()?;

        write_compiled_binary(&File::open(&executable.path)?, config, &stdout)?;

        #[cfg(unix)]
        {
            use std::fs::Permissions;
            use std::os::unix::fs::PermissionsExt;

            // If we wrote a regular file that was empty before we started, make it executable for
            // everyone who can read it.
            if before.is_file() && before.len() == 0 {
                let mode = before.permissions().mode() & 0o7777;
                stdout.set_permissions(Permissions::from_mode(mode | ((mode & 0o444) >> 2)))?;
            }
        }
        #[cfg(not(unix))]
        let _ = before;

        Ok(())
    }
}

/// Resolves once a watched file has changed and changes have settled. The future owns a share of
/// the watcher, not a borrow of `process`.
pub fn wait_for_changes(process: &Process) -> impl Future<Output = Result<(), KjError>> + use<> {
    let changes = process.watcher.as_ref().map(Watcher::wait_for_changes);
    async move {
        match changes {
            Some(changes) => changes.await,
            None => Err(KjError::new(
                cxx::KjExceptionType::Failed,
                "wait_for_changes() called without --watch".to_owned(),
            )),
        }
    }
}

fn read_words(mut input: impl Read, count: u64) -> io::Result<Vec<u64>> {
    let mut bytes = vec![0u8; usize::try_from(count * WORD_BYTES).map_err(io::Error::other)?];
    input.read_exact(&mut bytes)?;
    Ok(words_from_bytes(&bytes))
}

fn read_embedded_config(mut executable: &File) -> io::Result<Option<Vec<u64>>> {
    let size = executable.metadata()?.len();
    let Some(body_bytes) = size.checked_sub(TRAILER_BYTES) else {
        return Ok(None);
    };

    executable.seek(SeekFrom::Start(body_bytes))?;
    let trailer = read_words(executable, 3)?;
    let &[config_words, ref magic @ ..] = trailer.as_slice() else {
        return Ok(None);
    };
    if *magic != COMPILED_MAGIC_SUFFIX {
        return Ok(None);
    }

    let config_offset = config_words
        .checked_mul(WORD_BYTES)
        .and_then(|config_bytes| body_bytes.checked_sub(config_bytes))
        .ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                "compiled-in config is larger than the executable",
            )
        })?;
    executable.seek(SeekFrom::Start(config_offset))?;
    read_words(executable, config_words).map(Some)
}

fn write_compiled_binary(mut executable: &File, config: &[u64], output: &File) -> io::Result<()> {
    executable.seek(SeekFrom::Start(0))?;
    let mut output = BufWriter::new(output);
    let executable_bytes = io::copy(&mut executable, &mut output)?;

    // Pad to a word boundary if necessary.
    let padding = (WORD_BYTES - executable_bytes % WORD_BYTES) % WORD_BYTES;
    let padding = usize::try_from(padding).map_err(io::Error::other)?;
    output.write_all(&[0u8; 8][..padding])?;

    let config_words = u64::try_from(config.len()).map_err(io::Error::other)?;
    for word in config
        .iter()
        .chain(&[config_words])
        .chain(&COMPILED_MAGIC_SUFFIX)
    {
        output.write_all(&word.to_ne_bytes())?;
    }
    output.flush()
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A read-write file in the test's temp directory, removed when dropped.
    struct TempFile {
        path: PathBuf,
        file: File,
    }

    impl TempFile {
        fn new(contents: &[u8]) -> Self {
            use std::sync::atomic::AtomicUsize;
            use std::sync::atomic::Ordering;

            static COUNTER: AtomicUsize = AtomicUsize::new(0);
            let dir =
                std::env::var_os("TEST_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from);
            let path = dir.join(format!(
                "workerd-cli-test-{}-{}",
                std::process::id(),
                COUNTER.fetch_add(1, Ordering::Relaxed)
            ));
            let mut file = File::options()
                .read(true)
                .write(true)
                .create_new(true)
                .open(&path)
                .unwrap();
            file.write_all(contents).unwrap();
            Self { path, file }
        }
    }

    impl Drop for TempFile {
        fn drop(&mut self) {
            let _ = std::fs::remove_file(&self.path);
        }
    }

    #[test]
    fn compiled_config_round_trips() {
        for executable_len in [1usize, 7, 8, 9, 64] {
            let executable = TempFile::new(&vec![0x7fu8; executable_len]);
            let config = [0x0123_4567_89ab_cdef_u64, 0, u64::MAX];

            let output = TempFile::new(&[]);
            write_compiled_binary(&executable.file, &config, &output.file).unwrap();

            let size = output.file.metadata().unwrap().len();
            assert_eq!(size % WORD_BYTES, 0);
            assert_eq!(
                size,
                executable_len.next_multiple_of(8) as u64 + 6 * WORD_BYTES
            );
            assert_eq!(
                read_embedded_config(&output.file).unwrap().as_deref(),
                Some(&config[..])
            );
        }
    }

    #[test]
    fn plain_executable_has_no_config() {
        for contents in [&[][..], &[1; 23], &[1; 4096]] {
            assert_eq!(
                read_embedded_config(&TempFile::new(contents).file).unwrap(),
                None
            );
        }
    }

    #[test]
    fn oversized_config_is_an_error() {
        let mut bytes = vec![0u8; 16];
        bytes.extend_from_slice(&3u64.to_ne_bytes());
        for word in COMPILED_MAGIC_SUFFIX {
            bytes.extend_from_slice(&word.to_ne_bytes());
        }
        let error = read_embedded_config(&TempFile::new(&bytes).file).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::InvalidData);
    }
}
