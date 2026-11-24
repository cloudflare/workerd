//! The help pages, written by hand, one per command. `workerd -h` is the page people and agents
//! read first: it should be enough to run, test, compile and cluster an application without
//! another call. Each command's page lists every one of its options, except that test and bench
//! refer to serve for the groups in [`SHARED`].
//!
//! Style: minimal. Sentence-case headings, lowercase `<placeholders>`, at most 80 columns. Option
//! and command descriptions start at column 37, short labels' at column 17, and fit one line.
//! `args-test.rs` checks the width, that no row wraps, that each page lists
//! its command's options, and that `workerd -h` names every visible command.
//!
//! A block whose first line is indented starts on the line of its opening quote: `\` at the end
//! of a line also skips the next line's leading spaces.

macro_rules! config_arg {
    () => {
        "<config> is a wrangler.json or wrangler.jsonc; see workerd -h.\n"
    };
}

macro_rules! run_options {
    () => {
        "  -i, --inspector-addr <addr>        Accept DevTools connections
  -w, --watch                        Reload when files change
      --experimental                 Allow experimental features
      --verbose                      Log to stderr
      --data-dir <dir>               Keep state in dir, not in .wrangler/state
  -d, --directory-path <name>=<dir>  Point disk service name at dir
  -e, --external-addr <name>=<addr>  Point external service name at addr
  -h, --help                         Print this help
"
    };
}

macro_rules! socket_options {
    () => {
        "  -s, --socket-addr <name>=<addr>    Serve on addr, as in -s http=*:8080
  -S, --socket-fd <name>=<fd>        Serve on an inherited fd (systemd)
"
    };
}

macro_rules! cluster_options {
    () => {
        "\
Cluster (experimental):
      --cluster-addr <addr>          Cluster address, joining if config has none
      --cluster-advertise <addr>     Address peers reach this instance at
  s3:// buckets read AWS_ENDPOINT_URL, AWS_ACCESS_KEY_ID and the other
  AWS_* variables; gs:// buckets, GOOGLE_APPLICATION_CREDENTIALS.
  WORKERD_CLUSTER_KEY, or the file that WORKERD_CLUSTER_KEY_FILE names,
  overrides the config's cluster key, WORKERD_CLUSTER_BUCKET its bucket.
  On SIGTERM, after any shutdown delay, an instance hands its objects to
  its peers and exits within WORKERD_CLUSTER_STOP_TIMEOUT_MS, or else
  stopTimeoutMs (8 s by default). GET /ready on the cluster address
  answers OK once serving, until SIGTERM.
"
    };
}

macro_rules! stopping {
    () => {
        "\
Stopping:
  On SIGTERM the server drains: it stops accepting connections, lets
  the open ones finish, and exits. WORKERD_SHUTDOWN_DELAY_MS, or else
  shutdownDelayMs in the config, keeps it serving that many
  milliseconds first, for load balancers that go on sending connections
  for a while; a second SIGTERM drains at once. The supervisor's grace
  period must cover the delay and the drain, in a cluster the delay
  plus stopTimeoutMs. A cluster instance's GET /ready answers 503 from
  the first SIGTERM on.
"
    };
}

macro_rules! cluster_setup {
    () => {
        "\
Cluster setup:
  Start each instance with the same key and bucket. A wrangler.json
  joins with --cluster-addr. --data-dir holds its Durable Objects and
  those of the bucket's deployments.

    $ export WORKERD_CLUSTER_KEY=<secret>      # openssl rand -hex 32
    $ export WORKERD_CLUSTER_BUCKET=s3://bucket/prefix
    $ workerd serve --experimental wrangler.json \\
        --cluster-addr 10.0.0.1:7000 --data-dir /var/lib/workerd

  key is at least 32 bytes, the same everywhere. --cluster-addr is where
  this instance listens for peers, on a private network; behind NAT,
  --cluster-advertise is where they reach it. A .capnp config sets
  these in cluster = (...) instead.

  To serve more Workers on every instance, publish them to the bucket,
  then restart the instances:

    $ workerd deploy --experimental . --bucket s3://bucket/prefix
"
    };
}

macro_rules! tool_options {
    () => {
        "\
Tools:
      --control-fd <fd>              Write each socket's port to fd (JSON)
      --debug-port <addr>            Full-access debug RPC, for Miniflare
"
    };
}

macro_rules! trace_options {
    () => {
        "\
Tracing:
      --async-trace <path>           Write async activity to path (JSONL)
      --async-trace-stacks <n>       Keep n (1-64) stack frames; slow
      --async-trace-promises         Also trace promises; slow
      --perfetto-trace <path>=<cats> Write a Perfetto trace, in Perfetto builds
"
    };
}

macro_rules! python_options {
    () => {
        "\
Python:
  --pyodide-bundle-disk-cache-dir <dir> caches downloads;
  --python-snapshot-dir <dir>, --python-save-snapshot,
  --python-save-baseline-snapshot and --python-load-snapshot <path>
  manage snapshots.
"
    };
}

macro_rules! capnp_options {
    () => {
        "\
Cap'n Proto configs:
  <config> can also be a .capnp file, workerd's native format; <const>,
  where a command takes one, names the config constant in it.
  -b, --binary                       <config> is an encoded config; - is stdin
  -I, --import-path <dir>            Search dir for imports that start /
"
    };
}

macro_rules! shared_note {
    () => {
        "Tracing, Python and Cap'n Proto options: as for serve.\n"
    };
}

/// The option groups that test and bench refer to serve for.
#[cfg(test)]
pub const SHARED: &str = concat!(trace_options!(), python_options!(), capnp_options!());
#[cfg(test)]
pub const SHARED_NOTE: &str = shared_note!();

pub const MAIN: &str = "\
workerd runs Cloudflare Workers on your own machines: JavaScript,
TypeScript, Wasm and Python, from a wrangler.json, on one machine or many.
Durable Objects, KV, Queues, Cron Triggers, Workflows, D1 and static
assets are built in.

Usage:
  workerd serve <config>             Serve the Worker
  workerd test <config> [filter]     Run its test handlers
  workerd bench <config> [filter]    Run its bench handlers
  workerd compile <config> > app     Build a self-contained binary
  workerd <command> -h               Its options, as in workerd serve -h

Quick start:
  index.js       export default { fetch: () => new Response(\"hello\") };
  wrangler.json  { \"main\": \"index.js\", \"compatibility_date\": \"2026-11-01\" }

  $ workerd serve wrangler.json --watch        # http://localhost:8787

Cluster (experimental):
  Machines that share an object store bucket serve as one: each Durable
  Object runs on one machine, and any machine routes to it. The cluster
  key and the bucket grant full control and peer traffic is unencrypted,
  so keep peers on a private network. Setup: workerd serve --help.

Config:
  A wrangler.json or wrangler.jsonc, as for wrangler dev, with fields:
    $schema, name, main, compatibility_date, compatibility_flags, vars,
    durable_objects, exports, migrations, no_bundle
    with --experimental: kv_namespaces, queues, triggers, workflows,
    d1_databases, assets
  Other fields are errors. esbuild (node_modules or PATH) bundles main
  unless no_bundle is set. State: .wrangler/state/workerd/, or
  --data-dir. A .capnp config also works.

Options:
  -s, --socket-addr <name>=<addr>    Serve on addr, as in -s http=*:8080
  -i, --inspector-addr <addr>        Accept DevTools connections
  -w, --watch                        Reload when files change
      --experimental                 Allow experimental features
      --verbose                      Log to stderr
  -h, --help                         Print this help
  -V, --version                      Print the newest supported compat date

workerd is not a hardened sandbox; run untrusted code in a VM.

Docs:
  workerd        https://github.com/cloudflare/workerd
  Workers APIs   https://developers.cloudflare.com/workers/runtime-apis/
  Bugs           https://github.com/cloudflare/workerd/issues
";

pub const SERVE: &str = concat!(
    "\
workerd serve: serve the Workers in <config>.

Usage:
  workerd serve [options] <config> [const]

",
    config_arg!(),
    "\nOptions:\n",
    socket_options!(),
    run_options!(),
    "\n",
    cluster_options!(),
    "\n",
    tool_options!(),
    "\n",
    trace_options!(),
    "\n",
    python_options!(),
    "\n",
    capnp_options!(),
    "\n",
    stopping!(),
    "\n",
    cluster_setup!(),
);

pub const TEST: &str = concat!(
    "\
workerd test: run the test handlers of the Workers in <config>.

Usage:
  workerd test [options] <config> [filter]

A test is an exported handler with a test method. It passes unless it
throws. Export several under different names for several tests:

  export default {
    async test(ctrl, env, ctx) {
      if (1 + 1 != 2) throw new Error(\"math is broken\");
    },
  };

",
    config_arg!(),
    "\
[filter] selects tests by glob; each part defaults to *:
  <service>[:<entrypoint>]           For example api, or api:test*
  <const>:<service>:<entrypoint>     For a .capnp config

Options:
      --compat-date <date>           Set every Worker's compat date
      --predictable                  Fix random data and timestamps
      --no-verbose                   Don't log uncaught exceptions
      --all-autogates                Enable every autogate
      --gc-stress                    GC at each awaitIo; very slow
  -s, --socket-addr <name>=<addr>    Serve on addr, as in -s http=*:8080
",
    run_options!(),
    "\n",
    shared_note!(),
);

pub const BENCH: &str = concat!(
    "\
workerd bench: run the bench handlers of the Workers in <config>.

Usage:
  workerd bench [options] <config> [filter]

A bench handler registers cases, each timed after it returns:

  export default {
    bench(b, env, ctx) {
      const enc = new TextEncoder();
      b.run(\"encode\", () => b.blackBox(enc.encode(\"hi\")));
      b.run(\"fetch\", () => env.SVC.fetch(\"http://x/\"), { minTime: \"2s\" });
    },
  };

Each Worker's cold start is timed too, as the cases startup:isolate,
startup:script, startup:worker and startup:total. Results go to stdout as
a table.

",
    config_arg!(),
    "\
[filter] is test's filter, optionally followed by /<case>, a glob over
case names: <service>[:<entrypoint>][/<case>].

Options:
      --compat-date <date>           Date/latest for all; else newest if unset
      --format text|json             A table, or JSON with raw samples
      --output <path>                Write results to path
      --quick                        Check benchmarks run; don't measure
      --no-startup                   Don't time cold starts
      --trace                        Also run tail workers
      --no-verbose                   Don't log uncaught exceptions
      --all-autogates                Enable every autogate
",
    run_options!(),
    "\n",
    shared_note!(),
);

pub const COMPILE: &str = concat!(
    "\
workerd compile: build a self-contained binary from <config>.

Usage:
  workerd compile [options] <config> [const] > app

Writes to stdout a copy of this workerd with <config> and all its code
and data inside. The binary needs no other files:

  $ workerd compile wrangler.json > app && chmod +x app && ./app

It takes serve's options (./app --help); pass it --experimental if the
application needs it.

",
    config_arg!(),
    "
Options:
      --config-only                  Write only the encoded config (serve -b)
  -b, --binary                       <config> is an encoded config; - is stdin
  -I, --import-path <dir>            Search dir for imports that start /
      --verbose                      Log to stderr
  -h, --help                         Print this help
",
);

pub const DEPLOY: &str = "\
workerd deploy: publish a Worker to a cluster's bucket (experimental).

Usage:
  workerd deploy --experimental [project] --bucket <url>

Uploads [project] to the cluster's bucket as a named deployment and makes
that version current in the bucket's catalog. Every instance of the
cluster serves it beside its own config from its next start, so
restart the instances. Publishing an earlier version rolls back. Prints
the deployment version and catalog revision.

[project], . by default, is a wrangler project directory or its
wrangler.json or wrangler.jsonc, bundled as for serve. Its Worker becomes
a service named after the deployment. Its Durable Objects are stored in
each instance's disk service `do-storage`, keyed by deployment name and
class, so deploying under another name starts with empty objects.

Options:
      --bucket <url>                 s3:// or gs://bucket[/prefix]
      --name <name>                  Deployment name; default: the config's
      --endpoint <url>               s3:// endpoint; default AWS_ENDPOINT_URL
      --region <region>              s3:// region; AWS_REGION, or us-east-1
      --experimental                 Required
      --verbose                      Log to stderr
  -h, --help                         Print this help
  Credentials are read as for serve.

Cap'n Proto configs:
  [project] can be a .capnp config that holds only services, with --name;
  <const> names its constant.
  -b, --binary                       [project] is an encoded config; - is stdin
  -I, --import-path <dir>            Search dir for imports that start /
";

pub const COMPILED: &str = concat!(
    "\
This is a workerd binary with an application compiled in, by workerd
compile. Run it to serve the application.

Usage:
  app [options]

Options:
",
    socket_options!(),
    run_options!(),
    "  -V, --version                      Print the newest supported compat date\n\n",
    cluster_options!(),
    "\n",
    tool_options!(),
    "\n",
    trace_options!(),
    "\n",
    python_options!(),
    "\n",
    stopping!(),
);

pub const FUZZILLI: &str = concat!(
    "\
workerd fuzzilli: run the REPRL loop for the Fuzzilli fuzzer.

Usage:
  workerd fuzzilli [options] <config>

",
    config_arg!(),
    "\nOptions:\n",
    run_options!(),
    "\n",
    trace_options!(),
    "\n",
    python_options!(),
    "\n",
    capnp_options!(),
);
