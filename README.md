# 👷 `workerd`, Cloudflare's JavaScript/Wasm Runtime

![Banner](/docs/assets/banner.png)

`workerd` (pronounced: "worker-dee") is a JavaScript / Wasm server runtime based on the same code that powers [Cloudflare Workers](https://workers.dev).

You might use it:

* **As an application server**, to self-host applications designed for Cloudflare Workers.
* **As a development tool**, to develop and test such code locally.
* **As a programmable HTTP proxy** (forward or reverse), to efficiently intercept, modify, and
  route network requests.

## Introduction

### Design Principles

* **Server-first:** Designed for servers, not CLIs nor GUIs.

* **Standard-based:** Built-in APIs are based on web platform standards, such as `fetch()`.

* **Nanoservices:** Split your application into components that are decoupled and independently-deployable like microservices, but with performance of a local function call. When one nanoservice calls another, the callee runs in the same thread and process.

* **Homogeneous deployment:** Instead of deploying different microservices to different machines in your cluster, deploy all your nanoservices to every machine in the cluster, making load balancing much easier.

* **Capability bindings:** `workerd` configuration uses capabilities instead of global namespaces to connect nanoservices to each other and external resources. The result is code that is more composable -- and immune to SSRF attacks.

* **Always backwards compatible:** Updating `workerd` to a newer version will never break your JavaScript code. `workerd`'s version number is simply a date, corresponding to the maximum ["compatibility date"](https://developers.cloudflare.com/workers/platform/compatibility-dates/) supported by that version. You can always configure your worker to a past date, and `workerd` will emulate the API as it existed on that date.

[Read the blog post to learn more about these principles.](https://blog.cloudflare.com/workerd-open-source-workers-runtime/)

### WARNING: This is a beta. Work in progress

Although most of `workerd`'s code has been used in Cloudflare Workers for years, the `workerd` configuration format and top-level server code is brand new. We don't yet have much experience running this in production. As such, there will be rough edges, maybe even a few ridiculous bugs. Deploy to production at your own risk (but please tell us what goes wrong!).

The config format may change in backwards-incompatible ways before `workerd` leaves beta, but should remain stable after that.

A few caveats:

* **General error logging** is awkward. Traditionally we have separated error logs into "application errors" (e.g. a Worker threw an exception from JavaScript) and "internal errors" (bugs in the implementation which the Workers team should address). We then sent these errors to completely different places. In the `workerd` world, the server admin wants to see both of these, so logging has become entirely different and, at the moment, is a bit ugly. For now, it may help to run `workerd` with the `--verbose` flag, which causes application errors to be written to standard error in the same way that internal errors are (but may also produce more noise). We'll be working on making this better out-of-the-box.
* **Binary packages** are only available via npm, not as distro packages. This works well for testing with Miniflare, but is awkward for a production server that doesn't actually use Node at all.
* **Multi-threading** is not implemented. `workerd` runs in a single-threaded event loop. For now, to utilize multiple cores, we suggest running multiple instances of `workerd` and balancing load across them. We will likely add some built-in functionality for this in the near future.
* **Performance tuning** has not been done yet, and there is low-hanging fruit here. `workerd` performs decently as-is, but not spectacularly. Experiments suggest we can roughly double performance on a "hello world" load test with some tuning of compiler optimization flags and memory allocators.
* **Durable Objects** currently always run on the same machine that requested them, using local disk storage. This is sufficient for testing and small services that fit on a single machine. In scalable production, though, you would presumably want Durable Objects to be distributed across many machines, always choosing the same machine for the same object.
* **Parameterized workers** are not implemented yet. This is a new feature specified in the config schema, which doesn't have any precedent on Cloudflare.
* **Tests** for most APIs are conspicuously missing. This is because the testing harness we have used for the past five years is deeply tied to the internal version of the codebase. Ideally, we need to translate those tests into the new `workerd test` format and move them to this repo; this is an ongoing effort. For the time being, we will be counting on the internal tests to catch bugs. We understand this is not ideal for external contributors trying to test their changes.
* **Documentation** is growing quickly but is definitely still a work in progress.

### WARNING: `workerd` is not a hardened sandbox

`workerd` tries to isolate each Worker so that it can only access the resources it is configured to access. However, `workerd` on its own does not contain suitable defense-in-depth against the possibility of implementation bugs. When using `workerd` to run possibly-malicious code, you must run it inside an appropriate secure sandbox, such as a virtual machine. The Cloudflare Workers hosting service in particular [uses many additional layers of defense-in-depth](https://blog.cloudflare.com/mitigating-spectre-and-other-security-threats-the-cloudflare-workers-security-model/).

With that said, if you discover a bug that allows malicious code to break out of `workerd`, please submit it to [Cloudflare's bug bounty program](https://hackerone.com/cloudflare?type=team) for a reward.

## Getting Started

### Supported Platforms

In theory, `workerd` should work on any POSIX system that is supported by V8 and Windows.

In practice, `workerd` is tested on:

* Linux and macOS (x86-64 and arm64 architectures)
* Windows (x86-64 architecture)

On other platforms, you may have to do tinkering to make things work.

### Building `workerd`

To build `workerd`, you need:

* Bazel
  * If you use [Bazelisk](https://github.com/bazelbuild/bazelisk) (recommended), it will automatically download and use the right version of Bazel for building workerd.
* On Linux:
  * We use the clang/LLVM toolchain to build workerd and support version 16 and higher. Earlier versions of clang may still work, but are not officially supported.
  * Clang 16+ (e.g. package `clang-16` on Debian Bookworm). If clang is installed as `clang-<version>` please create a symlink to it in your PATH named `clang`, or use `--action_env=CC=clang-<version>` on `bazel` command lines to specify the compiler name.

  * libc++ 16+ (e.g. packages `libc++-16-dev` and `libc++abi-16-dev`)
  * LLD 16+ (e.g. package `lld-16`).
  * `python3`, `python3-distutils`, and `tcl8.6`
* On macOS:
  * Xcode 16 installation (available on macOS 14 and higher). **Full Xcode is required**, the Xcode command line tools alone are **not sufficient** for building.
  * Homebrew installed `tcl-tk` package (provides Tcl 8.6)
* On Windows:
  * Install [App Installer](https://learn.microsoft.com/en-us/windows/package-manager/winget/#install-winget)
    from the Microsoft Store for the `winget` package manager and then run
    [install-deps.bat](tools/windows/install-deps.bat) from an administrator prompt to install
    bazelisk, LLVM, and other dependencies required to build workerd on Windows.
  * Add `startup --output_user_root=C:/tmp` to the `.bazelrc` file in your user directory.
  * When developing at the command-line, run [bazel-env.bat](tools/windows/bazel-env.bat) in your shell first
    to select tools and Windows SDK versions before running bazel.

You may then build `workerd` at the command-line with:

```sh
bazel build //src/workerd/server:workerd
```

You can also build from within Visual Studio Code using the instructions in [docs/vscode.md](docs/vscode.md).

The compiled binary will be located at `bazel-bin/src/workerd/server/workerd`.

If you run a Bazel build before you've installed some dependencies (like clang or libc++), and then you install the dependencies, you must resync locally cached toolchains, or clean Bazel's cache, otherwise you might get strange errors:

```sh
bazel sync --configure
```

If that fails, you can try:

```sh
bazel clean --expunge
```

The cache will now be cleaned and you can try building again.

If you have a fairly recent clang packages installed you can build a more performant release
version of workerd:

```sh
bazel build --config=thin-lto //src/workerd/server:workerd
```

### Configuring `workerd`

`workerd` is configured using a config file written in Cap'n Proto text format.

A simple "Hello World!" config file might look like:

```capnp
using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    (name = "main", worker = .mainWorker),
  ],

  sockets = [
    # Serve HTTP on port 8080.
    ( name = "http",
      address = "*:8080",
      http = (),
      service = "main"
    ),
  ]
);

const mainWorker :Workerd.Worker = (
  serviceWorkerScript = embed "hello.js",
  compatibilityDate = "2023-02-28",
  # Learn more about compatibility dates at:
  # https://developers.cloudflare.com/workers/platform/compatibility-dates/
);
```

Where `hello.js` contains:

```javascript
addEventListener("fetch", event => {
  event.respondWith(new Response("Hello World"));
});
```

[Complete reference documentation is provided by the comments in workerd.capnp.](src/workerd/server/workerd.capnp)

[There is also a library of sample config files.](samples)

(TODO: Provide a more extended tutorial.)

### Running `workerd`

To serve your config, do:

`workerd serve my-config.capnp`

For more details about command-line usage, use `workerd --help`.

Prebuilt binaries are distributed via `npm`. Run `npx workerd ...` to use these. If you're running a prebuilt binary, you'll need to make sure your system has the right dependencies installed:

* On Linux:
  * glibc 2.31 or higher (already included on e.g. Ubuntu 20.04, Debian Bullseye)
* On macOS:
  * macOS 13.5 or higher
  * The Xcode command line tools, which can be installed with `xcode-select --install`
* x86_64 CPU with at least SSE4.2 and CLMUL ISA extensions, or arm64 CPU with CRC extension (enabled by default under armv8.1-a). These extensions are supported by all recent x86 and arm64 CPUs.

### Local Worker development with `wrangler`

You can use [Wrangler](https://developers.cloudflare.com/workers/wrangler/) (v3.0 or greater) to develop Cloudflare Workers locally, using `workerd`. Run:

`wrangler dev`

### Serving in production

`workerd` is designed to be unopinionated about how it runs.

One good way to manage `workerd` in production is using `systemd`. Particularly useful is `systemd`'s ability to open privileged sockets on `workerd`'s behalf while running the service itself under an unprivileged user account. To help with this, `workerd` supports inheriting sockets from the parent process using the `--socket-fd` flag.

Here's an example system service file, assuming your config defines two sockets named `http` and `https`:

```sh
# /etc/systemd/system/workerd.service
[Unit]
Description=workerd runtime
After=local-fs.target remote-fs.target network-online.target
Requires=local-fs.target remote-fs.target workerd.socket
Wants=network-online.target

[Service]
Type=exec
ExecStart=/usr/bin/workerd serve /etc/workerd/config.capnp --socket-fd http=3 --socket-fd https=4
Sockets=workerd.socket

# If workerd crashes, restart it.
Restart=always

# Run under an unprivileged user account.
User=nobody
Group=nogroup

# Hardening measure: Do not allow workerd to run suid-root programs.
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
```

And corresponding sockets file:

```sh
# /etc/systemd/system/workerd.socket
[Unit]
Description=sockets for workerd
PartOf=workerd.service

[Socket]
ListenStream=0.0.0.0:80
ListenStream=0.0.0.0:443

[Install]
WantedBy=sockets.target
```

(TODO: Fully explain how to get systemd to recognize these files and start the service.)
