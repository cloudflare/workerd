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
        let dir = std::env::var_os("TEST_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from);
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
