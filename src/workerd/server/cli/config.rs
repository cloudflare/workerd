//! Where the config comes from: a Cap'n Proto schema file (compiled by C++, the only compiler for
//! that format), an encoded config file (`--binary`), stdin, the config compiled into the
//! executable, or the config `make-pyodide-baseline-snapshot` synthesizes. Every source is
//! checked to be a well-formed `Config` message here, and ends up as a [`Config`] for the C++
//! driver.

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

    /// The config `make-pyodide-baseline-snapshot` runs: one Python worker with the given
    /// compatibility flag selecting the Python version.
    pub fn python_baseline(python_flag: &str) -> Self {
        let mut message = capnp::message::Builder::new_default();
        let config = message.init_root::<config::Builder>();
        let mut service = config.init_services(1).get(0);
        service.set_name("main");
        let mut worker = service.init_worker();
        worker.set_compatibility_date("2023-12-18");
        let mut flags = worker.reborrow().init_compatibility_flags(2);
        flags.set(0, python_flag);
        flags.set(1, "python_workers");
        let mut module = worker.init_modules(1).get(0);
        module.set_name("main.py");
        module.set_python_module("def test():\n pass");
        Self(words_from_bytes(&capnp::serialize::write_message_to_words(
            &message,
        )))
    }

    pub fn into_words(self) -> Vec<u64> {
        self.0
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
mod tests {
    use super::*;

    fn read(config: &Config) -> capnp::message::Reader<capnp::serialize::OwnedSegments> {
        capnp::serialize::read_message(bytes_from_words(&config.0).as_slice(), reader_options())
            .unwrap()
    }

    #[test]
    fn python_baseline_config_has_one_python_worker() {
        use workerd_capnp::service;
        use workerd_capnp::worker;

        let message = read(&Config::python_baseline("python_workers_20250116"));
        let config = message.get_root::<config::Reader>().unwrap();
        let services = config.get_services().unwrap();
        assert_eq!(services.len(), 1);
        let service = services.get(0);
        assert_eq!(service.get_name().unwrap(), "main");
        let service::Which::Worker(worker) = service.which().unwrap() else {
            panic!("expected a worker service")
        };
        let worker = worker.unwrap();
        let flags = worker.get_compatibility_flags().unwrap();
        assert_eq!(flags.get(0).unwrap(), "python_workers_20250116");
        assert_eq!(flags.get(1).unwrap(), "python_workers");
        let worker::Which::Modules(modules) = worker.which().unwrap() else {
            panic!("expected a modules worker")
        };
        assert_eq!(modules.unwrap().get(0).get_name().unwrap(), "main.py");
    }

    #[test]
    fn config_bytes_are_checked() {
        let good = Config::python_baseline("python_workers");
        let round_trip = Config::from_words(&good.0).unwrap();
        assert_eq!(round_trip.0, good.0);

        assert!(Config::from_message_bytes(b"not a capnp message!").is_err());
        // A truncated message fails too.
        let bytes = bytes_from_words(&good.0);
        assert!(Config::from_message_bytes(&bytes[..bytes.len() - 8]).is_err());
    }
}
