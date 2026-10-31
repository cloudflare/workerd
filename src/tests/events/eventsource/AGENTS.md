# EventSource

An informal specification of EventSource as implemented in workerd,
derived from, and kept in lockstep with, the test suite in this directory.
**The tests are the normative artifact.** Both the C++ implementation
(`src/workerd/api/eventsource.{h,c++}`) and the TypeScript implementation
(`src/per_isolate/eventsource/eventsource.ts`, behind the
`TS_EVENT_SOURCE` autogate) are covered. The TypeScript class subclasses
EventTarget through the seam in
`src/per_isolate/events/event-target-internals.ts` and does its I/O through
`EventSourceIo` (`src/workerd/api/eventsource-bootstrap.{h,c++}`), so
dispatch, fetch and request binding are shared; the divergences are in
class shape, in event stream parsing, and in how reads are driven.

## Interface

```webidl
[Exposed=Worker]
interface EventSource : EventTarget {
  constructor(USVString url, optional EventSourceInit eventSourceInitDict = {});
  static EventSource from(ReadableStream stream);  // Workers extension

  readonly attribute USVString url;
  readonly attribute boolean withCredentials;  // always false

  const unsigned short CONNECTING = 0;
  const unsigned short OPEN = 1;
  const unsigned short CLOSED = 2;
  readonly attribute unsigned short readyState;

  attribute EventHandler onopen;
  attribute EventHandler onmessage;
  attribute EventHandler onerror;
  undefined close();
};

dictionary EventSourceInit {
  boolean withCredentials = false;  // a truthy value throws NotSupportedError
  Fetcher fetcher;                  // Workers extension
};
```

- EventSource is always a global, with no compatibility flag.
- An EventSource can only be created within a request:
  `NotSupportedError`, from the constructor and from() alike.
- The URL must be absolute (`SyntaxError`); it is normalized as the URL
  parser does. Both arguments are converted, init's members in order,
  before anything is checked.
- `EventSource.from()` takes a ReadableStream that is neither locked nor
  disturbed, needs no receiver, and returns an EventSource that is
  already OPEN, with `url` `''`.
- `[Symbol.dispose]` is a function of its own that calls close().
- Every member is brand-checked and throws JSG's illegal-invocation
  TypeError for any other receiver. Members are enumerable, like every JSG
  member; names, lengths and constants' configurability follow
  `spec_compliant_property_attributes`.

## Core semantics

- **Request:** a GET through the init's `fetcher`, else the global
  fetch(), with `Accept: text/event-stream`, `Cache-Control: no-cache`, and
  `Last-Event-ID` on a reconnection that has one.
- **Response:** a status other than 200, or a content type other than
  `text/event-stream` (parameters allowed), fails the EventSource for good
  with an `AbortError`. A redirect moves `url` and the messages' `origin`.
  A response without a body is retried once, as an end of the stream; a
  second in a row fails the EventSource. A rejected fetch() closes it
  without reconnecting.
- **Reconnection:** the end of a connected EventSource's stream fires an
  'error' (`The server disconnected.`) in CONNECTING and reconnects after
  the reconnection time, 2 seconds unless a `retry` field set it, clamped to
  1 to 10 seconds. The end of an EventSource.from() stream closes it.
- **Parsing** follows the HTML standard in the TypeScript implementation;
  the C++ parser departs from it in the cases of ledger #4 to #9. The
  stream is decoded as UTF-8 with replacement. A string chunk from a
  from() stream is read as its UTF-8 bytes, any buffer or view as its
  bytes; anything else fails the EventSource with the TypeError the
  streams implementation's byte reads throw.
- **Events:** a trusted OpenEvent (whose class is not a global), trusted
  MessageEvents carrying `lastEventId` and the URL's origin (null for a
  from() source), and trusted ErrorEvents whose `error` says why. None
  bubbles or is cancelable.
- **Event handlers:** onopen, onmessage and onerror are HTML event handler
  attributes, positioned at first assignment and kept across reassignment;
  any other `on<type>` property is called as EventTarget calls it.
- **close()** is idempotent and fires nothing. Callbacks resumed by another
  request do nothing, and close() from another request throws.
- **Listener exceptions** under `spec_compliant_dispatch_exceptions`: a
  throwing 'open' or 'message' listener's exception is reported, the
  remaining listeners run, then the EventSource errors out with it and the
  rest of the messages are dropped. A throwing 'error' listener's exception
  is only reported. Without the flag the first throwing 'message' listener
  ends the dispatch, unreported, and errors the EventSource out.
- **Lifetime:** a pending read retains its EventSource; once the stream has
  ended, nothing does.
- **Prototype pollution:** the TypeScript implementation reaches built-ins
  only through primordials. The C++ implementation calls no built-in
  either, except that reading the TypeScript streams, the native pump
  resolves a promise with another, which looks up
  `Promise.prototype.then`; `pollution.js` leaves that unpatched for the
  `cpp-ts-streams` cell.

## Compatibility flags

| Flag (enable date) | Selects | Unflagged behavior tested by |
| --- | --- | --- |
| `set_tostring_tag` (2024-09-26) | `[object EventSource]` branding | `legacyToStringTag` |
| `spec_compliant_dispatch_exceptions` (2026-09-15) | reporting listener exceptions | `legacyThrowingMessageListener` |
| `spec_compliant_property_attributes` (dateless) | accessor names; lengths; non-configurable constants; the global as an own property | the main cells (the `-opt-in` cells enable it) |
| `immutable_api_prototypes` (dateless) | C++ constructor's `prototype` non-writable | the main cells (the `-opt-in` cells enable it) |
| `typescript_implemented_streams` (experimental) | the streams an EventSource reads | the main cells (the `-ts-streams` cells enable it) |

The main cells pin the two dated flags on; the legacy cells pin them off
by their disable names, so every cell keeps its `@all-compat-flags`
variant. The main cells also pin `nodejs_compat` (`node:assert`,
`node:util`), `enable_weak_ref` and `rpc` (for `gc.js`), and
`streams_enable_constructors`.

## Divergence ledger (C++ vs TypeScript)

| # | Area | C++ | TypeScript | Pinned in |
| --- | --- | --- | --- | --- |
| 1 | Constructor static inheritance | `getPrototypeOf(EventSource) === Function.prototype` | `=== EventTarget` | `prototypeChains` |
| 2 | Constructor's `prototype` | writable unless `immutable_api_prototypes` | never writable | `constructorAttributes` |
| 3 | Calling the constructor without `new` | JSG's TypeError | the language's TypeError | `constructorRequiresNew` |
| 4 | Empty `event` field | an event of type `''` | a 'message' | `emptyEventField` |
| 5 | BOM split across chunks | not stripped | stripped | `bomSplitAcrossChunks` |
| 6 | `id` field containing U+0000 | taken | ignored | `idWithNullIsIgnored` |
| 7 | `data` field with an empty value | dispatches nothing | dispatches empty data | `formatFieldData` |
| 8 | CRLF split across chunks | ends two lines | ends one | `crlfSplitAcrossChunks` |
| 9 | Last event ID across a reconnection | the last `id` field parsed | the last dispatched event's | `formatDataBeforeFinalEmptyLine` |
| 10 | A listener closes the EventSource | the rest of the messages read with that one are dispatched | dropped | `closeFromAListener` |
| 11 | close() of a from() source | goes on reading the stream | cancels it | `closeCancelsAFromStream` |
| 12 | `retry` value | anything KJ parses as a `uint32_t`; larger values ignored | ASCII digits only; any length, clamped | not pinned: observable only through reconnection timing |
| 13 | Message dispatch | from a task per write, held by the input gate | as each read completes | not pinned: no cheap observable |
| 14 | A from() source closed while a read is pending | retained until the read settles | collectable (its read is cancelled, #11) | not pinned: the C++ side is a leak |
| 15 | Collection after the last event | on the next major GC | one major GC later: an event wrapped before dispatch keeps its target alive until it is swept | not pinned: `gc.js` allows three collections |

The C++ implementation also logs a from() stream's non-byte chunk error
twice; the TypeScript one, once. Not in the ledger, as only the logs see
it.

## Assertion catalogue

| Module | Asserts |
| --- | --- |
| `api-surface.js` | static and prototype members, attributes, names and lengths; key order; prototype chains (#1); constructor attributes (#2); runtime class marks; toStringTag branding; dispose member; no own instance properties; the global |
| `inspect.js` | `util.inspect()` of an EventSource and of `EventSource.prototype` |
| `construction.js` | `new` required (#3); URL conversion and errors; absolute URLs; init, `withCredentials` and `fetcher` checks; conversion order; initial state; module scope refused |
| `illegal-invocation.js` | every member brand-checked against every non-EventSource receiver |
| `from.js` | argument checks; locked and disturbed streams; no receiver; synchronous open; reading to the end; chunk kinds; string chunks; non-byte chunks |
| `parser.js` | data joining; ignored lines; leading space; line endings; unterminated events; BOMs; UTF-8; named events; last event ID; #4 to #8; WPT format cases |
| `request.js` | request headers; a rejected fetch() closes |
| `response.js` | status; content type; redirects; responses without a body |
| `reconnection.js` | end of stream reconnects; Last-Event-ID; #9; `retry` |
| `event-shapes.js` | OpenEvent, MessageEvent and ErrorEvent shapes |
| `handlers.js` | default null; positional ordering; `on<type>` for a named event |
| `close.js` | idempotent and silent; dispose; closing from a listener (#10); cancelling a from() stream (#11) |
| `listener-exceptions.js` | throwing 'message', 'open' and 'error' listeners |
| `context.js` | cross-request callbacks and close(), with and without `handle_cross_request_promise_resolution`; close during reconnection; same-request delivery |
| `gc.js` | pending reads retain, ended streams release, for native and JavaScript sources and in a Durable Object |
| `pollution.js` | from() and connected delivery under patched built-ins |
| `legacy-shape.js` | `[object Object]` |
| `legacy-listener-exceptions.js` | the first throwing listener ends the dispatch, unreported |
| `server.js` | the event streams connected EventSources fetch, through `subrequest` |
| `context-worker.js` | `context.js`'s fixture services |
| `which-impl.js` | the cell's implementation and the flags the suite branches on |
| `helpers.js` | descriptor attribute strings; streams; event waiting and recording |

## Web Platform Tests

`src/wpt` runs the eventsource WPTs, but most need a base URL to resolve
relative URLs, which workerd lacks, or browser features (workers, CORS,
credentials). The format cases are adapted here instead, through
EventSource.from() or `server.js`: `format-bom-2`,
`format-data-before-final-empty-line`, `format-field-data`,
`format-field-event-empty`, `format-field-parsing`,
`format-leading-space`, `format-newlines` and `format-null-character`.

## Tests that live elsewhere (by design)

- `src/workerd/api/eventsource-test.c++`: a unit test of the C++
  implementation's native pump, removed with it.
- `eventSourceLegacyError` in
  `src/workerd/api/tests/legacy-dispatch-exceptions-test.js`, which pins
  the legacy dispatch policy across several APIs at once.
