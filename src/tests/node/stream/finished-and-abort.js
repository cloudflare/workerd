// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// stream.finished() / stream/promises finished() and stream.addAbortSignal()
// applied to web streams. Both need to observe a web stream's completion, or
// error it, WITHOUT taking its lock, which the standard API cannot do; Node
// does it through two well-known symbols its own web streams carry:
// Symbol.for('nodejs.webstream.isClosedPromise') (an object whose promise
// settles with the stream) and
// Symbol.for('nodejs.webstream.controllerErrorFunction') (errors the stream
// as its controller would).
//
// The two implementations diverge wholesale here. The TypeScript streams
// carry both hooks as non-enumerable prototype members, so the APIs work as
// in Node — with one deliberate difference: the readable error hook errors
// byte streams too (a Response body included), where Node's is a no-op for
// byte stream controllers. The C++ streams carry neither hook, and the APIs
// fail up front with ERR_WEB_STREAM_INTEROP_UNSUPPORTED rather than
// partially working.

import { finished, addAbortSignal, promises } from 'node:stream';
import { strictEqual, throws, rejects, ok } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const kIsClosedPromise = Symbol.for('nodejs.webstream.isClosedPromise');
const kControllerErrorFunction = Symbol.for(
  'nodejs.webstream.controllerErrorFunction'
);

const unsupported = (api) => ({
  name: 'TypeError',
  code: 'ERR_WEB_STREAM_INTEROP_UNSUPPORTED',
  message: `${api} is not supported for web streams by the streams implementation in use`,
});

function callbackOf(fn) {
  return new Promise((resolve) => fn(resolve));
}

// Whether a promise has settled within a short grace period.
function settlement(promise, ms = 20) {
  return Promise.race([
    promise.then(
      () => 'settled',
      () => 'settled'
    ),
    scheduler.wait(ms).then(() => 'pending'),
  ]);
}

// A source of the given kind whose controller is captured.
function sourceStream(bytes, extra = {}) {
  let controller;
  const stream = new ReadableStream({
    ...(bytes ? { type: 'bytes' } : {}),
    start(c) {
      controller = c;
    },
    ...extra,
  });
  return { stream, controller, chunk: bytes ? new Uint8Array([1]) : 'a' };
}

// The hooks' presence per implementation. Under TypeScript they live on the
// prototypes (a getter and a method, both non-enumerable), never as own
// instance properties; under C++ they do not exist at all.
export const interopHooksPresence = {
  test() {
    for (const [proto, instance] of [
      [ReadableStream.prototype, new ReadableStream()],
      [WritableStream.prototype, new WritableStream()],
    ]) {
      const closedDescriptor = Object.getOwnPropertyDescriptor(
        proto,
        kIsClosedPromise
      );
      const errorDescriptor = Object.getOwnPropertyDescriptor(
        proto,
        kControllerErrorFunction
      );
      if (usingTsImpl) {
        strictEqual(typeof closedDescriptor.get, 'function');
        strictEqual(closedDescriptor.enumerable, false);
        strictEqual(typeof errorDescriptor.value, 'function');
        strictEqual(errorDescriptor.enumerable, false);
        strictEqual(Object.hasOwn(instance, kIsClosedPromise), false);
        strictEqual(Object.hasOwn(instance, kControllerErrorFunction), false);
        ok(instance[kIsClosedPromise].promise instanceof Promise);
        // Repeated requests observe the same underlying promise.
        strictEqual(
          instance[kIsClosedPromise].promise,
          instance[kIsClosedPromise].promise
        );
      } else {
        strictEqual(closedDescriptor, undefined);
        strictEqual(errorDescriptor, undefined);
        strictEqual(instance[kIsClosedPromise], undefined);
        strictEqual(instance[kControllerErrorFunction], undefined);
      }
    }
  },
};

// finished() on a web ReadableStream: called back without error once the
// stream closes, without ever locking it.
export const finishedObservesReadableClose = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
    });
    if (!usingTsImpl) {
      throws(() => finished(rs, () => {}), unsupported('finished()'));
      strictEqual(rs.locked, false);
      return;
    }
    const done = callbackOf((cb) => finished(rs, cb));
    strictEqual(rs.locked, false);
    controller.enqueue(new Uint8Array([1]));
    controller.close();
    // Closing takes effect once the queue drains: read it out.
    const reader = rs.getReader();
    await reader.read();
    strictEqual((await reader.read()).done, true);
    strictEqual(await done, undefined);
  },
};

// finished() on a web ReadableStream that errors: called back with the
// stream's error instance.
export const finishedObservesReadableError = {
  async test() {
    if (!usingTsImpl) return;
    let controller;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
    });
    const done = callbackOf((cb) => finished(rs, cb));
    const boom = new Error('errored');
    controller.error(boom);
    strictEqual(await done, boom);
  },
};

// finished() on a web WritableStream: close resolves it, an error rejects it
// with the error instance.
export const finishedObservesWritable = {
  async test() {
    const closing = new WritableStream();
    if (!usingTsImpl) {
      throws(() => finished(closing, () => {}), unsupported('finished()'));
      return;
    }
    const closed = callbackOf((cb) => finished(closing, cb));
    await closing.getWriter().close();
    strictEqual(await closed, undefined);

    let controller;
    const erroring = new WritableStream({
      start(c) {
        controller = c;
      },
    });
    const errored = callbackOf((cb) => finished(erroring, cb));
    const boom = new Error('sink errored');
    controller.error(boom);
    strictEqual(await errored, boom);
  },
};

// finished() on a stream that has already settled still calls back.
export const finishedOnSettledStream = {
  async test() {
    if (!usingTsImpl) return;
    const closed = new ReadableStream({
      start(controller) {
        controller.close();
      },
    });
    strictEqual(await callbackOf((cb) => finished(closed, cb)), undefined);
    const boom = new Error('already errored');
    const errored = new ReadableStream({
      start(controller) {
        controller.error(boom);
      },
    });
    strictEqual(await callbackOf((cb) => finished(errored, cb)), boom);
  },
};

// finished() with a signal: aborting calls back with an AbortError carrying
// the signal's reason, and the stream itself is untouched.
export const finishedWithSignal = {
  async test() {
    if (!usingTsImpl) return;
    const rs = new ReadableStream();
    const controller = new AbortController();
    const done = callbackOf((cb) =>
      finished(rs, { signal: controller.signal }, cb)
    );
    const reason = new Error('stop waiting');
    controller.abort(reason);
    const err = await done;
    strictEqual(err.name, 'AbortError');
    strictEqual(err.cause, reason);
    strictEqual(rs.locked, false);
    rs.getReader().releaseLock();
  },
};

// stream/promises finished() over web streams.
export const promisesFinishedWebStreams = {
  async test() {
    const rs = new ReadableStream({
      start(controller) {
        controller.close();
      },
    });
    if (!usingTsImpl) {
      await rejects(promises.finished(rs), unsupported('finished()'));
      return;
    }
    await promises.finished(rs);
    const boom = new Error('promised error');
    const ws = new WritableStream({
      start(controller) {
        controller.error(boom);
      },
    });
    await rejects(promises.finished(ws), (err) => err === boom);
  },
};

// finished() on a teed source. Under the queued tee model the source's
// stream consumes nothing after tee(): it is closed by the source's own
// events — close requested, cancelled, errored — never by the branches'
// progress (src/per_isolate/webstreams/AGENTS.md). Closing the source
// settles it at once, before any branch has read, and the branches still
// deliver what was buffered. (The spec's source closes once the tee's
// reader has drained it.)
export const finishedObservesTeedSourceClose = {
  async test() {
    if (!usingTsImpl) return;
    for (const bytes of [false, true]) {
      const { stream: source, controller, chunk } = sourceStream(bytes);
      // Registered before the tee, so its closed promise already exists.
      const before = callbackOf((cb) => finished(source, cb));
      const [branch1, branch2] = source.tee();
      const after = callbackOf((cb) => finished(source, cb));
      controller.enqueue(chunk);
      strictEqual(await settlement(before), 'pending');
      controller.close();
      strictEqual(await before, undefined);
      strictEqual(await after, undefined);
      for (const branch of [branch1, branch2]) {
        const reader = branch.getReader();
        strictEqual((await reader.read()).done, false);
        strictEqual((await reader.read()).done, true);
      }
    }
  },
};

// close() before tee() with a chunk still buffered: the source stops
// consuming at the tee, and that is when its stream closes.
export const finishedObservesTeedSourceClosedBeforeTee = {
  async test() {
    if (!usingTsImpl) return;
    for (const bytes of [false, true]) {
      const { stream: source, controller, chunk } = sourceStream(bytes);
      controller.enqueue(chunk);
      controller.close();
      const done = callbackOf((cb) => finished(source, cb));
      strictEqual(await settlement(done), 'pending');
      const [branch1, branch2] = source.tee();
      strictEqual(await done, undefined);
      for (const branch of [branch1, branch2]) {
        const reader = branch.getReader();
        strictEqual((await reader.read()).done, false);
        strictEqual((await reader.read()).done, true);
      }
    }
  },
};

// Both branches cancelling cancels the source, which closes its stream:
// finished() settles without error, also for a source that never closes
// on its own. One branch cancelling alone leaves it pending.
export const finishedObservesTeedSourceCancel = {
  async test() {
    if (!usingTsImpl) return;
    for (const bytes of [false, true]) {
      let cancelled = false;
      const { stream: source } = sourceStream(bytes, {
        pull() {
          return new Promise(() => {});
        },
        cancel() {
          cancelled = true;
        },
      });
      const [branch1, branch2] = source.tee();
      const done = callbackOf((cb) => finished(source, cb));
      // Pending until the sibling cancels too (the shared cancel promise).
      const first = branch1.cancel('first');
      strictEqual(await settlement(done), 'pending');
      strictEqual(cancelled, false);
      await branch2.cancel('second');
      await first;
      strictEqual(cancelled, true);
      strictEqual(await done, undefined);
    }
  },
};

// The source's error reaches its stream after tee(). An error after
// close(), while the branches still have chunks to drain, still errors the
// branches — the source's stream, closed by the close, keeps its
// settlement.
export const finishedObservesTeedSourceError = {
  async test() {
    if (!usingTsImpl) return;
    for (const bytes of [false, true]) {
      {
        const { stream: source, controller } = sourceStream(bytes);
        const [branch1, branch2] = source.tee();
        const done = callbackOf((cb) => finished(source, cb));
        const boom = new Error('errored after tee');
        controller.error(boom);
        strictEqual(await done, boom);
        await rejects(branch1.getReader().read(), (err) => err === boom);
        await rejects(branch2.getReader().read(), (err) => err === boom);
      }
      {
        const { stream: source, controller, chunk } = sourceStream(bytes);
        const [branch1, branch2] = source.tee();
        const done = callbackOf((cb) => finished(source, cb));
        controller.enqueue(chunk);
        controller.close();
        strictEqual(await done, undefined);
        const boom = new Error('errored after close');
        controller.error(boom);
        await rejects(branch1.getReader().read(), (err) => err === boom);
        await rejects(branch2.getReader().read(), (err) => err === boom);
      }
    }
  },
};

// addAbortSignal() on a web ReadableStream: aborting errors the stream with
// an AbortError (cause: the signal's reason); pending and later reads reject
// with it. The stream is never locked by the registration.
export const addAbortSignalErrorsReadable = {
  async test() {
    const rs = new ReadableStream({
      pull() {
        return new Promise(() => {});
      },
    });
    const controller = new AbortController();
    if (!usingTsImpl) {
      throws(
        () => addAbortSignal(controller.signal, rs),
        unsupported('addAbortSignal()')
      );
      strictEqual(rs.locked, false);
      return;
    }
    strictEqual(addAbortSignal(controller.signal, rs), rs);
    strictEqual(rs.locked, false);
    const reader = rs.getReader();
    const pending = reader.read();
    const reason = new Error('abandon');
    controller.abort(reason);
    await rejects(pending, (err) => {
      strictEqual(err.name, 'AbortError');
      strictEqual(err.code, 'ABORT_ERR');
      strictEqual(err.cause, reason);
      return true;
    });
    await rejects(reader.closed, { name: 'AbortError' });
  },
};

// addAbortSignal() on a web WritableStream: aborting errors the stream as
// its controller would — the writer's promises reject with the AbortError
// and the sink's abort algorithm is not invoked.
export const addAbortSignalErrorsWritable = {
  async test() {
    let aborted = false;
    const ws = new WritableStream({
      abort() {
        aborted = true;
      },
    });
    const controller = new AbortController();
    if (!usingTsImpl) {
      throws(
        () => addAbortSignal(controller.signal, ws),
        unsupported('addAbortSignal()')
      );
      return;
    }
    addAbortSignal(controller.signal, ws);
    const writer = ws.getWriter();
    controller.abort();
    await rejects(writer.closed, { name: 'AbortError', code: 'ABORT_ERR' });
    await rejects(writer.write(new Uint8Array(1)), { name: 'AbortError' });
    strictEqual(aborted, false);
  },
};

// An already-aborted signal errors the stream at registration.
export const addAbortSignalAlreadyAborted = {
  async test() {
    if (!usingTsImpl) return;
    const rs = new ReadableStream();
    const controller = new AbortController();
    controller.abort();
    addAbortSignal(controller.signal, rs);
    await rejects(rs.getReader().read(), { name: 'AbortError' });
  },
};

// addAbortSignal() on one branch of a tee errors that branch alone: the
// sibling keeps its buffered chunks, keeps receiving what the source
// enqueues afterwards, and closes with the source. The source is not
// cancelled while the sibling is still reading.
export const addAbortSignalOnTeeBranchSparesSibling = {
  async test() {
    if (!usingTsImpl) return;
    let sourceController;
    let cancelReason;
    const source = new ReadableStream({
      start(c) {
        sourceController = c;
        c.enqueue('a');
        c.enqueue('b');
      },
      cancel(reason) {
        cancelReason = reason;
      },
    });
    const [branch1, branch2] = source.tee();
    const controller = new AbortController();
    addAbortSignal(controller.signal, branch1);
    const reader1 = branch1.getReader();
    const reader2 = branch2.getReader();
    strictEqual((await reader1.read()).value, 'a');
    strictEqual((await reader1.read()).value, 'b');
    const pending = reader1.read();
    const reason = new Error('one consumer gives up');
    controller.abort(reason);
    await rejects(pending, (err) => {
      strictEqual(err.name, 'AbortError');
      strictEqual(err.cause, reason);
      return true;
    });
    await rejects(reader1.closed, { name: 'AbortError' });
    // The sibling's buffered data and later chunks are intact.
    strictEqual((await reader2.read()).value, 'a');
    strictEqual((await reader2.read()).value, 'b');
    sourceController.enqueue('c');
    strictEqual((await reader2.read()).value, 'c');
    strictEqual(cancelReason, undefined);
    sourceController.close();
    strictEqual((await reader2.read()).done, true);
    await reader2.closed;
  },
};

// addAbortSignal() on the source itself errors every branch, as its
// controller's error() does. Once the source's stream has closed — for a
// teed source, at close() — the node layer treats it as finished:
// addAbortSignal() stops listening, as on any finished stream, and the
// hook is a no-op on a stream that is no longer readable, so a later abort
// leaves the branches to drain what was buffered. (The controller's own
// error() still errors them: finishedObservesTeedSourceError.)
export const addAbortSignalOnTeedSourceErrorsBranches = {
  async test() {
    if (!usingTsImpl) return;
    for (const bytes of [false, true]) {
      {
        const { stream: source } = sourceStream(bytes);
        const [branch1, branch2] = source.tee();
        const controller = new AbortController();
        addAbortSignal(controller.signal, source);
        controller.abort();
        await rejects(branch1.getReader().read(), { name: 'AbortError' });
        await rejects(branch2.getReader().read(), { name: 'AbortError' });
      }
      {
        const { stream: source, controller, chunk } = sourceStream(bytes);
        const [branch1, branch2] = source.tee();
        const ac = new AbortController();
        addAbortSignal(ac.signal, source);
        controller.enqueue(chunk);
        controller.close();
        strictEqual(await callbackOf((cb) => finished(source, cb)), undefined);
        ac.abort();
        source[kControllerErrorFunction](new Error('after close'));
        for (const branch of [branch1, branch2]) {
          const reader = branch.getReader();
          strictEqual((await reader.read()).done, false);
          strictEqual((await reader.read()).done, true);
        }
      }
    }
  },
};

// Once the aborted branch's sibling cancels too, the source has no consumer
// left and is cancelled — the aborted branch counts as gone.
export const addAbortSignalOnTeeBranchThenSiblingCancel = {
  async test() {
    if (!usingTsImpl) return;
    let cancelReason;
    const source = new ReadableStream({
      pull() {
        return new Promise(() => {});
      },
      cancel(reason) {
        cancelReason = reason;
      },
    });
    const [branch1, branch2] = source.tee();
    const controller = new AbortController();
    addAbortSignal(controller.signal, branch1);
    controller.abort();
    await rejects(branch1.getReader().read(), { name: 'AbortError' });
    strictEqual(cancelReason, undefined);
    const bye = new Error('the other consumer leaves too');
    await branch2.cancel(bye);
    ok(cancelReason instanceof AggregateError);
    ok(cancelReason.errors.includes(bye));
    ok(cancelReason.errors.some((err) => err.name === 'AbortError'));
  },
};

// Once the source has requested close, a branch errored through the hook is
// not a cancel: the spec errors that branch's own controller and never
// forwards the error to the source. With the sibling already cancelled, the
// aborted branch is the last consumer, and the source's cancel() still never
// runs; the sibling's cancel promise settles once the aborted branch has
// left. Before close the same sequence cancels the source with both reasons
// (addAbortSignalOnTeeBranchThenSiblingCancel).
export const addAbortSignalOnTeeBranchAfterCloseSkipsSourceCancel = {
  async test() {
    if (!usingTsImpl) return;
    for (const bytes of [false, true]) {
      let cancelCalls = 0;
      const {
        stream: source,
        controller,
        chunk,
      } = sourceStream(bytes, {
        cancel() {
          cancelCalls++;
        },
      });
      const [branch1, branch2] = source.tee();
      controller.enqueue(chunk);
      // branch1 still consumes, so the sibling's cancel promise waits on the
      // source. Close alone does not settle it: branch1 has not drained.
      const cancelled = branch2.cancel(new Error('the other consumer leaves'));
      strictEqual(await settlement(cancelled), 'pending');
      strictEqual(cancelCalls, 0);
      controller.close();
      strictEqual(await settlement(cancelled), 'pending');
      const ac = new AbortController();
      addAbortSignal(ac.signal, branch1);
      const reason = new Error('the last consumer gives up');
      ac.abort(reason);
      const reader = branch1.getReader();
      await rejects(reader.read(), (err) => {
        strictEqual(err.name, 'AbortError');
        strictEqual(err.cause, reason);
        return true;
      });
      await rejects(reader.closed, { name: 'AbortError' });
      // The last consumer gone, the source ends as when every consumer has
      // drained: the sibling's cancel settles without the source's cancel().
      strictEqual(await cancelled, undefined);
      strictEqual(await callbackOf((cb) => finished(source, cb)), undefined);
      strictEqual(cancelCalls, 0);
    }
  },
};

// A branch that has itself been teed is a permanently locked, inert shell:
// under the queued tee model its two branches took its place as consumers
// of the shared queue, and there is no per-branch controller for the hook
// to error (a deliberate divergence from the spec's tee). Aborting it
// changes nothing — not for its branches, not for its sibling — and the
// source is cancelled only once every consumer has left, with each one's
// reason (before close is requested; after it, see
// addAbortSignalOnTeeBranchAfterCloseSkipsSourceCancel).
export const addAbortSignalOnTeedAwayBranchIsInert = {
  async test() {
    if (!usingTsImpl) return;
    let sourceController;
    let cancelReason;
    const source = new ReadableStream({
      start(c) {
        sourceController = c;
      },
      cancel(reason) {
        cancelReason = reason;
      },
    });
    const [a, b] = source.tee();
    const [a1, a2] = a.tee();
    strictEqual(a.locked, true);
    const controller = new AbortController();
    addAbortSignal(controller.signal, a);
    const readers = [a1, a2, b].map((branch) => branch.getReader());
    const pending = readers.map((reader) => reader.read());
    controller.abort(new Error('aimed at the teed-away branch'));
    sourceController.enqueue('x');
    for (const read of pending) {
      strictEqual((await read).value, 'x');
    }
    strictEqual(a.locked, true);
    strictEqual(cancelReason, undefined);
    const reasons = [new Error('a1'), new Error('a2'), new Error('b')];
    const cancels = readers.map((reader, i) => reader.cancel(reasons[i]));
    await Promise.all(cancels);
    ok(cancelReason instanceof AggregateError);
    strictEqual(cancelReason.errors.length, 3);
    for (let i = 0; i < reasons.length; i++) {
      strictEqual(cancelReason.errors[i], reasons[i]);
    }
  },
};

// The same on a byte stream, with a BYOB read pending on one of the
// branches: aborting the teed-away branch leaves it pending; aborting that
// branch itself rejects it, and its sibling and the outer branch read on.
export const addAbortSignalOnTeedAwayByteBranchIsInert = {
  async test() {
    if (!usingTsImpl) return;
    let sourceController;
    const source = new ReadableStream({
      type: 'bytes',
      start(c) {
        sourceController = c;
      },
    });
    const [a, b] = source.tee();
    const [a1, a2] = a.tee();
    const teedAway = new AbortController();
    addAbortSignal(teedAway.signal, a);
    const leaf = new AbortController();
    addAbortSignal(leaf.signal, a1);
    const byob = a1.getReader({ mode: 'byob' });
    const pendingByob = byob.read(new Uint8Array(8));
    const reader2 = a2.getReader();
    const readerB = b.getReader();
    teedAway.abort();
    strictEqual(
      await Promise.race([
        pendingByob,
        scheduler.wait(20).then(() => 'still pending'),
      ]),
      'still pending'
    );
    leaf.abort();
    await rejects(pendingByob, { name: 'AbortError' });
    sourceController.enqueue(new Uint8Array([7]));
    strictEqual((await reader2.read()).value[0], 7);
    strictEqual((await readerB.read()).value[0], 7);
    sourceController.close();
    strictEqual((await reader2.read()).done, true);
    strictEqual((await readerB.read()).done, true);
  },
};

// A branch's cancel() promise settles with the source's cleanup, whether
// the sibling is aborted before or after the cancel: a deferred cleanup
// keeps it pending until the cleanup is done, a failing one rejects it.
export const addAbortSignalOnTeeBranchSettlesWithSourceCleanup = {
  async test() {
    if (!usingTsImpl) return;
    for (const abortFirst of [false, true]) {
      let finishCleanup;
      const deferred = new ReadableStream({
        pull() {
          return new Promise(() => {});
        },
        cancel() {
          return new Promise((resolve) => {
            finishCleanup = resolve;
          });
        },
      });
      const [b1, b2] = deferred.tee();
      const ac = new AbortController();
      addAbortSignal(ac.signal, b2);
      let cancelled;
      if (abortFirst) {
        ac.abort();
        cancelled = b1.cancel('done');
      } else {
        cancelled = b1.cancel('done');
        ac.abort();
      }
      let settled = false;
      cancelled.then(
        () => (settled = true),
        () => (settled = true)
      );
      await scheduler.wait(5);
      strictEqual(settled, false);
      finishCleanup();
      await cancelled;

      const cleanupFailure = new Error('cleanup failed');
      const failing = new ReadableStream({
        pull() {
          return new Promise(() => {});
        },
        cancel() {
          throw cleanupFailure;
        },
      });
      const [c1, c2] = failing.tee();
      const ac2 = new AbortController();
      addAbortSignal(ac2.signal, c2);
      let failed;
      if (abortFirst) {
        ac2.abort();
        failed = c1.cancel('done');
      } else {
        failed = c1.cancel('done');
        ac2.abort();
      }
      await rejects(failed, (err) => err === cleanupFailure);
    }
  },
};

// The same for a byte stream's tee: a pending BYOB read on the aborted
// branch rejects, and the sibling keeps reading what arrives afterwards.
export const addAbortSignalOnByteTeeBranchSparesSibling = {
  async test() {
    if (!usingTsImpl) return;
    let sourceController;
    const source = new ReadableStream({
      type: 'bytes',
      start(c) {
        sourceController = c;
      },
    });
    const [branch1, branch2] = source.tee();
    const controller = new AbortController();
    addAbortSignal(controller.signal, branch1);
    const reader1 = branch1.getReader({ mode: 'byob' });
    const reader2 = branch2.getReader();
    const pending = reader1.read(new Uint8Array(4));
    controller.abort();
    await rejects(pending, { name: 'AbortError' });
    await rejects(reader1.closed, { name: 'AbortError' });
    sourceController.enqueue(new Uint8Array([1, 2, 3]));
    const { value } = await reader2.read();
    strictEqual(Array.from(value).join(','), '1,2,3');
    sourceController.close();
    strictEqual((await reader2.read()).done, true);
  },
};

// The hooks cover runtime-provided (native-backed) streams too: aborting a
// Response body errors its reads. (In Node a Response body is a byte
// stream and its hook is a no-op there, so this registration does nothing.)
export const addAbortSignalOnResponseBody = {
  async test() {
    if (!usingTsImpl) return;
    const body = new Response('never fully read').body;
    const controller = new AbortController();
    addAbortSignal(controller.signal, body);
    const reader = body.getReader();
    controller.abort();
    await rejects(reader.read(), { name: 'AbortError' });
    await rejects(reader.closed, { name: 'AbortError' });
  },
};

// Never settled, to be looked at separately: finished() on a branch that
// has itself been teed (a shell that is not the controller's stream, so
// none of the source's events reach it), and on the source of a
// native-backed tee (a Response body: its C++ source is swapped out at tee
// time while the data keeps flowing into the branch sources, so nothing
// reports its end). Both stay pending after every branch has drained; the
// source of a queued tee settles.
export const finishedOnTeedAwayShellStaysPending = {
  async test() {
    if (!usingTsImpl) return;
    const source = new ReadableStream({
      start(c) {
        c.enqueue('a');
        c.close();
      },
    });
    const [a, b] = source.tee();
    const [a1, a2] = a.tee();
    const shell = callbackOf((cb) => finished(a, cb));
    const body = new Response('x').body;
    const [n1, n2] = body.tee();
    const native = callbackOf((cb) => finished(body, cb));
    for (const branch of [a1, a2, b, n1, n2]) {
      const reader = branch.getReader();
      while (!(await reader.read()).done);
    }
    strictEqual(await callbackOf((cb) => finished(source, cb)), undefined);
    strictEqual(await settlement(shell), 'pending');
    strictEqual(await settlement(native), 'pending');
  },
};
