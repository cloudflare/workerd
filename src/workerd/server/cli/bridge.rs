//! The FFI between the Rust entry point and the C++ side: the config compiler (config-compiler.h)
//! and the driver (cli-main.h).
//!
//! Each command is one call into C++ carrying its parsed options as plain structs. C++ calls back
//! into [`Process`] for `--watch`.

// The bridge macro expands to `unsafe` FFI declarations; the crate root denies unsafe code.
#![allow(unsafe_code)]

pub use crate::process::Process;
pub use crate::process::wait_for_changes;

#[cxx::bridge(namespace = "workerd::server::cli")]
pub mod ffi {
    /// A `<name>=<value>` option value.
    struct Override {
        name: String,
        value: String,
    }

    /// A `--socket-fd <name>=<fd>` value: `fd` is a duplicate of the inherited listening socket
    /// for the server to own (kj's `wrapListenSocketFd` with `TAKE_OWNERSHIP`).
    struct SocketFd {
        name: String,
        fd: i64,
    }

    struct CommonOptions {
        /// Prefixes usage errors, e.g. `workerd serve`.
        program_name: String,
        verbose: bool,
    }

    /// A parse error in a schema file. `line` and `column` are 1-based; `end_column` is 0 when
    /// the error has no extent on the line.
    struct ConfigParseError {
        file: String,
        line: u32,
        column: u32,
        end_column: u32,
        message: String,
    }

    struct CompiledConfig {
        /// The config as an encoded message (segment table, then segments), in 8-byte words.
        /// Empty if there were parse errors.
        message: Vec<u64>,
        errors: Vec<ConfigParseError>,
    }

    struct ServeOrTestOptions {
        directory_overrides: Vec<Override>,
        external_overrides: Vec<Override>,
        inspector_addr: KjMaybe<String>,
        perfetto_trace_path: KjMaybe<String>,
        perfetto_trace_categories: KjMaybe<String>,
        experimental: bool,
        pyodide_package_disk_cache_dir: KjMaybe<String>,
        pyodide_bundle_disk_cache_dir: KjMaybe<String>,
        python_save_snapshot: bool,
        python_save_baseline_snapshot: bool,
        python_load_snapshot: KjMaybe<String>,
        python_snapshot_dir: KjMaybe<String>,
    }

    struct ServeOptions {
        socket_addr_overrides: Vec<Override>,
        socket_fd_overrides: Vec<SocketFd>,
        control_fd: KjMaybe<u32>,
        debug_port: KjMaybe<String>,
    }

    struct TestOptions {
        no_verbose: bool,
        predictable: bool,
        gc_stress: bool,
        all_autogates: bool,
        compat_date: KjMaybe<String>,
        service_pattern: KjMaybe<String>,
        entrypoint_pattern: KjMaybe<String>,
    }

    unsafe extern "C++" {
        include!("workerd/server/cli-main.h");
        include!("workerd/server/config-compiler.h");

        /// Compiles a config written as a Cap'n Proto schema file. Every file it is about to read
        /// is first registered with `process.watch_file()`, so `--watch` cannot miss a change
        /// made while compiling. An error is a problem with the file or the arguments themselves
        /// (not found, no such constant, ...); parse errors in the file's contents are returned
        /// in `errors`.
        fn compile_config(
            path: &str,
            import_paths: &[String],
            const_name: KjMaybe<&str>,
            process: &Process,
        ) -> Result<CompiledConfig>;

        /// The release date this binary was built from, as ASCII.
        fn release_version() -> &'static [u8];
        fn perfetto_supported() -> bool;
        fn fuzzilli_supported() -> bool;
        /// The package lock file of the current Pyodide release.
        fn pyodide_lock() -> Result<String>;

        // Each command runs to completion and exits the process, except under KJ_CLEAN_SHUTDOWN,
        // where it returns the exit code. `config` is an encoded message (segment table, then
        // segments) in 8-byte words, owned by the driver for the run. An error means the C++
        // driver failed to start.
        fn run_serve(
            common: &CommonOptions,
            config: Vec<u64>,
            serve_or_test: &ServeOrTestOptions,
            serve: &ServeOptions,
            process: Box<Process>,
        ) -> Result<i32>;
        fn run_test(
            common: &CommonOptions,
            config: Vec<u64>,
            serve_or_test: &ServeOrTestOptions,
            test: &TestOptions,
            process: Box<Process>,
        ) -> Result<i32>;
    }

    extern "Rust" {
        type Process;

        /// Whether `--watch` was given.
        fn is_watching(self: &Process) -> bool;

        /// Adds a file the config depends on to the watched set (native path bytes); a no-op
        /// without `--watch`.
        fn watch_file(self: &Process, path: &[u8]) -> Result<()>;

        /// Resolves once a watched file has changed and changes have settled. Only called with
        /// `--watch`.
        async fn wait_for_changes(process: &Process) -> Result<()>;

        /// `--watch`'s reload: replaces the process with a fresh run of the executable and the
        /// original arguments, retrying while the executable is missing (mid-rebuild). Does not
        /// return; the C++ signature cannot say so.
        fn reload(self: &Process);
    }
}
