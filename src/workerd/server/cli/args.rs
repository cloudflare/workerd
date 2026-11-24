//! The command line, as clap definitions. The help pages are written by hand, in `help.rs`.
//!
//! Every option handler applied in argument order under `kj::MainBuilder`; here options are parsed
//! up front and the C++ driver applies options before positional arguments, which is the order
//! they depended on (e.g. `--watch` and `--binary` must take effect before `<config>` is
//! parsed).

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
}

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
