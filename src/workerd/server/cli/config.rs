//! Where the config comes from: a Cap'n Proto schema file (compiled by C++, the only compiler for
//! that format), an encoded config file (`--binary`), stdin, the config compiled into the
//! executable. Every source is checked to be a well-formed `Config` message here, and ends up as
//! a [`Config`] for the C++ driver.

use std::ffi::OsString;
use std::fmt;
use std::fs::File;
use std::io;
use std::io::Read;
use std::io::Write;

use capnp::message::ReaderOptions;
use workerd_capnp::config;

use crate::bridge::ffi;

/// A `Config` message, checked: encoded as a Cap'n Proto stream (segment table, then segments),
/// in 8-byte words.
pub struct Config(Vec<u64>);

impl Config {
    /// Checks that `bytes` are one well-formed message whose root reads as a `Config`, and keeps
    /// it re-encoded.
    pub fn from_message_bytes(mut bytes: &[u8]) -> capnp::Result<Self> {
        let message = capnp::serialize::read_message(&mut bytes, reader_options())?;
        // total_size() visits every pointer, so a malformed message fails here rather than in the
        // server.
        message.get_root::<config::Reader>()?.total_size()?;
        Ok(Self(words_from_bytes(
            &capnp::serialize::write_message_segments_to_words(&message.into_segments()),
        )))
    }

    /// The same, for a message already in words (the config compiler's output, or the config
    /// compiled into the executable).
    pub fn from_words(words: &[u64]) -> capnp::Result<Self> {
        Self::from_message_bytes(&bytes_from_words(words))
    }

    /// Reads an encoded config file (`--binary <config-file>`).
    pub fn read_file(path: &str) -> Result<Self, String> {
        let mut file = File::open(path).map_err(|_| "No such file.".to_owned())?;
        // Check that we have a file, not a directory.
        if !file
            .metadata()
            .map_err(|error| error.to_string())?
            .is_file()
        {
            return Err("Config path is not a file.".to_owned());
        }
        let mut bytes = Vec::new();
        file.read_to_end(&mut bytes)
            .map_err(|error| error.to_string())?;
        Self::from_message_bytes(&bytes).map_err(|error| error.to_string())
    }

    /// Reads one encoded config message from stdin (`--binary -`). Unbuffered, so nothing after
    /// the message is consumed: the config may tell the server to read stdin itself.
    pub fn read_stdin() -> Result<Self, String> {
        let mut stdin = stdin_file().map_err(|error| error.to_string())?;
        let message = capnp::serialize::read_message(&mut stdin, reader_options())
            .map_err(|error| error.to_string())?;
        message
            .get_root::<config::Reader>()
            .and_then(|root| root.total_size())
            .map_err(|error| error.to_string())?;
        Ok(Self(words_from_bytes(
            &capnp::serialize::write_message_segments_to_words(&message.into_segments()),
        )))
    }

    pub fn into_words(self) -> Vec<u64> {
        self.0
    }

    /// The encoded config as bytes.
    pub fn to_bytes(&self) -> Vec<u8> {
        bytes_from_words(&self.0)
    }

    /// Fills `shutdownDelayMs` from `SHUTDOWN_DELAY_MS` where the config leaves it at 0, then
    /// checks it. See `Config` in workerd.capnp. `var` reads an environment variable. A variable
    /// that is unset, or empty once trailing whitespace is trimmed, leaves the config as it is.
    /// One that is not UTF-8 or not a whole number of milliseconds, and a delay above
    /// `MAX_SHUTDOWN_DELAY_MS` from either source, is an error.
    pub fn with_shutdown_delay_environment(
        self,
        var: impl Fn(&str) -> Option<OsString>,
    ) -> Result<Self, String> {
        let bytes = self.to_bytes();
        let message = capnp::serialize::read_message(bytes.as_slice(), reader_options())
            .map_err(|error| error.to_string())?;
        let root = message
            .get_root::<config::Reader>()
            .map_err(|error| error.to_string())?;
        let configured = root.get_shutdown_delay_ms();
        if configured > MAX_SHUTDOWN_DELAY_MS {
            return Err(format!(
                "Config.shutdownDelayMs = {configured}: more than {MAX_SHUTDOWN_DELAY_MS} (an \
                 hour)."
            ));
        }
        if configured != 0 {
            return Ok(self);
        }
        let Some(delay) = shutdown_delay(var(SHUTDOWN_DELAY_MS))? else {
            return Ok(self);
        };
        let mut message = capnp::message::Builder::new_default();
        message.set_root(root).map_err(|error| error.to_string())?;
        message
            .get_root::<config::Builder>()
            .map_err(|error| error.to_string())?
            .set_shutdown_delay_ms(delay);
        Ok(Self(words_from_bytes(
            &capnp::serialize::write_message_to_words(&message),
        )))
    }

    /// Writes the encoded config to stdout (`compile --config-only`).
    pub fn write_to_stdout(&self) -> io::Result<()> {
        let mut out = io::stdout().lock();
        for word in &self.0 {
            out.write_all(&word.to_ne_bytes())?;
        }
        out.flush()
    }
}

/// The environment variable that holds the shutdown delay when the config leaves it at 0.
pub const SHUTDOWN_DELAY_MS: &str = "WORKERD_SHUTDOWN_DELAY_MS";

/// The largest shutdown delay, set in the config or in `SHUTDOWN_DELAY_MS`. The delay waits out
/// load balancers that still send connections, which takes seconds, so a larger value is a
/// mistake, such as a value in microseconds, that would leave a stopping process serving for days
/// and a cluster node killed by its supervisor before it hands its Durable Objects off.
const MAX_SHUTDOWN_DELAY_MS: u32 = 3_600_000;

/// The delay in `value`, the value of `SHUTDOWN_DELAY_MS`, without trailing whitespace such as a
/// final newline: digits only. `None` when it is unset or blank.
fn shutdown_delay(value: Option<OsString>) -> Result<Option<u32>, String> {
    let Some(value) = value else {
        return Ok(None);
    };
    let value = value.into_string().map_err(|value| {
        format!(
            "{SHUTDOWN_DELAY_MS}={}: not UTF-8 text.",
            value.to_string_lossy()
        )
    })?;
    let digits = value.trim_end();
    if digits.is_empty() {
        return Ok(None);
    }
    digits
        .bytes()
        .all(|byte| byte.is_ascii_digit())
        .then(|| digits.parse::<u32>().ok())
        .flatten()
        .filter(|delay| *delay <= MAX_SHUTDOWN_DELAY_MS)
        .map(Some)
        .ok_or_else(|| {
            format!(
                "{SHUTDOWN_DELAY_MS}={value}: not a whole number of milliseconds from 0 to \
                 {MAX_SHUTDOWN_DELAY_MS} (an hour)."
            )
        })
}

/// Configs can legitimately be very large and are not malicious, so use an effectively-infinite
/// traversal limit.
fn reader_options() -> ReaderOptions {
    *ReaderOptions::new().traversal_limit_in_words(None)
}

impl fmt::Display for ffi::ConfigParseError {
    /// `file:line:col-endcol: message`, the way compilers print it.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}:{}:{}", self.file, self.line, self.column)?;
        if self.end_column != 0 {
            write!(f, "-{}", self.end_column)?;
        }
        write!(f, ": {}", self.message)
    }
}

/// Converts bytes to native-endian words, ignoring a trailing partial word.
pub fn words_from_bytes(bytes: &[u8]) -> Vec<u64> {
    bytes
        .chunks_exact(8)
        .map(|chunk| {
            let mut word = [0u8; 8];
            word.copy_from_slice(chunk);
            u64::from_ne_bytes(word)
        })
        .collect()
}

fn bytes_from_words(words: &[u64]) -> Vec<u8> {
    words.iter().flat_map(|word| word.to_ne_bytes()).collect()
}

/// An unbuffered handle to stdin, sharing the process's descriptor.
#[cfg(unix)]
fn stdin_file() -> io::Result<File> {
    use std::os::fd::AsFd;
    Ok(File::from(io::stdin().as_fd().try_clone_to_owned()?))
}

#[cfg(windows)]
fn stdin_file() -> io::Result<File> {
    use std::os::windows::io::AsHandle;
    Ok(File::from(io::stdin().as_handle().try_clone_to_owned()?))
}

/// An unbuffered handle to stdout, so a whole executable can be copied into it efficiently.
#[cfg(unix)]
pub fn stdout_file() -> io::Result<File> {
    use std::os::fd::AsFd;
    Ok(File::from(io::stdout().as_fd().try_clone_to_owned()?))
}

#[cfg(windows)]
pub fn stdout_file() -> io::Result<File> {
    use std::os::windows::io::AsHandle;
    Ok(File::from(io::stdout().as_handle().try_clone_to_owned()?))
}

#[cfg(test)]
#[path = "config-test.rs"]
mod tests;
