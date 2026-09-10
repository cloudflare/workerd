# AbortSignal internals: cancellation hooks and the cross-request model

This document describes how `api::AbortSignal` (`src/workerd/api/basics.h`) delivers aborts
to native (C++) consumers, and which primitive to use when hooking work to a signal. It is
aimed at runtime code; the JS-visible behavior follows the WHATWG DOM spec.

## State model

An `AbortSignal` is a plain JS-heap object. Its abort state — `maybeAbortException` (a
`kj::Exception` derived from the abort reason) and `reason` (the JS value) — requires no
`IoContext` to create or read. Consequently:

- `new AbortController()`, `AbortSignal.abort()`, and objects that allocate signals (e.g.
  `WritableStream`, `request.signal`) work at global scope, during module evaluation.
- A single signal may be created in one request, observed in another, and aborted from a
  third (or from outside any request). JS-visible effects of an abort (state, events) happen
  synchronously in whichever context triggers it.

What *does* involve I/O — cancelling KJ promises, notifying RPC peers — is handled through
per-registration cells bound to the registering request, described below.

## Choosing a primitive

| You have...                                                    | Use                                     |
| -------------------------------------------------------------- | ---------------------------------------- |
| A `kj::Promise` to cancel on abort                               | `signal->wrap(js, promise)`               |
| A KJ-side object needing promise wrapping + a cancel callback | `signal->newCanceler(js)` + `AbortableImpl` |
| A native callback to run on abort, in your request's context  | `signal->addAbortAction(js, fn)`          |
| A JS-heap reaction (no IoContext involvement)                 | `signal->addAbortAlgorithm(js, fn)`       |

All four are no-ops (or immediate, see below) for `NEVER_ABORTS` signals, and all native
variants require an active `IoContext` at registration time.

### `wrap(js, promise)`

Returns a promise that rejects with a `kj::Exception` derived from the abort reason when the
signal aborts. If the signal is already aborted, the returned promise rejects immediately the
same way — indistinguishable from an abort arriving right after wrapping. Callers that want
to surface the JS reason with value identity (e.g. `fetch`) should pre-check `getAborted()`
and use `getReason()` themselves.

### `newCanceler(js)`

Returns `Cancellation{canceler, registration}`:

- `canceler` is a solely-owned `ReleasingCanceler` (`src/workerd/util/canceler.h`): wrap I/O
  promises through it, register `ReleasingCanceler::Listener`s for cancel callbacks. Its
  destructor *releases* (never cancels) still-wrapped promises; a `Listener` registered
  after cancellation fires immediately.
- `registration` keeps the canceler hooked to the signal. **Destroy the registration before
  the canceler** (declare it after the canceler member): the signal reaches the canceler by
  reference, valid only while the registration exists.

### `addAbortAction(js, fn)`

Registers `fn(js, exception)` to run at most once when the signal aborts, always under the
isolate lock, always in the IoContext current at registration time:

- Abort triggered in that context: `fn` runs synchronously during the abort.
- Abort triggered elsewhere (another request, or no request): delivery is deferred into the
  owning context via `IoCrossContextExecutor::tryExecute` and runs on its next turn. The
  deferred task re-takes the registration slot on arrival, so a consumer that went away in
  the meantime turns the delivery into a guaranteed no-op.
- Owning context already destroyed: the delivery is silently dropped — everything the action
  wanted to touch died with the context.

Dropping the returned handle (safe from any thread) guarantees `fn` never runs again, so
`fn` may capture plain references whose lifetime the holder ties to the handle. This is also
the single point that arms the RPC abort subscription for deserialized signals — every
native registration path funnels through it.

`ExecProcess`'s kill-on-abort is the canonical direct consumer.

### `addAbortAlgorithm(js, fn)`

The DOM spec's "add an algorithm to signal's abort algorithms", for reactions whose state
lives entirely on the JS heap. Runs under the isolate lock in *whichever* context triggers
the abort, before the `abort` event is dispatched; never runs for synthetic
`dispatchEvent('abort')` calls. The handle holds only a `jsg::WeakRef` to the signal and must
be dropped under the isolate lock. Capture JS-heap objects weakly (`JSG_THIS_WEAK`) unless
the signal genuinely should keep them alive: algorithms are owned and GC-visited by the
signal, so a strong capture makes a long-lived signal retain the captured object.

`addEventListener`'s `{signal}` option is the canonical consumer.

## Abort sequence

`triggerAbort` maps 1:1 onto the spec's "signal abort":

1. Record the abort reason (and exception) on this signal.
2. Record it on every not-yet-aborted dependent signal (`AbortSignal.any()` results), before
   any events fire anywhere.
3. Run this signal's abort steps: abort algorithms (FIFO, then emptied) → native
   registrations (routed per owner as above) → RPC clones (reason serialized once) → fire
   the `abort` event (listener exceptions are reported, not propagated; `abort()` cannot
   throw) → drop all listeners.
4. Run each collected dependent's abort steps, unlinking it from any remaining sources.

## Lifetime and reclamation

Native and RPC registrations are `kj::Arc`'d cells: an immutable
`IoCrossContextExecutor` plus a mutex-guarded slot holding the context-bound payload. The
consumer-side RAII handle clears the slot from any thread; because handles are attached to
request-owned objects (the wrapped promise, the `AbortableImpl`, ...), IoContext teardown
reclaims payloads automatically without touching the signal. Empty or defunct-context cells
are swept on the next registration, bounding a long-lived signal's footprint by its live
registrations (asserted by `crossRequestRegistrationChurn` in
`src/workerd/api/tests/abortsignal-test.js` via `getNativeRegistrationCountForTest()`).

Cells hold no JS-heap references and need no GC visitation; abort algorithms do, and are
visited by the signal.

## Signals received over RPC

A deserialized signal's pending abort reason arrives through a mutex-guarded, atomically
refcounted box (`ExternalPusherImpl::PendingAbortReasonBox`) written by the receiving
request's RPC machinery and readable from any context — so `getAborted()`/`getReason()`
answer correctly even for signals that crossed request boundaries before the abort was
processed. Actually *reacting* to the abort (running `triggerAbort`) requires the
subscription armed via `subscribeToRpcAbort()`, which happens automatically when an `abort`
listener, `onabort` handler, or any native registration is added. Only the receiving
request's context can arm it — the underlying promise belongs to that request — so arming
requested from any other context is routed there through the signal's
`IoCrossContextExecutor` and takes effect on the receiving request's next turn. If the
receiving request is already gone, the routing is dropped: no delivery is possible anymore.
Polling stays accurate regardless (if the peer could still abort, that request's teardown
wrote an implicit connection-lost abort into the box), and registrations made after that
point observe the abort through the already-aborted pre-checks instead.
