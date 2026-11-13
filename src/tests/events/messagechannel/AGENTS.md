# MessageChannel and MessagePort

An informal specification of MessageChannel and MessagePort as implemented
in workerd, derived from, and kept in lockstep with, the test suite in this
directory. **The tests are the normative artifact.** The implementation is
`src/per_isolate/messagechannel/messagechannel.ts`, whose classes subclass
the C++ EventTarget through the seam in
`src/per_isolate/events/event-target-internals.ts` and clone messages with
the runtime's serializer. `cloudflare-internal:messagechannel`
(`src/workerd/api/messagechannel.h`) hands the same classes to
`node:worker_threads`.

## Interface

```webidl
[Exposed=Worker]
interface MessageChannel {
  constructor();
  readonly attribute MessagePort port1;  // own data properties, not accessors
  readonly attribute MessagePort port2;
};

[Exposed=Worker]
interface MessagePort : EventTarget {
  undefined postMessage(any message,
                        optional (sequence<any> or PostMessageOptions) options);
  undefined start();
  undefined close();
  attribute EventHandler onmessage;
};
dictionary PostMessageOptions { sequence<any> transfer = []; };
```

- The classes are globals under `expose_global_message_channel`, and are
  always reachable through `node:worker_threads`, which hands out the same
  constructors.
- `new MessagePort()` throws `TypeError: Illegal constructor`, from a
  subclass too. MessageChannel is subclassable. Calling either constructor
  without `new` throws the language's TypeError.
- Every member is brand-checked and throws JSG's illegal-invocation
  TypeError for any other receiver, an object inheriting from
  `MessagePort.prototype` included.
- Methods and accessors are enumerable, like every JSG member; accessor
  names and lengths follow `spec_compliant_property_attributes`.
- Where the classes do not look like JSG's: MessagePort's constructor
  inherits from EventTarget's, as WebIDL requires; both constructors'
  `prototype` is non-writable whatever `immutable_api_prototypes` says;
  `constructor` is the first of `MessagePort.prototype`'s own string keys;
  and postMessage() recognizes a proxy for an array as an array
  (`Array.isArray()`), where JSG converts it as a dictionary.

## Core semantics

- **Transfer:** unsupported. A non-empty transfer list throws
  `Error: Transfer list is not supported`, after the options and every
  list element have been read and before the message is serialized.
- **Cloning:** postMessage() structured-clones its message first, so an
  uncloneable value throws `DataCloneError` (SharedArrayBuffer a
  `TypeError`) even on a closed pair, and getters run. The peer is looked
  for afterwards, so a getter that closes the port drops the message.
- **Delivery:** deferred (never inside postMessage()), in order, as a
  trusted MessageEvent whose `source` and `target` are the receiving port,
  `origin` null, `ports` empty.
- **The port message queue** follows the port's 'message' listeners, as in
  Node.js: the first one (onmessage or addEventListener()) starts delivery,
  and removing the last, by `removeEventListener()`, `once` or an
  AbortSignal, returns the port to queueing. `start()` starts delivery
  without a listener, so queued messages go to nobody. Per spec, only
  onmessage or start() would enable the queue.
- **onmessage** is an HTML event handler attribute: positioned at first
  assignment, kept across reassignment, repositioned after being cleared.
  A non-callable object is retained and starts the port but is never
  called; a primitive clears it. The handler's `this` is the global object.
- **close()** closes both ports and fires a trusted 'close' on each, the
  peer's first; a throwing 'close' listener's exception escapes close()
  under every dispatch policy. A closed port is terminal. A port whose
  peer is collected closes without a 'close' event, when it next looks for
  the peer, at a time that depends on the collector (not pinned).
- **Listener exceptions** under `spec_compliant_dispatch_exceptions`: each
  is reported, the remaining listeners run, then the port dispatches a
  'messageerror' whose `data` is the exception. Without the flag the first
  throwing listener ends the dispatch and its exception is redispatched as
  a 'message'; if that throws too, the delivery's reaction rejects,
  unhandled.
- **Prototype pollution:** delivery and listener bookkeeping call nothing
  user code can patch.

## Compatibility flags

| Flag (enable date) | Selects | Unflagged behavior tested by |
| --- | --- | --- |
| `expose_global_message_channel` (2025-08-15) | classes as globals | `legacyClassesAreNotGlobals`, `legacyClassesThroughWorkerThreads` |
| `set_tostring_tag` (2024-09-26) | `[object MessagePort]` branding | `legacyToStringTag` |
| `spec_compliant_dispatch_exceptions` (2026-09-15) | reporting and 'messageerror' | `legacyExceptionIsRedispatched`, `legacyEscapingExceptionIsAnUnhandledRejection` |
| `spec_compliant_property_attributes` (dateless) | accessor names and lengths; globals as own properties | the main cell (the `-opt-in` cell enables it) |
| `immutable_api_prototypes` (dateless) | nothing: the constructors' `prototype` is non-writable regardless | `constructorAttributes` (the `-opt-in` cell enables it) |

The main cell pins the three dated flags on; the legacy cell pins them off
by their disable names, so every cell keeps its `@all-compat-flags`
variant. `nodejs_compat` and `enable_nodejs_worker_threads_module` are
pinned everywhere for `node:assert`, `node:test`, `node:util` and
`node:worker_threads`.

## Assertion catalogue

| Module | Asserts |
| --- | --- |
| `api-surface.js` | member attributes, names and lengths; key order; prototype chains; constructor attributes; runtime class marks; toStringTag branding; MessageChannel's own `port1`/`port2`; globals and `node:worker_threads` identity |
| `inspect.js` | `util.inspect()` of a port, a channel and `MessagePort.prototype` |
| `construction.js` | MessagePort not constructible, from a subclass either; `new` required; MessageChannel subclassable |
| `illegal-invocation.js` | every member brand-checked against every non-port receiver |
| `post-message-arguments.js` | options conversion errors; accepted empty forms; `transfer` type; non-empty lists refused; read order; proxied arrays |
| `cloning.js` | SharedArrayBuffer and uncloneable values; closed pairs still serialize; closing during serialization drops the message; the receiver gets a copy; Blob round trip |
| `delivery.js` | both directions, before and while listening; event shape; deferral and order |
| `message-event.js` | user-constructed MessageEvent with port `source` and `ports` |
| `onmessage.js` | default null; positional ordering; repositioning after clearing; non-callable and primitive values; handler receiver |
| `port-queue.js` | starting by listener and by start(); requeueing on removal, `once` and abort; closed ports terminal; closed pairs drop |
| `close.js` | 'close' on both ports, peer first, idempotent; plain `onclose` property; throwing 'close' listener escapes |
| `listener-exceptions.js` | spec reporting order, 'messageerror', injected messages; throwing 'messageerror' listener |
| `gc.js` | queued messages survive collection, including during start(); ports survive collection |
| `pollution.js` | delivery under patched `Promise.prototype`, `Array.prototype.push` and `WeakRef.prototype.deref` |
| `legacy-shape.js` | not globals; usable through `node:worker_threads`; `[object Object]` |
| `legacy-listener-exceptions.js` | redispatch as 'message'; escaping exception as an unhandled rejection |
| `flags.js` | the flags the suite branches on |
| `helpers.js` | descriptor attribute strings |

## Web Platform Tests

Most of the webmessaging WPTs need ports to be transferable or run under
web workers in HTML files, so only cases from
`Channel_postMessage_Blob.any.js` and
`Channel_postMessage_DataCloneErr.any.js` are adapted here (`cloning.js`).
Known not to pass:

- `Channel_postMessage_clone_port.any.js`
- `Channel_postMessage_clone_port_error.any.js`
- `Channel_postMessage_ports_readonly_array.any.js`
- `Channel_postMessage_transfer_xsite_incoming_messages.window.js`
- `Channel_postMessage_with_transfer_incoming_messages.any.js`
- `Channel_postMessage_with_transfer_outgoing_messages.any.js`
- `message-channels/close-event/garbage-collected.tentative.any.js`: per
  spec, a port's 'close' fires when its peer is collected; workerd does
  not fire it.
