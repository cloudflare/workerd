//! The command line, as clap definitions. The help pages are written by hand, in `help.rs`.
//!
//! Every option handler applied in argument order under `kj::MainBuilder`; here options are parsed
//! up front and the C++ driver applies options before positional arguments, which is the order
//! they depended on (e.g. `--watch` and `--binary` must take effect before `<config>` is
//! parsed).

use std::time::Duration;

use clap::Args;
use clap::Parser;
use clap::Subcommand;
use clap::ValueEnum;
use workerd_server::bench;
use workerd_server::bridge::ffi as server;

use crate::help;

#[derive(Parser, Debug)]
#[command(name = "workerd", args_override_self = true, override_help = help::MAIN)]
pub struct Main {
    #[command(flatten)]
    pub global: GlobalArgs,

    #[command(subcommand)]
    pub command: Command,
}

#[derive(Parser, Debug)]
#[command(name = "workerd", args_override_self = true, override_help = help::COMPILED)]
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
    #[arg(long, global = true)]
    pub verbose: bool,
}

#[derive(Subcommand, Debug)]
pub enum Command {
    #[command(override_help = help::SERVE)]
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

    #[command(override_help = help::COMPILE)]
    Compile {
        #[command(flatten)]
        config: ConfigFileArgs,

        #[command(flatten)]
        const_name: ConstNameArg,

        #[arg(long)]
        config_only: bool,
    },

    #[command(override_help = help::FUZZILLI, hide = true)]
    Fuzzilli {
        #[command(flatten)]
        config: ConfigFileArgs,

        #[command(flatten)]
        serve_or_test: ServeOrTestArgs,
    },

    #[command(override_help = help::TEST)]
    Test {
        #[command(flatten)]
        config: ConfigFileArgs,

        #[command(flatten)]
        serve_or_test: ServeOrTestArgs,

        #[command(flatten)]
        test: TestArgs,
    },

    #[command(override_help = help::BENCH)]
    Bench {
        #[command(flatten)]
        config: ConfigFileArgs,

        #[command(flatten)]
        serve_or_test: ServeOrTestArgs,

        #[command(flatten)]
        bench: BenchArgs,
    },

    #[command(override_help = help::DEPLOY, hide = true)]
    Deploy {
        #[arg(value_name = "project", default_value = ".")]
        project: String,

        #[arg(value_name = "const")]
        const_name: Option<String>,

        #[command(flatten)]
        deploy: DeployArgs,
    },

    #[command(override_help = help::REPL)]
    Repl {
        #[command(flatten)]
        repl: ReplArgs,
    },
}

#[derive(Args, Debug)]
pub struct ConfigFileArgs {
    #[arg(short = 'I', long = "import-path", value_name = "dir")]
    pub import_paths: Vec<String>,

    #[arg(short, long)]
    pub binary: bool,

    #[arg(value_name = "config")]
    pub config_file: String,
}

#[derive(Args, Debug)]
pub struct ConstNameArg {
    #[arg(value_name = "const")]
    pub const_name: Option<String>,
}

#[derive(Args, Debug, Default)]
#[expect(
    clippy::struct_excessive_bools,
    reason = "one bool per command-line flag"
)]
pub struct ServeOrTestArgs {
    #[arg(short = 'd', long = "directory-path", value_name = "name>=<dir", value_parser = parse_override)]
    pub directory_overrides: Vec<Override>,

    #[arg(long, value_name = "dir")]
    pub data_dir: Option<String>,

    #[arg(short = 'e', long = "external-addr", value_name = "name>=<addr", value_parser = parse_override)]
    pub external_overrides: Vec<Override>,

    #[arg(short = 'i', long, value_name = "addr")]
    pub inspector_addr: Option<String>,

    #[arg(
        long,
        value_name = "path>=<categories",
        value_parser = parse_override
    )]
    pub perfetto_trace: Option<Override>,

    #[arg(long, value_name = "path")]
    pub async_trace: Option<String>,

    #[arg(long, value_name = "n", value_parser = clap::value_parser!(u32).range(1..=64))]
    pub async_trace_stacks: Option<u32>,

    #[arg(long)]
    pub async_trace_promises: bool,

    #[arg(short, long)]
    pub watch: bool,

    #[arg(long)]
    pub experimental: bool,

    #[arg(long, value_name = "path")]
    pub pyodide_bundle_disk_cache_dir: Option<String>,

    #[arg(long)]
    pub python_save_snapshot: bool,

    #[arg(long)]
    pub python_save_baseline_snapshot: bool,

    #[arg(long, value_name = "path")]
    pub python_load_snapshot: Option<String>,

    #[arg(long, value_name = "path")]
    pub python_snapshot_dir: Option<String>,
}

#[derive(Args, Debug)]
pub struct ServeArgs {
    #[arg(short = 's', long = "socket-addr", value_name = "name>=<addr", value_parser = parse_override)]
    pub socket_addr_overrides: Vec<Override>,

    #[arg(short = 'S', long = "socket-fd", value_name = "name>=<fd", value_parser = parse_socket_fd)]
    pub socket_fd_overrides: Vec<SocketFd>,

    #[arg(long, value_name = "fd", value_parser = parse_control_fd)]
    pub control_fd: Option<u32>,

    #[arg(long = "cluster-addr", value_name = "addr")]
    pub cluster_address: Option<String>,

    #[arg(long = "cluster-advertise", value_name = "addr")]
    pub cluster_advertise: Option<String>,

    #[arg(long, value_name = "addr")]
    pub debug_port: Option<String>,

    /// Serve on <N> event-loop threads; more than one requires --experimental.
    ///
    /// The threads take requests from one queue, each when it has nothing else to run, so a
    /// request is served by a thread that is free, whichever connection it came on; a separate front
    /// (--front-threads) accepts the connections of the config's HTTP, HTTPS and TCP sockets and
    /// speaks TLS and HTTP for them. Every thread runs its own copy of every
    /// Worker: a Worker's global scope runs once per thread, its global state is per thread, and
    /// a service binding reaches the Worker of the same thread. Each thread loads its own dynamic
    /// Workers too, so threads can run different code under one name if a Worker Loader's
    /// getCode callback returns different code for it. Durable Objects all run on one
    /// thread, where the other threads call them, and a cluster instance is a member of its
    /// cluster on that thread. A Durable Object does not see the trace span of a caller on
    /// another thread. --inspector-addr and the options that save a Python snapshot are not
    /// supported with more than one thread. UDP sockets, cron triggers and --debug-port are
    /// served by one thread only.
    #[arg(
        long,
        value_name = "N",
        default_value_t = 1,
        value_parser = clap::value_parser!(u32).range(1..=MAX_THREADS)
    )]
    pub threads: u32,

    /// With --threads above one: run the front that accepts connections for the event-loop
    /// threads on <N> threads of its own (default: 2).
    ///
    /// The front accepts from the config's HTTP, HTTPS and TCP sockets, performs the TLS
    /// handshakes and reads and writes HTTP; the event-loop threads run the Workers. It needs
    /// more threads only when it, not the Workers, is what limits throughput.
    #[arg(
        long,
        value_name = "N",
        value_parser = clap::value_parser!(u32).range(1..=MAX_THREADS)
    )]
    pub front_threads: Option<u32>,

    /// With --threads above one: let <N> requests wait for an event-loop thread before the
    /// front stops taking more (default: 256).
    ///
    /// A request, or a connection of a TCP socket, waits in the front's queue until a thread
    /// has nothing else to run. When that many wait, the front reads no further requests from
    /// its connections and accepts no further connections until a thread takes one, so clients
    /// wait in the listen backlog and behind TCP flow control instead of in the server's
    /// memory. No request is refused or answered with an error for it.
    #[arg(
        long,
        value_name = "N",
        value_parser = clap::value_parser!(u32).range(1..=MAX_FRONT_QUEUE)
    )]
    pub front_queue: Option<u32>,
}

/// The most `--front-queue`: more would not be a limit on anything a process can hold.
const MAX_FRONT_QUEUE: i64 = 1 << 20;

/// The most `--threads`: every thread compiles its own copy of every Worker, so more than a
/// machine has cores only costs memory.
const MAX_THREADS: i64 = 256;

#[derive(Args, Debug)]
pub struct DeployArgs {
    #[arg(long, value_name = "name")]
    pub name: Option<String>,

    #[arg(long, value_name = "url", value_parser = parse_bucket)]
    pub bucket: String,

    #[arg(long, value_name = "url", value_parser = parse_endpoint)]
    pub endpoint: Option<String>,

    #[arg(long, value_name = "region")]
    pub region: Option<String>,

    #[arg(short = 'I', long = "import-path", value_name = "dir")]
    pub import_paths: Vec<String>,

    #[arg(short, long)]
    pub binary: bool,

    #[arg(long)]
    pub experimental: bool,
}

#[derive(Args, Debug)]
pub struct ReplArgs {
    #[arg(value_name = "config")]
    pub config_file: Option<String>,

    #[arg(value_name = "const", requires = "config_file")]
    pub const_name: Option<String>,

    #[arg(long, value_name = "date")]
    pub compat_date: Option<String>,

    #[arg(long = "compat-flag", value_name = "flag")]
    pub compat_flags: Vec<String>,

    #[arg(long = "autogate", value_name = "name")]
    pub autogates: Vec<String>,

    #[arg(long = "v8-flag", value_name = "flag", allow_hyphen_values = true)]
    pub v8_flags: Vec<String>,

    #[arg(long, value_name = "seconds", value_parser = parse_seconds)]
    pub input_timeout: Option<Duration>,

    #[arg(short = 'i', long)]
    pub interactive: bool,

    #[arg(long)]
    pub experimental: bool,
}

impl DeployArgs {
    /// Build the bucket options used by the instances' `cluster.bucketOptions` setting. A gs://
    /// bucket takes none, so it refuses `--endpoint` and `--region`.
    pub fn bucket_options(&self) -> Result<Vec<String>, String> {
        if self.bucket.starts_with("gs://") {
            let set = [
                ("--endpoint", self.endpoint.is_some()),
                ("--region", self.region.is_some()),
            ];
            if let Some((flag, _)) = set.iter().find(|(_, set)| *set) {
                return Err(format!(
                    "{flag} is for an s3:// bucket. Remove it for the gs:// bucket."
                ));
            }
        }
        let endpoint = self.endpoint.iter().map(|e| format!("aws_endpoint={e}"));
        let region = self.region.iter().map(|r| format!("aws_region={r}"));
        Ok(endpoint.chain(region).collect())
    }
}

/// Parse a positive number of seconds, such as `30` or `0.5`.
fn parse_seconds(param: &str) -> Result<Duration, &'static str> {
    let expected = "expected a positive number of seconds, such as 30 or 0.5";
    let seconds: f64 = param.parse().map_err(|_| expected)?;
    match Duration::try_from_secs_f64(seconds) {
        Ok(duration) if !duration.is_zero() => Ok(duration),
        _ => Err(expected),
    }
}

/// Parse `--bucket` as `s3://<bucket>[/<prefix>]` or `gs://<bucket>[/<prefix>]`. The bucket
/// client uses the same format for `cluster.bucket`.
fn parse_bucket(param: &str) -> Result<String, &'static str> {
    let rest = param
        .strip_prefix("s3://")
        .or_else(|| param.strip_prefix("gs://"))
        .ok_or("expected s3://<bucket>[/<prefix>] or gs://<bucket>[/<prefix>]")?;
    let (bucket, _) = rest.split_once('/').unwrap_or((rest, ""));
    if bucket.is_empty() || bucket.contains(char::is_whitespace) {
        return Err(
            "expected a bucket name in s3://<bucket>[/<prefix>] or gs://<bucket>[/<prefix>]",
        );
    }
    Ok(param.to_owned())
}

/// Parse `--endpoint` as an HTTP or HTTPS URL with a host and no path. The bucket client signs
/// request paths that start with the bucket name.
fn parse_endpoint(param: &str) -> Result<String, &'static str> {
    const EXPECTED: &str =
        "expected http://<host>[:<port>] or https://<host>[:<port>], with no path";
    let rest = param
        .strip_prefix("https://")
        .or_else(|| param.strip_prefix("http://"))
        .ok_or(EXPECTED)?;
    let (authority, path) = rest
        .find(['/', '?', '#'])
        .map_or((rest, ""), |at| rest.split_at(at));
    if authority.is_empty()
        || authority.contains(|c: char| c.is_whitespace() || c == '@')
        || !matches!(path, "" | "/")
    {
        return Err(EXPECTED);
    }
    Ok(param.to_owned())
}

#[derive(Args, Debug, Default)]
#[expect(
    clippy::struct_excessive_bools,
    reason = "one bool per command-line flag"
)]
pub struct TestArgs {
    #[arg(long)]
    pub no_verbose: bool,

    #[arg(long)]
    pub predictable: bool,

    #[arg(long)]
    pub gc_stress: bool,

    #[arg(long)]
    pub all_autogates: bool,

    #[arg(long, value_name = "date")]
    pub compat_date: Option<String>,

    #[arg(short = 's', long = "socket-addr", value_name = "name>=<addr", value_parser = parse_override)]
    pub socket_addr_overrides: Vec<Override>,

    #[arg(value_name = "filter", value_parser = parse_test_filter)]
    pub filter: Option<TestFilter>,
}

#[derive(Args, Debug)]
#[expect(
    clippy::struct_excessive_bools,
    reason = "one bool per command-line flag"
)]
pub struct BenchArgs {
    #[arg(long)]
    pub no_verbose: bool,

    #[arg(long)]
    pub all_autogates: bool,

    #[arg(long, value_name = "date")]
    pub compat_date: Option<String>,

    #[arg(long)]
    pub quick: bool,

    #[arg(long, value_enum, default_value_t = BenchFormat::Text)]
    pub format: BenchFormat,

    #[arg(long, value_name = "path")]
    pub output: Option<String>,

    #[arg(long)]
    pub trace: bool,

    #[arg(long)]
    pub no_startup: bool,

    #[arg(value_name = "filter", value_parser = parse_bench_filter)]
    pub filter: Option<BenchFilter>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, ValueEnum)]
pub enum BenchFormat {
    Text,
    Json,
}

/// `workerd bench`'s `<filter>`: `test`'s `<filter>` and `/<case-pattern>`, each optional.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct BenchFilter {
    pub test: TestFilter,
    pub case_pattern: Option<String>,
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

fn parse_bench_filter(param: &str) -> Result<BenchFilter, &'static str> {
    let (rest, case_pattern) = match param.split_once('/') {
        Some((rest, case_pattern)) => (rest, Some(case_pattern.to_owned())),
        None => (param, None),
    };
    let test = if rest.is_empty() {
        TestFilter::default()
    } else {
        parse_test_filter(rest)?
    };
    Ok(BenchFilter { test, case_pattern })
}

fn parse_control_fd(param: &str) -> Result<u32, &'static str> {
    param
        .parse()
        .map_err(|_| "Output value must be a file descriptor (non-negative integer).")
}

/// `<name>=<value>` options as a map, last value per name winning.
pub fn override_map(overrides: Vec<Override>) -> std::collections::HashMap<String, String> {
    overrides
        .into_iter()
        .map(|Override { name, value }| (name, value))
        .collect()
}

impl From<&ServeOrTestArgs> for server::ServeOrTestOptions {
    fn from(args: &ServeOrTestArgs) -> Self {
        let (perfetto_trace_path, perfetto_trace_categories) = args
            .perfetto_trace
            .clone()
            .map(|Override { name, value }| (name, value))
            .unzip();
        Self {
            perfetto_trace_path: perfetto_trace_path.into(),
            perfetto_trace_categories: perfetto_trace_categories.into(),
            async_trace_path: args.async_trace.clone().into(),
            async_trace_stacks: args.async_trace_stacks.into(),
            async_trace_promises: args.async_trace_promises,
            experimental: args.experimental,
            pyodide_bundle_disk_cache_dir: args.pyodide_bundle_disk_cache_dir.clone().into(),
            python_save_snapshot: args.python_save_snapshot,
            python_save_baseline_snapshot: args.python_save_baseline_snapshot,
            python_load_snapshot: args.python_load_snapshot.clone().into(),
            python_snapshot_dir: args.python_snapshot_dir.clone().into(),
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

/// Why `serve` cannot run on the `--threads` asked for with the other options given, if it
/// cannot. The server reports the inspector as a config error, too
/// (`workerd_server::threads::obstacles`).
pub fn threads_refused(serve: &ServeArgs, serve_or_test: &ServeOrTestArgs) -> Option<&'static str> {
    if serve.threads <= 1 {
        // One thread accepts its connections itself.
        if serve.front_threads.is_some() {
            return Some(
                "--front-threads: There is a front only with more than one thread (--threads).",
            );
        }
        return serve.front_queue.map(
            |_| "--front-queue: There is a front only with more than one thread (--threads).",
        );
    }
    if !serve_or_test.experimental {
        return Some(
            "--threads: Serving on more than one thread is an experimental feature which may \
             change or go away in the future. You must run workerd with `--experimental` to use \
             this feature.",
        );
    }
    if serve_or_test.inspector_addr.is_some() {
        return Some("--threads: --inspector-addr is not supported with more than one thread.");
    }
    if serve_or_test.python_save_snapshot || serve_or_test.python_save_baseline_snapshot {
        return Some(
            "--threads: Saving a Python snapshot is not supported with more than one thread.",
        );
    }
    None
}

impl From<&TestArgs> for server::TestOptions {
    fn from(args: &TestArgs) -> Self {
        Self {
            no_verbose: args.no_verbose,
            predictable: args.predictable,
            gc_stress: args.gc_stress,
            all_autogates: args.all_autogates,
            compat_date: args.compat_date.clone().into(),
        }
    }
}

impl BenchArgs {
    /// The server's options for the run, with `case_pattern` from the filter.
    pub fn options(&self, case_pattern: Option<String>) -> bench::Options {
        bench::Options {
            no_verbose: self.no_verbose,
            all_autogates: self.all_autogates,
            compat_date: self.compat_date.clone(),
            quick: self.quick,
            case_pattern,
            json: self.format == BenchFormat::Json,
            output: self.output.clone(),
            trace: self.trace,
            no_startup: self.no_startup,
        }
    }
}

#[cfg(test)]
#[path = "args-test.rs"]
mod tests;
