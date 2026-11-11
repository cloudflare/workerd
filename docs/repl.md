# The workerd REPL

`workerd repl` evaluates JavaScript typed at a console, in a Worker's global scope, and prints each
result. Its `workerd` global loads configs, serves them in the same process as `workerd serve`
would, and calls into their services and sockets. Use it to try out an API, inspect what a value
looks like at runtime, or poke at a config's Workers without writing a test.

The REPL is a tool for developing workerd. It is experimental: it requires `--experimental`, and
its behavior, commands and JavaScript API may change or go away in any release.

## Quick start

Build workerd and start the REPL:

```sh
bazel build //src/workerd/server:workerd
bazel-bin/src/workerd/server/workerd repl --experimental
```

Type JavaScript at the `>` prompt. An input can `await` at top level:

```
workerd REPL (experimental). ".help" for help; ".exit" or Ctrl+D to quit.
> 1 + 1
2
> const { Buffer } = await import('node:buffer'); Buffer.from('hi').toString('base64')
'aGk='
> (await fetch('data:text/plain,hello')).status
200
> await new Promise((resolve) => setTimeout(() => resolve('later'), 10))
'later'
> throw new Error('boom')
Uncaught Error: boom
    at file:///bundle/repl.js:1:7
```

Load a config and call its Workers:

```
> s = await workerd.loadConfig('config.capnp')
> r = await s.service('main').fetch('http://example.com/')
> await r.text()
> await s.service('main', { entrypoint: 'Api' }).add(2, 3)
> await (await s.socket('http').fetch('http://example.com/')).text()
> await s.close()
```

Press Ctrl+D, or type `.exit`, to quit.

## Command line

| Option                 | Effect                                                                                                                                                                |
| ---------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `--experimental`       | Required. Also lets the compatibility flags and the loaded configs use experimental features, as it does for `workerd serve`.                                         |
| `--compat-date=<date>` | The compatibility date of the REPL's JavaScript. Defaults to today. Any valid date is accepted, as for `workerd test`; `2999-12-31` gives every flag's behavior.      |
| `--compat-flag=<flag>` | Enables a compatibility flag in the REPL's JavaScript. Repeat it for more. `nodejs_compat` is on unless a flag turns it, or `nodejs_compat_v2`, on or off explicitly. |

The compatibility settings apply to the JavaScript typed into the REPL. Each loaded config's
Workers use their own settings from the config.

Results are colored when stdout is a terminal, unless `NO_COLOR` is set.

## Typing input

An input runs once it is finished. While a string, template literal, comment or bracket is still
open, Enter continues the input on the next line, at a `...` prompt:

```
> function f() {
...   return 42
... }
undefined
> f()
42
```

The REPL decides this without parsing JavaScript, so it can misjudge a `/` that could start either
a regular expression or a division: it takes a `/` after `}` to start a regular expression, and
one after `)` to divide.

Each result is printed with Node's `util.inspect()`. An exception prints as `Uncaught` followed by
the error and its stack.

### Top-level `await`

An input that uses `await` outside a function runs inside an async function, and the REPL prints
what that function's last statement evaluates to, once it settles. The event loop keeps running
while the REPL waits, so timers, `fetch()` and the loaded servers all make progress.

A declaration inside an async function would be local to it. So in an input that awaits at top
level, a `const`, `let` or `var` declaration of a single name becomes an assignment to a global,
which later inputs can see. A destructuring declaration does not: assign instead, as in
`({ a } = await f())`.

An input that doesn't await at top level runs as a script. A promise it produces prints as a
promise; `await` it to see what it settles to.

### The environment

The REPL's JavaScript runs in a Worker with no bindings and no handlers: it never receives an
event. Its global scope has the Workers APIs its compatibility date and flags give it, and
`workerd`. `import()` resolves as it would from a Worker's main module, so built-in modules such as
`node:buffer` and `cloudflare:workers` can be imported.

One request context lasts the whole session. Timers, promises and `waitUntil()` tasks keep running
between inputs, and I/O objects such as a response body stay usable from one input to the next.
`console.log()` writes to stdout.

## Commands

A line that starts with `.` and a letter, at the start of an input, is a command:

| Command    | Effect                                                     |
| ---------- | ---------------------------------------------------------- |
| `.help`    | Shows the commands, keys and `workerd` API.                |
| `.servers` | Prints the loaded configs' servers (`workerd.servers`).    |
| `.clear`   | Clears the screen.                                         |
| `.exit`    | Quits, closing every loaded server first (as Ctrl+D does). |

## Keys

On a Unix terminal, the REPL edits lines itself:

| Key                   | Effect                                                                              |
| --------------------- | ----------------------------------------------------------------------------------- |
| Enter                 | Submits the line. The input runs if it is finished.                                 |
| Left, Right, Ctrl+B/F | Moves the cursor by a character.                                                    |
| Home, End, Ctrl+A/E   | Moves the cursor to the start or end of the line.                                   |
| Up, Down, Ctrl+P/N    | Steps through earlier lines, including earlier sessions' (see [History](#history)). |
| Backspace, Delete     | Deletes the character before or under the cursor.                                   |
| Ctrl+K, Ctrl+U        | Deletes to the end, or to the start, of the line.                                   |
| Ctrl+W                | Deletes the word before the cursor.                                                 |
| Ctrl+L                | Clears the screen.                                                                  |
| Tab                   | Completes a global's name, or a property name after `a.b.`; see below.              |
| Ctrl+C                | Abandons the input being typed. While an input runs, Ctrl+C ends the REPL.          |
| Ctrl+D                | Quits on an empty line; otherwise deletes the character under the cursor.           |

Tab completes the name before the cursor. With a dot, the names are the properties, own and
inherited, of the value before it (an identifier chain such as `s.service`); without one, they are
the global scope's. Completing evaluates that chain, so it can run property getters. Tab inserts as
much as the candidates have in common; when there is nothing more to insert, it lists them.

### History

On a terminal, each line you submit is saved to `~/.workerd_repl_history` as you submit it, and
the next session's history starts with the latest 1000 lines. Set `WORKERD_REPL_HISTORY` to save
them in another file, or to the empty string not to save them. The file is created readable by
its owner only. If it can't be read or written, the REPL warns once and saves no more history for
the session. A line that repeats the one before it, or is empty, isn't saved.

### Without a terminal

Elsewhere, such as when input is piped in or on Windows, the REPL reads plain lines and saves no
history. Prompts and the REPL's own messages go to stderr and results to stdout, so a script can
drive it:

```sh
printf '%s\n' "s = await workerd.loadConfig('config.capnp')" \
    "await (await s.service('main').fetch('http://x/')).text()" |
  workerd repl --experimental 2>/dev/null
```

## Loading configs

`await workerd.loadConfig(path, { constName })` loads a config and starts serving it. It resolves
to a server handle once every socket listens. `path` is relative to the working directory, and
names a Cap'n Proto config or a `wrangler.json` / `wrangler.jsonc`, read as `workerd serve` reads
it. `constName` names the constant that holds a Cap'n Proto config, as `workerd serve`'s second
argument does. If the config doesn't load or has errors, the promise rejects with an error that
lists them, and nothing is left running.

Each config is served by a server of its own, as `workerd serve` would serve it, in the REPL's
process. A config can be loaded more than once, and several configs can be loaded at the same time.

- The config's HTTP, HTTPS and TCP sockets listen on in-process names instead of their addresses.
  A config whose ports are in use still loads, and nothing outside the process can connect to it.
  Use `socket()` to send it requests. A UDP socket listens on its address.
- Outbound connections, such as those of a `network` or `external` service, reach the network as
  they do under `workerd serve`.
- The config's `v8Flags` and `autogates` are ignored, with a warning: those are set once per
  process, and the REPL has set them already.
- The Workers' `console.log()` writes to stdout.

`workerd.servers` lists the servers loaded and not yet closed. `workerd.version` is the version of
workerd.

### The server handle

| Member                                 | What it is                                                                                                                                      |
| -------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| `path`                                 | The config's path, as given to `loadConfig()`.                                                                                                  |
| `warnings`                             | The warnings reported while the config loaded.                                                                                                  |
| `closed`                               | Whether `close()` has been called.                                                                                                              |
| `service(name, { entrypoint, props })` | A `Fetcher` for the config's service `name`, for `fetch()` and RPC: its `entrypoint` (the default export if none), with `props` as `ctx.props`. |
| `socket(name)`                         | A `Fetcher` whose `fetch()` sends the request to the config's HTTP socket `name`, as a client connecting to the socket would.                   |
| `close()`                              | Drains the server and shuts it down. Resolves once it has; calling it again is harmless.                                                        |

`service()` reaches any service in the config, not only a Worker, as a binding to it would. It
throws if the config has no such service, or the Worker has no such entrypoint.

`socket()` takes HTTP sockets only, not HTTPS, TCP or UDP ones, and its `Fetcher` supports only
`fetch()`. The request goes through the socket as a client's would: the URL's host becomes the
`Host` header, the socket's HTTP options apply, and the Worker sees the request as it would see
one that arrived at the socket.

A `Fetcher` from `service()` or `socket()` can't be passed to another Worker, such as in an RPC
call to a loaded Worker; that fails with a `DataCloneError`.

### Closing

Closing a server revokes everything it handed out. Requests still in progress are canceled, and
every later use of a `Fetcher`, RPC stub or unread response body from that server fails with
`Error: The server for <path> was closed.` Quitting the REPL closes every server that is still
loaded, then waits for the REPL's `waitUntil()` tasks to finish.

## Limitations

- Line editing works on Unix terminals only. The cursor is drawn in the wrong place on a line with
  wide characters, such as CJK, or a line longer than the terminal is wide.
- There is no way to interrupt an input that is running except to end the REPL.
- The REPL has no inspector, so DevTools can't attach to it or to the loaded configs.

## How it works

This section is for people working on the REPL. The code is in `src/workerd/server/cli/repl/`
(Rust) and `src/workerd/server/repl/` (C++), and `src/workerd/server/AGENTS.md` maps the files.
The paths below are relative to `src/workerd/server/`.

**Two threads.** The console (`cli/repl/console.rs`) runs on a thread of its own, blocked reading
input, so that the event loop keeps running between inputs. It sends each finished input to the
session loop (`cli/repl/session.rs`), which runs on the event loop's thread and owns V8. Before it
sends an input, the console works out without a parser (`cli/repl/input.rs`) whether the input is
finished, and a few _candidates_: ways to compile it, such as a plain script or, for an input that
awaits at top level, an async function that returns its last statement's value. The session
evaluates the first candidate that compiles.

**The environment.** `ReplEnvironment` (`repl/repl-environment.h`) is a Worker that never receives
an event, compiled on the REPL's isolate under the new module registry. One `IoContext` serves the
whole session, so I/O objects created by one input stay usable in the next. The `workerd` global
and the server handles are C++ JSG types (`repl/repl-api.h`), registered with the workerd API
isolate but not part of the Workers API's types.

**Loaded configs.** `workerd.loadConfig()` reads the config as `workerd serve` does
(`repl_load_config`, `cli/repl/session.rs`), then builds a `WorkerFactory` for it over the REPL's
V8 and event loop and hands it to the Rust server's `InProcessServer` (`server/in_process.rs`).
`InProcessServer::start()` serves the config in the background, its stream sockets on `loopback:`
names. `ReplConfigServer` (`repl/repl-server.h`) is the handle the JavaScript API holds.

**Host channels.** `service()` and `socket()` are the in-process server's host channels
(`server/in_process/host.rs`). The REPL's `IoContext` keys headers by its own
`kj::HttpHeaderTable`, and each loaded server by its factory's, and a header id from one table
must not be used with headers of another. So the factory takes the REPL's table as
`WorkerFactory::Options::hostHeaderTable`, and headers cross between the two tables by name. A
socket's channel is an HTTP client of the socket's `loopback:` name. Every channel and every
request started on one is held through a revoker: closing the server revokes them all before the
server shuts down, so that nothing the REPL's JavaScript still holds keeps the factory alive.

**Tests.** `repl/repl-environment-test.c++` evaluates inputs and loads configs given as text;
`cli/repl/editor-test.rs`, `cli/repl/history-test.rs` and `cli/repl/input-test.rs` cover the line
editor, the history file and the input heuristics; `server/in_process/host-test.rs` covers
revocation.
