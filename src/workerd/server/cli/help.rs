//! The help pages, written by hand, one per command. `workerd -h` is the page people and agents
//! read first: it should be enough to run, test, compile and cluster an application without
//! another call. Each command's page lists every one of its options.
//!
//! Style: sentence-case headings, lowercase `<placeholders>`, at most 80 columns. Option and
//! command descriptions start at column 37, short labels' at column 17. `args-test.rs` checks the
//! width, the option column, that each page lists its command's options, and that `workerd -h`
//! names every visible command.
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
  -w, --watch                        Reload when the config, code or
                                     binary changes; for development
      --experimental                 Allow experimental features: clusters,
                                     KV, Queues, Cron, Workflows, D1, assets
      --verbose                      Log to stderr
  -d, --directory-path <name>=<dir>  Point the disk service name at dir
  -e, --external-addr <name>=<addr>  Point the external service name at
                                     addr
  -h, --help                         Print this help
"
    };
}

macro_rules! socket_options {
    () => {
        "  -s, --socket-addr <name>=<addr>    Listen on addr for the socket name;
                                     wrangler's is http, on localhost:8787
  -S, --socket-fd <name>=<fd>        Listen on an inherited descriptor
                                     (systemd socket activation)
"
    };
}

macro_rules! cluster_options {
    () => {
        "\
Cluster (experimental):
      --cluster-addr <addr>          Listen for peers on addr, kept private
      --cluster-advertise <addr>     Address peers reach this instance at
  s3:// buckets read AWS_ENDPOINT_URL, AWS_ACCESS_KEY_ID and the other
  AWS_* variables; gs:// buckets, GOOGLE_APPLICATION_CREDENTIALS. On
  SIGTERM an instance hands its objects to its peers and exits, within 8 s.
  GET /ready on the peer address answers OK once serving.
"
    };
}

macro_rules! cluster_setup {
    () => {
        "\
Cluster setup:
  Publish the Worker to the bucket, then start each instance; restart
  them after each deploy.

    $ workerd deploy --experimental . --bucket s3://bucket/prefix
    $ workerd serve --experimental instance.capnp config

    # instance.capnp, for a wrangler.json named app
    using Workerd = import \"/workerd/workerd.capnp\";
    const config :Workerd.Config = (
      cluster = (bucket = \"s3://bucket/prefix\", key = \"<secret>\",
                 address = \"10.0.0.1:7000\"),
      services = [(name = \"do-storage\",
                   disk = (path = \"/var/lib/workerd\", writable = true))],
      sockets = [(name = \"http\", address = \"*:8787\", service = \"app\")],
    );

  key is at least 32 bytes, the same everywhere (openssl rand -hex 32).
  address is where peers reach this instance, on a private network.
"
    };
}

macro_rules! tool_options {
    () => {
        "\
Tools:
      --control-fd <fd>              Write a JSON line to fd as each socket
                                     listens, with its port
      --debug-port <addr>            Serve debug RPC with full access to
                                     every service; for Miniflare only
"
    };
}

macro_rules! trace_options {
    () => {
        "\
Tracing:
      --async-trace <path>           Write async activity (requests, I/O,
                                     what caused what) to path as JSON
                                     lines; it holds URLs and storage keys
      --async-trace-stacks <n>       Record n (1-64) stack frames per async
                                     resource; slow
      --async-trace-promises         Also trace promises; slow
      --perfetto-trace <path>=<categories>
                                     Write a Perfetto trace, in builds with
                                     Perfetto
"
    };
}

macro_rules! python_options {
    () => {
        "\
Python:
      --pyodide-package-disk-cache-dir <dir>
                                     Cache packages in dir
      --pyodide-bundle-disk-cache-dir <dir>
                                     Cache Pyodide bundles in dir
      --python-snapshot-dir <dir>    Keep snapshots in dir
      --python-save-snapshot         Save a dedicated snapshot
      --python-save-baseline-snapshot
                                     Save a baseline snapshot
      --python-load-snapshot <path>  Load a snapshot
"
    };
}

macro_rules! capnp_options {
    () => {
        "\
Cap'n Proto configs:
  <config> can also be a .capnp file, workerd's native format; <const>,
  where a command takes one, names the config constant in it.
  -b, --binary                       <config> is an encoded config, as
                                     compile --config-only writes; - is
                                     stdin
  -I, --import-path <dir>            Search dir for imports that start /
"
    };
}

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
  workerd <command> --help           Everything else about a command

Quick start:
  index.js       export default { fetch: () => new Response(\"hello\") };
  wrangler.json  { \"main\": \"index.js\", \"compatibility_date\": \"2026-11-01\" }

  $ workerd serve wrangler.json --watch        # http://localhost:8787

Cluster (experimental):
  Several machines that share an S3 or GCS bucket serve as one. Each
  Durable Object runs on one machine at a time, and any machine routes
  requests to it. There is no database or coordinator. On SIGTERM a
  machine hands its objects to the others, then exits. Anyone with the
  cluster key, or write access to the bucket, controls every Worker and
  object in it, and peer traffic is unencrypted: keep the cluster port on
  a private network. Setup: workerd serve --help.

Config:
  A wrangler.json or wrangler.jsonc, as for wrangler dev. Fields: name,
  main, compatibility_date, compatibility_flags, vars, durable_objects,
  exports, migrations, no_bundle; with --experimental, kv_namespaces,
  queues, triggers, workflows, d1_databases, assets. Other fields are
  errors. esbuild, from node_modules or PATH, bundles main unless
  no_bundle is set. State lives in .wrangler/state/workerd/. A .capnp
  config also works.

Options:
  -s, --socket-addr <name>=<addr>    Serve on addr, as in -s http=*:8080
  -i, --inspector-addr <addr>        Accept DevTools connections
  -w, --watch                        Reload when files change
      --experimental                 Allow experimental features
      --verbose                      Log to stderr
  -h, --help                         Print this help
  -V, --version                      Print the newest supported compat date

More, in each command's --help:
  serve          systemd sockets, service overrides, cluster settings,
                 --debug-port, tracing, Python
  test           filters, --predictable, --compat-date
  bench          writing benchmarks, filters, JSON output
  compile        --config-only

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
      --compat-date <date>           Give every Worker this compatibility
                                     date; Workers must not set one.
                                     0000-00-00 is the oldest behavior,
                                     9999-12-31 the newest
      --predictable                  Use fixed values for random data and
                                     timestamps
      --no-verbose                   Don't log uncaught exceptions
      --all-autogates                Enable every autogate
      --gc-stress                    Run a full GC at each awaitIo
                                     continuation; very slow
  -s, --socket-addr <name>=<addr>    Listen on addr for the socket name
",
    run_options!(),
    "\n",
    trace_options!(),
    "\n",
    python_options!(),
    "\n",
    capnp_options!(),
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
      --compat-date <date>           Give every Worker this compatibility
                                     date, as for test; latest, the
                                     default, is the newest this build
                                     supports
      --format text|json             text is a table; json is the full
                                     report, with raw samples
      --output <path>                Write the results to path
      --quick                        Use short time budgets, to check that
                                     benchmarks run rather than measure
      --no-startup                   Don't time cold starts
      --trace                        Run tail workers, otherwise skipped
      --no-verbose                   Don't log uncaught exceptions
      --all-autogates                Enable every autogate
",
    run_options!(),
    "\n",
    trace_options!(),
    "\n",
    python_options!(),
    "\n",
    capnp_options!(),
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
      --config-only                  Write only the encoded config, for
                                     workerd serve -b
  -b, --binary                       <config> is an encoded config; - is
                                     stdin
  -I, --import-path <dir>            Search dir for .capnp imports
      --verbose                      Log to stderr
  -h, --help                         Print this help
",
);

pub const DEPLOY: &str = "\
workerd deploy: publish an application to a cluster's bucket
(experimental).

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
      --bucket <url>                 s3://bucket[/prefix] or
                                     gs://bucket[/prefix]
      --name <name>                  1-64 letters, digits, - or _,
                                     starting with a letter or digit;
                                     default: the config's name
      --endpoint <url>               For s3://: the endpoint; default
                                     AWS_ENDPOINT_URL
      --region <region>              For s3://: default AWS_REGION, else
                                     us-east-1
      --experimental                 Required
      --verbose                      Log to stderr
  -h, --help                         Print this help
  Credentials are read as for serve.

Cap'n Proto configs:
  [project] can be a .capnp config that holds only services, with --name;
  <const> names its constant.
  -b, --binary                       [project] is an encoded config; - is
                                     stdin
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
    "\n",
    cluster_options!(),
    "\n",
    tool_options!(),
    "\n",
    trace_options!(),
    "\n",
    python_options!(),
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

pub const BASELINE_SNAPSHOT: &str = "\
workerd make-pyodide-baseline-snapshot: write a Pyodide baseline memory
snapshot for <python-version> to <output-dir>.

Usage:
  workerd make-pyodide-baseline-snapshot <python-version> <output-dir>

Options:
      --verbose                      Log to stderr
  -h, --help                         Print this help
";
