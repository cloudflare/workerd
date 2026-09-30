//! The command line, as clap definitions.
//!
//! Every option handler applied in argument order under `kj::MainBuilder`; here options are parsed
//! up front and the C++ driver applies options before positional arguments, which is the order
//! they depended on (e.g. `--watch` and `--binary` must take effect before `<config-file>` is
//! parsed).

use clap::Args;
use clap::Parser;
use clap::Subcommand;

use crate::bridge::ffi;

/// Runs the Workers JavaScript/Wasm runtime.
#[derive(Parser, Debug)]
#[command(name = "workerd", args_override_self = true)]
pub struct Main {
    #[command(flatten)]
    pub global: GlobalArgs,

    #[command(subcommand)]
    pub command: Command,
}

/// Serve requests based on the compiled config.
#[derive(Parser, Debug)]
#[command(
    name = "workerd",
    args_override_self = true,
    after_help = "This binary has an embedded configuration."
)]
pub struct CompiledMain {
    #[command(flatten)]
    pub global: GlobalArgs,

    #[command(flatten)]
    pub serve_or_test: ServeOrTestArgs,

    #[command(flatten)]
    pub serve: ServeArgs,
}

#[derive(Args, Debug)]
pub struct GlobalArgs {
    /// Log informational messages to stderr; useful for debugging.
    #[arg(long, global = true)]
    pub verbose: bool,
}

#[derive(Subcommand, Debug)]
pub enum Command {
    /// run the server
    #[command(
        long_about = "Serve requests based on a config.",
        after_long_help = "Serves requests based on the configuration specified in <config-file>."
    )]
    Serve {
        #[command(flatten)]
        config: ConfigFileArgs,

        #[command(flatten)]
        const_name: ConstNameArg,

        #[command(flatten)]
        serve_or_test: ServeOrTestArgs,

        #[command(flatten)]
        serve: ServeArgs,
    },

    /// create a self-contained binary
    #[command(
        long_about = "Builds a self-contained binary from a config.",
        after_long_help = "This parses a config file in the same manner as the \"serve\" command, \
            but instead of then running it, it outputs a new binary to stdout that embeds the \
            config and all associated Worker code and data as one self-contained unit. This \
            binary may then be executed on another system to run the config -- without any other \
            files being present on that system."
    )]
    Compile {
        #[command(flatten)]
        config: ConfigFileArgs,

        #[command(flatten)]
        const_name: ConstNameArg,

        /// Only write the encoded binary config to stdout. Do not attach it to an executable. The
        /// encoded config can be used as input to the "serve" command, without the need for any
        /// other files to be present.
        #[arg(long)]
        config_only: bool,
    },

    /// run reprl for fuzzing
    #[command(
        long_about = "Creates a custom signal handler and depending on the config leverages \
            Stdin.reprl() to communicate with fuzzilli.",
        hide = !ffi::fuzzilli_supported()
    )]
    Fuzzilli {
        #[command(flatten)]
        config: ConfigFileArgs,

        #[command(flatten)]
        serve_or_test: ServeOrTestArgs,
    },

    /// run unit tests
    #[command(
        long_about = "Runs tests based on a config.",
        after_long_help = TEST_HELP,
    )]
    Test {
        #[command(flatten)]
        config: ConfigFileArgs,

        #[command(flatten)]
        serve_or_test: ServeOrTestArgs,

        #[command(flatten)]
        test: TestArgs,
    },

    /// outputs the package lock file used by Pyodide
    #[command(long_about = "Outputs the package lock file used by Pyodide.")]
    PyodideLock,

    /// Make a Pyodide baseline memory snapshot
    MakePyodideBaselineSnapshot {
        #[arg(value_name = "python-version")]
        python_version: String,

        #[arg(value_name = "output-directory")]
        output_directory: String,
    },
}

const TEST_HELP: &str = "\
Runs tests for services defined in <config-file>. <filter>, if given, specifies exactly which \
tests to run. It has one of the following formats:
    <service-pattern>
    <service-pattern>:<entrypoint-pattern>
    <const-name>:<service-pattern>:<entrypoint-pattern>
<service-pattern> is a glob pattern matching names of services which should be tested. If not \
specified, '*' is assumed (which matches all services). <entrypoint-pattern> is a glob pattern \
matching entrypoints within each service which should be tested; again, the default is '*'. \
<const-name> has the same meaning as for the `serve` command (this is rarely used).

Tests can be defined by exporting a function called `test` instead of (or in addition to) \
`fetch`. Example:
    export default {
      async test(ctrl, env, ctx) {
        if (1 + 1 != 2) {
          throw new Error('math is broken!');
        }
      }
    }
The test passes if the test function completes without throwing. Multiple tests can be exported \
under different entrypoint names:
    export let test1 = {
      async test(ctrl, env, ctx) {
        ...
      }
    }
    export let test2 = {
      async test(ctrl, env, ctx) {
        ...
      }
    }
";

#[derive(Args, Debug)]
pub struct ConfigFileArgs {
    /// Add <dir> to the list of directories searched for non-relative imports in the config file
    /// (ones that start with a '/').
    #[arg(short = 'I', long = "import-path", value_name = "dir")]
    pub import_paths: Vec<String>,

    /// Specifies that the configuration file is an encoded binary Cap'n Proto message, rather than
    /// the usual text format. This is particularly useful when driving the server from
    /// higher-level tooling that automatically generates a config.
    #[arg(short, long)]
    pub binary: bool,

    #[arg(value_name = "config-file")]
    pub config_file: String,
}

#[derive(Args, Debug)]
pub struct ConstNameArg {
    #[arg(value_name = "const-name")]
    pub const_name: Option<String>,
}

#[derive(Args, Debug, Default)]
#[expect(
    clippy::struct_excessive_bools,
    reason = "one bool per command-line flag"
)]
pub struct ServeOrTestArgs {
    /// Override the directory named <name> to point to <path> instead of the path specified in the
    /// config file.
    #[arg(short = 'd', long = "directory-path", value_name = "name>=<path", value_parser = parse_override)]
    pub directory_overrides: Vec<Override>,

    /// Override the external service named <name> to connect to the address <addr> instead of the
    /// address specified in the config file.
    #[arg(short = 'e', long = "external-addr", value_name = "name>=<addr", value_parser = parse_override)]
    pub external_overrides: Vec<Override>,

    /// Enable the inspector protocol to connect to the address <addr>.
    #[arg(short = 'i', long, value_name = "addr")]
    pub inspector_addr: Option<String>,

    /// Enable perfetto tracing output to the specified file.
    #[arg(
        long,
        value_name = "path>=<categories",
        value_parser = parse_override,
        hide = !ffi::perfetto_supported()
    )]
    pub perfetto_trace: Option<Override>,

    /// Watch configuration files (and server binary) and reload if they change. Useful for
    /// development, but not recommended in production.
    #[arg(short, long)]
    pub watch: bool,

    /// Permit the use of experimental features which may break backwards compatibility in a
    /// future release.
    #[arg(long)]
    pub experimental: bool,

    /// Use <path> as a disk cache to avoid repeatedly fetching packages from the internet.
    #[arg(long, value_name = "path")]
    pub pyodide_package_disk_cache_dir: Option<String>,

    /// Use <path> as a disk cache to avoid repeatedly fetching Pyodide bundles from the internet.
    #[arg(long, value_name = "path")]
    pub pyodide_bundle_disk_cache_dir: Option<String>,

    /// Save a dedicated snapshot to the disk cache
    #[arg(long)]
    pub python_save_snapshot: bool,

    /// Save a baseline snapshot to the disk cache
    #[arg(long)]
    pub python_save_baseline_snapshot: bool,

    /// Load a snapshot from the python snapshot directory.
    #[arg(long, value_name = "path")]
    pub python_load_snapshot: Option<String>,

    /// Set the snapshot snapshot directory.
    #[arg(long, value_name = "path")]
    pub python_snapshot_dir: Option<String>,
}

#[derive(Args, Debug)]
pub struct ServeArgs {
    /// Override the socket named <name> to bind to the address <addr> instead of the address
    /// specified in the config file.
    #[arg(short = 's', long = "socket-addr", value_name = "name>=<addr", value_parser = parse_override)]
    pub socket_addr_overrides: Vec<Override>,

    /// Override the socket named <name> to listen on the already-open socket descriptor <fd>
    /// instead of the address specified in the config file.
    #[arg(short = 'S', long = "socket-fd", value_name = "name>=<fd", value_parser = parse_socket_fd)]
    pub socket_fd_overrides: Vec<SocketFd>,

    /// Enable sending of control messages on descriptor <fd>. Currently this only reports the port
    /// each socket is listening on when ready.
    #[arg(long, value_name = "fd", value_parser = parse_control_fd)]
    pub control_fd: Option<u32>,

    /// Listen on the specified address for debug RPC connections. This exposes a privileged
    /// interface that allows access to all services in the process. For use by miniflare and local
    /// development only.
    #[arg(long, value_name = "addr")]
    pub debug_port: Option<String>,
}

#[derive(Args, Debug, Default)]
#[expect(
    clippy::struct_excessive_bools,
    reason = "one bool per command-line flag"
)]
pub struct TestArgs {
    /// Disable INFO-level logging for this test. Otherwise, INFO logging is enabled by default for
    /// tests in order to show uncaught exceptions, but it can be noisey.
    #[arg(long)]
    pub no_verbose: bool,

    /// Enable predictable mode. This makes workerd behave more deterministically by using pre-set
    /// values instead of random data or timestamps to facilitate testing.
    #[arg(long)]
    pub predictable: bool,

    /// Force a full V8 GC at each awaitIo continuation. Detects KJ async objects on the JS heap
    /// without `IoOwn` wrapping. Very slow.
    #[arg(long)]
    pub gc_stress: bool,

    /// Enable all autogates. This is useful for testing code paths that are guarded by autogates.
    #[arg(long)]
    pub all_autogates: bool,

    /// Set the compatibility date for all workers. When specified, workers must NOT specify
    /// compatibilityDate in the config. Use '0000-00-00' for oldest behavior or '9999-12-31' for
    /// newest behavior.
    #[arg(long, value_name = "date")]
    pub compat_date: Option<String>,

    #[arg(value_name = "filter", value_parser = parse_test_filter)]
    pub filter: Option<TestFilter>,
}

/// `<filter>`: `<service-pattern>`, `<service-pattern>:<entrypoint-pattern>`, or
/// `<const-name>:<service-pattern>:<entrypoint-pattern>`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct TestFilter {
    pub const_name: Option<String>,
    pub service_pattern: Option<String>,
    pub entrypoint_pattern: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Override {
    pub name: String,
    pub value: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SocketFd {
    pub name: String,
    pub fd: u32,
}

fn parse_override(param: &str) -> Result<Override, &'static str> {
    let (name, value) = param.split_once('=').ok_or("Expected <name>=<value>")?;
    Ok(Override {
        name: name.to_owned(),
        value: value.to_owned(),
    })
}

fn parse_socket_fd(param: &str) -> Result<SocketFd, &'static str> {
    let Override { name, value } = parse_override(param)?;
    let fd = value
        .parse()
        .map_err(|_| "Socket value must be a file descriptor (non-negative integer).")?;
    Ok(SocketFd { name, fd })
}

fn parse_test_filter(param: &str) -> Result<TestFilter, &'static str> {
    let parts: Vec<&str> = param.split(':').collect();
    let owned = |part: &str| Some(part.to_owned());
    Ok(match parts.as_slice() {
        [service] => TestFilter {
            service_pattern: owned(service),
            ..TestFilter::default()
        },
        [service, entrypoint] => TestFilter {
            service_pattern: owned(service),
            entrypoint_pattern: owned(entrypoint),
            ..TestFilter::default()
        },
        [const_name, service, entrypoint] => TestFilter {
            const_name: owned(const_name),
            service_pattern: owned(service),
            entrypoint_pattern: owned(entrypoint),
        },
        _ => return Err("Too many colons."),
    })
}

fn parse_control_fd(param: &str) -> Result<u32, &'static str> {
    param
        .parse()
        .map_err(|_| "Output value must be a file descriptor (non-negative integer).")
}

impl From<Override> for ffi::Override {
    fn from(Override { name, value }: Override) -> Self {
        Self { name, value }
    }
}

impl From<ServeOrTestArgs> for ffi::ServeOrTestOptions {
    fn from(args: ServeOrTestArgs) -> Self {
        let (perfetto_trace_path, perfetto_trace_categories) = args
            .perfetto_trace
            .map(|Override { name, value }| (name, value))
            .unzip();
        Self {
            directory_overrides: args
                .directory_overrides
                .into_iter()
                .map(Into::into)
                .collect(),
            external_overrides: args
                .external_overrides
                .into_iter()
                .map(Into::into)
                .collect(),
            inspector_addr: args.inspector_addr.into(),
            perfetto_trace_path: perfetto_trace_path.into(),
            perfetto_trace_categories: perfetto_trace_categories.into(),
            experimental: args.experimental,
            pyodide_package_disk_cache_dir: args.pyodide_package_disk_cache_dir.into(),
            pyodide_bundle_disk_cache_dir: args.pyodide_bundle_disk_cache_dir.into(),
            python_save_snapshot: args.python_save_snapshot,
            python_save_baseline_snapshot: args.python_save_baseline_snapshot,
            python_load_snapshot: args.python_load_snapshot.into(),
            python_snapshot_dir: args.python_snapshot_dir.into(),
        }
    }
}

/// The name of a socket given both `--socket-addr` and `--socket-fd`, if any. Each is applied as
/// a whole, so there is no meaningful "last one wins" between the two kinds.
pub fn socket_overridden_twice(serve: &ServeArgs) -> Option<&str> {
    serve
        .socket_fd_overrides
        .iter()
        .map(|fd| fd.name.as_str())
        .find(|name| {
            serve
                .socket_addr_overrides
                .iter()
                .any(|addr| addr.name == *name)
        })
}

impl ffi::TestOptions {
    /// The options for a test run, with the service and entrypoint patterns from `filter`.
    pub fn new(args: TestArgs, filter: TestFilter) -> Self {
        Self {
            no_verbose: args.no_verbose,
            predictable: args.predictable,
            gc_stress: args.gc_stress,
            all_autogates: args.all_autogates,
            compat_date: args.compat_date.into(),
            service_pattern: filter.service_pattern.into(),
            entrypoint_pattern: filter.entrypoint_pattern.into(),
        }
    }
}

#[cfg(test)]
mod tests {
    use clap::CommandFactory;

    use super::*;

    fn parse(args: &[&str]) -> Main {
        Main::try_parse_from(std::iter::once("workerd").chain(args.iter().copied())).unwrap()
    }

    #[test]
    fn definitions_are_consistent() {
        Main::command().debug_assert();
        CompiledMain::command().debug_assert();
    }

    #[test]
    fn wd_test_invocation() {
        let Command::Test {
            config,
            serve_or_test,
            test,
        } = parse(&[
            "test",
            "--predictable",
            "foo.wd-test",
            "--compat-date=2000-01-01",
            "-dTEST_TMPDIR=/tmp/x",
        ])
        .command
        else {
            panic!("expected test")
        };
        assert_eq!(config.config_file, "foo.wd-test");
        assert!(test.predictable);
        assert_eq!(test.compat_date.as_deref(), Some("2000-01-01"));
        assert_eq!(test.filter, None);
        assert_eq!(
            serve_or_test.directory_overrides,
            [Override {
                name: "TEST_TMPDIR".into(),
                value: "/tmp/x".into()
            }]
        );
    }

    #[test]
    fn serve_options() {
        let Command::Serve {
            config,
            const_name,
            serve_or_test,
            serve,
        } = parse(&[
            "serve",
            "--binary",
            "-",
            "-I",
            "inc",
            "-wb",
            "--experimental",
            "--socket-addr",
            "http=*:8080",
            "--control-fd=3",
            "--inspector-addr=127.0.0.1:9229",
            "--inspector-addr=127.0.0.1:9230",
        ])
        .command
        else {
            panic!("expected serve")
        };
        assert_eq!(config.config_file, "-");
        assert!(config.binary);
        assert_eq!(config.import_paths, ["inc"]);
        assert_eq!(const_name.const_name, None);
        assert!(serve_or_test.watch);
        assert!(serve_or_test.experimental);
        // Later occurrences override earlier ones, as with kj::MainBuilder.
        assert_eq!(
            serve_or_test.inspector_addr.as_deref(),
            Some("127.0.0.1:9230")
        );
        assert_eq!(serve.control_fd, Some(3));
        assert_eq!(
            serve.socket_addr_overrides,
            [Override {
                name: "http".into(),
                value: "*:8080".into()
            }]
        );
    }

    #[test]
    fn serve_const_name_and_verbose_after_subcommand() {
        let main = parse(&["serve", "config.capnp", "myConfig", "--verbose"]);
        assert!(main.global.verbose);
        let Command::Serve { const_name, .. } = main.command else {
            panic!("expected serve")
        };
        assert_eq!(const_name.const_name.as_deref(), Some("myConfig"));
    }

    #[test]
    fn test_filters() {
        let filter = |param| parse_test_filter(param);
        assert_eq!(
            filter("svc"),
            Ok(TestFilter {
                service_pattern: Some("svc".into()),
                ..TestFilter::default()
            })
        );
        assert_eq!(
            filter("svc:ep"),
            Ok(TestFilter {
                service_pattern: Some("svc".into()),
                entrypoint_pattern: Some("ep".into()),
                ..TestFilter::default()
            })
        );
        assert_eq!(
            filter("cfg:svc:"),
            Ok(TestFilter {
                const_name: Some("cfg".into()),
                service_pattern: Some("svc".into()),
                entrypoint_pattern: Some(String::new()),
            })
        );
        assert_eq!(filter("a:b:c:d"), Err("Too many colons."));
    }

    #[test]
    fn bad_values_are_usage_errors() {
        for args in [
            &["serve", "c.capnp", "--socket-addr=noequals"][..],
            &["serve", "c.capnp", "--socket-fd=http=abc"],
            &["serve", "c.capnp", "--control-fd=-1"],
            &["serve"],
            &["test", "c.capnp", "a", "b"],
            &["unknown"],
        ] {
            let argv = std::iter::once("workerd").chain(args.iter().copied());
            assert!(Main::try_parse_from(argv).is_err(), "{args:?}");
        }
    }

    #[test]
    fn socket_overridden_twice_finds_the_conflict() {
        let Command::Serve { serve, .. } =
            parse(&["serve", "c.capnp", "-shttp=*:8080", "-Sadmin=3", "-Shttp=4"]).command
        else {
            panic!("expected serve")
        };
        assert_eq!(socket_overridden_twice(&serve), Some("http"));
        let Command::Serve { serve, .. } =
            parse(&["serve", "c.capnp", "-shttp=*:8080", "-Sadmin=3"]).command
        else {
            panic!("expected serve")
        };
        assert_eq!(socket_overridden_twice(&serve), None);
    }

    #[test]
    fn compiled_binary_has_no_config_arguments() {
        let main = CompiledMain::try_parse_from(["workerd", "-s", "http=*:8080"]).unwrap();
        assert_eq!(main.serve.socket_addr_overrides.len(), 1);
        assert!(CompiledMain::try_parse_from(["workerd", "config.capnp"]).is_err());
    }
}
