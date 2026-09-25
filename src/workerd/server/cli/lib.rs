//! The `workerd` command-line entry point.
//!
//! `main()` finds the executable (a config compiled in by `workerd compile` selects a different
//! command line), parses the command line, produces the encoded config (C++ compiles schema
//! files), and hands the command to the C++ driver (cli-main.c++), which loads the config into a
//! `Server` and runs it. C++ calls back into [`process::Process`] for `--watch`.

// Two modules allow `unsafe`: bridge.rs, whose `cxx::bridge` macro expands to FFI declarations,
// and socket_fd.rs, where a descriptor number from the command line becomes an owned descriptor.
// Everything else is safe code over std, socket2 and the bridge.
#![deny(unsafe_code)]

mod args;
mod bridge;
mod config;
mod process;
mod socket_fd;
mod watch;

use std::fmt::Display;
use std::io::IsTerminal;
use std::process::ExitCode;
use std::sync::OnceLock;

use clap::CommandFactory;
use clap::FromArgMatches;
use clap::error::ErrorKind;

use crate::bridge::ffi;
use crate::config::Config;
use crate::process::Executable;
use crate::process::Process;
use crate::socket_fd::InheritedSocket;
use crate::watch::Watcher;

/// Runs workerd with this process's arguments.
pub fn main() -> ExitCode {
    let program_name = std::env::args_os().next().map_or_else(
        || "workerd".to_owned(),
        |arg0| arg0.to_string_lossy().into_owned(),
    );

    let executable = Executable::find();
    if executable.is_none() {
        eprintln!(
            "Unable to find and open the program executable, so unable to determine if there is a \
             compiled-in config file. Proceeding on the assumption that there is not."
        );
    }
    let embedded_config = match executable
        .as_ref()
        .map(Executable::read_embedded_config)
        .transpose()
    {
        Ok(config) => config.flatten(),
        Err(error) => {
            eprintln!(
                "{program_name}: failed to read the config compiled into this executable: {error}"
            );
            return ExitCode::FAILURE;
        }
    };

    let result = match embedded_config {
        Some(embedded_config) => run_compiled(&program_name, executable, &embedded_config),
        None => run_command(&program_name, parse(args::Main::command()), executable),
    };

    match result {
        Ok(code) => u8::try_from(code).map_or(ExitCode::FAILURE, ExitCode::from),
        Err(error) => {
            eprintln!("*** Uncaught exception ***\n{error}");
            ExitCode::FAILURE
        }
    }
}

/// Serves the config compiled into this executable.
fn run_compiled(
    program_name: &str,
    executable: Option<Executable>,
    embedded_config: &[u64],
) -> Result<i32, cxx::KjException> {
    type T = args::CompiledMain;

    let args: T = parse(T::command());
    check_serve_or_test::<T>(&args.serve_or_test);
    let config = Config::from_words(embedded_config).unwrap_or_else(|error| {
        usage_error::<T>(format!("the config compiled into this executable: {error}"))
    });
    let watcher = watcher::<T>(&args.serve_or_test, executable.as_ref());
    let (serve, inherited_sockets) = serve_options::<T>(args.serve);
    ffi::run_serve(
        &common_options(program_name.to_owned(), &args.global),
        config.into_words(),
        &args.serve_or_test.into(),
        &serve,
        Box::new(Process::new(executable, watcher, inherited_sockets)),
    )
}

fn run_command(
    program_name: &str,
    args: args::Main,
    executable: Option<Executable>,
) -> Result<i32, cxx::KjException> {
    type T = args::Main;

    let common = common_options(
        format!("{program_name} {}", args.command_name()),
        &args.global,
    );

    match args.command {
        args::Command::Serve {
            config,
            const_name,
            serve_or_test,
            serve,
        } => {
            check_serve_or_test::<T>(&serve_or_test);
            let watcher = watcher::<T>(&serve_or_test, executable.as_ref());
            let (serve, inherited_sockets) = serve_options::<T>(serve);
            let process = Box::new(Process::new(executable, watcher, inherited_sockets));
            let config = load_config::<T>(config, const_name.const_name, &process);
            ffi::run_serve(
                &common,
                config.into_words(),
                &serve_or_test.into(),
                &serve,
                process,
            )
        }
        args::Command::Compile {
            config,
            const_name,
            config_only,
        } => {
            run_compile(config, const_name.const_name, config_only, executable);
            Ok(0)
        }
        args::Command::Fuzzilli {
            config,
            serve_or_test,
        } => {
            if !ffi::fuzzilli_supported() {
                exit_with(&T::command().error(
                    ErrorKind::InvalidSubcommand,
                    "unrecognized subcommand 'fuzzilli'",
                ));
            }
            check_serve_or_test::<T>(&serve_or_test);
            let watcher = watcher::<T>(&serve_or_test, executable.as_ref());
            let process = Box::new(Process::new(executable, watcher, Vec::new()));
            let config = load_config::<T>(config, None, &process);
            ffi::run_test(
                &common,
                config.into_words(),
                &serve_or_test.into(),
                &ffi::TestOptions::new(args::TestArgs::default(), args::TestFilter::default()),
                process,
            )
        }
        args::Command::Test {
            config,
            serve_or_test,
            mut test,
        } => {
            check_serve_or_test::<T>(&serve_or_test);
            let filter = test.filter.take().unwrap_or_default();
            let watcher = watcher::<T>(&serve_or_test, executable.as_ref());
            let process = Box::new(Process::new(executable, watcher, Vec::new()));
            let config = load_config::<T>(config, filter.const_name.clone(), &process);
            ffi::run_test(
                &common,
                config.into_words(),
                &serve_or_test.into(),
                &ffi::TestOptions::new(test, filter),
                process,
            )
        }
        args::Command::PyodideLock => {
            println!("{}", ffi::pyodide_lock()?);
            Ok(0)
        }
        args::Command::MakePyodideBaselineSnapshot {
            python_version,
            output_directory,
        } => run_baseline_snapshot(&common, &python_version, output_directory, executable),
    }
}

/// `workerd compile`: the config, encoded, either on its own (`--config-only`) or appended to a
/// copy of this executable.
fn run_compile(
    config: args::ConfigFileArgs,
    const_name: Option<String>,
    config_only: bool,
    executable: Option<Executable>,
) {
    type T = args::Main;

    if std::io::stdout().is_terminal() {
        eprintln!(
            "Refusing to write binary to the terminal. Please use `>` to send the output to a file."
        );
        std::process::exit(1);
    }
    if !config_only && executable.is_none() {
        usage_error::<T>(
            "Unable to find and open the program's own executable, so cannot produce a new binary \
             with compiled-in config.",
        );
    }
    let process = Process::new(executable, None, Vec::new());
    let config = load_config::<T>(config, const_name, &process);
    let written = if config_only {
        config.write_to_stdout()
    } else {
        process.write_compiled_binary(&config.into_words())
    };
    written.unwrap_or_else(|error| usage_error::<T>(format!("writing to stdout: {error}")));
}

/// Runs a synthesized Python worker's tests with a baseline snapshot saved to `output_directory`.
fn run_baseline_snapshot(
    common: &ffi::CommonOptions,
    python_version: &str,
    output_directory: String,
    executable: Option<Executable>,
) -> Result<i32, cxx::KjException> {
    ffi::run_test(
        common,
        Config::python_baseline(python_version).into_words(),
        &ffi::ServeOrTestOptions {
            experimental: true,
            python_save_baseline_snapshot: true,
            pyodide_bundle_disk_cache_dir: Some(".".to_owned()).into(),
            pyodide_package_disk_cache_dir: Some(output_directory).into(),
            ..args::ServeOrTestArgs::default().into()
        },
        &ffi::TestOptions::new(args::TestArgs::default(), args::TestFilter::default()),
        Box::new(Process::new(executable, None, Vec::new())),
    )
}

/// Parses this process's arguments with `command`, exiting on `--help`, `--version`, and usage
/// errors like `kj::MainBuilder` did: usage errors exit with status 1, and `--version` prints
/// "workerd <version>" from every subcommand.
fn parse<T: FromArgMatches>(command: clap::Command) -> T {
    let version = release_version();
    let result = command
        .version(version)
        .propagate_version(true)
        .try_get_matches()
        .and_then(|matches| T::from_arg_matches(&matches));
    match result {
        Ok(args) => args,
        Err(error) if error.kind() == ErrorKind::DisplayVersion => {
            println!("workerd {version}");
            std::process::exit(0);
        }
        Err(error) => exit_with(&error),
    }
}

/// Prints `error` and exits: with status 1 for usage errors, 0 for `--help`.
fn exit_with(error: &clap::Error) -> ! {
    let _ = error.print();
    std::process::exit(i32::from(error.use_stderr()));
}

/// Reports a usage error found after parsing, e.g. a config file that does not exist.
fn usage_error<T: CommandFactory>(message: impl Display) -> ! {
    exit_with(&T::command().error(ErrorKind::ValueValidation, message))
}

/// The release version, for `--version`; clap holds it for the process's life.
fn release_version() -> &'static str {
    static VERSION: OnceLock<String> = OnceLock::new();
    VERSION.get_or_init(|| String::from_utf8_lossy(ffi::release_version()).into_owned())
}

fn common_options(program_name: String, global: &args::GlobalArgs) -> ffi::CommonOptions {
    ffi::CommonOptions {
        program_name,
        verbose: global.verbose,
    }
}

/// The config to run: a schema file compiled now (the compiler registers every file it reads with
/// the process's watcher before reading it), or an encoded message read from a file or stdin.
/// Exits with a usage error on a problem with the file itself. Parse errors in a schema file are
/// printed; without `--watch` they end the process, with it the process waits for the files to
/// change and reloads.
fn load_config<T: CommandFactory>(
    config: args::ConfigFileArgs,
    const_name: Option<String>,
    process: &Process,
) -> Config {
    let args::ConfigFileArgs {
        import_paths,
        binary,
        config_file,
    } = config;
    for dir in &import_paths {
        if !std::path::Path::new(dir).is_dir() {
            usage_error::<T>(format!("--import-path={dir}: No such directory."));
        }
    }
    if binary {
        // An encoded message has no named constants.
        if let Some(name) = const_name {
            usage_error::<T>(format!(
                "{name}: No such constant is defined in the config file."
            ));
        }
        return if config_file == "-" {
            Config::read_stdin().unwrap_or_else(|error| usage_error::<T>(format!("-: {error}")))
        } else {
            Config::read_file(&config_file)
                .unwrap_or_else(|message| usage_error::<T>(format!("{config_file}: {message}")))
        };
    }
    if config_file == "-" {
        usage_error::<T>("-: Reading config from stdin is only allowed with --binary.");
    }

    let compiled = ffi::compile_config(
        &config_file,
        &import_paths,
        const_name.as_deref().into(),
        process,
    )
    .unwrap_or_else(|error| usage_error::<T>(format!("{config_file}: {error}")));
    if compiled.errors.is_empty() {
        return Config::from_words(&compiled.message)
            .unwrap_or_else(|error| usage_error::<T>(format!("{config_file}: {error}")));
    }

    for error in &compiled.errors {
        eprintln!("{error}");
    }
    let Some(watcher) = process.watcher() else {
        std::process::exit(1);
    };
    // In --watch mode, it's annoying if the server exits and stops watching. Wait for someone to
    // fix the config.
    eprintln!("Can't start server due to config errors, waiting for config files to change...");
    if let Err(error) = watcher.block_until_changes() {
        eprintln!("--watch: {}", error.description());
        std::process::exit(1);
    }
    process.reload()
}

/// Starts watching for `--watch`, exiting with a usage error where it cannot work.
fn watcher<T: CommandFactory>(
    serve_or_test: &args::ServeOrTestArgs,
    executable: Option<&Executable>,
) -> Option<Watcher> {
    if !serve_or_test.watch {
        return None;
    }
    // Reloading re-executes the server, which is not implemented on Windows.
    #[cfg(windows)]
    {
        let _ = executable;
        usage_error::<T>(
            "--watch: File watching is not yet implemented on your OS. Sorry! Pull requests \
             welcome!",
        );
    }
    #[cfg(not(windows))]
    {
        let Some(executable) = executable else {
            usage_error::<T>(
                "--watch: Can't use --watch when we're unable to find our own executable.",
            );
        };
        Some(
            Watcher::new(executable.path())
                .unwrap_or_else(|error| usage_error::<T>(format!("--watch: {error}"))),
        )
    }
}

/// Rejects `--perfetto-trace` on builds without perfetto, and a `--pyodide-package-disk-cache-dir`
/// that does not exist.
fn check_serve_or_test<T: CommandFactory>(serve_or_test: &args::ServeOrTestArgs) {
    if serve_or_test.perfetto_trace.is_some() && !ffi::perfetto_supported() {
        exit_with(&T::command().error(
            ErrorKind::UnknownArgument,
            "--perfetto-trace: perfetto tracing is not supported by this build",
        ));
    }
    if let Some(dir) = &serve_or_test.pyodide_package_disk_cache_dir
        && !std::path::Path::new(dir).is_dir()
    {
        usage_error::<T>(format!(
            "--pyodide-package-disk-cache-dir={dir}: package disk cache dir must exist"
        ));
    }
}

/// The `serve` options for the bridge, taking ownership of every `--socket-fd` socket (the server
/// gets a duplicate). A socket given both an address and a descriptor, or a descriptor that is
/// not a listening socket, is a usage error.
fn serve_options<T: CommandFactory>(
    serve: args::ServeArgs,
) -> (ffi::ServeOptions, Vec<InheritedSocket>) {
    if let Some(name) = args::socket_overridden_twice(&serve) {
        usage_error::<T>(format!(
            "socket '{name}' is given both --socket-addr and --socket-fd; use one"
        ));
    }
    let mut inherited = Vec::with_capacity(serve.socket_fd_overrides.len());
    let mut socket_fd_overrides = Vec::with_capacity(serve.socket_fd_overrides.len());
    for args::SocketFd { name, fd } in serve.socket_fd_overrides {
        let socket = InheritedSocket::take(fd).unwrap_or_else(|error| {
            let message = match error {
                socket_fd::Error::NotListening => {
                    format!("Socket for {name} is not listening.")
                }
                error => error.to_string(),
            };
            usage_error::<T>(format!("--socket-fd={name}={fd}: {message}"))
        });
        let fd = socket
            .duplicate_for_server()
            .unwrap_or_else(|error| usage_error::<T>(format!("--socket-fd={name}={fd}: {error}")));
        socket_fd_overrides.push(ffi::SocketFd { name, fd });
        inherited.push(socket);
    }
    let options = ffi::ServeOptions {
        socket_addr_overrides: serve
            .socket_addr_overrides
            .into_iter()
            .map(Into::into)
            .collect(),
        socket_fd_overrides,
        control_fd: serve.control_fd.into(),
        debug_port: serve.debug_port.into(),
    };
    (options, inherited)
}

impl args::Main {
    /// The subcommand's name as typed, e.g. "serve".
    const fn command_name(&self) -> &'static str {
        match self.command {
            args::Command::Serve { .. } => "serve",
            args::Command::Compile { .. } => "compile",
            args::Command::Fuzzilli { .. } => "fuzzilli",
            args::Command::Test { .. } => "test",
            args::Command::PyodideLock => "pyodide-lock",
            args::Command::MakePyodideBaselineSnapshot { .. } => "make-pyodide-baseline-snapshot",
        }
    }
}
