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

Or name the config on the command line, and the REPL loads it into `s` before the first prompt:

```sh
bazel-bin/src/workerd/server/workerd repl --experimental config.capnp
```

Press Ctrl+D, or type `.exit`, to quit.

## Command line

```
workerd repl --experimental [options] [<config-file> [<const-name>]]
```

A `<config-file>` is loaded into the global `s`, as
`s = await workerd.loadConfig('<config-file>', { constName: '<const-name>' })` would load it. The
REPL shows that input at its first prompt and prints its result. A config that fails to load is
reported like any failed input, and the REPL carries on with `s` unset.

| Option                 | Effect                                                                                                                                                                                                                     |
| ---------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `--experimental`       | Required. Also lets the compatibility flags and the loaded configs use experimental features, as it does for `workerd serve`, and turns on `service_binding_extra_handlers` (see [the server handle](#the-server-handle)). |
| `--compat-date=<date>` | The compatibility date of the REPL's JavaScript. Defaults to today. Any valid date is accepted, as for `workerd test`; `2999-12-31` gives every flag's behavior.                                                           |
| `--compat-flag=<flag>` | Enables a compatibility flag in the REPL's JavaScript. Repeat it for more. `nodejs_compat` is on unless a flag turns it, or `nodejs_compat_v2`, on or off explicitly.                                                      |
| `--autogate=<name>`    | Enables an autogate, named as in `WORKERD_AUTOGATES` but in kebab-case, such as `compression-rs`. Repeat it for more. A name that is no autogate is an error.                                                              |
| `--v8-flag=<flag>`     | Passes a flag to V8, such as `--v8-flag=--expose-gc`. Repeat it for more. A flag V8 does not know is an error.                                                                                                             |

The compatibility settings apply to the JavaScript typed into the REPL. Each loaded config's
Workers use their own settings from the config.

Autogates and V8 flags are set once for the process, so they apply to the REPL's JavaScript and to
every config it loads.

Results are colored when stdout is a terminal, unless `NO_COLOR` is set.

## Typing input

An input runs once it is finished. While a bracket, a template literal or a `/* */` comment is still
open, Enter continues the input on the next line, at a `...` prompt:

```
> function f() {
...   return 42
... }
undefined
> f()
42
```

Enter also continues the input after a line that ends inside a string with a backslash. A string
left open at the end of any other line is a syntax error, as it is in JavaScript.

The REPL decides this without parsing JavaScript, so it can misjudge a `/` that could start either
a regular expression or a division: it takes a `/` after `}` to start a regular expression, and
one after `)` to divide.

Each result is printed with Node's `util.inspect()`. An exception prints as `Uncaught` followed by
the error and its stack.

As in Node's REPL, `_` holds the last result and `_error` the last exception. Assigning to one of
them keeps the assigned value: it no longer follows the inputs.

### Top-level `await`

An input that uses `await` outside a function runs inside an async function, and the REPL prints
what that function's last statement evaluates to, once it settles. The event loop keeps running
while the REPL waits, so timers, `fetch()` and the loaded servers all make progress.

A declaration inside an async function would be local to it. So in an input that awaits at top
level, a `const`, `let` or `var` declaration at the input's top level (not in a block) that gives
each of its names a value, such as `const x = await f()`, becomes an assignment to a global, which
later inputs can see. Other declarations stay local to the input: one with a destructuring pattern
(assign instead, as in `({ a } = await f())`), one with a name it gives no value, such as
`let x;`, one in a block, and a function or class declaration.

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

| Key                   | Effect                                                                                  |
| --------------------- | --------------------------------------------------------------------------------------- |
| Enter                 | Submits the line. The input runs if it is finished.                                     |
| Left, Right, Ctrl+B/F | Moves the cursor by a character.                                                        |
| Home, End, Ctrl+A/E   | Moves the cursor to the start or end of the line.                                       |
| Up, Down, Ctrl+P/N    | Steps through earlier lines, including earlier sessions' (see [History](#history)).     |
| Backspace, Delete     | Deletes the character before or under the cursor.                                       |
| Ctrl+K, Ctrl+U        | Deletes to the end, or to the start, of the line.                                       |
| Ctrl+W                | Deletes the word before the cursor.                                                     |
| Ctrl+L                | Clears the screen.                                                                      |
| Tab                   | Completes a global's name, or a property name after `a.b.`; see below.                  |
| Ctrl+C                | Abandons the input being typed. While an input runs, see [Interrupting](#interrupting). |
| Ctrl+D                | Quits on an empty line; otherwise deletes the character under the cursor.               |

Tab completes the name before the cursor. With a dot, the names are the properties, own and
inherited, of the value before it (an identifier chain such as `s.service`); without one, they are
the global scope's. Completing evaluates that chain, so it can run property getters. Tab inserts as
much as the candidates have in common; when there is nothing more to insert, it lists them.

Pasted text is taken as text in a terminal that marks pastes (bracketed paste mode, which most
terminals support): its tabs are inserted rather than completing, and its other control characters
and escape sequences are dropped. Each line of a paste is submitted as Enter submits a line, so a
pasted multi-line input runs once its last line is in.

### Interrupting

On Unix, Ctrl+C while an input awaits stops awaiting it, whether or not input comes from a
terminal. The REPL says `Interrupted`, and prompts for the next input. The input's promise isn't
canceled, and what it started, such as a request, goes on; `_` and `_error` stay as they were.
JavaScript that runs without awaiting, such as a loop, can't be stopped that way: press Ctrl+C
again to end the REPL. On Windows, Ctrl+C ends the REPL.

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

`await workerd.loadConfig(path, { constName, watch })` loads a config and starts serving it. It resolves
to a server handle once every socket listens. `path` is relative to the working directory, and
names a Cap'n Proto config or a `wrangler.json` / `wrangler.jsonc`, read as `workerd serve` reads
it. `constName` names the constant that holds a Cap'n Proto config, as `workerd serve`'s second
argument does. If the config doesn't load or has errors, the promise rejects with an error that
lists them, and nothing is left running. With `watch: true`, the config [reloads](#reloading)
whenever a file it was loaded from changes.

Each config is served by a server of its own, as `workerd serve` would serve it, in the REPL's
process. A config can be loaded more than once, and several configs can be loaded at the same time.

- The config's HTTP, HTTPS and TCP sockets listen on in-process names instead of their addresses.
  A config whose ports are in use still loads, and nothing outside the process can connect to it.
  Use `socket()` to send it requests. A UDP socket listens on its address.
- Outbound connections, such as those of a `network` or `external` service, reach the network as
  they do under `workerd serve`.
- The config's `v8Flags` and `autogates` are ignored, with a warning: those are set once per
  process, and the REPL has set them already. Pass `--v8-flag` and `--autogate` to the REPL
  instead.
- The Workers' `console.log()` writes to stdout.

`workerd.servers` lists the servers loaded and not yet closed. `workerd.version` is the version of
workerd. `workerd.evict()` evicts a [Durable Object](#durable-objects).

### The server handle

| Member                                   | What it is                                                                                                                                      |
| ---------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| `path`                                   | The config's path, as given to `loadConfig()`.                                                                                                  |
| `warnings`                               | The warnings reported while the config loaded, or last reloaded.                                                                                |
| `closed`                                 | Whether `close()` has been called.                                                                                                              |
| `watching`                               | Whether a change to a file the config was loaded from reloads it: `loadConfig()` was given `watch: true`, and the server isn't closed.          |
| `service(name, { entrypoint, props })`   | A `Fetcher` for the config's service `name`, for `fetch()` and RPC: its `entrypoint` (the default export if none), with `props` as `ctx.props`. |
| `socket(name)`                           | A `Fetcher` whose `fetch()` sends the request to the config's HTTP socket `name`, as a client connecting to the socket would.                   |
| `durableObject(service, className, key)` | A stub for the [Durable Object](#durable-objects) `key` names, of the class `className` that the config's service `service` exports.            |
| `reload()`                               | Loads the config again and serves it in place of the old one. Resolves to the handle once the new server is serving.                            |
| `close()`                                | Drains the server and shuts it down. Resolves once it has; calling it again is harmless.                                                        |

`service()` reaches any service in the config, not only a Worker, as a binding to it would. It
throws if the config has no such service, or the Worker has no such entrypoint.

The REPL's JavaScript has the `service_binding_extra_handlers` flag, so a `Fetcher` from
`service()` also runs the Worker's `scheduled()` and `queue()` handlers:

```
> await s.service('main').scheduled({ cron: '*/5 * * * *' })
{ outcome: 'ok', noRetry: false }
> (await s.service('main').queue('jobs', [{ id: '1', timestamp: new Date(), attempts: 1, body: { n: 1 } }])).outcome
'ok'
```

`socket()` takes HTTP sockets only, not HTTPS, TCP or UDP ones, and its `Fetcher` supports only
`fetch()`. The request goes through the socket as a client's would: the URL's host becomes the
`Host` header, the socket's HTTP options apply, and the Worker sees the request as it would see
one that arrived at the socket.

A `Fetcher` from `service()`, `socket()` or `durableObject()` can't be passed to another Worker,
such as in an RPC call to a loaded Worker; that fails with a `DataCloneError`.

### Durable Objects

`s.durableObject(service, className, key)` is a stub for one of the config's Durable Objects, for
RPC and `fetch()`, as a binding to the class would reach it. `key` is the object's name, as
`idFromName()` takes it, `{ name }`, or `{ id }` with the object's id as `idFromString()` takes it.
The objects of an ephemeral class have names but no ids.

`await workerd.evict(stub, { webSockets })` evicts the object behind a stub, as the runtime evicts
an idle one, and as `evict()` from `cloudflare:unsafe` does. Its durable storage is kept, and on
its next request it starts again, running its constructor. Its hibernatable WebSockets hibernate,
or close with `webSockets: 'close'`. The promise rejects if the object isn't running.

```
> c = s.durableObject('main', 'Counter', 'alice')
> await c.increment()
1
> await workerd.evict(c)
undefined
```

### Reloading

`await s.reload()` reads the config again, as `loadConfig()` read it, and starts a new server on
it. Once that is serving, it takes the old server's place behind the handle, and the old server is
closed as `close()` would close it: what it handed out fails with
`Error: The server for <path> was reloaded.`, so ask the handle for new `Fetcher`s. Durable
Objects and other state held in memory start afresh. If the config no longer loads, or the new
server doesn't start, the promise rejects with the problems and the old server runs on.

A config loaded with `watch: true` reloads by itself whenever one of the files it was loaded from
changes: the config, the files it imports and embeds, or for a `wrangler.json`, the Worker's
modules. The REPL waits until changes stop arriving for half a second, then reloads, and reports
the outcome on stderr, after whatever the console has written so far, such as a prompt:

```
> s = await workerd.loadConfig('config.capnp', { watch: true })
ReplServer {
  path: 'config.capnp',
  closed: false,
  watching: true,
  warnings: [ [length]: 0 ]
}
> Reloaded config.capnp: a file it was loaded from changed.
```

A reload that fails is reported the same way, and the next change reloads again, so fixing the
config brings the server up to date. Reloads, asked for or not, run one at a time.

### Closing

Closing a server revokes everything it handed out. Requests still in progress are canceled, and
every later use of a `Fetcher`, RPC stub or unread response body from that server fails with
`Error: The server for <path> was closed.` Quitting the REPL closes every server that is still
loaded, then waits for the REPL's `waitUntil()` tasks to finish.

## Limitations

- Line editing works on Unix terminals only. The cursor is drawn in the wrong place on a line with
  wide characters, such as CJK, or tabs, or a line longer than the terminal is wide.
- Only an input that awaits can be [interrupted](#interrupting) without ending the REPL.
- A Durable Object's alarm runs when it is due, and not before: an alarm runs only at the time it
  is set for, which only the object can change.
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
evaluates the first candidate that compiles, except that it passes over the plain script if the
script uses `await` as a name: a script reads `await (x)` as a call to a function named `await`.
Compiling the script in a class static block, where `await` can't be a name, tells.

**The environment.** `ReplEnvironment` (`repl/repl-environment.h`) is a Worker that never receives
an event, compiled on the REPL's isolate under the new module registry. One `IoContext` serves the
whole session, so I/O objects created by one input stay usable in the next. The `workerd` global
and the server handles are C++ JSG types (`repl/repl-api.h`), registered with the workerd API
isolate but not part of the Workers API's types.

**Loaded configs.** `workerd.loadConfig()` reads the config as `workerd serve` does
(`repl_load_config`, `cli/repl/session.rs`), then builds a `WorkerFactory` for it over the REPL's
V8 and event loop and hands it to the Rust server's `InProcessServer` (`server/in_process.rs`).
`InProcessServer::start()` serves the config in the background, its stream sockets on `loopback:`
names. `ReplConfigServer` (`repl/repl-server.h`) is the handle the JavaScript API holds. A reload
does all of that again and swaps the new `InProcessServer` in before closing the old one. The
config loader (`ReplConfigLoader`) is an interface so that the C++ tests can serve configs given as
text. Its watches are the CLI's file watcher (`cli/watch.rs`, as `serve --watch` uses it), which
`repl_load_config` registers each file it reads with.

**Host channels.** `service()` and `socket()` are the in-process server's host channels
(`server/in_process/host.rs`). The REPL's `IoContext` keys headers by its own
`kj::HttpHeaderTable`, and each loaded server by its factory's, and a header id from one table
must not be used with headers of another. So the factory takes the REPL's table as
`WorkerFactory::Options::hostHeaderTable`, and headers cross between the two tables by name. A
socket's channel is an HTTP client of the socket's `loopback:` name. Every channel and every
request started on one is held through a revoker: closing the server revokes them all before the
server shuts down, so that nothing the REPL's JavaScript still holds keeps the factory alive.

**Tests.** `repl/repl-environment-test.c++` evaluates inputs and loads configs given as text,
with watches the test fires; `tests/repl/repl-test.mjs` runs the `workerd` binary for what only
the command line, the console and the file watcher do;
`cli/repl/editor-test.rs`, `cli/repl/history-test.rs` and `cli/repl/input-test.rs` cover the line
editor, the history file and the input heuristics; `server/in_process/host-test.rs` covers
revocation.
