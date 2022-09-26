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

[Read the blog post to learn more about these principles.](https://blog.cloudflare.com/workerd-open-source-workers-runtime/).

### WARNING: This is a beta. Work in progress.

Although most of `workerd`'s code has been used in Cloudflare Workers for years, the `workerd` configuration format and top-level server code is brand new. We don't yet have much experience running this in production. As such, there will be rough edges, maybe even a few ridiculous bugs. Deploy to production at your own risk (but please tell us what goes wrong!).

The config format may change in backwards-incompatible ways before `workerd` leaves beta, but should remain stable after that.

As of this writing, some major features are missing which we intend to fix shortly:

* **General error logging** is awkward. Traditionally we have separated error logs into "application errors" (e.g. a Worker threw an exception from JavaScript) and "internal errors" (bugs in the implementation which the Workers team should address). We then sent these errors to completely different places. In the `workerd` world, the server admin wants to see both of these, so logging has become entirely different and, at the moment, is a bit ugly. For now, it may help to run `workerd` with the `--verbose` flag, which causes application errors to be written to standard error in the same way that internal errors are (but may also produce more noise). We'll be working on making this better out-of-the-box.
* **Binary packages** for various distributions are not built yet. We intend to provide these once out of beta.
* **Wrangler/Miniflare integration** is in progress. The [Wrangler CLI tool](https://developers.cloudflare.com/workers/wrangler/) and [Miniflare](https://miniflare.dev/) will soon support local testing using `workerd` (replacing the previous simulated environment on top of Node). Wrangler should also support generating `workerd` configuration directly from a Wrangler project.
* **Multi-threading** is not implemented. `workerd` runs in a single-threaded event loop. For now, to utilize multiple cores, we suggest running multiple instances of `workerd` and balancing load across them. We will likely add some built-in functionality for this in the near future.
* **Performance tuning** has not been done yet, and there is low-hanging fruit here. `workerd` performs decently as-is, but not spectacularly. Experiments suggest we can roughly double performance on a "hello world" load test with some tuning of compiler optimization flags and memory allocators.
* **Durable Objects** are currently supported only in a mode that uses in-memory storage -- i.e., not actually "durable". This is useful for local testing of DO-based apps, but not for production. Durable Objects that are actually durable, or distributed across multiple machines, are a longer-term project. Cloudflare's internal implementation of this is heavily tied to the specifics of Cloudflare's network, so a new implementation needs to be developed for public consumption.
* **Cache API** emulation is not implemented yet.
* **Cron trigger** emulation is not supported yet. We need to figure out how, exactly, this should work in the first place. Typically if you have a cluster of machines, you only want a cron event to run on one of the machines, so some sort of coordination or external driver is needed.
* **Parameterized workers** are not implemented yet. This is a new feature specified in the config schema, which doesn't have any precedent on Cloudflare.
* **Devtools inspection** is not supported yet, but this should be straightforward to hook up.
* **Tests** for most APIs are conspicuously missing. This is because the testing harness we have used for the past five years is deeply tied to the internal version of the codebase. We need to develop a new test harness for `workerd` and revise our API tests to use it. For the time being, we will be counting on the internal tests to catch bugs. We understand this is not ideal for external contributors trying to test their changes.
* **Documentation** is growing quickly but is definitely still a work in progress.

### WARNING: `workerd` is not a hardened sandbox

`workerd` tries to isolate each Worker so that it can only access the resources it is configured to access. However, `workerd` on its own does not contain suitable defense-in-depth against the possibliity of implementation bugs. When using `workerd` to run possibly-malicious code, you must run it inside an appropriate secure sandbox, such as a virtual machine. The Cloudflare Workers hosting service in particular [uses many additional layers of defense-in-depth](https://blog.cloudflare.com/mitigating-spectre-and-other-security-threats-the-cloudflare-workers-security-model/).

With that said, if you discover a bug that allows malicious code to break out of `workerd`, please submit it to [Cloudflare's bug bounty program](https://hackerone.com/cloudflare?type=team) for a reward.

## Getting Started

### Supported Platforms

In theory, `workerd` should work on any POSIX system that is supported by V8.

In practice, `workerd` is tested on Linux and macOS under x86-64 and arm64 architectures.
On other platforms, you may have to do tinkering to make things work.

Windows users should run `workerd` under WSL (1 or 2).

### Building `workerd`

To build `workerd`, you need:

* [Bazel](https://bazel.build/)
* On Linux:
  * Clang 11+ (e.g. package `clang` on Debian Bullseye)
  * libc++ 11+ (e.g. packages `libc++-dev` and `libc++abi-dev` on Debian Bullseye)
* On macOS:
  * full XCode 13+ installation

You may then build using:

```
bazel build -c opt //src/workerd/server:workerd
```

The compiled binary will be located at `bazel-bin/src/workerd/server/workerd`.

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
  compatibilityDate = "2022-09-16",
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

### Serving in production

`workerd` is designed to be unopinionated about how it runs.

One good way to manage `workerd` in production is using `systemd`. Particularly useful is `systemd`'s ability to open privileged sockets on `workerd`'s behalf while running the service itself under an unprivileged user account. To help with this, `workerd` supports inheriting sockets from the parent process using the `--socket-fd` flag.

Here's an example system service file, assuming your config defines two sockets named `http` and `https`:

```
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

```
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
