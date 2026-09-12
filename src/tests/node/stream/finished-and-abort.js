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
// in Node. The C++ streams carry neither, and the APIs fail up front with
// ERR_WEB_STREAM_INTEROP_UNSUPPORTED rather than partially working.

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

// A branch that has itself been teed is a permanently locked, inert shell:
// under the queued tee model its two branches took its place as consumers
// of the shared queue, and there is no per-branch controller for the hook
// to error (a deliberate divergence from the spec's tee). Aborting it
// changes nothing — not for its branches, not for its sibling — and the
// source is cancelled only once every consumer has left, with each one's
// reason.
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
// Response body errors its reads.
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
