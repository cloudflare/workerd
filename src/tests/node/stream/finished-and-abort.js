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
